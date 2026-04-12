// ═══════════════════════════════════════════════════════════════════════
//  ADB — Quest detection, pull/install APKs, backup/restore game data
// ═══════════════════════════════════════════════════════════════════════

use anyhow::{bail, Context, Result};
use std::path::{Path, PathBuf};
use std::process::Command;

// ── Find ADB ────────────────────────────────────────────────────────────

pub fn find_adb() -> Result<PathBuf> {
    if let Ok(p) = which::which("adb") {
        return Ok(p);
    }
    for dir in [
        r"C:\platform-tools",
        r"C:\Android\platform-tools",
        r"C:\AstroTools\scrcpy",
        r"C:\Rookie Sideloader\platform-tools",
        r"C:\Program Files\Meta Quest Developer Hub\resources\bin",
        r"C:\Program Files (x86)\Android\android-sdk\platform-tools",
    ] {
        let p = PathBuf::from(dir).join("adb.exe");
        if p.exists() { return Ok(p); }
    }
    for var in ["ANDROID_HOME", "ANDROID_SDK_ROOT"] {
        if let Ok(sdk) = std::env::var(var) {
            let p = PathBuf::from(&sdk).join("platform-tools").join("adb.exe");
            if p.exists() { return Ok(p); }
        }
    }
    bail!("ADB not found. Install Android Platform-Tools and add to PATH.")
}

fn adb_s(serial: &str, args: &[&str]) -> Result<std::process::Output> {
    let bin = find_adb()?;
    Command::new(&bin)
        .arg("-s").arg(serial)
        .args(args)
        .output()
        .with_context(|| format!("adb -s {} {}", serial, args.join(" ")))
}

pub fn shell(serial: &str, cmd: &str) -> Result<String> {
    let o = adb_s(serial, &["shell", cmd])?;
    Ok(String::from_utf8_lossy(&o.stdout).trim().to_string())
}

// ── Device Discovery ────────────────────────────────────────────────────

#[derive(Debug, Clone)]
pub struct Device {
    pub serial: String,
    pub model: String,
    pub is_quest: bool,
}

pub fn list_devices() -> Result<Vec<Device>> {
    let bin = find_adb()?;
    let o = Command::new(&bin).args(["devices", "-l"]).output()?;
    let stdout = String::from_utf8_lossy(&o.stdout);
    let mut devs = Vec::new();
    for line in stdout.lines().skip(1) {
        let parts: Vec<&str> = line.split_whitespace().collect();
        if parts.len() < 2 || parts[1] != "device" { continue; }
        let serial = parts[0].to_string();
        let model = parts.iter()
            .find_map(|p| p.strip_prefix("model:"))
            .unwrap_or("Unknown").to_string();
        let is_quest = model.contains("Quest")
            || model.contains("Hollywood")
            || model.contains("Eureka")
            || model.contains("Seacliff")
            || shell(&serial, "getprop ro.product.brand 2>/dev/null")
                .map(|b| b.to_lowercase().contains("oculus") || b.to_lowercase().contains("meta"))
                .unwrap_or(false);
        devs.push(Device { serial, model, is_quest });
    }
    Ok(devs)
}

// ── Package Info ────────────────────────────────────────────────────────

#[derive(Debug, Clone)]
pub struct InstalledApp {
    pub package: String,
    pub apk_path: String,
    pub version: String,
}

pub fn get_installed_app(serial: &str, package: &str) -> Result<Option<InstalledApp>> {
    let out = shell(serial, &format!("pm path {} 2>/dev/null", package))?;
    if !out.starts_with("package:") { return Ok(None); }
    let apk = out.strip_prefix("package:").unwrap_or(&out).trim().to_string();
    let ver_out = shell(serial, &format!(
        "dumpsys package {} 2>/dev/null | grep versionName | head -1", package
    )).unwrap_or_default();
    let version = ver_out.split("versionName=").nth(1)
        .and_then(|v| v.split_whitespace().next())
        .unwrap_or("?").to_string();
    Ok(Some(InstalledApp { package: package.to_string(), apk_path: apk, version }))
}

/// Check which known games are installed
pub fn find_installed_games(serial: &str, packages: &[&str]) -> Result<Vec<InstalledApp>> {
    let mut found = Vec::new();
    for pkg in packages {
        if let Some(app) = get_installed_app(serial, pkg)? {
            found.push(app);
        }
    }
    Ok(found)
}

// ── Pull APK ────────────────────────────────────────────────────────────

pub fn pull_apk(serial: &str, app: &InstalledApp, dest_dir: &Path) -> Result<PathBuf> {
    let local = dest_dir.join(format!("{}.apk", app.package));
    log::info!("Pulling APK from device...");
    let o = adb_s(serial, &["pull", &app.apk_path, &local.to_string_lossy()])?;
    if !o.status.success() {
        bail!("Failed to pull APK: {}", String::from_utf8_lossy(&o.stderr));
    }
    let size = std::fs::metadata(&local).map(|m| m.len() / 1_000_000).unwrap_or(0);
    log::info!("Pulled {} ({} MB)", local.display(), size);
    Ok(local)
}

// ── Install APK ─────────────────────────────────────────────────────────

pub fn install_apk(serial: &str, apk: &Path) -> Result<()> {
    log::info!("Installing {}...", apk.file_name().unwrap_or_default().to_string_lossy());
    let o = adb_s(serial, &["install", "-r", "-d", "-g", &apk.to_string_lossy()])?;
    let out = String::from_utf8_lossy(&o.stdout);
    if out.contains("Success") { return Ok(()); }
    let err = String::from_utf8_lossy(&o.stderr);
    bail!("Install failed: {}{}", out.trim(), err.trim())
}

pub fn uninstall(serial: &str, package: &str) -> Result<()> {
    log::info!("Uninstalling {}...", package);
    let o = adb_s(serial, &["shell", "pm", "uninstall", package])?;
    let out = String::from_utf8_lossy(&o.stdout);
    if !out.contains("Success") && !out.contains("Unknown package") {
        log::warn!("Uninstall result: {}", out.trim());
    }
    Ok(())
}

pub fn force_stop(serial: &str, package: &str) -> Result<()> {
    shell(serial, &format!("am force-stop {}", package))?;
    Ok(())
}

pub fn launch(serial: &str, package: &str) -> Result<()> {
    shell(serial, &format!(
        "monkey -p {} -c android.intent.category.LAUNCHER 1 2>/dev/null", package
    ))?;
    Ok(())
}

// ── Push file to device ─────────────────────────────────────────────────

pub fn push(serial: &str, local: &Path, remote: &str) -> Result<()> {
    let o = adb_s(serial, &["push", &local.to_string_lossy(), remote])?;
    if !o.status.success() {
        bail!("Push failed: {}", String::from_utf8_lossy(&o.stderr));
    }
    Ok(())
}

// ── OBB / Data Backup & Restore ─────────────────────────────────────────
// Rename dirs BEFORE uninstall so Android doesn't wipe them,
// rename back AFTER install so the game finds them.

pub fn backup_game_dirs(serial: &str, package: &str) -> Result<Vec<(String, String)>> {
    let mut backed = Vec::new();
    let dirs = [
        format!("/sdcard/Android/obb/{}", package),
        format!("/sdcard/Android/data/{}", package),
    ];
    for dir in &dirs {
        let bak = format!("{}.modloader_bak", dir);
        let exists = shell(serial, &format!("[ -d '{}' ] && echo Y || echo N", dir))?;
        if exists.contains('Y') {
            shell(serial, &format!("rm -rf '{}'", bak))?;
            shell(serial, &format!("mv '{}' '{}'", dir, bak))?;
            log::info!("Backed up {} → {}", dir, bak);
            backed.push((dir.clone(), bak));
        }
    }
    Ok(backed)
}

pub fn restore_game_dirs(serial: &str, backups: &[(String, String)]) -> Result<()> {
    for (orig, bak) in backups {
        let exists = shell(serial, &format!("[ -d '{}' ] && echo Y || echo N", bak))?;
        if exists.contains('Y') {
            // If new install created the dir, merge
            let new_exists = shell(serial, &format!("[ -d '{}' ] && echo Y || echo N", orig))?;
            if new_exists.contains('Y') {
                shell(serial, &format!("cp -a '{}'/* '{}/' 2>/dev/null; rm -rf '{}'", bak, orig, bak))?;
            } else {
                shell(serial, &format!("mv '{}' '{}'", bak, orig))?;
            }
            log::info!("Restored {} → {}", bak, orig);
        }
    }
    Ok(())
}

/// Check if libmodloader.so is already in the APK's native lib dir
pub fn is_modloader_installed(serial: &str, package: &str) -> Result<bool> {
    let out = shell(serial, &format!(
        "pm dump {} 2>/dev/null | grep nativeLibraryDir | head -1", package
    ))?;
    let dir = out.split('=').nth(1).unwrap_or("").trim();
    if dir.is_empty() { return Ok(false); }
    let check = shell(serial, &format!("ls {}/libmodloader.so 2>/dev/null", dir))?;
    Ok(check.contains("libmodloader"))
}
