// modloader/src/mods/mod_loader.cpp
// Scans mods/ directory, loads each mod's main.lua in a sandboxed environment
// Tracks mod status, errors, supports hot-reload via ADB
// Includes SIGSEGV recovery to prevent one mod from crashing the entire process

#include "modloader/mod_loader.h"
#include "modloader/lua_engine.h"
#include "modloader/logger.h"
#include "modloader/paths.h"

#include <dirent.h>
#include <sys/stat.h>
#include <cstring>
#include <algorithm>
#include <signal.h>
#include <setjmp.h>
#include <atomic>

namespace mod_loader
{

    static std::vector<ModInfo> s_mods;
    static std::mutex s_mods_mutex;
    static std::atomic<int> s_active_mod_loaders{0};

    // ═══ SIGSEGV recovery for mod loading ═══════════════════════════════════
    // When a mod causes a SIGSEGV during loading, we recover via siglongjmp
    // instead of letting the process die. This allows the modloader to skip
    // the crashing mod and continue loading the remaining mods.
    //
    // CRITICAL: jmpbuf and flag must be thread_local. Signal handlers are
    // process-wide but SIGSEGV is delivered to the faulting thread.
    // Without thread_local, a game thread SIGSEGV during mod loading would
    // siglongjmp using the modloader thread's jmpbuf = undefined behavior.
    //
    // NOTE: These are NOT static — they are referenced by crash_handler.cpp
    // as the LOWEST PRIORITY recovery point in the unified signal handler.
    // This ensures that inner recovery guards (hook install, hook execution,
    // ProcessEvent, safe_call) fire BEFORE mod-loading recovery. Previously,
    // a separate mod_load_signal_handler would bypass those inner guards,
    // causing DobbyHook() crashes to kill entire mods instead of just
    // skipping the failed hook.
    thread_local sigjmp_buf s_mod_jmpbuf;
    thread_local volatile sig_atomic_t s_in_mod_loading = 0;

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

    // ═══ Scan and load all mods ═════════════════════════════════════════════
    int load_all()
    {
        s_active_mod_loaders.fetch_add(1, std::memory_order_relaxed);

        std::string mods_path = paths::mods_dir();

        if (!dir_exists(mods_path))
        {
            logger::log_warn("MOD", "Mods directory does not exist: %s", mods_path.c_str());
            // Try to create it
            mkdir(mods_path.c_str(), 0755);
            s_active_mod_loaders.fetch_sub(1, std::memory_order_relaxed);
            return 0;
        }

        DIR *dir = opendir(mods_path.c_str());
        if (!dir)
        {
            logger::log_error("MOD", "Failed to open mods directory: %s", mods_path.c_str());
            s_active_mod_loaders.fetch_sub(1, std::memory_order_relaxed);
            return 0;
        }

        std::vector<std::string> mod_dirs;
        struct dirent *entry;
        while ((entry = readdir(dir)) != nullptr)
        {
            // Skip . and ..
            if (entry->d_name[0] == '.')
                continue;

            std::string mod_path = mods_path + "/" + entry->d_name;
            if (!dir_exists(mod_path))
                continue;

            std::string main_lua = mod_path + "/main.lua";
            if (!file_exists(main_lua))
            {
                logger::log_warn("MOD", "Skipping '%s' — no main.lua found", entry->d_name);
                continue;
            }

            mod_dirs.push_back(entry->d_name);
        }
        closedir(dir);

        // Sort alphabetically for deterministic load order
        std::sort(mod_dirs.begin(), mod_dirs.end());

        int loaded = 0;
        int failed = 0;

        // NOTE: No separate signal handler installed here.
        // crash_handler.cpp's unified handler checks s_in_mod_loading as
        // the LOWEST priority recovery, ensuring inner guards (hook install,
        // hook execution, ProcessEvent, safe_call) fire first.

        for (const auto &mod_name : mod_dirs)
        {
            if (load_mod(mod_name))
            {
                loaded++;
            }
            else
            {
                failed++;
            }
        }

        logger::log_info("MOD", "Loaded %d mods, %d failed (from %s)", loaded, failed, mods_path.c_str());
        s_active_mod_loaders.fetch_sub(1, std::memory_order_relaxed);
        return loaded;
    }

    // ═══ Load a single mod ══════════════════════════════════════════════════
    bool load_mod(const std::string &name)
    {
        s_active_mod_loaders.fetch_add(1, std::memory_order_relaxed);

        std::lock_guard<std::mutex> lock(s_mods_mutex);

        // Check if already loaded
        for (auto &m : s_mods)
        {
            if (m.name == name && m.status == ModStatus::LOADED)
            {
                logger::log_warn("MOD", "Mod '%s' is already loaded", name.c_str());
                s_active_mod_loaders.fetch_sub(1, std::memory_order_relaxed);
                return true;
            }
        }

        std::string mods_path = paths::mods_dir();
        std::string mod_dir = mods_path + "/" + name;
        std::string main_lua = mod_dir + "/main.lua";

        if (!file_exists(main_lua))
        {
            logger::log_error("MOD", "Mod '%s': main.lua not found at %s", name.c_str(), main_lua.c_str());
            ModInfo mi;
            mi.name = name;
            mi.path = mod_dir;
            mi.status = ModStatus::FAILED;
            mi.error = "main.lua not found";
            mi.error_count = 1;
            s_mods.push_back(mi);
            s_active_mod_loaders.fetch_sub(1, std::memory_order_relaxed);
            return false;
        }

        logger::log_info("MOD", "Loading %s", main_lua.c_str());

        // Set up SIGSEGV recovery point — if the mod causes a crash,
        // siglongjmp brings us back here with the signal number
        s_in_mod_loading = 1;
        int crash_sig = sigsetjmp(s_mod_jmpbuf, 1);
        if (crash_sig != 0)
        {
            // We got here via siglongjmp — the mod CRASHED
            s_in_mod_loading = 0;

            const char *sig_name = "UNKNOWN";
            if (crash_sig == SIGSEGV)
                sig_name = "SIGSEGV";
            else if (crash_sig == SIGBUS)
                sig_name = "SIGBUS";
            else if (crash_sig == SIGABRT)
                sig_name = "SIGABRT";

            logger::log_error("MOD", "%s: CRASHED with %s during load — skipping to next mod",
                              name.c_str(), sig_name);
            logger::log_error("MOD", "%s: This mod caused a native crash. Check for invalid memory access.",
                              name.c_str());

            ModInfo mi;
            mi.name = name;
            mi.path = mod_dir;
            mi.status = ModStatus::FAILED;
            mi.error = std::string("Native crash: ") + sig_name;
            mi.error_count = 1;

            // Remove any existing entry for this mod
            s_mods.erase(
                std::remove_if(s_mods.begin(), s_mods.end(),
                               [&name](const ModInfo &m)
                               { return m.name == name; }),
                s_mods.end());
            s_mods.push_back(mi);

            s_active_mod_loaders.fetch_sub(1, std::memory_order_relaxed);
            return false;
        }

        // Create a sandboxed environment for this mod
        sol::environment env = lua_engine::create_mod_environment(name);

        // Execute the mod's main.lua in its environment
        auto result = lua_engine::exec_file_in_env(main_lua, env);

        // Clear the recovery point
        s_in_mod_loading = 0;

        ModInfo mi;
        mi.name = name;
        mi.path = mod_dir;

        if (result.success)
        {
            mi.status = ModStatus::LOADED;
            mi.error_count = 0;
            logger::log_info("MOD", "%s: loaded successfully", name.c_str());
        }
        else
        {
            mi.status = ModStatus::ERRORED;
            mi.error = result.error;
            mi.error_count = 1;
            logger::log_error("MOD", "%s: %s", name.c_str(), result.error.c_str());
            logger::log_info("MOD", "%s: loaded with Lua errors — continuing", name.c_str());
        }

        // Remove any existing entry for this mod
        s_mods.erase(
            std::remove_if(s_mods.begin(), s_mods.end(),
                           [&name](const ModInfo &m)
                           { return m.name == name; }),
            s_mods.end());

        s_mods.push_back(mi);
        s_active_mod_loaders.fetch_sub(1, std::memory_order_relaxed);
        return result.success;
    }

    // ═══ Hot-reload a mod ═══════════════════════════════════════════════════
    bool reload_mod(const std::string &name)
    {
        logger::log_info("MOD", "Hot-reloading mod: %s", name.c_str());

        // Remove the old entry
        {
            std::lock_guard<std::mutex> lock(s_mods_mutex);
            s_mods.erase(
                std::remove_if(s_mods.begin(), s_mods.end(),
                               [&name](const ModInfo &m)
                               { return m.name == name; }),
                s_mods.end());
        }

        // Reload
        bool ok = load_mod(name);

        if (ok)
        {
            logger::log_info("MOD", "%s: hot-reload successful", name.c_str());
        }
        else
        {
            logger::log_error("MOD", "%s: hot-reload failed", name.c_str());
        }

        return ok;
    }

    // ═══ Query mod status ═══════════════════════════════════════════════════
    const std::vector<ModInfo> &get_all_mods()
    {
        return s_mods;
    }

    int loaded_count()
    {
        std::lock_guard<std::mutex> lock(s_mods_mutex);
        int count = 0;
        for (const auto &m : s_mods)
        {
            if (m.status == ModStatus::LOADED)
                count++;
        }
        return count;
    }

    int failed_count()
    {
        std::lock_guard<std::mutex> lock(s_mods_mutex);
        int count = 0;
        for (const auto &m : s_mods)
        {
            if (m.status == ModStatus::FAILED || m.status == ModStatus::ERRORED)
                count++;
        }
        return count;
    }

    int total_count()
    {
        std::lock_guard<std::mutex> lock(s_mods_mutex);
        return static_cast<int>(s_mods.size());
    }

    const ModInfo *find_mod(const std::string &name)
    {
        std::lock_guard<std::mutex> lock(s_mods_mutex);
        for (const auto &m : s_mods)
        {
            if (m.name == name)
                return &m;
        }
        return nullptr;
    }

    bool is_any_mod_loading()
    {
        return s_active_mod_loaders.load(std::memory_order_relaxed) > 0;
    }

} // namespace mod_loader
