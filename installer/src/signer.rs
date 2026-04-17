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
    // 2a. apksigner (Android SDK — best, does v1+v2+v3 signing)
    match try_apksigner(&aligned) {
        Ok(()) => return Ok(aligned),
        Err(e) => log::debug!("apksigner: {}", e),
    }

    // 2b. uber-apk-signer (standalone jar — does zipalign + v1+v2 signing)
    match try_uber_sign(&aligned) {
        Ok(out) => return Ok(out),
        Err(e) => log::debug!("uber-apk-signer: {}", e),
    }

    // 2c. Auto-download uber-apk-signer and try
    log::info!("No local signing tool found — downloading uber-apk-signer...");
    match tools_setup::setup_uber_signer() {
        Ok(jar) => {
            log::info!("Downloaded uber-apk-signer, retrying...");
            match run_uber_sign(&jar, &aligned) {
                Ok(out) => return Ok(out),
                Err(e) => log::error!("uber-apk-signer failed after download: {}", e),
            }
        }
        Err(e) => {
            log::warn!("Failed to download uber-apk-signer: {}", e);
        }
    }

    // 2d. jarsigner (JDK — v1 signing only)
    // NOTE: jarsigner only does v1. Quest needs v2+. This is a last resort
    // and may fail verification, but it's better than no signature at all.
    match try_jarsigner(&aligned) {
        Ok(()) => {
            log::warn!(
                "Signed with jarsigner (v1 only). Quest requires v2+ signing. \
                 The APK may fail to install. Install Android SDK Build-Tools \
                 or Java (for uber-apk-signer) for proper v2 signing."
            );
            return Ok(aligned);
        }
        Err(e) => log::debug!("jarsigner: {}", e),
    }

    bail!(
        "Could not sign the APK. No signing tool available.\n\n\
         Please install ONE of the following:\n\
         • Java (JDK 17+) from https://adoptium.net/ — enables uber-apk-signer auto-download\n\
         • Android SDK Build-Tools — includes apksigner + zipalign\n\n\
         Then restart the installer."
    )
}

/// Verify that an APK is properly signed before install.
/// Tries apksigner first, falls back to uber-apk-signer --onlyVerify,
/// and if neither is available, logs a warning and proceeds (best-effort).
pub fn verify_apk_full_signature(apk: &Path) -> Result<()> {
    // Try 1: apksigner verify (best — gives per-scheme breakdown)
    if let Some(tool) = find_build_tool("apksigner") {
        return verify_with_apksigner(&tool, apk);
    }

    // Try 2: uber-apk-signer --onlyVerify
    if let Some(jar) = crate::tools_setup::find_uber_signer() {
        return verify_with_uber(&jar, apk);
    }

    // Try 3: download uber-apk-signer and verify
    if let Ok(jar) = crate::tools_setup::setup_uber_signer() {
        return verify_with_uber(&jar, apk);
    }

    // No verification tool available — warn but don't block.
    // The APK was just signed by us seconds ago, so it should be valid.
    log::warn!(
        "No APK verification tool available (apksigner / uber-apk-signer). \
         Skipping signature verification. The APK was signed moments ago and \
         should be valid."
    );
    Ok(())
}

/// Verify using Android SDK apksigner
fn verify_with_apksigner(tool: &Path, apk: &Path) -> Result<()> {
    let output = Command::new(tool)
        .arg("verify")
        .arg("--verbose")
        .arg("--print-certs")
        .arg(apk)
        .output()
        .context("apksigner verify")?;

    let stdout = String::from_utf8_lossy(&output.stdout);
    let stderr = String::from_utf8_lossy(&output.stderr);
    let combined = format!("{}\n{}", stdout, stderr);

    if !output.status.success() {
        // apksigner verify returning non-zero means the APK is genuinely broken.
        bail!("apksigner verify failed:\n{}", combined);
    }

    let mut v1_ok = false;
    let mut v2_ok = false;
    let mut v3_ok = false;
    for raw in combined.lines() {
        let line = raw.trim().to_ascii_lowercase();
        if line.contains("verified using v1 scheme") {
            v1_ok = line.contains(": true");
        }
        if line.contains("verified using v2 scheme") {
            v2_ok = line.contains(": true");
        }
        if line.contains("verified using v3 scheme") || line.contains("verified using v3.1 scheme") {
            if line.contains(": true") { v3_ok = true; }
        }
    }

    // Quest runs Android 12+ — needs v2 or v3.
    if !v2_ok && !v3_ok {
        bail!(
            "Signature verification failed (required: v2 or v3 = true).\n\
             v1={} v2={} v3={}\n\
             apksigner output:\n{}",
            v1_ok, v2_ok, v3_ok, combined
        );
    }

    log::info!("Signature verified (v1={} v2={} v3={}): {}", v1_ok, v2_ok, v3_ok, apk.display());
    Ok(())
}

/// Verify using uber-apk-signer --onlyVerify
fn verify_with_uber(jar: &Path, apk: &Path) -> Result<()> {
    let output = Command::new("java")
        .arg("-jar").arg(jar)
        .arg("--onlyVerify")
        .arg("--apks").arg(apk)
        .output()
        .context("uber-apk-signer verify")?;

    let stdout = String::from_utf8_lossy(&output.stdout);
    let stderr = String::from_utf8_lossy(&output.stderr);

    if !output.status.success() {
        bail!(
            "uber-apk-signer verification failed:\n{}\n{}",
            stdout, stderr
        );
    }

    log::info!("Signature verified via uber-apk-signer: {}", apk.display());
    Ok(())
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

    // Common output patterns from uber-apk-signer:
    //   <name>-aligned-debugSigned.apk
    //   <name>-debugSigned.apk
    // Also search directory in case naming differs across versions.
    let stem = apk.file_stem().unwrap_or_default().to_string_lossy();

    let direct_candidates = [
        out_dir.join(format!("{}-aligned-debugSigned.apk", stem)),
        out_dir.join(format!("{}-debugSigned.apk", stem)),
        out_dir.join(format!("{}-signed.apk", stem)),
    ];
    for signed in &direct_candidates {
        if signed.exists() {
            log::info!("Signed with uber-apk-signer → {}", signed.display());
            return Ok(signed.clone());
        }
    }

    // Fallback: pick newest *signed*.apk that starts with the input stem.
    let mut scanned: Vec<(std::time::SystemTime, PathBuf)> = Vec::new();
    if let Ok(rd) = std::fs::read_dir(out_dir) {
        for entry in rd.flatten() {
            let p = entry.path();
            if p.extension().and_then(|e| e.to_str()).unwrap_or_default().to_ascii_lowercase() != "apk" {
                continue;
            }
            let name = p.file_name().and_then(|n| n.to_str()).unwrap_or_default();
            if !name.starts_with(&stem.to_string()) {
                continue;
            }
            let lname = name.to_ascii_lowercase();
            if !(lname.contains("debugsigned") || lname.contains("signed")) {
                continue;
            }
            let modified = std::fs::metadata(&p)
                .and_then(|m| m.modified())
                .unwrap_or(std::time::SystemTime::UNIX_EPOCH);
            scanned.push((modified, p));
        }
    }
    scanned.sort_by(|a, b| b.0.cmp(&a.0));
    if let Some((_, signed)) = scanned.into_iter().next() {
        log::info!("Signed with uber-apk-signer → {}", signed.display());
        return Ok(signed);
    }

    // IMPORTANT: never return the input path here. If we can't locate a signed
    // output, returning the original APK can cause INSTALL_PARSE_FAILED_NO_CERTIFICATES.
    let stderr = String::from_utf8_lossy(&output.stderr);
    let stdout = String::from_utf8_lossy(&output.stdout);
    bail!(
        "uber-apk-signer completed but no signed APK was found in {}.\nstdout:\n{}\nstderr:\n{}",
        out_dir.display(),
        stdout,
        stderr
    )
}

// ── apksigner (Android SDK) ────────────────────────────────────────────

fn try_apksigner(apk: &Path) -> Result<()> {
    let tool = find_build_tool("apksigner")
        .ok_or_else(|| anyhow::anyhow!("apksigner not found"))?;

    let ks = debug_keystore()?;

    let output = Command::new(&tool)
        .arg("sign")
        .arg("--v1-signing-enabled").arg("true")
        .arg("--v2-signing-enabled").arg("true")
        .arg("--v3-signing-enabled").arg("true")
        .arg("--v4-signing-enabled").arg("false")
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
    log::info!("Signed with apksigner (v1+v2+v3)");
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
