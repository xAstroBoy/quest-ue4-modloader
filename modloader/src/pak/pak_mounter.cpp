// modloader/src/pak/pak_mounter.cpp
// Mount external .pak files via FPakPlatformFile::Mount
// ═══════════════════════════════════════════════════════════════════════
// APPROACH: Hook the engine's own Mount and MountAllPakFiles functions
// with Dobby BEFORE the engine's init runs. The main boot thread has a
// 5-second delay for JNI readiness, but the engine starts initializing
// immediately. An early hook thread (zero delay) installs these hooks
// before the engine calls MountAllPakFiles, so we can inject our custom
// PAKs into the engine's OWN mount sequence at high priority.
//
// This guarantees:
//   1. Our hooks fire when the engine mounts its PAKs (not after)
//   2. Custom PAKs are mounted INSIDE the engine's mount pass
//   3. Custom PAKs at priority 1000+ override game PAKs for all future loads
// ═══════════════════════════════════════════════════════════════════════

#include "modloader/pak_mounter.h"
#include "modloader/symbols.h"
#include "modloader/logger.h"
#include "modloader/paths.h"
#include "modloader/game_profile.h"

#include <dobby.h>
#include <dlfcn.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <cstring>
#include <vector>
#include <algorithm>
#include <atomic>
#include <android/log.h>

namespace pak_mounter
{

    // ═══ State ══════════════════════════════════════════════════════════════
    static std::vector<PakInfo> s_mounted;
    static std::mutex s_pak_mutex;

    // Cached FPakPlatformFile instance — set by hook or FPlatformFileManager
    static void *s_pak_platform_file = nullptr;
    static bool s_pak_instance_tried = false;

    // ═══ Dobby hook state ═══════════════════════════════════════════════════
    // Original Mount function trampoline (set by Dobby — bypasses the hook)
    static ue::PakMountFn s_original_mount = nullptr;

    // Captured FPakPlatformFile instance from the engine's own Mount call
    static std::atomic<bool> s_capture_done{false};
    static std::atomic<bool> s_custom_mount_done{false};
    static std::atomic<bool> s_early_hooks_installed{false};

    // MountAllPakFiles originals — real signature:
    // void MountAllPakFiles(const TArray<FString>& PakFolders, const FString& MountPoint)
    // ARM64 ABI: x0=this, x1=&PakFolders, x2=&MountPoint
    using MountAllPakFilesFn = void (*)(void *, void *, void *);
    static MountAllPakFilesFn s_original_mount_all1 = nullptr;
    static MountAllPakFilesFn s_original_mount_all2 = nullptr;

    // Early paks path — built dynamically from game profile package name.
    // Before JNI paths::init is available, we construct the path from the
    // package name detected by game_profile::init() (which reads /proc/self/cmdline).
    // We use the Android standard external files dir convention — this is NOT
    // hardcoding; it's the same path getExternalFilesDir(null) returns.
    // Every app has read/write access to its own external files dir without root.
    static std::string build_early_paks_path()
    {
        const std::string &pkg = game_profile::package_name();
        if (pkg.empty())
        {
            // Read package name directly from /proc/self/cmdline as fallback
            char buf[256] = {};
            FILE *f = fopen("/proc/self/cmdline", "r");
            if (f)
            {
                size_t n = fread(buf, 1, sizeof(buf) - 1, f);
                fclose(f);
                if (n > 0 && buf[0] != '\0')
                {
                    return std::string("/storage/emulated/0/Android/data/") + buf + "/files/paks";
                }
            }
            // Absolute last resort — cannot determine package. This path won't
            // resolve to anything useful, but won't crash either.
            __android_log_print(ANDROID_LOG_ERROR, "UEModLoader",
                                "[PAK] CRITICAL: Cannot determine package name for PAK path!");
            return "";
        }
        return "/storage/emulated/0/Android/data/" + pkg + "/files/paks";
    }

    static std::string s_early_paks_path;

    // Forward declarations
    static void *get_pak_platform_file();
    static void mount_custom_paks_early(void *pak_pf);
    static void mount_custom_paks_now();

    // ═══ Helpers ════════════════════════════════════════════════════════════
    static bool file_exists(const std::string &path)
    {
        struct stat st;
        return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
    }

    static bool dir_exists(const std::string &path)
    {
        struct stat st;
        return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    }

    // Convert UTF-8 std::string to std::u16string (char16_t)
    // UE4 Android uses char16_t for TCHAR, not wchar_t
    static std::u16string to_u16(const std::string &utf8)
    {
        std::u16string result;
        result.reserve(utf8.size());
        for (unsigned char c : utf8)
        {
            result.push_back(static_cast<char16_t>(c));
        }
        return result;
    }

    // Convert char16_t* to std::string for logging
    static std::string u16_to_log(const char16_t *s)
    {
        if (!s)
            return "(null)";
        std::string result;
        while (*s)
        {
            char c = static_cast<char>(*s & 0x7F);
            result += (c >= 32 && c < 127) ? c : '?';
            ++s;
        }
        return result;
    }

    void reset_cache()
    {
        std::lock_guard<std::mutex> lock(s_pak_mutex);
        s_pak_platform_file = nullptr;
        s_pak_instance_tried = false;
        logger::log_info("PAK", "Cache reset — will re-resolve FPakPlatformFile on next mount");
    }

    // ═══ Mount custom PAKs from app sandbox path — used by EARLY hooks ═══
    // Called inside MountAllPakFiles hooks BEFORE the logger or paths module
    // is initialized. Uses __android_log_print directly.
    // Path is derived from the app's own package name (no root needed).
    static void mount_custom_paks_early(void *pak_pf)
    {
        bool expected = false;
        if (!s_custom_mount_done.compare_exchange_strong(expected, true))
        {
            return; // Already mounted
        }

        if (!s_original_mount || !pak_pf)
        {
            __android_log_print(ANDROID_LOG_ERROR, "UEModLoader",
                                "[PAK-EARLY] Cannot mount: mount_fn=%p, instance=%p",
                                (void *)s_original_mount, pak_pf);
            s_custom_mount_done.store(false); // Allow retry
            return;
        }

        __android_log_print(ANDROID_LOG_INFO, "UEModLoader",
                            "[PAK-EARLY] === MOUNTING CUSTOM PAKS INSIDE ENGINE MOUNT PASS ===");

        // Build path if not yet done
        if (s_early_paks_path.empty())
        {
            s_early_paks_path = build_early_paks_path();
        }
        if (s_early_paks_path.empty())
        {
            __android_log_print(ANDROID_LOG_ERROR, "UEModLoader",
                                "[PAK-EARLY] Cannot determine paks directory — skipping");
            s_custom_mount_done.store(false);
            return;
        }
        const char *paks_path = s_early_paks_path.c_str();

        // Open the paks directory
        DIR *dir = opendir(paks_path);
        if (!dir)
        {
            __android_log_print(ANDROID_LOG_WARN, "UEModLoader",
                                "[PAK-EARLY] Paks dir not found: %s — creating", paks_path);
            mkdir(paks_path, 0755);
            return;
        }

        // Collect .pak files
        std::vector<std::string> pak_files;
        struct dirent *entry;
        while ((entry = readdir(dir)) != nullptr)
        {
            if (entry->d_name[0] == '.')
                continue;
            std::string name(entry->d_name);
            if (name.size() > 4 && name.substr(name.size() - 4) == ".pak")
            {
                pak_files.push_back(name);
            }
        }
        closedir(dir);

        if (pak_files.empty())
        {
            __android_log_print(ANDROID_LOG_INFO, "UEModLoader",
                                "[PAK-EARLY] No custom PAK files in %s", paks_path);
            return;
        }

        std::sort(pak_files.begin(), pak_files.end());

        int mounted_count = 0;
        for (size_t i = 0; i < pak_files.size(); i++)
        {
            std::string full_path = s_early_paks_path + "/" + pak_files[i];

            struct stat st;
            if (stat(full_path.c_str(), &st) != 0)
            {
                __android_log_print(ANDROID_LOG_ERROR, "UEModLoader",
                                    "[PAK-EARLY] stat failed: %s", full_path.c_str());
                continue;
            }

            // Convert to char16_t for UE4
            std::u16string u16path = to_u16(full_path);
            uint32_t priority = 1000 + static_cast<uint32_t>(i);

            __android_log_print(ANDROID_LOG_INFO, "UEModLoader",
                                "[PAK-EARLY] Mounting: %s (%.1f MB, priority=%u)",
                                pak_files[i].c_str(), st.st_size / (1024.0 * 1024.0), priority);

            // Use the trampoline to bypass our own hook
            bool result = s_original_mount(pak_pf, u16path.c_str(), priority, nullptr, true);

            if (result)
            {
                __android_log_print(ANDROID_LOG_INFO, "UEModLoader",
                                    "[PAK-EARLY] MOUNTED: %s @ priority %u", pak_files[i].c_str(), priority);
                mounted_count++;

                // Also record in our tracking
                std::lock_guard<std::mutex> lock(s_pak_mutex);
                PakInfo pi;
                pi.name = pak_files[i];
                pi.path = full_path;
                pi.mounted = true;
                pi.file_size = st.st_size;
                s_mounted.push_back(pi);
            }
            else
            {
                __android_log_print(ANDROID_LOG_ERROR, "UEModLoader",
                                    "[PAK-EARLY] FAILED: %s — Mount returned false", pak_files[i].c_str());
            }
        }

        __android_log_print(ANDROID_LOG_INFO, "UEModLoader",
                            "[PAK-EARLY] Mounted %d/%zu custom PAKs INSIDE engine mount pass",
                            mounted_count, pak_files.size());
    }

    // ═══ Mount custom PAKs synchronously — fallback for late init ══════════
    // Used by install_hooks() proactive path if early hooks didn't fire
    static void mount_custom_paks_now()
    {
        bool expected = false;
        if (!s_custom_mount_done.compare_exchange_strong(expected, true))
        {
            return; // Already mounted (by early hooks or previous call)
        }

        logger::log_info("PAK", "=== SYNCHRONOUS PAK MOUNT — mounting custom PAKs at priority 1000+ ===");

        int count = mount_all();

        if (count > 0)
        {
            logger::log_info("PAK", "Synchronous mount complete — %d custom PAK(s) mounted at HIGH priority", count);
        }
        else
        {
            std::string paks_path = paths::paks_dir();
            DIR *dir = opendir(paks_path.c_str());
            if (dir)
            {
                int pak_count = 0;
                struct dirent *entry;
                while ((entry = readdir(dir)) != nullptr)
                {
                    std::string name(entry->d_name);
                    if (name.size() > 4 && name.substr(name.size() - 4) == ".pak")
                    {
                        pak_count++;
                    }
                }
                closedir(dir);
                if (pak_count == 0)
                {
                    logger::log_info("PAK", "No custom PAK files in %s — nothing to mount", paks_path.c_str());
                }
                else
                {
                    logger::log_warn("PAK", "Found %d PAK file(s) but mount_all returned 0", pak_count);
                }
            }
            else
            {
                logger::log_info("PAK", "Paks directory does not exist: %s", paks_path.c_str());
            }
        }
    }

    // ═══ Capture the FPakPlatformFile instance from engine call ═════════════
    static void capture_instance(void *this_ptr, const char *source)
    {
        if (s_capture_done.load(std::memory_order_relaxed))
            return;

        // Atomic compare-exchange to ensure only one capture
        bool expected = false;
        if (!s_capture_done.compare_exchange_strong(expected, true))
            return;

        s_pak_platform_file = this_ptr;
        s_pak_instance_tried = true;

        __android_log_print(ANDROID_LOG_INFO, "UEModLoader",
                            "[PAK] !!! FPakPlatformFile CAPTURED via %s @ 0x%lX !!!",
                            source, reinterpret_cast<uintptr_t>(this_ptr));
    }

    // ═══ Dobby hook: FPakPlatformFile::Mount ════════════════════════════════
    // Fires every time the engine mounts ANY PAK — capture instance on first call
    static bool hooked_pak_mount(void *this_ptr, const char16_t *pak_filename,
                                 uint32_t pak_order, const char16_t *mount_point,
                                 bool load_index)
    {
        // Capture instance on first engine Mount call
        capture_instance(this_ptr, "Mount");

        // Log what the engine is mounting (first 10 calls only to avoid spam)
        static int log_count = 0;
        if (log_count < 10)
        {
            std::string fname = u16_to_log(pak_filename);
            auto slash = fname.rfind('/');
            if (slash != std::string::npos)
                fname = fname.substr(slash + 1);
            __android_log_print(ANDROID_LOG_INFO, "UEModLoader",
                                "[PAK] [Engine Mount #%d] %s (order=%u)", log_count, fname.c_str(), pak_order);
            log_count++;
        }

        // Call original Mount — let the engine proceed normally
        return s_original_mount(this_ptr, pak_filename, pak_order, mount_point, load_index);
    }

    // ═══ Dobby hook: MountAllPakFiles #1 ════════════════════════════════════
    // Key hook: fires when engine initiates bulk PAK loading.
    // We let the engine mount its PAKs FIRST (original call), then IMMEDIATELY
    // mount our custom PAKs at high priority INTO the same batch.
    // Real signature: void MountAllPakFiles(const TArray<FString>&, const FString&)
    static void hooked_mount_all_1(void *this_ptr, void *pak_folders, void *mount_point)
    {
        capture_instance(this_ptr, "MountAllPakFiles#1");

        __android_log_print(ANDROID_LOG_INFO, "UEModLoader",
                            "[PAK] MountAllPakFiles#1 called (this=%p, folders=%p, mount=%p) — running original first...",
                            this_ptr, pak_folders, mount_point);

        // Let engine mount its game PAKs — pass all arguments through
        s_original_mount_all1(this_ptr, pak_folders, mount_point);

        __android_log_print(ANDROID_LOG_INFO, "UEModLoader",
                            "[PAK] MountAllPakFiles#1 original returned — mounting custom PAKs NOW");

        // Mount our custom PAKs immediately AFTER the engine's own PAKs
        // This is inside the engine's init — before it starts loading level assets
        mount_custom_paks_early(this_ptr);
    }

    // ═══ Dobby hook: MountAllPakFiles #2 ════════════════════════════════════
    static void hooked_mount_all_2(void *this_ptr, void *pak_folders, void *mount_point)
    {
        capture_instance(this_ptr, "MountAllPakFiles#2");

        __android_log_print(ANDROID_LOG_INFO, "UEModLoader",
                            "[PAK] MountAllPakFiles#2 called (this=%p, folders=%p, mount=%p) — running original first...",
                            this_ptr, pak_folders, mount_point);

        s_original_mount_all2(this_ptr, pak_folders, mount_point);

        __android_log_print(ANDROID_LOG_INFO, "UEModLoader",
                            "[PAK] MountAllPakFiles#2 original returned — mounting custom PAKs NOW");

        mount_custom_paks_early(this_ptr);
    }

    // ═══ Install early Dobby hooks — called from early hook thread ══════════
    // This runs BEFORE the main boot thread, BEFORE the engine's init.
    // Only needs libUE4.so base address and dlsym handle.
    void install_early_hooks(uintptr_t base, void *lib_handle)
    {
        if (s_early_hooks_installed.load())
            return;

        // Build early paks path from game profile
        s_early_paks_path = build_early_paks_path();

        __android_log_print(ANDROID_LOG_INFO, "UEModLoader",
                            "[PAK-EARLY] Installing Dobby hooks BEFORE engine init (base=0x%lX, paks=%s)",
                            (unsigned long)base, s_early_paks_path.c_str());

        // ── Resolve FPakPlatformFile::Mount via dlsym ──
        // Try multiple mangled name variants — differs between UE4/UE5 and compiler versions
        void *mount_addr = nullptr;
        if (lib_handle)
        {
            // Common mangled names for FPakPlatformFile::Mount across UE versions
            static const char *mount_manglings[] = {
                "_ZN16FPakPlatformFile5MountEPKDsjS1_b",  // UE4 typical
                "_ZN18FPakPlatformFile5MountEPKcijPKS0_", // alternate UE4
                "_ZN20FPakPlatformFile5MountEPKDsjS1_b",  // UE4 with longer class name
                "_ZN18FPakPlatformFile5MountEPKDsjS1_b",  // UE5 variant
                nullptr};
            for (int i = 0; mount_manglings[i] && !mount_addr; i++)
            {
                mount_addr = dlsym(lib_handle, mount_manglings[i]);
            }
        }

        // Fallback: use game-profile offset for Mount symbol
        if (!mount_addr)
        {
            const auto &fallbacks = game_profile::profile().fallback_offsets;
            for (const auto &fb : fallbacks)
            {
                if (fb.symbol_name == "FPakPlatformFile::Mount" && fb.offset != 0)
                {
                    mount_addr = reinterpret_cast<void *>(base + fb.offset);
                    __android_log_print(ANDROID_LOG_INFO, "UEModLoader",
                                        "[PAK-EARLY] Mount from profile fallback offset 0x%lX → 0x%lX",
                                        (unsigned long)fb.offset, (unsigned long)reinterpret_cast<uintptr_t>(mount_addr));
                    break;
                }
            }
        }

        if (!mount_addr)
        {
            __android_log_print(ANDROID_LOG_WARN, "UEModLoader",
                                "[PAK-EARLY] Mount not found via dlsym or fallback — early PAK hooks limited");
        }
        else
        {
            __android_log_print(ANDROID_LOG_INFO, "UEModLoader",
                                "[PAK-EARLY] Mount resolved @ 0x%lX",
                                (unsigned long)reinterpret_cast<uintptr_t>(mount_addr));
        }

        // ── Hook 1: FPakPlatformFile::Mount ──
        int status = -1;
        if (mount_addr)
        {
            status = DobbyHook(
                mount_addr,
                reinterpret_cast<dobby_dummy_func_t>(hooked_pak_mount),
                reinterpret_cast<dobby_dummy_func_t *>(&s_original_mount));
            if (status == 0)
            {
                __android_log_print(ANDROID_LOG_INFO, "UEModLoader",
                                    "[PAK-EARLY] Hook 1: Mount hooked @ 0x%lX",
                                    (unsigned long)reinterpret_cast<uintptr_t>(mount_addr));
            }
            else
            {
                __android_log_print(ANDROID_LOG_ERROR, "UEModLoader",
                                    "[PAK-EARLY] Hook 1: Dobby FAILED on Mount (status=%d)", status);
            }
        }

        // ── Hook 2+3: MountAllPakFiles — resolve from game profile fallbacks ──
        // These offsets are game-specific and come from the game profile.
        // We look for fallback entries named "MountAllPakFiles_1" and "MountAllPakFiles_2".
        // Also try dlsym for the mangled names.
        void *mount_all1_addr = nullptr;
        void *mount_all2_addr = nullptr;

        if (lib_handle)
        {
            // Try dlsym for MountAllPakFiles variants
            static const char *mapf_manglings[] = {
                "_ZN16FPakPlatformFile16MountAllPakFilesERK6TArrayI7FStringS2_IS1_17FDefaultAllocatorES4_ERKS1_",
                "_ZN18FPakPlatformFile16MountAllPakFilesERK6TArrayI7FStringS2_IS1_17FDefaultAllocatorES4_ERKS1_",
                nullptr};
            for (int i = 0; mapf_manglings[i]; i++)
            {
                void *addr = dlsym(lib_handle, mapf_manglings[i]);
                if (addr)
                {
                    if (!mount_all1_addr)
                        mount_all1_addr = addr;
                    else if (!mount_all2_addr)
                        mount_all2_addr = addr;
                }
            }
        }

        // Fallback: game profile offsets
        if (!mount_all1_addr || !mount_all2_addr)
        {
            const auto &fallbacks = game_profile::profile().fallback_offsets;
            for (const auto &fb : fallbacks)
            {
                if (fb.symbol_name == "MountAllPakFiles_1" && fb.offset != 0 && !mount_all1_addr)
                {
                    mount_all1_addr = reinterpret_cast<void *>(base + fb.offset);
                }
                if (fb.symbol_name == "MountAllPakFiles_2" && fb.offset != 0 && !mount_all2_addr)
                {
                    mount_all2_addr = reinterpret_cast<void *>(base + fb.offset);
                }
            }
        }

        // Hook MountAllPakFiles #1
        if (mount_all1_addr)
        {
            status = DobbyHook(
                mount_all1_addr,
                reinterpret_cast<dobby_dummy_func_t>(hooked_mount_all_1),
                reinterpret_cast<dobby_dummy_func_t *>(&s_original_mount_all1));
            if (status == 0)
            {
                __android_log_print(ANDROID_LOG_INFO, "UEModLoader",
                                    "[PAK-EARLY] Hook 2: MountAllPakFiles#1 hooked @ 0x%lX",
                                    (unsigned long)reinterpret_cast<uintptr_t>(mount_all1_addr));
            }
            else
            {
                __android_log_print(ANDROID_LOG_ERROR, "UEModLoader",
                                    "[PAK-EARLY] Hook 2: Dobby FAILED on MountAllPakFiles#1 (status=%d)", status);
            }
        }
        else
        {
            __android_log_print(ANDROID_LOG_WARN, "UEModLoader",
                                "[PAK-EARLY] MountAllPakFiles#1 not found — skipping hook");
        }

        // Hook MountAllPakFiles #2
        if (mount_all2_addr)
        {
            status = DobbyHook(
                mount_all2_addr,
                reinterpret_cast<dobby_dummy_func_t>(hooked_mount_all_2),
                reinterpret_cast<dobby_dummy_func_t *>(&s_original_mount_all2));
            if (status == 0)
            {
                __android_log_print(ANDROID_LOG_INFO, "UEModLoader",
                                    "[PAK-EARLY] Hook 3: MountAllPakFiles#2 hooked @ 0x%lX",
                                    (unsigned long)reinterpret_cast<uintptr_t>(mount_all2_addr));
            }
            else
            {
                __android_log_print(ANDROID_LOG_ERROR, "UEModLoader",
                                    "[PAK-EARLY] Hook 3: Dobby FAILED on MountAllPakFiles#2 (status=%d)", status);
            }
        }
        else
        {
            __android_log_print(ANDROID_LOG_WARN, "UEModLoader",
                                "[PAK-EARLY] MountAllPakFiles#2 not found — skipping hook");
        }

        s_early_hooks_installed.store(true);

        __android_log_print(ANDROID_LOG_INFO, "UEModLoader",
                            "[PAK-EARLY] All hooks installed — waiting for engine to call MountAllPakFiles...");
    }

    // ═══ Install Dobby hooks (called from main boot thread) ═════════════════
    // If early hooks already installed, this is a no-op for hooks but still
    // does proactive FPlatformFileManager resolution.
    void install_hooks()
    {
        if (s_early_hooks_installed.load())
        {
            logger::log_info("PAK", "Early hooks already installed — skipping Dobby re-hook");
        }
        else
        {
            // Early hooks didn't install (early thread failed?) — install now as fallback
            logger::log_info("PAK", "Installing Dobby hooks (late fallback — early thread missed)...");

            uintptr_t base = symbols::lib_base();

            void *mount_addr = reinterpret_cast<void *>(symbols::PakMount);
            if (mount_addr && !s_original_mount)
            {
                int status = DobbyHook(
                    mount_addr,
                    reinterpret_cast<dobby_dummy_func_t>(hooked_pak_mount),
                    reinterpret_cast<dobby_dummy_func_t *>(&s_original_mount));
                if (status == 0)
                {
                    logger::log_info("PAK", "Hook 1/3: Mount hooked via Dobby @ 0x%lX",
                                     reinterpret_cast<uintptr_t>(mount_addr));
                }
                else
                {
                    logger::log_error("PAK", "Hook 1/3: Dobby FAILED to hook Mount (status=%d)", status);
                }
            }

            // Resolve MountAllPakFiles from game profile
            const auto &fallbacks = game_profile::profile().fallback_offsets;
            if (!s_original_mount_all1)
            {
                for (const auto &fb : fallbacks)
                {
                    if (fb.symbol_name == "MountAllPakFiles_1" && fb.offset != 0)
                    {
                        void *mount_all1_addr = reinterpret_cast<void *>(base + fb.offset);
                        int status = DobbyHook(
                            mount_all1_addr,
                            reinterpret_cast<dobby_dummy_func_t>(hooked_mount_all_1),
                            reinterpret_cast<dobby_dummy_func_t *>(&s_original_mount_all1));
                        if (status == 0)
                        {
                            logger::log_info("PAK", "Hook 2: MountAllPakFiles#1 hooked @ 0x%lX",
                                             reinterpret_cast<uintptr_t>(mount_all1_addr));
                        }
                        break;
                    }
                }
            }

            if (!s_original_mount_all2)
            {
                for (const auto &fb : fallbacks)
                {
                    if (fb.symbol_name == "MountAllPakFiles_2" && fb.offset != 0)
                    {
                        void *mount_all2_addr = reinterpret_cast<void *>(base + fb.offset);
                        int status = DobbyHook(
                            mount_all2_addr,
                            reinterpret_cast<dobby_dummy_func_t>(hooked_mount_all_2),
                            reinterpret_cast<dobby_dummy_func_t *>(&s_original_mount_all2));
                        if (status == 0)
                        {
                            logger::log_info("PAK", "Hook 3: MountAllPakFiles#2 hooked @ 0x%lX",
                                             reinterpret_cast<uintptr_t>(mount_all2_addr));
                        }
                        break;
                    }
                }
            }
        }

        // Check if custom PAKs already mounted by early hooks
        if (s_custom_mount_done.load())
        {
            logger::log_info("PAK", "Custom PAKs already mounted by early hooks — skipping proactive mount");

            // Log what was mounted
            std::lock_guard<std::mutex> lock(s_pak_mutex);
            for (const auto &p : s_mounted)
            {
                logger::log_info("PAK", "  [early] %s — %s", p.name.c_str(),
                                 p.mounted ? "MOUNTED" : "FAILED");
            }
            return;
        }

        // Proactive resolution — early hooks may not have fired yet
        logger::log_info("PAK", "Custom PAKs not yet mounted — trying proactive resolution...");
        void *pf = get_pak_platform_file();
        if (pf)
        {
            logger::log_info("PAK", "FPakPlatformFile found proactively @ 0x%lX",
                             reinterpret_cast<uintptr_t>(pf));
            s_capture_done.store(true, std::memory_order_relaxed);
            mount_custom_paks_now();
        }
        else
        {
            logger::log_warn("PAK", "FPakPlatformFile not available — relying on hooks");
        }
    }

    // ═══ Get FPakPlatformFile instance ══════════════════════════════════════
    // Prefers the captured instance from the hook. Falls back to FPlatformFileManager.
    static void *get_pak_platform_file()
    {
        // If already captured by hook or previous successful lookup, use it
        if (s_pak_platform_file)
            return s_pak_platform_file;

        // Fallback: resolve via FPlatformFileManager::Get().FindPlatformFile("PakFile")
        logger::log_info("PAK", "No hook capture yet — trying FPlatformFileManager fallback...");

        void *mgr_get_fn = symbols::resolve("_ZN20FPlatformFileManager3GetEv");
        if (!mgr_get_fn)
        {
            mgr_get_fn = symbols::resolve("FPlatformFileManager_Get");
        }
        if (!mgr_get_fn)
        {
            logger::log_error("PAK", "FPlatformFileManager::Get() not found");
            return nullptr;
        }

        auto get_fn = reinterpret_cast<ue::FPlatformFileManager_GetFn>(mgr_get_fn);
        void *mgr = get_fn();
        if (!mgr)
        {
            logger::log_error("PAK", "FPlatformFileManager::Get() returned null");
            return nullptr;
        }

        void *find_fn_addr = symbols::resolve("_ZN20FPlatformFileManager16FindPlatformFileEPKDs");
        if (!find_fn_addr)
        {
            find_fn_addr = symbols::resolve("FPlatformFileManager_FindPlatformFile");
        }
        if (!find_fn_addr)
        {
            logger::log_error("PAK", "FPlatformFileManager::FindPlatformFile() not found");
            return nullptr;
        }

        auto find_platform_file = reinterpret_cast<ue::FPlatformFileManager_FindPlatformFileFn>(find_fn_addr);
        static const char16_t pak_name[] = u"PakFile";
        void *pak_file = find_platform_file(mgr, pak_name);

        if (!pak_file)
        {
            logger::log_error("PAK", "FindPlatformFile(\"PakFile\") returned null");
            return nullptr;
        }

        logger::log_info("PAK", "FPakPlatformFile found via FPlatformFileManager @ 0x%lX",
                         reinterpret_cast<uintptr_t>(pak_file));
        s_pak_platform_file = pak_file;
        return pak_file;
    }

    // ═══ Get the Mount function to call ═════════════════════════════════════
    // Prefers the original trampoline (bypasses our hook). Falls back to symbols::PakMount.
    static ue::PakMountFn get_mount_fn()
    {
        if (s_original_mount)
            return s_original_mount;
        return symbols::PakMount;
    }

    // ═══ Mount a single .pak file ═══════════════════════════════════════════
    bool mount(const std::string &pak_name)
    {
        std::lock_guard<std::mutex> lock(s_pak_mutex);

        // Check if already mounted
        for (const auto &p : s_mounted)
        {
            if (p.name == pak_name && p.mounted)
            {
                logger::log_info("PAK", "'%s' already mounted", pak_name.c_str());
                return true;
            }
        }

        std::string paks_dir_path = paths::paks_dir();
        std::string full_path;

        // If pak_name already looks like a full path, use it directly
        if (!pak_name.empty() && pak_name[0] == '/')
        {
            full_path = pak_name;
        }
        else
        {
            if (pak_name.size() > 4 && pak_name.substr(pak_name.size() - 4) == ".pak")
            {
                full_path = paks_dir_path + "/" + pak_name;
            }
            else
            {
                full_path = paks_dir_path + "/" + pak_name + ".pak";
                if (!file_exists(full_path))
                {
                    full_path = paks_dir_path + "/" + pak_name;
                }
            }
        }

        if (!file_exists(full_path))
        {
            logger::log_error("PAK", "PAK file not found: %s", full_path.c_str());
            PakInfo pi;
            pi.name = pak_name;
            pi.path = full_path;
            pi.mounted = false;
            pi.error = "file not found";
            s_mounted.push_back(pi);
            return false;
        }

        struct stat st;
        stat(full_path.c_str(), &st);
        int64_t file_size = st.st_size;

        // Get the Mount function (prefer trampoline to bypass our hook)
        ue::PakMountFn mount_fn = get_mount_fn();
        if (!mount_fn)
        {
            logger::log_error("PAK", "No Mount function available — cannot mount '%s'", pak_name.c_str());
            PakInfo pi;
            pi.name = pak_name;
            pi.path = full_path;
            pi.mounted = false;
            pi.error = "PakMount not resolved";
            s_mounted.push_back(pi);
            return false;
        }

        // Get the FPakPlatformFile instance
        void *pak_pf = get_pak_platform_file();
        if (!pak_pf)
        {
            logger::log_error("PAK", "Cannot mount '%s' — FPakPlatformFile instance not found", pak_name.c_str());
            PakInfo pi;
            pi.name = pak_name;
            pi.path = full_path;
            pi.mounted = false;
            pi.error = "FPakPlatformFile instance not found";
            s_mounted.push_back(pi);
            return false;
        }

        // Convert path to char16_t (UE4 Android TCHAR = char16_t)
        std::u16string u16path = to_u16(full_path);

        // Mount with HIGH PRIORITY (1000) — overrides game PAKs (same as Frida modloader)
        // Game PAKs typically use priority 0-4. Priority 1000 ensures we always win.
        static int s_priority_counter = 0;
        uint32_t priority = 1000 + s_priority_counter++;

        logger::log_info("PAK", "Mounting: %s (%.1f MB, priority=%u, instance=0x%lX, fn=%s)",
                         full_path.c_str(),
                         file_size / (1024.0 * 1024.0),
                         priority,
                         reinterpret_cast<uintptr_t>(pak_pf),
                         s_original_mount ? "trampoline" : "direct");

        bool result = mount_fn(pak_pf, u16path.c_str(), priority, nullptr, true);

        PakInfo pi;
        pi.name = pak_name;
        pi.path = full_path;
        pi.mounted = result;
        pi.file_size = file_size;

        if (result)
        {
            logger::log_info("PAK", "MOUNTED: %s @ priority %u", pak_name.c_str(), priority);
        }
        else
        {
            pi.error = "Mount returned false";
            logger::log_error("PAK", "FAILED to mount: %s (Mount returned false)", pak_name.c_str());
        }

        s_mounted.push_back(pi);
        return result;
    }

    // ═══ Mount all paks in paks/ directory ══════════════════════════════════
    int mount_all()
    {
        std::string paks_path = paths::paks_dir();

        if (!dir_exists(paks_path))
        {
            logger::log_info("PAK", "Paks directory does not exist: %s — creating", paks_path.c_str());
            mkdir(paks_path.c_str(), 0755);
            return 0;
        }

        DIR *dir = opendir(paks_path.c_str());
        if (!dir)
        {
            logger::log_error("PAK", "Failed to open paks directory: %s", paks_path.c_str());
            return 0;
        }

        std::vector<std::string> pak_files;
        struct dirent *entry;
        while ((entry = readdir(dir)) != nullptr)
        {
            if (entry->d_name[0] == '.')
                continue;
            std::string name(entry->d_name);
            if (name.size() > 4 && name.substr(name.size() - 4) == ".pak")
            {
                pak_files.push_back(name);
            }
        }
        closedir(dir);

        std::sort(pak_files.begin(), pak_files.end());

        if (pak_files.empty())
        {
            logger::log_info("PAK", "No PAK files found in %s", paks_path.c_str());
            return 0;
        }

        logger::log_info("PAK", "Found %zu PAK file(s) to mount:", pak_files.size());
        for (size_t i = 0; i < pak_files.size(); i++)
        {
            logger::log_info("PAK", "  [%zu] %s", i, pak_files[i].c_str());
        }

        int mounted = 0;
        for (const auto &pak : pak_files)
        {
            if (mount(pak))
                mounted++;
        }

        logger::log_info("PAK", "Mounted %d/%zu PAK files from %s",
                         mounted, pak_files.size(), paks_path.c_str());
        return mounted;
    }

    // ═══ Query mounted paks ═════════════════════════════════════════════════
    const std::vector<PakInfo> &get_all()
    {
        return s_mounted;
    }

    bool is_mounted(const std::string &pak_name)
    {
        std::lock_guard<std::mutex> lock(s_pak_mutex);
        for (const auto &p : s_mounted)
        {
            if (p.name == pak_name && p.mounted)
                return true;
        }
        return false;
    }

} // namespace pak_mounter
