#pragma once
// modloader/include/modloader/ue_console.h
// ═══════════════════════════════════════════════════════════════════════════
// UE4/5 Console Command & CheatManager API
//
// Exposes the full power of UE's console command system and CheatManager
// to Lua mods. Works on any UE4/5 game — no root needed, runs inside
// the app's own sandbox.
//
// CONSOLE COMMANDS:
//   Execute any UE4/5 console command as if typed in the game's console.
//   Uses PlayerController::ConsoleCommand() or GEngine->Exec() internally.
//   Works for: stat, showflag, viewmode, r., sg., t., etc.
//
// CHEAT MANAGER:
//   Spawns and manages UCheatManager on the PlayerController.
//   Provides wrappers for all built-in UE cheats:
//     God, Fly, Ghost, Walk, Teleport, Slomo, ChangeSize, etc.
//   Also supports game-specific CheatManager subclasses.
//
// VIEWMODES:
//   Force rendering viewmodes: Wireframe, Unlit, Lit, DetailLighting,
//   LightingOnly, PathTracing, etc.
//
// SHOW FLAGS:
//   Toggle engine show flags: Fog, Particles, Atmosphere, Bloom,
//   PostProcessing, AmbientOcclusion, etc.
//
// STAT COMMANDS:
//   Toggle stats: FPS, Unit, UnitGraph, Engine, Game, etc.
// ═══════════════════════════════════════════════════════════════════════════

#include <string>
#include <functional>
#include <vector>
#include "modloader/types.h"

namespace ue_console
{

    // ═══ Initialization ═════════════════════════════════════════════════════

    // Initialize the console command subsystem.
    // Must be called after symbols are resolved (ProcessEvent, reflection up).
    void init();

    // ═══ Console Command Execution ══════════════════════════════════════════

    // Execute a console command string (like typing in the UE console).
    // Routes through PlayerController::ConsoleCommand() first, falls back
    // to GEngine->Exec(). Returns the command's output string (may be empty).
    //
    // Examples:
    //   exec("stat fps")
    //   exec("viewmode wireframe")
    //   exec("showflag.fog 0")
    //   exec("r.SetRes 1920x1080")
    //   exec("slomo 0.5")
    //   exec("god")
    std::string exec(const std::string &command);

    // Execute a console command via a specific world context.
    // Useful when you need to target a specific world (PIE, etc.)
    std::string exec_with_world(const std::string &command, ue::UObject *world);

    // ═══ CheatManager ═══════════════════════════════════════════════════════

    // Get or spawn the CheatManager on the local PlayerController.
    // If no CheatManager exists, creates one using StaticConstructObject.
    // Returns nullptr if the PC or CheatManager class can't be found.
    ue::UObject *get_or_spawn_cheat_manager();

    // Check if a CheatManager is currently active
    bool has_cheat_manager();

    // Get the current CheatManager (nullptr if none exists)
    ue::UObject *get_cheat_manager();

    // ═══ Built-in Cheats (via ProcessEvent on CheatManager) ═════════════════

    // Movement modes
    bool cheat_god();   // Toggle god mode (invulnerability)
    bool cheat_fly();   // Toggle fly mode
    bool cheat_ghost(); // Toggle ghost mode (noclip)
    bool cheat_walk();  // Return to walking mode

    // Game speed
    bool cheat_slomo(float rate); // Set game speed (1.0 = normal, 0.5 = half, 2.0 = double)

    // Teleport
    bool cheat_teleport(); // Teleport to crosshair location

    // Size/scale
    bool cheat_change_size(float factor); // Scale player size

    // Damage
    bool cheat_damage_target(float amount); // Damage whatever you're looking at

    // Summon/Destroy
    bool cheat_summon(const std::string &class_name); // Spawn actor by class name
    bool cheat_destroy_target();                      // Destroy whatever you're looking at

    // Pause
    bool cheat_pause(); // Toggle game pause

    // ═══ ViewMode Control ═══════════════════════════════════════════════════
    // These use console commands internally. ViewMode index values are
    // engine-defined and match EViewModeIndex.

    bool set_viewmode(const std::string &mode_name);

    // Common viewmode presets
    bool viewmode_wireframe();
    bool viewmode_unlit();
    bool viewmode_lit();
    bool viewmode_detail_lighting();
    bool viewmode_lighting_only();
    bool viewmode_light_complexity();
    bool viewmode_shader_complexity();
    bool viewmode_collision();

    // ═══ Show Flags ═════════════════════════════════════════════════════════

    // Toggle a show flag by name. Value: 0=off, 1=on, 2=toggle.
    bool set_show_flag(const std::string &flag_name, int value);

    // Get list of common show flag names
    std::vector<std::string> get_common_show_flags();

    // ═══ Stat Commands ══════════════════════════════════════════════════════

    // Toggle a stat display by name (e.g. "fps", "unit", "game")
    bool toggle_stat(const std::string &stat_name);

    // ═══ Engine CVars ═══════════════════════════════════════════════════════

    // Set an engine CVar by name (r., sg., t., etc.)
    bool set_cvar(const std::string &name, const std::string &value);

    // Get an engine CVar value (returns empty string on failure)
    std::string get_cvar(const std::string &name);

} // namespace ue_console
