// modloader/src/core/init.cpp
// Boot sequence orchestration — correct dependency order per instructions:
//   1. paths → logger → crash handler
//   2. wait for libUE4 → resolve symbols
//   3. ProcessEvent hook + ADB bridge (BEFORE reflection wait)
//   4. Wait for GUObjectArray (120s, MIN_OBJECTS >= 5000, heavy diagnostics)
//   5. Walk reflection → SDK → class rebuilder → Lua → mods → paks → notifications

#include "modloader/init.h"
#include "modloader/logger.h"
#include "modloader/paths.h"
#include "modloader/crash_handler.h"
#include "modloader/symbols.h"
#include "modloader/pattern_scanner.h"
#include "modloader/reflection_walker.h"
#include "modloader/lua_dump_generator.h"
#include "modloader/class_rebuilder.h"
#include "modloader/process_event_hook.h"
#include "modloader/native_hooks.h"
#include "modloader/lua_engine.h"
#include "modloader/mod_loader.h"
#include "modloader/pak_mounter.h"
#include "modloader/adb_bridge.h"
#include "modloader/notification.h"
#include "modloader/object_monitor.h"
#include "modloader/config.h"
#include "modloader/game_profile.h"
#include "modloader/auto_offsets.h"
#include "modloader/safe_call.h"

#include <chrono>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>
#include <cstdint>
#include <cstring>
#include <cinttypes>
#include <fstream>

namespace init
{

    static bool s_initialized = false;

    // ═══ Wait for engine library to be fully loaded ═════════════════════════
    static bool wait_for_engine_lib(int timeout_ms)
    {
        const std::string &lib_name = game_profile::engine_lib_name();
        auto start = std::chrono::steady_clock::now();
        while (true)
        {
            void *lib = dlopen(lib_name.c_str(), RTLD_NOLOAD | RTLD_NOW);
            if (lib)
            {
                dlclose(lib);
                return true;
            }

            // For unknown games, also try the other common name
            if (game_profile::detected_game() == game_profile::GameID::UNKNOWN)
            {
                const char *alt = (lib_name == "libUE4.so") ? "libUnreal.so" : "libUE4.so";
                lib = dlopen(alt, RTLD_NOLOAD | RTLD_NOW);
                if (lib)
                {
                    dlclose(lib);
                    return true;
                }
            }

            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start)
                               .count();
            if (elapsed >= timeout_ms)
                return false;

            usleep(50000); // 50ms
        }
    }

    // ═══ Hex dump memory region for diagnostics ═════════════════════════════
    static void hex_dump(const char *label, uintptr_t addr, int bytes)
    {
        if (addr == 0)
        {
            logger::log_warn("DIAG", "%s: address is NULL", label);
            return;
        }
        char line[256];
        for (int i = 0; i < bytes; i += 16)
        {
            int n = snprintf(line, sizeof(line), "%s @ 0x%lX+0x%02X: ",
                             label, (unsigned long)addr, i);
            for (int j = 0; j < 16 && (i + j) < bytes; j++)
            {
                uint8_t b = *reinterpret_cast<const uint8_t *>(addr + i + j);
                n += snprintf(line + n, sizeof(line) - n, "%02X ", b);
            }
            logger::log_info("DIAG", "%s", line);
        }
    }

    // ═══ Wait for GUObjectArray to populate ═════════════════════════════════
    // Per instructions: 120s timeout, MIN_OBJECTS >= 5000, poll 100ms, log every 5s
    // Heavy diagnostic logging to understand any failures
    static bool wait_for_guobjectarray(int timeout_ms)
    {
        if (!symbols::GUObjectArray)
        {
            logger::log_error("BOOT", "GUObjectArray symbol is NULL — cannot wait");
            return false;
        }

        uintptr_t guoa = reinterpret_cast<uintptr_t>(symbols::GUObjectArray);
        uintptr_t embedded_start = guoa + ue::GUOBJECTARRAY_TO_OBJECTS;

        logger::log_info("BOOT", "GUObjectArray wait — base: 0x%lX, embedded: 0x%lX",
                         (unsigned long)guoa, (unsigned long)embedded_start);
        logger::log_info("BOOT", "  GUOBJECTARRAY_TO_OBJECTS = 0x%X", ue::GUOBJECTARRAY_TO_OBJECTS);
        logger::log_info("BOOT", "  TUOBJECTARRAY_NUM_ELEMENTS = 0x%X", ue::TUOBJECTARRAY_NUM_ELEMENTS);
        logger::log_info("BOOT", "  NumElements addr = 0x%lX",
                         (unsigned long)(embedded_start + ue::TUOBJECTARRAY_NUM_ELEMENTS));

        // Diagnostic: dump raw bytes of GUObjectArray region
        hex_dump("GUObjectArray", guoa, 64);

        constexpr int MIN_OBJECTS = 5000;
        constexpr int POLL_INTERVAL_MS = 100;
        int waited = 0;
        int last_log_time = 0;

        while (waited < timeout_ms)
        {
            // Read Objects** pointer
            uintptr_t obj_ptrs = ue::read_field<uintptr_t>(
                reinterpret_cast<const void *>(embedded_start), 0);

            // Read NumElements
            int32_t count = ue::read_field<int32_t>(
                reinterpret_cast<const void *>(embedded_start), ue::TUOBJECTARRAY_NUM_ELEMENTS);

            bool ptrs_valid = ue::is_valid_ptr(reinterpret_cast<const void *>(obj_ptrs));

            // Log every 5 seconds
            if (waited - last_log_time >= 5000 || waited == 0)
            {
                logger::log_info("BOOT", "GUObjectArray poll @ %dms — Objects**: 0x%lX (%s), NumElements: %d",
                                 waited,
                                 (unsigned long)obj_ptrs,
                                 ptrs_valid ? "valid" : "INVALID",
                                 count);

                // Dump fresh bytes every 15 seconds
                if (waited % 15000 == 0 && waited > 0)
                {
                    hex_dump("GUObjectArray(live)", guoa, 64);
                }

                last_log_time = waited;
            }

            if (ptrs_valid && count >= MIN_OBJECTS)
            {
                logger::log_info("BOOT", "GUObjectArray populated — %d objects (waited %dms)", count, waited);
                return true;
            }

            // Also accept count > 100 after 30s as a fallback — maybe this game has fewer objects
            if (ptrs_valid && count > 100 && waited >= 30000)
            {
                logger::log_warn("BOOT", "GUObjectArray has %d objects after %dms — below %d threshold but proceeding",
                                 count, waited, MIN_OBJECTS);
                return true;
            }

            usleep(POLL_INTERVAL_MS * 1000);
            waited += POLL_INTERVAL_MS;
        }

        // Timeout — log final state with full diagnostics
        uintptr_t final_obj_ptrs = ue::read_field<uintptr_t>(
            reinterpret_cast<const void *>(embedded_start), 0);
        int32_t final_count = ue::read_field<int32_t>(
            reinterpret_cast<const void *>(embedded_start), ue::TUOBJECTARRAY_NUM_ELEMENTS);

        logger::log_error("BOOT", "GUObjectArray TIMEOUT after %dms — Objects**: 0x%lX, NumElements: %d",
                          timeout_ms, (unsigned long)final_obj_ptrs, final_count);

        // Final hex dump for debugging
        hex_dump("GUObjectArray(timeout)", guoa, 64);

        // If we have ANY objects, proceed anyway
        if (final_count > 0 && ue::is_valid_ptr(reinterpret_cast<const void *>(final_obj_ptrs)))
        {
            logger::log_warn("BOOT", "Proceeding with %d objects despite timeout", final_count);
            return true;
        }

        return false;
    }

    // ═══ Deferred init — runs after engine is fully initialized ═════════════
    // Waits for GUObjectArray, walks reflection, generates SDK, mounts PAKs.
    // This runs on its own thread so mods load instantly at boot.
    static void *deferred_init_thread(void *arg)
    {
        bool notif_ok = (reinterpret_cast<uintptr_t>(arg) != 0);

        // ── Re-install crash handler on deferred thread ─────────────────────
        // By now (~5-10s after boot), the Oculus VR runtime has likely
        // installed its own SIGSEGV handler. Re-assert ours so the safe-call
        // guard in dispatch_full() keeps working for the lifetime of the process.
        crash_handler::reinstall();

        // Wait for GUObjectArray to populate — engine needs time to init
        logger::log_info("DEFER", "Waiting for GUObjectArray to populate (120s timeout, min 5000 objects)...");
        bool guoa_ok = wait_for_guobjectarray(120000);

        // Walk reflection graph
        logger::log_info("DEFER", "Walking UE4 reflection graph...");
        if (guoa_ok)
        {
            reflection::walk_all();
        }
        else
        {
            logger::log_warn("DEFER", "Attempting walk despite GUObjectArray timeout...");
            reflection::walk_all();
        }

        auto &classes = reflection::get_classes();
        auto &structs = reflection::get_structs();
        auto &enums = reflection::get_enums();
        logger::log_info("DEFER", "Reflection complete: %zu classes, %zu structs, %zu enums",
                         classes.size(), structs.size(), enums.size());

        // Generate SDK
        if (config::auto_dump_on_boot() && (!classes.empty() || !structs.empty() || !enums.empty()))
        {
            logger::log_info("DEFER", "Generating SDK...");
            sdk_gen::generate();
            logger::log_info("DEFER", "SDK written to %s", paths::sdk_dir().c_str());

            // ── GNames dump ─────────────────────────────────────────────────
            {
                std::string gnames_path = paths::data_dir() + "/gnames_dump.txt";
                std::ofstream gnames_ofs(gnames_path);
                if (gnames_ofs.is_open())
                {
                    int gn_count = 0;
                    reflection::walk_all_fnames([&](int32_t index, const std::string &name)
                                                {
                        gnames_ofs << "[" << index << "] " << name << "\n";
                        gn_count++; });
                    gnames_ofs.close();
                    logger::log_info("DEFER", "GNames dump: %d names → %s", gn_count, gnames_path.c_str());
                }
                else
                {
                    logger::log_error("DEFER", "GNames dump: cannot open %s", gnames_path.c_str());
                }
            }

            // ── GObjects dump ───────────────────────────────────────────────
            {
                std::string gobjects_path = paths::data_dir() + "/gobjects_dump.txt";
                std::ofstream gobjects_ofs(gobjects_path);
                if (gobjects_ofs.is_open())
                {
                    int32_t total = reflection::get_live_object_count();
                    int go_count = 0;
                    gobjects_ofs << "# GUObjectArray Dump — " << total << " total slots\n";
                    gobjects_ofs << "# Format: [Index] ClassName FullPath (Address) Flags=0xHEX\n\n";
                    for (int32_t i = 0; i < total; i++)
                    {
                        ue::UObject *obj = reflection::get_object_by_index(i);
                        if (!obj || !ue::is_valid_ptr(obj))
                            continue;
                        std::string obj_name = reflection::get_short_name(obj);
                        if (obj_name.empty())
                            continue;
                        std::string full_name = reflection::get_full_name(obj);
                        std::string cls_name = "???";
                        ue::UClass *cls = ue::uobj_get_class(obj);
                        if (cls && ue::is_valid_ptr(cls))
                            cls_name = reflection::get_short_name(reinterpret_cast<const ue::UObject *>(cls));
                        int32_t flags = ue::uobj_get_flags(obj);
                        char addr_buf[32];
                        snprintf(addr_buf, sizeof(addr_buf), "0x%lX", (unsigned long)(uintptr_t)obj);
                        gobjects_ofs << "[" << i << "] " << cls_name << " " << full_name
                                     << " (" << addr_buf << ") Flags=0x" << std::hex << flags << std::dec << "\n";
                        go_count++;
                    }
                    gobjects_ofs.close();
                    logger::log_info("DEFER", "GObjects dump: %d objects → %s", go_count, gobjects_path.c_str());
                }
                else
                {
                    logger::log_error("DEFER", "GObjects dump: cannot open %s", gobjects_path.c_str());
                }
            }
        }
        else if (!config::auto_dump_on_boot())
        {
            logger::log_info("DEFER", "SDK auto-dump disabled by config — skipping");
        }
        else
        {
            logger::log_warn("DEFER", "Skipping SDK generation — no types found");
        }

        // PAK mounting is handled by the Dobby hooks installed in boot().
        // The hooks capture FPakPlatformFile from the engine's own Mount calls
        // and schedule custom PAK mounting with a 2s delay at priority 1000+.
        // No need to mount here — the hooks handle it automatically.
        logger::log_info("DEFER", "PAK mounting handled by Dobby hooks (Frida-style capture)");

        // Post SDK notification
        if (notif_ok && !classes.empty())
        {
            notification::post_sdk(static_cast<int>(classes.size()));
        }

        // Start object monitor if configured
        if (config::object_monitor_enabled() && config::auto_dump_on_level_change())
        {
            object_monitor::start(
                config::monitor_poll_interval_ms(),
                config::monitor_growth_threshold(),
                config::monitor_cooldown_ms());
            logger::log_info("DEFER", "Object monitor started");
        }

        logger::log_info("DEFER", "Deferred init complete — %zu classes (PAK mount via hooks)",
                         classes.size());

        // ── Final signal handler re-install + periodic watchdog ─────────────
        // Re-install one more time after all deferred work is done, then
        // keep re-installing every 5 seconds for the first 60 seconds of
        // runtime to ensure the Oculus VR runtime can't permanently replace us.
        crash_handler::reinstall();
        for (int i = 0; i < 12; i++)
        {
            struct timespec ts = {5, 0};
            nanosleep(&ts, nullptr);
            crash_handler::reinstall();
        }
        logger::log_info("DEFER", "Signal handler watchdog complete (60s of protection)");

        return nullptr;
    }

    // ═══ Main boot sequence ═════════════════════════════════════════════════
    bool boot()
    {
        if (s_initialized)
            return true;

        auto boot_start = std::chrono::steady_clock::now();

        // ── Step 1: Initialize paths (JNI) ──────────────────────────────────
        paths::init();

        // ── Step 2: Initialize logger ───────────────────────────────────────
        logger::init(paths::log_path());
        logger::log_info("BOOT", "=== SESSION START ===");
        logger::log_info("BOOT", "UE ModLoader initializing");
        logger::log_info("BOOT", "Data dir: %s", paths::data_dir().c_str());

        // ── Step 2.5: Detect game and load profile ──────────────────────────
        game_profile::init();
        logger::log_info("BOOT", "Game detected: %s (%s), Engine lib: %s",
                         game_profile::display_name().c_str(),
                         game_profile::package_name().c_str(),
                         game_profile::engine_lib_name().c_str());

        // Apply game-specific type offsets to the runtime UE type system
        ue::apply_type_offsets(game_profile::offsets());
        logger::log_info("BOOT", "Type offsets applied from game profile:");
        logger::log_info("BOOT", "  FUObjectItem size: 0x%X", game_profile::offsets().FUObjectItem_size);
        logger::log_info("BOOT", "  FField::Next: 0x%X, FField::Name: 0x%X",
                         game_profile::offsets().FField_next, game_profile::offsets().FField_name);
        logger::log_info("BOOT", "  FProp::ElementSize: 0x%X, FProp::Flags: 0x%X, FProp::Offset: 0x%X",
                         game_profile::offsets().FProp_element_size,
                         game_profile::offsets().FProp_property_flags,
                         game_profile::offsets().FProp_offset_internal);

        // ── Step 3: Install crash handler ───────────────────────────────────
        crash_handler::install();
        logger::log_info("BOOT", "Crash handler installed");

        // ── Step 3.1: Initialize safe-call subsystem ────────────────────────
        safe_call::init();
        logger::log_info("BOOT", "Safe-call subsystem initialized (try/catch + signal recovery)");

        // ── Step 3.5: Load config.json ──────────────────────────────────────
        config::load(paths::data_dir());

        // ── Step 4: Wait for engine library ──────────────────────────────────
        logger::log_info("BOOT", "Waiting for %s to load...", game_profile::engine_lib_name().c_str());
        if (!wait_for_engine_lib(30000))
        {
            logger::log_error("BOOT", "Timeout waiting for %s — aborting",
                              game_profile::engine_lib_name().c_str());
            return false;
        }

        // ── Step 5: Initialize symbols ──────────────────────────────────────
        symbols::init();
        logger::log_info("BOOT", "Symbol resolver initialized — %s base: 0x%lX",
                         game_profile::engine_lib_name().c_str(),
                         (unsigned long)symbols::lib_base());

        // ── Step 6: Initialize pattern scanner ──────────────────────────────
        pattern::init();
        logger::log_info("BOOT", "Pattern scanner initialized");

        // ── Step 6.5: Run dynamic offset discovery ──────────────────────────
        // Auto-discovers GNames, GUObjectArray, ProcessEvent, FUObjectItem size,
        // and other critical offsets for ANY UE version. Discovered values are
        // merged into the game profile as fallbacks (game profile takes priority).
        {
            auto_offsets::init();
            auto discovery = auto_offsets::discover_all();
            logger::log_info("BOOT", "Auto-offset discovery: %d found, %d failed, version=%s",
                             discovery.total_discoveries, discovery.failed_discoveries,
                             discovery.version_string.c_str());

            // Apply discoveries to the game profile (only fills gaps)
            auto_offsets::apply_to_profile(discovery);

            // Re-apply type offsets if auto-discovery changed them
            ue::apply_type_offsets(game_profile::offsets());
            logger::log_info("BOOT", "Type offsets updated after auto-discovery");
        }

        // ── Step 7: Resolve core symbols ────────────────────────────────────
        symbols::resolve_core_symbols();

        // ── Step 7.5: Install PAK mount hooks IMMEDIATELY after symbols ────
        // CRITICAL: Must happen BEFORE the symbol dump (which takes ~800ms).
        // The engine may mount its PAKs during that window. Our hooks must
        // be in place first, and we mount custom PAKs synchronously — no delay.
        pak_mounter::install_hooks();
        logger::log_info("BOOT", "PAK mount hooks installed + custom PAKs mounted (synchronous)");

        // ── Step 7.6: Dump all ELF symbols for diagnostics ──────────────────
        {
            std::string sym_dump_path = paths::data_dir() + "/symbol_dump.txt";
            int sym_count = symbols::dump_symbols(sym_dump_path);
            if (sym_count > 0)
            {
                logger::log_info("BOOT", "Symbol dump: %d symbols written to %s", sym_count, sym_dump_path.c_str());
            }
            else
            {
                logger::log_warn("BOOT", "Symbol dump failed or empty");
            }
        }

        // ── Step 8: Install ProcessEvent hook EARLY ─────────────────────────
        // Per instructions: ProcessEvent hook fires BEFORE reflection walk
        pe_hook::install();
        logger::log_info("BOOT", "ProcessEvent hook installed via Dobby (pre-reflection)");

        // ── Step 9: Initialize native hook subsystem EARLY ──────────────────
        native_hooks::init();
        logger::log_info("BOOT", "Native hook subsystem initialized");
        // ── Step 9.5: Install built-in crash guards ─────────────────────────
        // Hooks known-crashy game functions with NO callbacks — just routes
        // them through dispatch_full()'s sigsetjmp safe-call guard.
        // Must happen BEFORE mods load (which also install hooks).
        // ⚠ DISABLED: Crash guards hook RedrawViewports, UpdateViewTargetInternal,
        // GetProjectionData, UpdateRotation, TickActor — when sigsetjmp catches a
        // crash it returns 0, which causes the VR compositor to get NO frames
        // rendered → black screen. The guards must stay OFF.
        // native_hooks::install_builtin_crash_guards();
        logger::log_info("BOOT", "Built-in crash guards SKIPPED (cause black screen in VR)");
        // ── Step 10: Start ADB bridge EARLY ─────────────────────────────────
        // Per instructions: ADB bridge starts BEFORE reflection walk
        adb_bridge::start();
        logger::log_info("BOOT", "ADB bridge started on :%d", adb_bridge::ADB_BRIDGE_PORT);

        // ── Step 11: Initialize notifications ───────────────────────────────
        bool notif_ok = notification::init();
        if (notif_ok)
        {
            logger::log_info("BOOT", "JNI notification system initialized");
        }
        else
        {
            logger::log_warn("BOOT", "JNI notification init failed — continuing without notifications");
        }

        // ── Step 12: Initialize reflection system (no walk yet) ─────────────
        reflection::init();
        rebuilder::init();

        // ── Step 13: Initialize Lua engine ──────────────────────────────────
        if (!lua_engine::init())
        {
            logger::log_error("BOOT", "Failed to initialize Lua engine");
            return false;
        }

        // ── Step 14: Load mods IMMEDIATELY (no GUObjectArray wait) ──────────
        // Mods register hooks — they don't need GUObjectArray to load.
        // PE hooks fire lazily when ProcessEvent is called.
        // Native hooks resolve symbols from ELF, not from the object array.
        int mods_loaded = mod_loader::load_all();
        int mods_failed = mod_loader::failed_count();
        logger::log_info("BOOT", "Mods: %d loaded, %d failed", mods_loaded, mods_failed);

        // ── Step 15: Post initial boot notification ─────────────────────────
        if (notif_ok)
        {
            notification::post_boot(mods_loaded, mods_failed);
        }

        // ── Step 15.5: Re-install crash handler ─────────────────────────────
        // The Oculus VR runtime and/or Frida may have replaced our SIGSEGV
        // handler during mod loading or between steps. Re-assert it now
        // so the safe-call guard keeps working.
        crash_handler::reinstall();

        // ── Boot complete (fast path) ───────────────────────────────────────
        auto boot_end = std::chrono::steady_clock::now();
        auto boot_ms = std::chrono::duration_cast<std::chrono::milliseconds>(boot_end - boot_start).count();

        logger::log_info("BOOT", "Boot complete in %lldms — %d mods, ADB on :%d",
                         (long long)boot_ms, mods_loaded, adb_bridge::ADB_BRIDGE_PORT);
        logger::log_info("BOOT", "Deferring GUObjectArray wait + reflection + SDK + PAK to background thread");

        s_initialized = true;

        // ── Step 16: Spawn deferred init thread ─────────────────────────────
        // GUObjectArray, reflection walk, SDK dump, PAK mounting all happen
        // in the background AFTER the engine has fully initialized.
        // This avoids the 120s timeout that blocks mod loading.
        pthread_t deferred_tid;
        pthread_attr_t deferred_attr;
        pthread_attr_init(&deferred_attr);
        pthread_attr_setdetachstate(&deferred_attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&deferred_attr, 4 * 1024 * 1024);
        pthread_create(&deferred_tid, &deferred_attr, deferred_init_thread, reinterpret_cast<void *>(notif_ok ? 1 : 0));
        pthread_attr_destroy(&deferred_attr);

        return true;
    }

    bool is_initialized()
    {
        return s_initialized;
    }

} // namespace init
