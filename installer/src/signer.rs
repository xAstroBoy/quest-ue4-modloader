// ═══════════════════════════════════════════════════════════════════════
//  Signer — zipalign + sign APKs (apksigner / uber-apk-signer / jarsigner)
// ═══════════════════════════════════════════════════════════════════════

use anyhow::{bail, Context, Result};
use std::path::{Path, PathBuf};
use std::process::Command;

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
    // Build list of candidate filenames: name, name.exe, name.bat
    let exe = format!("{}.exe", name);
    let bat = format!("{}.bat", name);
    let candidates = [name, exe.as_str(), bat.as_str()];

    // 1. PATH (which handles .exe/.bat on Windows automatically)
    if let Ok(p) = which::which(name) { return Some(p); }
    if let Ok(p) = which::which(&bat) { return Some(p); }

    // Helper: check all candidate filenames in a build-tools dir
    let check_bt = |bt: &Path| -> Option<PathBuf> {
        for c in &candidates {
            let p = bt.join(c);
            if p.exists() { return Some(p); }
        }
        None
    };

    // 2. Env vars
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
    // 3. Hardcoded common paths
    for dir in SDK_SEARCH_DIRS {
        if let Some(bt) = find_latest_build_tools(Path::new(dir)) {
            if let Some(p) = check_bt(&bt) { return Some(p); }
        }
    }
    None
}

// ── Zipalign ────────────────────────────────────────────────────────────

/// Zipalign an APK (required for Android R+ / API 30+)
pub fn zipalign(apk: &Path) -> Result<PathBuf> {
    let tool = find_build_tool("zipalign")
        .ok_or_else(|| anyhow::anyhow!("zipalign not found"))?;
    let aligned = apk.with_file_name(
        format!("{}-aligned.apk", apk.file_stem().unwrap_or_default().to_string_lossy())
    );
    let output = Command::new(&tool)
        .arg("-p")       // page-align shared libs
        .arg("-f")       // overwrite output
        .arg("4")        // 4-byte alignment
        .arg(apk)
        .arg(&aligned)
        .output()
        .context("zipalign")?;
    if !output.status.success() {
        bail!("zipalign failed: {}", String::from_utf8_lossy(&output.stderr));
    }
    log::info!("Zipaligned → {}", aligned.display());
    Ok(aligned)
}

/// Sign the APK with the best available tool.
/// Zipaligns first, then signs.
pub fn sign_apk(apk: &Path) -> Result<PathBuf> {
    log::info!("Signing APK...");

    // Step 1: Zipalign (required for Android R+)
    let aligned = match zipalign(apk) {
        Ok(a) => a,
        Err(e) => {
            log::warn!("zipalign not available ({}), continuing without it", e);
            apk.to_path_buf()
        }
    };

    // Step 2: Sign (prefer apksigner for v2 signing, then uber, then jarsigner)
    if let Ok(()) = try_apksigner(&aligned) {
        return Ok(aligned);
    }
    if let Ok(out) = try_uber_sign(&aligned) {
        return Ok(out);
    }
    if let Ok(()) = try_jarsigner(&aligned) {
        return Ok(aligned);
    }

    bail!(
        "No signing tool found. Install one of:\n\
         1. apksigner (Android SDK Build-Tools)\n\
         2. uber-apk-signer: https://github.com/nicehash/uber-apk-signer\n\
         3. jarsigner (JDK)"
    )
}

// ── uber-apk-signer ────────────────────────────────────────────────────

fn find_uber_signer() -> Option<PathBuf> {
    // Check PATH
    if let Ok(p) = which::which("uber-apk-signer") { return Some(p); }
    // Check common jar locations
    for dir in [r"C:\tools", r"C:\apktool", r".", r"tools"] {
        for name in ["uber-apk-signer.jar", "uber-apk-signer-1.3.0.jar"] {
            let p = PathBuf::from(dir).join(name);
            if p.exists() { return Some(p); }
        }
    }
    None
}

fn try_uber_sign(apk: &Path) -> Result<PathBuf> {
    let jar = find_uber_signer().ok_or_else(|| anyhow::anyhow!("uber-apk-signer not found"))?;
    let out_dir = apk.parent().unwrap_or(Path::new("."));

    let output = Command::new("java")
        .arg("-jar").arg(&jar)
        .arg("--apks").arg(apk)
        .arg("--out").arg(out_dir)
        .arg("--overwrite")
        .output()
        .context("uber-apk-signer")?;

    if !output.status.success() {
        bail!("uber-apk-signer failed: {}", String::from_utf8_lossy(&output.stderr));
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

    // Generate a debug keystore if needed
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
    let tool = which::which("jarsigner").map_err(|_| anyhow::anyhow!("jarsigner not found"))?;
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
    // Use Android's default debug keystore
    let home = std::env::var("USERPROFILE")
        .or_else(|_| std::env::var("HOME"))
        .unwrap_or_else(|_| ".".to_string());
    let ks = PathBuf::from(&home).join(".android").join("debug.keystore");
    if ks.exists() { return Ok(ks); }

    // Create one with keytool
    log::info!("Creating debug keystore...");
    std::fs::create_dir_all(ks.parent().unwrap())?;
    let keytool = which::which("keytool").context("keytool not found — install JDK")?;
    let output = Command::new(&keytool)
        .args([
            "-genkeypair",
            "-v",
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
