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
    /// Smali class to inject System.loadLibrary("modloader") into
    /// (the main Activity's onCreate)
    pub smali_target: &'static str,
    /// Native library name the game uses (for reference only)
    #[allow(dead_code)]
    pub native_lib: &'static str,
}

/// All supported games
pub const GAMES: &[GameProfile] = &[
    GameProfile {
        name: "Resident Evil 4 VR",
        package: "com.Armature.VR4",
        smali_target: "com/Armature/VR4/OculusMobileActivity",
        native_lib: "libUE4.so",
    },
    GameProfile {
        name: "Pinball FX VR",
        package: "com.zenstudios.PFXVRQuest",
        smali_target: "com/epicgames/unreal/GameActivity",
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
