// ═══════════════════════════════════════════════════════════════════════
//  Smali — apktool decompile/recompile, inject System.loadLibrary
// ═══════════════════════════════════════════════════════════════════════

use anyhow::{bail, Context, Result};
use regex::Regex;
use std::path::{Path, PathBuf};
use std::process::Command;

use crate::game_db;
use crate::tools_setup;

// ── Find apktool ────────────────────────────────────────────────────────

pub fn find_apktool() -> Result<PathBuf> {
    // Use tools_setup which has auto-download capability
    if let Some(p) = tools_setup::find_apktool() {
        return Ok(p);
    }
    bail!("apktool not found. The installer can download it automatically — use the GUI or run with --setup-tools")
}

fn run_apktool(args: &[&str]) -> Result<String> {
    let tool = find_apktool()?;
    let ext = tool.extension().unwrap_or_default();
    let output = if ext == "jar" {
        Command::new("java").arg("-jar").arg(&tool).args(args).output()
    } else {
        Command::new(&tool).args(args).output()
    }.with_context(|| format!("apktool {}", args.join(" ")))?;

    let stdout = String::from_utf8_lossy(&output.stdout).to_string();
    let stderr = String::from_utf8_lossy(&output.stderr).to_string();
    if !output.status.success() {
        bail!("apktool failed:\n{}\n{}", stdout, stderr);
    }
    Ok(format!("{}\n{}", stdout, stderr))
}

// ── Decompile ───────────────────────────────────────────────────────────

pub fn decompile(apk: &Path, out_dir: &Path) -> Result<PathBuf> {
    log::info!("Decompiling APK with apktool...");
    let out_str = out_dir.to_string_lossy().to_string();
    run_apktool(&["d", "-f", "-o", &out_str, &apk.to_string_lossy()])?;
    if !out_dir.join("AndroidManifest.xml").exists() {
        bail!("Decompile failed — no AndroidManifest.xml in {}", out_dir.display());
    }
    log::info!("Decompiled to {}", out_dir.display());
    Ok(out_dir.to_path_buf())
}

// ── Find smali class file ───────────────────────────────────────────────

fn find_smali_file(decompiled: &Path, class_path: &str) -> Result<PathBuf> {
    // Try smali, smali_classes2, smali_classes3...
    for i in 0..20 {
        let dir = if i == 0 {
            decompiled.join("smali")
        } else {
            decompiled.join(format!("smali_classes{}", i + 1))
        };
        let file = dir.join(format!("{}.smali", class_path));
        if file.exists() {
            return Ok(file);
        }
    }
    bail!("Smali class not found: {}.smali\nSearched in smali/ through smali_classes20/", class_path)
}

// ── Auto-detect main activity from AndroidManifest.xml ──────────────────

/// Parse the decompiled AndroidManifest.xml to find the main launcher Activity.
/// Returns the smali-style class path (e.g. "com/epicgames/ue4/GameActivity").
pub fn detect_main_activity(decompiled: &Path) -> Result<String> {
    let manifest = decompiled.join("AndroidManifest.xml");
    if !manifest.exists() {
        bail!("AndroidManifest.xml not found in {}", decompiled.display());
    }
    let content = std::fs::read_to_string(&manifest)?;

    // Strategy 1: Find activity with MAIN intent filter
    // The decompiled XML has <activity android:name="..."> with nested <intent-filter>
    // containing <action android:name="android.intent.action.MAIN"/>
    let re_activity = Regex::new(
        r#"(?s)<activity[^>]*android:name="([^"]+)"[^>]*>.*?</activity>"#
    ).unwrap();

    for cap in re_activity.captures_iter(&content) {
        let block = cap.get(0).unwrap().as_str();
        let name = cap.get(1).unwrap().as_str();
        if block.contains("android.intent.action.MAIN") {
            let class_path = activity_name_to_smali(name, &content);
            log::info!("Auto-detected main activity from manifest: {}", class_path);
            return Ok(class_path);
        }
    }

    // Strategy 2: Find any activity with VR category (Quest games)
    for cap in re_activity.captures_iter(&content) {
        let block = cap.get(0).unwrap().as_str();
        let name = cap.get(1).unwrap().as_str();
        if block.contains("com.oculus.intent.category.VR") {
            let class_path = activity_name_to_smali(name, &content);
            log::info!("Auto-detected VR activity from manifest: {}", class_path);
            return Ok(class_path);
        }
    }

    // Strategy 3: Find activity whose name contains "GameActivity" or "MainActivity"
    for cap in re_activity.captures_iter(&content) {
        let name = cap.get(1).unwrap().as_str();
        if name.contains("GameActivity") || name.contains("MainActivity") || name.contains("SplashActivity") {
            let class_path = activity_name_to_smali(name, &content);
            log::info!("Auto-detected activity by name pattern: {}", class_path);
            return Ok(class_path);
        }
    }

    bail!("Could not detect main activity from AndroidManifest.xml")
}

/// Convert an Android activity name to a smali-style class path.
/// Handles both fully-qualified ("com.epicgames.ue4.GameActivity") and
/// shorthand (".GameActivity") forms.
fn activity_name_to_smali(name: &str, manifest_content: &str) -> String {
    let full_name = if name.starts_with('.') {
        // Shorthand: prepend the package name
        let re_pkg = Regex::new(r#"package="([^"]+)""#).unwrap();
        if let Some(cap) = re_pkg.captures(manifest_content) {
            format!("{}{}", cap.get(1).unwrap().as_str(), name)
        } else {
            name.to_string()
        }
    } else {
        name.to_string()
    };
    // Convert dots to slashes for smali path
    full_name.replace('.', "/")
}

/// Try to find a working smali injection target. Tries in order:
/// 1. Game profile's primary target
/// 2. Game profile's fallback targets
/// 3. Auto-detected from AndroidManifest.xml
/// 4. Common UE4/UE5 activities
///
/// Returns the smali class path that was found.
pub fn find_injection_target(decompiled: &Path, game: Option<&game_db::GameProfile>) -> Result<String> {
    let mut tried = Vec::new();

    // 1. Try game profile's primary target
    if let Some(g) = game {
        if find_smali_file(decompiled, g.smali_target).is_ok() {
            log::info!("Using game profile smali target: {}", g.smali_target);
            return Ok(g.smali_target.to_string());
        }
        tried.push(g.smali_target.to_string());

        // 2. Try game profile's fallbacks
        for fb in g.smali_fallbacks {
            if find_smali_file(decompiled, fb).is_ok() {
                log::info!("Using fallback smali target: {}", fb);
                return Ok(fb.to_string());
            }
            tried.push(fb.to_string());
        }
    }

    // 3. Auto-detect from AndroidManifest.xml
    if let Ok(detected) = detect_main_activity(decompiled) {
        if find_smali_file(decompiled, &detected).is_ok() {
            log::info!("Using manifest-detected smali target: {}", detected);
            return Ok(detected);
        }
        tried.push(detected);
    }

    // 4. Try common UE4/UE5 activity classes
    for common in game_db::UE_COMMON_ACTIVITIES {
        if !tried.iter().any(|t| t == *common) {
            if find_smali_file(decompiled, common).is_ok() {
                log::info!("Using common UE activity: {}", common);
                return Ok(common.to_string());
            }
            tried.push(common.to_string());
        }
    }

    // 5. Last resort: scan all smali dirs for any GameActivity
    if let Some(found) = scan_for_game_activity(decompiled) {
        log::info!("Found GameActivity via filesystem scan: {}", found);
        return Ok(found);
    }

    bail!(
        "Could not find smali injection target.\n\
         Tried: {}\n\
         The APK may use a non-standard activity class.",
        tried.join(", ")
    )
}

/// Scan all smali directories for any file named *GameActivity.smali or *Activity.smali
/// that contains an onCreate method.
fn scan_for_game_activity(decompiled: &Path) -> Option<String> {
    for i in 0..20 {
        let dir = if i == 0 {
            decompiled.join("smali")
        } else {
            decompiled.join(format!("smali_classes{}", i + 1))
        };
        if !dir.exists() { continue; }

        // Look for GameActivity.smali files
        if let Some(found) = walk_for_activity(&dir, &dir, "GameActivity.smali") {
            return Some(found);
        }
    }

    // Broader: look for any Activity with onCreate
    for i in 0..20 {
        let dir = if i == 0 {
            decompiled.join("smali")
        } else {
            decompiled.join(format!("smali_classes{}", i + 1))
        };
        if !dir.exists() { continue; }

        if let Some(found) = walk_for_activity(&dir, &dir, "Activity.smali") {
            // Verify it has onCreate
            let smali_path = dir.join(format!("{}.smali", found));
            if let Ok(content) = std::fs::read_to_string(&smali_path) {
                if content.contains("onCreate(Landroid/os/Bundle;)V") {
                    return Some(found);
                }
            }
        }
    }

    None
}

/// Walk a directory tree looking for a file ending with the given suffix.
/// Returns the smali-style class path relative to the smali root.
fn walk_for_activity(root: &Path, dir: &Path, suffix: &str) -> Option<String> {
    let entries = std::fs::read_dir(dir).ok()?;
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            if let Some(found) = walk_for_activity(root, &path, suffix) {
                return Some(found);
            }
        } else if let Some(name) = path.file_name().and_then(|n| n.to_str()) {
            if name.ends_with(suffix) {
                // Convert filesystem path to smali class path
                let rel = path.strip_prefix(root).ok()?;
                let class_path = rel.to_string_lossy()
                    .replace('\\', "/")
                    .trim_end_matches(".smali")
                    .to_string();
                return Some(class_path);
            }
        }
    }
    None
}

// ── Inject loadLibrary ──────────────────────────────────────────────────

pub fn inject_loadlibrary(decompiled: &Path, smali_class: &str) -> Result<()> {
    let file = find_smali_file(decompiled, smali_class)?;
    log::info!("Injecting into {}", file.display());

    let content = std::fs::read_to_string(&file)?;

    // Check if already injected
    if content.contains("\"modloader\"") {
        log::info!("Already injected — skipping");
        return Ok(());
    }

    // Find onCreate method and inject after invoke-super
    let inject_code = r#"
    const-string v0, "modloader"
    invoke-static {v0}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V
"#;

    // Strategy 1: inject after .method ... onCreate, after invoke-super
    // (?s) = dotall so .*? crosses the ~70 lines between .method and invoke-super
    let re_oncreate = Regex::new(r"(?ms)(\.method[^\n]*onCreate\(Landroid/os/Bundle;\)V.*?invoke-super[^\n]*onCreate[^\n]*\n)")
        .unwrap();

    let new_content = if let Some(m) = re_oncreate.find(&content) {
        let end = m.end();
        format!("{}{}{}", &content[..end], inject_code, &content[end..])
    } else {
        // Strategy 2: just after .method ... onCreate and .locals
        let re_simple = Regex::new(r"(?m)(\.method[^\n]*onCreate\(Landroid/os/Bundle;\)V\n(?:\s*\.locals\s+\d+\n)?)")
            .unwrap();
        if let Some(m) = re_simple.find(&content) {
            let end = m.end();
            format!("{}{}{}", &content[..end], inject_code, &content[end..])
        } else {
            bail!("Cannot find onCreate method in {}", file.display());
        }
    };

    // Make sure .locals is high enough (need at least 1 register for v0)
    let new_content = bump_locals(&new_content);

    std::fs::write(&file, new_content)?;
    log::info!("Injected System.loadLibrary(\"modloader\") into onCreate");
    Ok(())
}

/// Ensure .locals count in onCreate is at least 1
fn bump_locals(content: &str) -> String {
    let re = Regex::new(r"(?m)(\.method[^\n]*onCreate\(Landroid/os/Bundle;\)V\n\s*\.locals\s+)(\d+)")
        .unwrap();
    re.replace(content, |caps: &regex::Captures| {
        let prefix = &caps[1];
        let n: u32 = caps[2].parse().unwrap_or(0);
        if n < 1 {
            format!("{}1", prefix)
        } else {
            format!("{}{}", prefix, n)
        }
    }).to_string()
}

// ── Add libmodloader.so into APK ────────────────────────────────────────

pub fn add_native_lib(decompiled: &Path, so_path: &Path) -> Result<()> {
    let dest = decompiled.join("lib").join("arm64-v8a");
    std::fs::create_dir_all(&dest)?;
    let target = dest.join("libmodloader.so");
    std::fs::copy(so_path, &target)?;
    log::info!("Added libmodloader.so to lib/arm64-v8a/");
    Ok(())
}

// ── Fix manifest: extractNativeLibs, debuggable, allowBackup ────────────

pub fn fix_manifest(decompiled: &Path) -> Result<()> {
    let manifest = decompiled.join("AndroidManifest.xml");
    let content = std::fs::read_to_string(&manifest)?;

    let mut new_content = content.clone();
    let mut changes = Vec::new();

    // ── extractNativeLibs="true" — required for native .so loading ──
    if new_content.contains("android:extractNativeLibs=\"false\"") {
        new_content = new_content.replace(
            "android:extractNativeLibs=\"false\"",
            "android:extractNativeLibs=\"true\"",
        );
        changes.push("extractNativeLibs → true");
    } else if !new_content.contains("android:extractNativeLibs") {
        new_content = new_content.replace(
            "<application ",
            "<application android:extractNativeLibs=\"true\" ",
        );
        changes.push("added extractNativeLibs=\"true\"");
    }

    // ── debuggable="true" — enables ADB debugging, logcat, etc. ──
    if new_content.contains("android:debuggable=\"false\"") {
        new_content = new_content.replace(
            "android:debuggable=\"false\"",
            "android:debuggable=\"true\"",
        );
        changes.push("debuggable → true");
    } else if !new_content.contains("android:debuggable") {
        new_content = new_content.replace(
            "<application ",
            "<application android:debuggable=\"true\" ",
        );
        changes.push("added debuggable=\"true\"");
    }

    // ── allowBackup="true" — enables ADB backup, data persistence ──
    if new_content.contains("android:allowBackup=\"false\"") {
        new_content = new_content.replace(
            "android:allowBackup=\"false\"",
            "android:allowBackup=\"true\"",
        );
        changes.push("allowBackup → true");
    } else if !new_content.contains("android:allowBackup") {
        new_content = new_content.replace(
            "<application ",
            "<application android:allowBackup=\"true\" ",
        );
        changes.push("added allowBackup=\"true\"");
    }

    if new_content != content {
        std::fs::write(&manifest, &new_content)?;
        log::info!("Manifest fixes: {}", changes.join(", "));
    } else {
        log::info!("Manifest already has required attributes");
    }
    Ok(())
}

// ── Recompile ───────────────────────────────────────────────────────────

pub fn recompile(decompiled: &Path, out_apk: &Path) -> Result<()> {
    log::info!("Recompiling APK...");
    let out_str = out_apk.to_string_lossy().to_string();
    run_apktool(&["b", "-o", &out_str, &decompiled.to_string_lossy()])?;
    if !out_apk.exists() {
        bail!("Recompile failed — {} not created", out_apk.display());
    }
    log::info!("Recompiled to {}", out_apk.display());
    Ok(())
}
