// ═══════════════════════════════════════════════════════════════════════
//  Game Database — known games and their smali injection targets
//  NO binary patching here. The modloader + Lua mods handle game logic.
// ═══════════════════════════════════════════════════════════════════════

/// A game the modloader supports
#[derive(Debug, Clone)]
pub struct GameProfile {
    /// Human-readable name
    pub name: &'static str,
    /// Android package name
    pub package: &'static str,
    /// Primary smali class to inject System.loadLibrary("modloader") into
    /// (the main Activity's onCreate)
    pub smali_target: &'static str,
    /// Fallback smali targets to try if the primary is not found
    pub smali_fallbacks: &'static [&'static str],
    /// Native library name the game uses (for reference only)
    #[allow(dead_code)]
    pub native_lib: &'static str,
}

/// Common UE4/UE5 Activity classes found on Quest games.
/// Used as a last-resort fallback when no game profile matches.
pub const UE_COMMON_ACTIVITIES: &[&str] = &[
    "com/epicgames/ue4/GameActivity",
    "com/epicgames/unreal/GameActivity",
    "com/epicgames/ue4/SplashActivity",
];

/// All supported games
pub const GAMES: &[GameProfile] = &[
    GameProfile {
        name: "Resident Evil 4 VR",
        package: "com.Armature.VR4",
        smali_target: "com/epicgames/ue4/GameActivity",
        smali_fallbacks: &[
            "com/Armature/VR4/OculusMobileActivity",
            "com/epicgames/unreal/GameActivity",
        ],
        native_lib: "libUE4.so",
    },
    GameProfile {
        name: "Pinball FX VR",
        package: "com.zenstudios.PFXVRQuest",
        smali_target: "com/epicgames/unreal/GameActivity",
        smali_fallbacks: &[
            "com/epicgames/ue4/GameActivity",
        ],
        native_lib: "libUnreal.so",
    },
];

/// Find a game profile by package name
pub fn find_by_package(pkg: &str) -> Option<&'static GameProfile> {
    GAMES.iter().find(|g| g.package == pkg)
}

/// Get display list of all supported games
pub fn game_names() -> Vec<&'static str> {
    GAMES.iter().map(|g| g.name).collect()
}
