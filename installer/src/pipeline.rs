// ═══════════════════════════════════════════════════════════════════════
//  Pipeline — the full install flow, used by both CLI and GUI
// ═══════════════════════════════════════════════════════════════════════

use crate::{adb, game_db, signer, smali};
use anyhow::{bail, Result};
use std::path::{Path, PathBuf};

/// Progress callback: (step_number, total_steps, message)
pub type ProgressFn = Box<dyn Fn(u32, u32, &str) + Send>;

/// Where the modloader .so comes from
pub fn find_modloader_so(override_path: Option<&str>) -> Result<PathBuf> {
    // 1. Explicit override
    if let Some(p) = override_path {
        let pb = PathBuf::from(p);
        if pb.exists() { return Ok(pb); }
        bail!("Specified modloader .so not found: {}", p);
    }
    // 2. Next to installer binary
    if let Ok(exe) = std::env::current_exe() {
        let dir = exe.parent().unwrap_or(Path::new("."));
        let p = dir.join("libmodloader.so");
        if p.exists() { return Ok(p); }
    }
    // 3. Current working dir
    let p = PathBuf::from("libmodloader.so");
    if p.exists() { return Ok(p); }
    // 4. modloader/build dir
    let p = PathBuf::from("modloader/build/libmodloader.so");
    if p.exists() { return Ok(p); }
    // 5. Relative to workspace
    let p = PathBuf::from("../modloader/build/libmodloader.so");
    if p.exists() { return Ok(p); }

    bail!(
        "libmodloader.so not found.\n\
         Place it next to the installer or pass --modloader-so <path>"
    )
}

/// Full install pipeline
pub fn install(
    serial: &str,
    game: &game_db::GameProfile,
    so_path: &Path,
    progress: Option<ProgressFn>,
) -> Result<()> {
    let total = 9u32;
    let report = |step: u32, msg: &str| {
        log::info!("[{}/{}] {}", step, total, msg);
        if let Some(ref cb) = progress {
            cb(step, total, msg);
        }
    };

    // 1. Stop the game
    report(1, &format!("Stopping {}...", game.package));
    adb::force_stop(serial, game.package)?;

    // 2. Check if already installed
    let already = adb::is_modloader_installed(serial, game.package)?;
    if already {
        log::warn!("Modloader already installed for {}. Re-patching anyway.", game.name);
    }

    // 3. Pull APK
    report(2, "Pulling APK from device...");
    let work_dir = tempfile::tempdir()?;
    let app = adb::get_installed_app(serial, game.package)?
        .ok_or_else(|| anyhow::anyhow!("{} is not installed on device", game.name))?;
    let apk = adb::pull_apk(serial, &app, work_dir.path())?;

    // 4. Decompile
    report(3, "Decompiling APK...");
    let decompiled = work_dir.path().join("decompiled");
    smali::decompile(&apk, &decompiled)?;

    // 5. Inject loadLibrary + add .so + fix manifest
    report(4, "Injecting modloader...");
    smali::inject_loadlibrary(&decompiled, game.smali_target)?;
    smali::add_native_lib(&decompiled, so_path)?;
    smali::fix_manifest(&decompiled)?;

    // 6. Recompile
    report(5, "Recompiling APK...");
    let patched_apk = work_dir.path().join("patched.apk");
    smali::recompile(&decompiled, &patched_apk)?;

    // 7. Sign
    report(6, "Signing APK...");
    let signed_apk = signer::sign_apk(&patched_apk)?;

    // 8. Backup game data, uninstall old, install new, restore data
    report(7, "Backing up game data...");
    let backups = adb::backup_game_dirs(serial, game.package)?;

    report(8, "Installing patched APK...");
    adb::uninstall(serial, game.package)?;
    adb::install_apk(serial, &signed_apk)?;

    report(9, "Restoring game data...");
    adb::restore_game_dirs(serial, &backups)?;

    log::info!("✅ {} modloader installed successfully!", game.name);
    Ok(())
}
