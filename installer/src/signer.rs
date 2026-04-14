// ═══════════════════════════════════════════════════════════════════════
//  Signer — zipalign + sign APKs
//  Auto-downloads uber-apk-signer if no signing tool is available.
//  Priority: apksigner (SDK) → uber-apk-signer → jarsigner (JDK)
// ═══════════════════════════════════════════════════════════════════════

use anyhow::{bail, Context, Result};
use std::path::{Path, PathBuf};
use std::process::Command;

use crate::tools_setup;

/// Common SDK install paths (Windows)
const SDK_SEARCH_DIRS: &[&str] = &[
    r"C:\Program Files (x86)\Android\android-sdk",
    r"C:\Program Files\Android\android-sdk",
    r"C:\Android\android-sdk",
    r"C:\Android\Sdk",
    r"C:\Users\Public\Android\Sdk",
];

/// Find the latest build-tools directory from an SDK root
fn find_latest_build_tools(sdk: &Path) -> Option<PathBuf> {
    let bt = sdk.join("build-tools");
    if !bt.exists() { return None; }
    let mut versions: Vec<PathBuf> = std::fs::read_dir(&bt).ok()?
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| p.is_dir())
        .collect();
    versions.sort();
    versions.last().cloned()
}

/// Search all known SDK locations for a build-tools binary
fn find_build_tool(name: &str) -> Option<PathBuf> {
    let exe = format!("{}.exe", name);
    let bat = format!("{}.bat", name);
    let candidates = [name, exe.as_str(), bat.as_str()];

    if let Ok(p) = which::which(name) { return Some(p); }
    if let Ok(p) = which::which(&bat) { return Some(p); }

    let check_bt = |bt: &Path| -> Option<PathBuf> {
        for c in &candidates {
            let p = bt.join(c);
            if p.exists() { return Some(p); }
        }
        None
    };

    for var in ["ANDROID_HOME", "ANDROID_SDK_ROOT", "LOCALAPPDATA"] {
        if let Ok(val) = std::env::var(var) {
            let sdk = if var == "LOCALAPPDATA" {
                PathBuf::from(&val).join("Android").join("Sdk")
            } else {
                PathBuf::from(&val)
            };
            if let Some(bt) = find_latest_build_tools(&sdk) {
                if let Some(p) = check_bt(&bt) { return Some(p); }
            }
        }
    }
    for dir in SDK_SEARCH_DIRS {
        if let Some(bt) = find_latest_build_tools(Path::new(dir)) {
            if let Some(p) = check_bt(&bt) { return Some(p); }
        }
    }
    None
}

// ── Zipalign ────────────────────────────────────────────────────────────

fn zipalign(apk: &Path) -> Result<PathBuf> {
    let tool = find_build_tool("zipalign")
        .ok_or_else(|| anyhow::anyhow!("zipalign not found"))?;
    let aligned = apk.with_file_name(
        format!("{}-aligned.apk", apk.file_stem().unwrap_or_default().to_string_lossy())
    );
    let output = Command::new(&tool)
        .arg("-p").arg("-f").arg("4")
        .arg(apk).arg(&aligned)
        .output()
        .context("zipalign")?;
    if !output.status.success() {
        bail!("zipalign failed: {}", String::from_utf8_lossy(&output.stderr));
    }
    log::info!("Zipaligned → {}", aligned.display());
    Ok(aligned)
}

// ═══════════════════════════════════════════════════════════════════════
//  Main entry point — sign with best available tool, auto-download if needed
// ═══════════════════════════════════════════════════════════════════════

/// Sign the APK with the best available tool.
/// Zipaligns first if possible, then signs.
/// **Auto-downloads uber-apk-signer** if no signing tool is found.
pub fn sign_apk(apk: &Path) -> Result<PathBuf> {
    log::info!("Signing APK...");

    // Step 1: Zipalign (optional — uber-apk-signer does its own alignment)
    let aligned = match zipalign(apk) {
        Ok(a) => a,
        Err(e) => {
            log::warn!("zipalign not available ({}), continuing without it", e);
            apk.to_path_buf()
        }
    };

    // Step 2: Try signing tools in order of preference
    // 2a. apksigner (Android SDK — best, does v2/v3 signing)
    match try_apksigner(&aligned) {
        Ok(()) => return Ok(aligned),
        Err(e) => log::debug!("apksigner: {}", e),
    }

    // 2b. uber-apk-signer (standalone jar — does zipalign + v1/v2 signing)
    match try_uber_sign(&aligned) {
        Ok(out) => return Ok(out),
        Err(e) => log::debug!("uber-apk-signer: {}", e),
    }

    // 2c. jarsigner (JDK — v1 signing only, but works)
    match try_jarsigner(&aligned) {
        Ok(()) => return Ok(aligned),
        Err(e) => log::debug!("jarsigner: {}", e),
    }

    // Step 3: No signing tool found — auto-download uber-apk-signer and retry
    log::info!("No signing tool found — downloading uber-apk-signer...");
    match tools_setup::setup_uber_signer() {
        Ok(jar) => {
            log::info!("Downloaded uber-apk-signer, retrying...");
            match run_uber_sign(&jar, &aligned) {
                Ok(out) => return Ok(out),
                Err(e) => log::error!("uber-apk-signer failed after download: {}", e),
            }
        }
        Err(e) => {
            log::error!("Failed to download uber-apk-signer: {}", e);
        }
    }

    // Step 4: Last resort — try jarsigner with auto-created keystore
    // (user might have Java but no keytool in PATH — unlikely but possible)

    bail!(
        "Could not sign the APK. No signing tool available.\n\n\
         The installer tried to auto-download uber-apk-signer but it requires Java.\n\
         Please install Java from https://adoptium.net/ (Temurin JDK 17+)\n\
         Then restart the installer.\n\n\
         Alternatively, install one of:\n\
         • Android SDK Build-Tools (includes apksigner + zipalign)\n\
         • uber-apk-signer: https://github.com/patrickfav/uber-apk-signer\n\
         • JDK (includes jarsigner + keytool)"
    )
}

// ── uber-apk-signer ────────────────────────────────────────────────────

fn try_uber_sign(apk: &Path) -> Result<PathBuf> {
    // Use tools_setup to find uber-apk-signer (checks PATH + tools dir + common locations)
    let jar = tools_setup::find_uber_signer()
        .ok_or_else(|| anyhow::anyhow!("uber-apk-signer not found"))?;
    run_uber_sign(&jar, apk)
}

fn run_uber_sign(jar: &Path, apk: &Path) -> Result<PathBuf> {
    let out_dir = apk.parent().unwrap_or(Path::new("."));

    let output = Command::new("java")
        .arg("-jar").arg(jar)
        .arg("--apks").arg(apk)
        .arg("--out").arg(out_dir)
        .arg("--overwrite")
        .output()
        .context("uber-apk-signer")?;

    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        let stdout = String::from_utf8_lossy(&output.stdout);
        bail!("uber-apk-signer failed:\n{}\n{}", stdout, stderr);
    }

    // uber-apk-signer outputs to <name>-aligned-debugSigned.apk
    let stem = apk.file_stem().unwrap_or_default().to_string_lossy();
    let signed = out_dir.join(format!("{}-aligned-debugSigned.apk", stem));
    if signed.exists() {
        log::info!("Signed with uber-apk-signer → {}", signed.display());
        return Ok(signed);
    }

    // Sometimes it signs in-place
    if apk.exists() {
        log::info!("Signed with uber-apk-signer (in-place)");
        return Ok(apk.to_path_buf());
    }

    bail!("uber-apk-signer produced no output")
}

// ── apksigner (Android SDK) ────────────────────────────────────────────

fn try_apksigner(apk: &Path) -> Result<()> {
    let tool = find_build_tool("apksigner")
        .ok_or_else(|| anyhow::anyhow!("apksigner not found"))?;

    let ks = debug_keystore()?;

    let output = Command::new(&tool)
        .arg("sign")
        .arg("--ks").arg(&ks)
        .arg("--ks-pass").arg("pass:android")
        .arg("--ks-key-alias").arg("androiddebugkey")
        .arg("--key-pass").arg("pass:android")
        .arg(apk)
        .output()
        .context("apksigner")?;

    if !output.status.success() {
        bail!("apksigner failed: {}", String::from_utf8_lossy(&output.stderr));
    }
    log::info!("Signed with apksigner");
    Ok(())
}

// ── jarsigner ──────────────────────────────────────────────────────────

fn try_jarsigner(apk: &Path) -> Result<()> {
    let tool = which::which("jarsigner")
        .map_err(|_| anyhow::anyhow!("jarsigner not found"))?;
    let ks = debug_keystore()?;

    let output = Command::new(&tool)
        .arg("-keystore").arg(&ks)
        .arg("-storepass").arg("android")
        .arg("-keypass").arg("android")
        .arg("-sigalg").arg("SHA256withRSA")
        .arg("-digestalg").arg("SHA-256")
        .arg(apk)
        .arg("androiddebugkey")
        .output()
        .context("jarsigner")?;

    if !output.status.success() {
        bail!("jarsigner failed: {}", String::from_utf8_lossy(&output.stderr));
    }
    log::info!("Signed with jarsigner");
    Ok(())
}

// ── Debug keystore ─────────────────────────────────────────────────────

fn debug_keystore() -> Result<PathBuf> {
    let home = std::env::var("USERPROFILE")
        .or_else(|_| std::env::var("HOME"))
        .unwrap_or_else(|_| ".".to_string());
    let ks = PathBuf::from(&home).join(".android").join("debug.keystore");
    if ks.exists() { return Ok(ks); }

    // Create one with keytool
    log::info!("Creating debug keystore...");
    std::fs::create_dir_all(ks.parent().unwrap())?;
    let keytool = which::which("keytool")
        .context("keytool not found — install JDK from https://adoptium.net/")?;
    let output = Command::new(&keytool)
        .args([
            "-genkeypair", "-v",
            "-keystore", &ks.to_string_lossy(),
            "-alias", "androiddebugkey",
            "-keyalg", "RSA",
            "-keysize", "2048",
            "-validity", "10000",
            "-storepass", "android",
            "-keypass", "android",
            "-dname", "CN=Android Debug,O=Android,C=US",
        ])
        .output()
        .context("keytool")?;

    if !output.status.success() {
        bail!("keytool failed: {}", String::from_utf8_lossy(&output.stderr));
    }
    Ok(ks)
}
