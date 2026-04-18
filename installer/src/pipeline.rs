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

    // 2. Try to rebuild from source if build script exists
    let build_dirs: &[&str] = &[
        "modloader",
        "../modloader",
    ];
    for dir in build_dirs {
        let build_bat = Path::new(dir).join("build.bat");
        if build_bat.exists() {
            log::info!("Found modloader source at {} — rebuilding...", dir);
            let status = std::process::Command::new("cmd")
                .args(["/C", &build_bat.to_string_lossy()])
                .current_dir(dir)
                .status();
            match status {
                Ok(s) if s.success() => {
                    // Check for stripped deploy copy first, then regular build
                    let deploy = Path::new(dir).join("build/deploy/libmodloader.so");
                    if deploy.exists() {
                        log::info!("✓ Modloader rebuilt — using {}", deploy.display());
                        return Ok(deploy);
                    }
                    let built = Path::new(dir).join("build/libmodloader.so");
                    if built.exists() {
                        log::info!("✓ Modloader rebuilt — using {}", built.display());
                        return Ok(built);
                    }
                    log::warn!("Build succeeded but .so not found in {}/build/", dir);
                }
                Ok(s) => log::warn!("Modloader build failed (exit {})", s),
                Err(e) => log::warn!("Could not run build.bat: {}", e),
            }
        }
    }

    // 3. Next to installer binary
    if let Ok(exe) = std::env::current_exe() {
        let dir = exe.parent().unwrap_or(Path::new("."));
        let p = dir.join("libmodloader.so");
        if p.exists() { return Ok(p); }
    }
    // 4. Current working dir
    let p = PathBuf::from("libmodloader.so");
    if p.exists() { return Ok(p); }
    // 5. modloader/build dir (pre-built, no rebuild attempted)
    let p = PathBuf::from("modloader/build/deploy/libmodloader.so");
    if p.exists() { return Ok(p); }
    let p = PathBuf::from("modloader/build/libmodloader.so");
    if p.exists() { return Ok(p); }
    // 6. Relative to workspace
    let p = PathBuf::from("../modloader/build/deploy/libmodloader.so");
    if p.exists() { return Ok(p); }
    let p = PathBuf::from("../modloader/build/libmodloader.so");
    if p.exists() { return Ok(p); }

    bail!(
        "libmodloader.so not found.\n\
         Place it next to the installer or pass --modloader-so <path>"
    )
}

/// Patch a local APK file — no device needed.
/// Decompiles the APK, injects the modloader, recompiles, and signs.
pub fn patch_local_apk(
    apk_path: &str,
    package_hint: Option<&str>,
    so_override: Option<&str>,
    output: Option<&str>,
) -> Result<()> {
    let apk = PathBuf::from(apk_path);
    if !apk.exists() {
        bail!("APK not found: {}", apk_path);
    }

    let so_path = find_modloader_so(so_override)?;
    log::info!("libmodloader.so: {}", so_path.display());

    // Resolve game profile if we have a package hint
    let game_profile = package_hint
        .and_then(|pkg| game_db::find_by_package(pkg));

    // Determine game name from profile or APK filename
    let game_name = if let Some(g) = game_profile {
        g.name
    } else {
        let stem = apk.file_stem().unwrap_or_default().to_string_lossy();
        let detected = game_db::GAMES.iter().find(|g| stem.contains(g.package));
        detected.map(|g| g.name).unwrap_or("Unknown Game")
    };

    let work_dir = tempfile::tempdir()?;

    // 1. Decompile
    log::info!("[1/5] Decompiling {}...", apk.file_name().unwrap_or_default().to_string_lossy());
    let decompiled = work_dir.path().join("decompiled");
    smali::decompile(&apk, &decompiled)?;

    // 2. Smart injection — tries profile targets, manifest auto-detect, common UE activities
    log::info!("[2/5] Injecting modloader...");
    let target = smali::find_injection_target(&decompiled, game_profile)?;
    smali::inject_loadlibrary(&decompiled, &target)?;

    smali::add_native_lib(&decompiled, &so_path)?;
    smali::fix_manifest(&decompiled)?;

    // 3. Recompile
    log::info!("[3/5] Recompiling APK...");
    let recompiled = work_dir.path().join("patched.apk");
    smali::recompile(&decompiled, &recompiled)?;

    // 4. Sign
    log::info!("[4/5] Signing APK...");
    let signed = signer::sign_apk(&recompiled)?;

    // 5. Copy to output
    let output_path = if let Some(out) = output {
        PathBuf::from(out)
    } else {
        let stem = apk.file_stem().unwrap_or_default().to_string_lossy();
        apk.parent().unwrap_or(Path::new(".")).join(format!("{}-modded.apk", stem))
    };

    std::fs::copy(&signed, &output_path)?;
    let size_mb = std::fs::metadata(&output_path).map(|m| m.len() as f64 / 1_000_000.0).unwrap_or(0.0);

    log::info!("[5/5] Done!");
    log::info!("═══════════════════════════════════════════");
    log::info!("✅ Patched APK: {} ({:.1} MB)", output_path.display(), size_mb);
    log::info!("   Game: {}", game_name);
    log::info!("═══════════════════════════════════════════");
    log::info!("");
    log::info!("Install on device:");
    log::info!("  adb install -r -d -g {}", output_path.display());

    Ok(())
}

/// Full install pipeline (device-connected mode)
pub fn install(
    serial: &str,
    game: &game_db::GameProfile,
    so_path: &Path,
    progress: Option<ProgressFn>,
) -> Result<()> {
    let total = 10u32;
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

    // 5. Smart injection — tries profile targets, manifest auto-detect, common UE activities
    report(4, "Injecting modloader...");
    let target = smali::find_injection_target(&decompiled, Some(game))?;
    log::info!("Injection target: {}", target);
    smali::inject_loadlibrary(&decompiled, &target)?;
    smali::add_native_lib(&decompiled, so_path)?;
    smali::fix_manifest(&decompiled)?;

    // 6. Recompile
    report(5, "Recompiling APK...");
    let patched_apk = work_dir.path().join("patched.apk");
    smali::recompile(&decompiled, &patched_apk)?;

    // 7. Sign
    report(6, "Signing APK...");
    let signed_apk = signer::sign_apk(&patched_apk)?;

    // 7b. Verify full signing BEFORE touching installed app
    report(7, "Verifying full APK signature (v1+v2)...");
    signer::verify_apk_full_signature(&signed_apk)?;

    // 8. Temporarily rename OBB/data, uninstall old, install new, restore dirs
    report(8, "Temporarily renaming OBB/data...");
    let backups = adb::backup_game_dirs(serial, game.package)?;

    report(9, "Installing patched APK...");
    let install_result = (|| -> Result<()> {
        adb::uninstall(serial, game.package)?;
        adb::install_apk(serial, &signed_apk)?;
        Ok(())
    })();

    // ALWAYS restore dirs — even if uninstall/install failed — so the
    // game data is never left orphaned as .modloader_bak
    if let Err(e) = adb::restore_game_dirs(serial, &backups) {
        log::error!("Failed to restore game dirs: {}", e);
    }

    // Now propagate any install failure
    install_result?;

    report(10, "Finalizing — setting up directories & permissions...");
    setup_game_dirs(serial, game)?;

    log::info!("✅ {} modloader installed successfully!", game.name);
    Ok(())
}

/// Post-install: grant runtime permissions only.
///
/// CRITICAL: Do NOT create directories under /sdcard/Android/data/<pkg>/ via
/// adb shell. On Android 11+, this path is scoped storage — only the app
/// itself can create files/dirs there. Creating them as the shell user (UID 2000)
/// corrupts the FUSE permission layer, breaks MTP (Windows file explorer shows
/// infinite loading), and makes files undeletable.
///
/// The modloader (running as the app) creates mods/ and paks/ directories
/// at startup via paths::ensure_dir(). Let it do its job.
fn setup_game_dirs(serial: &str, game: &game_db::GameProfile) -> Result<()> {
    let pkg = game.package;

    // Grant runtime storage permissions via Android's permission manager
    let perms = [
        "android.permission.READ_EXTERNAL_STORAGE",
        "android.permission.WRITE_EXTERNAL_STORAGE",
        "android.permission.MANAGE_EXTERNAL_STORAGE",
    ];
    for perm in &perms {
        adb::shell(serial, &format!("pm grant {} {} 2>/dev/null", pkg, perm))?;
    }

    // Enable "All files access" for Android 11+
    adb::shell(serial, &format!(
        "appops set {} MANAGE_EXTERNAL_STORAGE allow 2>/dev/null", pkg
    ))?;

    // Create the non-scoped data directory
    // /sdcard/UnrealModloader/<pkg>/ is outside scoped storage — fully accessible
    // by MTP, file browsers, adb, etc. No ownership issues.
    adb::shell(serial, &format!("mkdir -p /sdcard/UnrealModloader/{}/mods", pkg))?;
    adb::shell(serial, &format!("mkdir -p /sdcard/UnrealModloader/{}/paks", pkg))?;

    // Clean up old scoped-storage leftovers from previous installer versions
    let cleanup_paths = [
        format!("/sdcard/Android/data/{}.modloader_bak", pkg),
        format!("/sdcard/Android/obb/{}.modloader_bak", pkg),
    ];
    for path in &cleanup_paths {
        adb::shell(serial, &format!("rm -rf '{}' 2>/dev/null", path))?;
    }

    log::info!("Modloader dirs created at /sdcard/UnrealModloader/{}/", pkg);
    Ok(())
}
