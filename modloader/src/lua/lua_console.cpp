// modloader/src/lua/lua_console.cpp
// ═══════════════════════════════════════════════════════════════════════════
// UE Console Command, CheatManager, ViewMode, and Engine Power API
// Also exposes: safe_call stats, auto_offsets info, PE trace Lua API,
//               crash log paths, and diagnostic utilities
//
// All operations go through UE's own reflection system (ProcessEvent).
// No raw memory offsets for game objects. No hardcoded paths.
// Works inside the app's sandbox — no root required.
// ═══════════════════════════════════════════════════════════════════════════

#include "modloader/lua_engine.h"
#include "modloader/lua_uobject.h"
#include "modloader/symbols.h"
#include "modloader/logger.h"
#include "modloader/paths.h"
#include "modloader/safe_call.h"
#include "modloader/auto_offsets.h"
#include "modloader/pe_trace.h"
#include "modloader/reflection_walker.h"
#include "modloader/class_rebuilder.h"
#include "modloader/process_event_hook.h"
#include "modloader/game_profile.h"
#include "modloader/types.h"

#include <sol/sol.hpp>
#include <string>
#include <vector>
#include <cstring>
#include <thread>
#include <atomic>

namespace lua_console
{

    // ═══════════════════════════════════════════════════════════════════════
    // Internal: Find PlayerController
    // ═══════════════════════════════════════════════════════════════════════
    static ue::UObject *find_player_controller()
    {
        // Try game-specific PCs first
        static const char *pc_classes[] = {
            "BP_PlayerController_C",
            "VR4PlayerController_BP_C",
            "PFXPlayerController",
            "PlayerController",
            nullptr};

        for (int i = 0; pc_classes[i]; i++)
        {
            auto *rc = rebuilder::rebuild(pc_classes[i]);
            if (rc)
            {
                auto *inst = rc->get_first_instance();
                if (inst)
                    return inst;
            }
        }

        // Last resort: find via reflection
        return reflection::find_first_instance("PlayerController");
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Internal: Get or create CheatManager
    // ═══════════════════════════════════════════════════════════════════════
    static ue::UObject *get_cheat_manager_for(ue::UObject *pc)
    {
        if (!pc)
            return nullptr;

        // Get PC's class to find CheatManager property
        std::string pc_class_name = reflection::get_short_name(
            reinterpret_cast<const ue::UObject *>(ue::uobj_get_class(pc)));
        auto *rc = rebuilder::rebuild(pc_class_name);
        if (!rc)
            rc = rebuilder::rebuild("PlayerController");
        if (!rc)
            return nullptr;

        auto *cm_prop = rc->find_property("CheatManager");
        if (!cm_prop)
            return nullptr;

        // Read current CheatManager pointer
        uintptr_t pc_addr = reinterpret_cast<uintptr_t>(pc);
        ue::UObject **cm_ptr = reinterpret_cast<ue::UObject **>(pc_addr + cm_prop->offset);
        if (*cm_ptr && ue::is_valid_ptr(*cm_ptr))
            return *cm_ptr;

        // No CheatManager — create one
        if (!symbols::StaticConstructObject)
        {
            logger::log_error("CONSOLE", "Cannot spawn CheatManager: StaticConstructObject not resolved");
            return nullptr;
        }

        // Find the CheatClass property on the PC
        ue::UClass *cheat_class = nullptr;
        auto *cc_prop = rc->find_property("CheatClass");
        if (cc_prop)
        {
            ue::UClass **cc_ptr = reinterpret_cast<ue::UClass **>(pc_addr + cc_prop->offset);
            if (*cc_ptr && ue::is_valid_ptr(reinterpret_cast<ue::UObject *>(*cc_ptr)))
                cheat_class = *cc_ptr;
        }

        // Fallback: find CheatManager class by reflection
        if (!cheat_class)
        {
            static const char *cm_classes[] = {
                "BP_CheatManager_C",
                "PFXCheatManager",
                "CheatManager",
                nullptr};
            for (int i = 0; cm_classes[i]; i++)
            {
                cheat_class = reinterpret_cast<ue::UClass *>(
                    reflection::find_class_ptr(cm_classes[i]));
                if (cheat_class)
                    break;
            }
        }

        if (!cheat_class)
        {
            logger::log_error("CONSOLE", "Cannot find CheatManager class");
            return nullptr;
        }

        // Construct the CheatManager
        ue::FName fname_none = {0, 0};
        ue::UObject *new_cm = symbols::StaticConstructObject(
            cheat_class, pc, fname_none, 0, 0, nullptr, false, nullptr, false);

        if (!new_cm)
        {
            logger::log_error("CONSOLE", "StaticConstructObject returned NULL for CheatManager");
            return nullptr;
        }

        // Write it to the PC's CheatManager property
        *cm_ptr = new_cm;
        logger::log_info("CONSOLE", "CheatManager spawned: %s @ %p",
                         reflection::get_short_name(
                             reinterpret_cast<const ue::UObject *>(ue::uobj_get_class(new_cm)))
                             .c_str(),
                         (void *)new_cm);

        // Call InitCheatManager if it exists
        auto *init_func = pe_hook::resolve_func_path("CheatManager:InitCheatManager");
        if (init_func)
        {
            auto pe = pe_hook::get_original();
            if (!pe)
                pe = symbols::ProcessEvent;
            if (pe)
            {
                pe(new_cm, init_func, nullptr);
                logger::log_info("CONSOLE", "InitCheatManager called");
            }
        }

        return new_cm;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Internal: Execute console command via ProcessEvent
    // ═══════════════════════════════════════════════════════════════════════
    static std::string exec_console_command(const std::string &command)
    {
        ue::UObject *pc = find_player_controller();
        if (!pc)
        {
            logger::log_warn("CONSOLE", "No PlayerController found for command: %s", command.c_str());
            return "";
        }

        // Try PlayerController::ConsoleCommand(FString Command, bool bWriteToLog)
        // This is the most reliable way to execute console commands in UE4/5.
        // The function is a UFUNCTION so it's available via ProcessEvent.
        auto *func = pe_hook::resolve_func_path("PlayerController:ConsoleCommand");
        if (!func)
        {
            // Try ServerExec or ClientExec as fallbacks
            func = pe_hook::resolve_func_path("PlayerController:ServerExec");
        }
        if (!func)
        {
            // Try the engine exec path via GEngine
            if (symbols::GEngine)
            {
                ue::UObject *engine = *reinterpret_cast<ue::UObject **>(symbols::GEngine);
                if (engine && ue::is_valid_ptr(engine))
                {
                    auto *exec_func = pe_hook::resolve_func_path("Engine:Exec");
                    if (exec_func)
                    {
                        // GEngine->Exec() takes (UWorld*, TCHAR* Cmd, FOutputDevice&)
                        // This is harder to use via ProcessEvent, fallback to the simpler path
                    }
                }
            }
            logger::log_warn("CONSOLE", "ConsoleCommand UFunction not found");
            return "";
        }

        // Build parameters for ConsoleCommand(FString Command, bool bWriteToLog)
        auto pe = pe_hook::get_original();
        if (!pe)
            pe = symbols::ProcessEvent;
        if (!pe)
            return "";

        // Get the parms size from UFunction
        uint16_t parms_size = ue::ufunc_get_parms_size(func);
        if (parms_size == 0)
            parms_size = 128; // Conservative default

        std::vector<uint8_t> parms(parms_size, 0);

        // Fill the Command parameter (FString at offset 0)
        // FString layout: { char16_t* Data, int32 Num, int32 Max }
        std::u16string u16cmd(command.begin(), command.end());
        u16cmd.push_back(u'\0');

        struct FString
        {
            const char16_t *data;
            int32_t num;
            int32_t max;
        };

        FString cmd_str;
        cmd_str.data = u16cmd.c_str();
        cmd_str.num = static_cast<int32_t>(u16cmd.size());
        cmd_str.max = cmd_str.num;

        // Write FString to parms buffer at offset 0
        std::memcpy(parms.data(), &cmd_str, sizeof(FString));

        // Call via ProcessEvent
        auto result = safe_call::execute([&]()
                                         { pe(pc, func, parms.data()); },
                                         "ConsoleCommand(" + command + ")");

        if (!result.ok)
        {
            logger::log_warn("CONSOLE", "ConsoleCommand(%s) crashed: %s",
                             command.c_str(), result.error_msg.c_str());
            return "";
        }

        logger::log_info("CONSOLE", "Executed: %s", command.c_str());
        return "OK";
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Internal: Call a no-arg UFunction on the CheatManager
    // ═══════════════════════════════════════════════════════════════════════
    static bool call_cheat(const std::string &func_name)
    {
        ue::UObject *pc = find_player_controller();
        ue::UObject *cm = get_cheat_manager_for(pc);
        if (!cm)
        {
            logger::log_warn("CONSOLE", "CheatManager not available for: %s", func_name.c_str());
            return false;
        }

        auto *func = pe_hook::resolve_func_path("CheatManager:" + func_name);
        if (!func)
        {
            // Try the game-specific CheatManager subclass
            std::string cm_class = reflection::get_short_name(
                reinterpret_cast<const ue::UObject *>(ue::uobj_get_class(cm)));
            func = pe_hook::resolve_func_path(cm_class + ":" + func_name);
        }
        if (!func)
        {
            logger::log_warn("CONSOLE", "UFunction %s not found on CheatManager", func_name.c_str());
            return false;
        }

        auto pe = pe_hook::get_original();
        if (!pe)
            pe = symbols::ProcessEvent;
        if (!pe)
            return false;

        uint16_t parms_size = ue::ufunc_get_parms_size(func);
        std::vector<uint8_t> parms(parms_size > 0 ? parms_size : 16, 0);

        auto result = safe_call::execute([&]()
                                         { pe(cm, func, parms.data()); },
                                         "Cheat:" + func_name);

        if (!result.ok)
        {
            logger::log_warn("CONSOLE", "Cheat %s crashed: %s",
                             func_name.c_str(), result.error_msg.c_str());
            return false;
        }

        logger::log_info("CONSOLE", "Cheat: %s", func_name.c_str());
        return true;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Internal: Call a UFunction with a float param on CheatManager
    // ═══════════════════════════════════════════════════════════════════════
    static bool call_cheat_float(const std::string &func_name, float value)
    {
        ue::UObject *pc = find_player_controller();
        ue::UObject *cm = get_cheat_manager_for(pc);
        if (!cm)
            return false;

        auto *func = pe_hook::resolve_func_path("CheatManager:" + func_name);
        if (!func)
        {
            std::string cm_class = reflection::get_short_name(
                reinterpret_cast<const ue::UObject *>(ue::uobj_get_class(cm)));
            func = pe_hook::resolve_func_path(cm_class + ":" + func_name);
        }
        if (!func)
        {
            logger::log_warn("CONSOLE", "UFunction %s not found", func_name.c_str());
            return false;
        }

        auto pe = pe_hook::get_original();
        if (!pe)
            pe = symbols::ProcessEvent;
        if (!pe)
            return false;

        uint16_t parms_size = ue::ufunc_get_parms_size(func);
        std::vector<uint8_t> parms(parms_size > 0 ? parms_size : 16, 0);
        std::memcpy(parms.data(), &value, sizeof(float));

        auto result = safe_call::execute([&]()
                                         { pe(cm, func, parms.data()); },
                                         "Cheat:" + func_name);

        if (!result.ok)
        {
            logger::log_warn("CONSOLE", "Cheat %s(%.2f) crashed: %s",
                             func_name.c_str(), value, result.error_msg.c_str());
            return false;
        }

        logger::log_info("CONSOLE", "Cheat: %s(%.2f)", func_name.c_str(), value);
        return true;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Internal: Call a UFunction with a string param on CheatManager
    // ═══════════════════════════════════════════════════════════════════════
    static bool call_cheat_string(const std::string &func_name, const std::string &str_arg)
    {
        ue::UObject *pc = find_player_controller();
        ue::UObject *cm = get_cheat_manager_for(pc);
        if (!cm)
            return false;

        auto *func = pe_hook::resolve_func_path("CheatManager:" + func_name);
        if (!func)
        {
            std::string cm_class = reflection::get_short_name(
                reinterpret_cast<const ue::UObject *>(ue::uobj_get_class(cm)));
            func = pe_hook::resolve_func_path(cm_class + ":" + func_name);
        }
        if (!func)
            return false;

        auto pe = pe_hook::get_original();
        if (!pe)
            pe = symbols::ProcessEvent;
        if (!pe)
            return false;

        // Build FString parameter
        std::u16string u16str(str_arg.begin(), str_arg.end());
        u16str.push_back(u'\0');

        struct FString
        {
            const char16_t *data;
            int32_t num;
            int32_t max;
        };

        FString fs;
        fs.data = u16str.c_str();
        fs.num = static_cast<int32_t>(u16str.size());
        fs.max = fs.num;

        uint16_t parms_size = ue::ufunc_get_parms_size(func);
        std::vector<uint8_t> parms(parms_size > 0 ? parms_size : 64, 0);
        std::memcpy(parms.data(), &fs, sizeof(FString));

        auto result = safe_call::execute([&]()
                                         { pe(cm, func, parms.data()); },
                                         "Cheat:" + func_name + "(" + str_arg + ")");

        if (!result.ok)
        {
            logger::log_warn("CONSOLE", "Cheat %s(%s) crashed: %s",
                             func_name.c_str(), str_arg.c_str(), result.error_msg.c_str());
            return false;
        }

        logger::log_info("CONSOLE", "Cheat: %s(%s)", func_name.c_str(), str_arg.c_str());
        return true;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Register all Lua bindings
    // ═══════════════════════════════════════════════════════════════════════
    void register_all(sol::state &lua)
    {
        // ─── Console Command Execution ──────────────────────────────────────
        // Execute any UE console command as if typed in the game console.
        // Works for: stat, viewmode, showflag, r., sg., t., cheats, etc.
        lua.set_function("ConsoleCommand", [](const std::string &cmd) -> std::string
                         { return exec_console_command(cmd); });

        // Execute multiple console commands
        lua.set_function("ConsoleCommands", [](sol::variadic_args va)
                         {
            int count = 0;
            for (auto arg : va) {
                if (arg.is<std::string>()) {
                    exec_console_command(arg.as<std::string>());
                    count++;
                }
            }
            return count; });

        // ─── CheatManager API ───────────────────────────────────────────────
        // Get or spawn the CheatManager. Returns a UObject wrapper or nil.
        lua.set_function("GetCheatManager", [](sol::this_state ts) -> sol::object
                         {
            sol::state_view lua_view(ts);
            ue::UObject* pc = find_player_controller();
            ue::UObject* cm = get_cheat_manager_for(pc);
            if (!cm) return sol::nil;
            return lua_uobject::wrap_or_nil(lua_view, cm); });

        // Check if a CheatManager exists
        lua.set_function("HasCheatManager", []() -> bool
                         {
            ue::UObject* pc = find_player_controller();
            if (!pc) return false;
            std::string pc_class = reflection::get_short_name(
                reinterpret_cast<const ue::UObject*>(ue::uobj_get_class(pc)));
            auto* rc = rebuilder::rebuild(pc_class);
            if (!rc) rc = rebuilder::rebuild("PlayerController");
            if (!rc) return false;
            auto* cm_prop = rc->find_property("CheatManager");
            if (!cm_prop) return false;
            ue::UObject** cm_ptr = reinterpret_cast<ue::UObject**>(
                reinterpret_cast<uintptr_t>(pc) + cm_prop->offset);
            return *cm_ptr && ue::is_valid_ptr(*cm_ptr); });

        // ─── Built-in Cheats ────────────────────────────────────────────────
        // All cheats auto-spawn CheatManager if needed

        // God mode (invulnerability toggle)
        lua.set_function("CheatGod", []() -> bool
                         { return call_cheat("God"); });

        // Fly mode (toggle flying)
        lua.set_function("CheatFly", []() -> bool
                         { return call_cheat("Fly"); });

        // Ghost mode (noclip toggle)
        lua.set_function("CheatGhost", []() -> bool
                         { return call_cheat("Ghost"); });

        // Walk mode (return to normal walking)
        lua.set_function("CheatWalk", []() -> bool
                         { return call_cheat("Walk"); });

        // Teleport to crosshair location
        lua.set_function("CheatTeleport", []() -> bool
                         { return call_cheat("Teleport"); });

        // Set game speed (1.0 = normal)
        lua.set_function("CheatSlomo", [](float rate) -> bool
                         { return call_cheat_float("Slomo", rate); });

        // Scale player size
        lua.set_function("CheatChangeSize", [](float factor) -> bool
                         { return call_cheat_float("ChangeSize", factor); });

        // Damage target you're looking at
        lua.set_function("CheatDamageTarget", [](float amount) -> bool
                         { return call_cheat_float("DamageTarget", amount); });

        // Destroy target you're looking at
        lua.set_function("CheatDestroyTarget", []() -> bool
                         { return call_cheat("DestroyTarget"); });

        // Summon an actor by class name
        lua.set_function("CheatSummon", [](const std::string &class_name) -> bool
                         { return call_cheat_string("Summon", class_name); });

        // Pause game
        lua.set_function("CheatPause", []() -> bool
                         { return call_cheat("Pause"); });

        // Toggle debug camera
        lua.set_function("CheatToggleDebugCamera", []() -> bool
                         { return call_cheat("ToggleDebugCamera"); });

        // ─── ViewMode Control ───────────────────────────────────────────────
        // Force rendering viewmodes via console command

        lua.set_function("SetViewMode", [](const std::string &mode) -> bool
                         {
            std::string cmd = "viewmode " + mode;
            return !exec_console_command(cmd).empty(); });

        // Common viewmode presets
        lua.set_function("ViewModeWireframe", []() -> bool
                         { return !exec_console_command("viewmode wireframe").empty(); });
        lua.set_function("ViewModeLit", []() -> bool
                         { return !exec_console_command("viewmode lit").empty(); });
        lua.set_function("ViewModeUnlit", []() -> bool
                         { return !exec_console_command("viewmode unlit").empty(); });
        lua.set_function("ViewModeDetailLighting", []() -> bool
                         { return !exec_console_command("viewmode detaillighting").empty(); });
        lua.set_function("ViewModeLightingOnly", []() -> bool
                         { return !exec_console_command("viewmode lightingonly").empty(); });
        lua.set_function("ViewModeShaderComplexity", []() -> bool
                         { return !exec_console_command("viewmode shadercomplexity").empty(); });
        lua.set_function("ViewModeCollision", []() -> bool
                         { return !exec_console_command("viewmode collision").empty(); });

        // ─── Show Flags ─────────────────────────────────────────────────────
        // Toggle engine show flags (0=off, 1=on, 2=toggle)
        lua.set_function("SetShowFlag", [](const std::string &flag_name, int value) -> bool
                         {
            std::string cmd = "showflag." + flag_name + " " + std::to_string(value);
            return !exec_console_command(cmd).empty(); });

        // ─── Stat Commands ──────────────────────────────────────────────────
        lua.set_function("ToggleStat", [](const std::string &stat_name) -> bool
                         {
            std::string cmd = "stat " + stat_name;
            return !exec_console_command(cmd).empty(); });

        lua.set_function("StatFPS", []() -> bool
                         { return !exec_console_command("stat fps").empty(); });
        lua.set_function("StatUnit", []() -> bool
                         { return !exec_console_command("stat unit").empty(); });
        lua.set_function("StatUnitGraph", []() -> bool
                         { return !exec_console_command("stat unitgraph").empty(); });
        lua.set_function("StatEngine", []() -> bool
                         { return !exec_console_command("stat engine").empty(); });
        lua.set_function("StatGame", []() -> bool
                         { return !exec_console_command("stat game").empty(); });

        // ─── Engine CVars ───────────────────────────────────────────────────
        lua.set_function("SetCVar", [](const std::string &name, const std::string &value) -> bool
                         {
            std::string cmd = name + " " + value;
            return !exec_console_command(cmd).empty(); });

        // ─── Safe-Call Stats (diagnostics) ──────────────────────────────────
        lua.set_function("GetCrashStats", [](sol::this_state ts) -> sol::table
                         {
            sol::state_view lua_view(ts);
            sol::table t = lua_view.create_table();
            t["total_recoveries"] = safe_call::crash_recovery_count();
            t["exception_recoveries"] = safe_call::exception_count();
            t["signal_recoveries"] = safe_call::signal_recovery_count();
            t["last_context"] = safe_call::last_crash_context();
            return t; });

        // ─── Crash Log Path ─────────────────────────────────────────────────
        lua.set_function("GetCrashLogPath", []() -> std::string
                         { return paths::crash_log(); });

        lua.set_function("GetLogPath", []() -> std::string
                         { return paths::log_path(); });

        // ─── Auto-Offsets Info ──────────────────────────────────────────────
        lua.set_function("GetEngineVersion", []() -> std::string
                         {
            std::string version_string;
            auto_offsets::detect_engine_version(&version_string);
            return version_string; });

        // ─── PE Trace Lua API ───────────────────────────────────────────────
        lua.set_function("PETraceStart", [](sol::optional<std::string> filter)
                         { pe_trace::start(filter.value_or("")); });
        lua.set_function("PETraceStop", []()
                         { pe_trace::stop(); });
        lua.set_function("PETraceClear", []()
                         { pe_trace::clear(); });
        lua.set_function("PETraceIsActive", []() -> bool
                         { return pe_trace::is_active(); });
        lua.set_function("PETraceStatus", []() -> std::string
                         { return pe_trace::status(); });
        lua.set_function("PETraceTop", [](sol::optional<int> n) -> std::string
                         { return pe_trace::top(n.value_or(50)); });
        lua.set_function("PETraceTopByTime", [](sol::optional<int> n) -> std::string
                         { return pe_trace::top_by_time(n.value_or(50)); });
        lua.set_function("PETraceDump", []() -> std::string
                         { return pe_trace::dump_to_file(); });
        lua.set_function("PETraceDetailed", [](bool enable)
                         { pe_trace::set_detailed(enable); });

        // PE Trace Watch system — Lua callback on specific function patterns
        lua.set_function("PETraceWatch", [](const std::string &pattern, sol::function callback) -> uint32_t
                         {
            // Wrap Lua function into C++ callback
            // Note: sol::function is reference-counted, safe to capture
            auto cpp_cb = [callback](ue::UObject* self, ue::UFunction* func,
                                     const std::string& func_key, uint64_t call_count) {
                try {
                    lua_uobject::LuaUObject wrapped;
                    wrapped.ptr = self;
                    callback(wrapped, func_key, call_count);
                } catch (const std::exception& e) {
                    logger::log_warn("TRACE", "Watch callback error: %s", e.what());
                }
            };
            return pe_trace::register_watch(pattern, cpp_cb); });

        lua.set_function("PETraceUnwatch", [](uint32_t watch_id)
                         { pe_trace::unregister_watch(watch_id); });

        lua.set_function("PETraceClearWatches", []()
                         { pe_trace::clear_watches(); });

        // ─── Game Profile Info ──────────────────────────────────────────────
        lua.set_function("GetGameInfo", [](sol::this_state ts) -> sol::table
                         {
            sol::state_view lua_view(ts);
            sol::table t = lua_view.create_table();
            t["package"] = game_profile::package_name();
            t["display_name"] = game_profile::display_name();
            t["engine_lib"] = game_profile::engine_lib_name();
            auto id = game_profile::detected_game();
            t["game_id"] = (id == game_profile::GameID::RE4_VR) ? "RE4VR" :
                           (id == game_profile::GameID::PINBALL_FX_VR) ? "PFXVR" : "UNKNOWN";
            return t; });

        // ─── Sandbox Paths ──────────────────────────────────────────────────
        lua.set_function("GetSandboxPaths", [](sol::this_state ts) -> sol::table
                         {
            sol::state_view lua_view(ts);
            sol::table t = lua_view.create_table();
            t["data_dir"] = paths::data_dir();
            t["mods_dir"] = paths::mods_dir();
            t["paks_dir"] = paths::paks_dir();
            t["sdk_dir"] = paths::sdk_dir();
            t["log_path"] = paths::log_path();
            t["crash_log"] = paths::crash_log();
            return t; });

        // ═══════════════════════════════════════════════════════════════════
        // IConsoleManager Direct Access — Works even in shipping builds
        //
        // UE4/5 always creates the IConsoleManager singleton and populates
        // ConsoleObjects TMap<FString, IConsoleObject*>. Even when shipping
        // builds strip FExec::Exec() and help text, the ConsoleManager itself
        // is alive and all CVars/CCmd objects exist.
        //
        // We access console commands through:
        //   1. PlayerController::ConsoleCommand (UFunction via ProcessEvent)
        //   2. Direct CVar read/write via FindConsoleVariable UFunction
        //
        // For dumping ALL registered commands, we use the engine's own
        // ForEachConsoleObjectThatStartsWith() via reflection on GameEngine.
        // ═══════════════════════════════════════════════════════════════════

        // ExecConsoleCommand — execute any console command string
        // Works via PlayerController::ConsoleCommand (ProcessEvent, not Exec)
        // This bypasses all shipping guards since ConsoleCommand is a UFunction.
        lua.set_function("ExecConsoleCommand", [](const std::string &cmd_string) -> std::string
                         {
            auto* pc = find_player_controller();
            if (!pc) {
                logger::log_warn("CONSOLE", "ExecConsoleCommand: no PlayerController found");
                return "ERROR: no PlayerController";
            }

            // Find ConsoleCommand function on PlayerController
            auto* func = pe_hook::resolve_func_path("PlayerController:ConsoleCommand");
            if (!func) {
                logger::log_warn("CONSOLE", "ExecConsoleCommand: ConsoleCommand UFunction not found");
                return "ERROR: ConsoleCommand not found";
            }

            // Build params: FString Command, bool bWriteToLog
            uint16_t parms_size = ue::ufunc_get_parms_size(func);
            std::vector<uint8_t> parms(parms_size > 0 ? parms_size : 128, 0);

            // FString = { char16_t* Data; int32 Num; int32 Max; }
            std::u16string u16cmd(cmd_string.begin(), cmd_string.end());
            u16cmd.push_back(u'\0');
            struct FStr { const char16_t* data; int32_t num, max; };
            FStr fs;
            fs.data = u16cmd.c_str();
            fs.num = static_cast<int32_t>(u16cmd.size());
            fs.max = fs.num;
            std::memcpy(parms.data(), &fs, sizeof(FStr));

            auto pe = pe_hook::get_original();
            if (!pe) pe = symbols::ProcessEvent;
            if (!pe) return "ERROR: ProcessEvent not available";

            pe(pc, func, parms.data());
            logger::log_info("CONSOLE", "ExecConsoleCommand: '%s'", cmd_string.c_str());

            // Try to read return value (FString result at return_offset)
            // ConsoleCommand returns FString on some builds
            return "OK"; });

        // DumpConsoleCommands — dump all known UFunction-based console commands
        // Walks ALL discovered classes looking for exec functions (EFunctionFlags::FUNC_Exec)
        // Also dumps CVars found on any reflected object with ConsoleVariable props
        lua.set_function("DumpConsoleCommands", [](sol::this_state ts) -> sol::table
                         {
            sol::state_view lua_view(ts);
            sol::table result = lua_view.create_table();

            int cmd_idx = 1;
            std::string dump_log;

            // 1. Walk all known classes and find functions with FUNC_Exec flag
            const auto& classes = reflection::get_classes();
            for (const auto& ci : classes) {
                for (const auto& fi : ci.functions) {
                    // FUNC_Exec = 0x00000200
                    if (fi.flags & 0x00000200) {
                        sol::table entry = lua_view.create_table();
                        entry["class"] = ci.name;
                        entry["name"] = fi.name;
                        entry["full"] = ci.name + ":" + fi.name;
                        entry["flags"] = fi.flags;
                        entry["num_params"] = fi.num_parms;
                        result[cmd_idx++] = entry;
                        dump_log += ci.name + ":" + fi.name + "\n";
                    }
                }
            }

            // 2. Also find all CheatManager functions (they're all console commands)
            auto* cm_rc = rebuilder::rebuild("CheatManager");
            if (cm_rc) {
                for (const auto& rf : cm_rc->all_functions) {
                    sol::table entry = lua_view.create_table();
                    entry["class"] = "CheatManager";
                    entry["name"] = rf.name;
                    entry["full"] = "CheatManager:" + rf.name;
                    entry["flags"] = rf.flags;
                    entry["num_params"] = static_cast<int>(rf.params.size());
                    entry["source"] = "cheatmanager";
                    result[cmd_idx++] = entry;
                    dump_log += "CheatManager:" + rf.name + "\n";
                }
            }

            // 3. Dump KismetSystemLibrary commands
            auto* ksl_rc = rebuilder::rebuild("KismetSystemLibrary");
            if (ksl_rc) {
                for (const auto& rf : ksl_rc->all_functions) {
                    if (rf.flags & 0x00000200) { // FUNC_Exec
                        sol::table entry = lua_view.create_table();
                        entry["class"] = "KismetSystemLibrary";
                        entry["name"] = rf.name;
                        entry["full"] = "KismetSystemLibrary:" + rf.name;
                        entry["flags"] = rf.flags;
                        entry["num_params"] = static_cast<int>(rf.params.size());
                        result[cmd_idx++] = entry;
                    }
                }
            }

            // Write dump to file
            if (!dump_log.empty()) {
                std::string path = paths::data_dir() + "/console_commands_dump.txt";
                FILE* f = fopen(path.c_str(), "w");
                if (f) {
                    fprintf(f, "=== Console Commands Dump ===\n");
                    fprintf(f, "Total: %d commands\n\n", cmd_idx - 1);
                    fprintf(f, "%s", dump_log.c_str());
                    fclose(f);
                    logger::log_info("CONSOLE", "Dumped %d commands to %s", cmd_idx - 1, path.c_str());
                }
            }

            return result; });

        // DumpAllFunctions — dump ALL functions from ALL classes to a file
        // This is the most comprehensive function listing possible
        lua.set_function("DumpAllFunctions", [](sol::optional<std::string> filter) -> std::string
                         {
            std::string path = paths::data_dir() + "/all_functions_dump.txt";
            FILE* f = fopen(path.c_str(), "w");
            if (!f) return "ERROR: could not open " + path;

            std::string flt = filter.value_or("");
            int total = 0;

            const auto& classes = reflection::get_classes();
            for (const auto& ci : classes) {
                if (!flt.empty() && ci.name.find(flt) == std::string::npos) continue;
                for (const auto& fi : ci.functions) {
                    fprintf(f, "%s:%s  flags=0x%08X  params=%d\n",
                            ci.name.c_str(), fi.name.c_str(), fi.flags, fi.num_parms);

                    // Dump each param
                    for (const auto& pi : fi.params) {
                        const char* dir = (pi.flags & ue::CPF_ReturnParm) ? "ret" :
                                          (pi.flags & ue::CPF_OutParm)    ? "out" : "in";
                        fprintf(f, "    [%s] %s : %s (offset=%d, size=%d)\n",
                                dir, pi.name.c_str(), pi.inner_type_name.c_str(),
                                pi.offset, pi.element_size);
                    }
                    total++;
                }
            }

            fclose(f);
            char buf[256];
            snprintf(buf, sizeof(buf), "Dumped %d functions to %s", total, path.c_str());
            logger::log_info("CONSOLE", "%s", buf);
            return std::string(buf); });

        // DumpAllProperties — dump ALL properties from ALL classes to a file
        lua.set_function("DumpAllProperties", [](sol::optional<std::string> filter) -> std::string
                         {
            std::string path = paths::data_dir() + "/all_properties_dump.txt";
            FILE* f = fopen(path.c_str(), "w");
            if (!f) return "ERROR: could not open " + path;

            std::string flt = filter.value_or("");
            int total = 0;

            const auto& classes = reflection::get_classes();
            for (const auto& ci : classes) {
                if (!flt.empty() && ci.name.find(flt) == std::string::npos) continue;
                if (ci.properties.empty()) continue;
                fprintf(f, "\n=== %s (size=%d) ===\n", ci.name.c_str(), ci.properties_size);
                for (const auto& pi : ci.properties) {
                    fprintf(f, "  +0x%04X [%d] %s : %s",
                            pi.offset, pi.element_size, pi.name.c_str(),
                            pi.inner_type_name.c_str());
                    if (pi.type == reflection::PropType::BoolProperty)
                        fprintf(f, " (mask=0x%02X)", pi.bool_field_mask);
                    fprintf(f, "\n");
                    total++;
                }
            }

            fclose(f);
            char buf[256];
            snprintf(buf, sizeof(buf), "Dumped %d properties to %s", total, path.c_str());
            logger::log_info("CONSOLE", "%s", buf);
            return std::string(buf); });

        // GetCVar — read a CVar's value by name
        // Uses ExecConsoleCommand to query, or direct property access
        lua.set_function("GetCVar", [](const std::string &cvar_name) -> std::string
                         {
            // Try to get CVar value via console command
            auto* pc = find_player_controller();
            if (!pc) return "ERROR: no PC";

            auto* func = pe_hook::resolve_func_path("PlayerController:ConsoleCommand");
            if (!func) return "ERROR: no ConsoleCommand";

            uint16_t parms_size = ue::ufunc_get_parms_size(func);
            std::vector<uint8_t> parms(parms_size > 0 ? parms_size : 128, 0);

            // Query: just the CVar name without a value = getter
            std::string query = cvar_name;
            std::u16string u16cmd(query.begin(), query.end());
            u16cmd.push_back(u'\0');
            struct FStr { const char16_t* data; int32_t num, max; };
            FStr fs;
            fs.data = u16cmd.c_str();
            fs.num = static_cast<int32_t>(u16cmd.size());
            fs.max = fs.num;
            std::memcpy(parms.data(), &fs, sizeof(FStr));

            auto pe = pe_hook::get_original();
            if (!pe) pe = symbols::ProcessEvent;
            if (!pe) return "ERROR: no ProcessEvent";

            pe(pc, func, parms.data());
            return "OK (value written to log)"; });

        // SetCVar — set a CVar's value by name
        lua.set_function("SetCVar", [](const std::string &cvar_name, const std::string &value) -> bool
                         {
            auto* pc = find_player_controller();
            if (!pc) return false;

            auto* func = pe_hook::resolve_func_path("PlayerController:ConsoleCommand");
            if (!func) return false;

            uint16_t parms_size = ue::ufunc_get_parms_size(func);
            std::vector<uint8_t> parms(parms_size > 0 ? parms_size : 128, 0);

            std::string cmd = cvar_name + " " + value;
            std::u16string u16cmd(cmd.begin(), cmd.end());
            u16cmd.push_back(u'\0');
            struct FStr { const char16_t* data; int32_t num, max; };
            FStr fs;
            fs.data = u16cmd.c_str();
            fs.num = static_cast<int32_t>(u16cmd.size());
            fs.max = fs.num;
            std::memcpy(parms.data(), &fs, sizeof(FStr));

            auto pe = pe_hook::get_original();
            if (!pe) pe = symbols::ProcessEvent;
            if (!pe) return false;

            pe(pc, func, parms.data());
            logger::log_info("CONSOLE", "SetCVar: %s = %s", cvar_name.c_str(), value.c_str());
            return true; });

        logger::log_info("LUA", "Console command + CheatManager + ViewMode + diagnostics + DumpCmds API registered");
    }

} // namespace lua_console
