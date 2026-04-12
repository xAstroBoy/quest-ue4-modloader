// ═══════════════════════════════════════════════════════════════════════
//  Smali — apktool decompile/recompile, inject System.loadLibrary
// ═══════════════════════════════════════════════════════════════════════

use anyhow::{bail, Context, Result};
use regex::Regex;
use std::path::{Path, PathBuf};
use std::process::Command;

// ── Find apktool ────────────────────────────────────────────────────────

pub fn find_apktool() -> Result<PathBuf> {
    // Try apktool on PATH
    if let Ok(p) = which::which("apktool") { return Ok(p); }
    if let Ok(p) = which::which("apktool.bat") { return Ok(p); }
    // Common locations
    for dir in [r"C:\apktool", r"C:\tools\apktool"] {
        let bat = PathBuf::from(dir).join("apktool.bat");
        let jar = PathBuf::from(dir).join("apktool.jar");
        if bat.exists() { return Ok(bat); }
        if jar.exists() { return Ok(jar); }
    }
    bail!("apktool not found. Install it: https://apktool.org/docs/install")
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
    for i in 0..10 {
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
    bail!("Smali class not found: {}.smali\nSearched in smali/ through smali_classes10/", class_path)
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

// ── Fix manifest: extractNativeLibs ─────────────────────────────────────

pub fn fix_manifest(decompiled: &Path) -> Result<()> {
    let manifest = decompiled.join("AndroidManifest.xml");
    let content = std::fs::read_to_string(&manifest)?;

    let mut new_content = content.clone();

    // Ensure extractNativeLibs="true" in <application>
    if new_content.contains("android:extractNativeLibs=\"false\"") {
        new_content = new_content.replace(
            "android:extractNativeLibs=\"false\"",
            "android:extractNativeLibs=\"true\"",
        );
        log::info!("Fixed extractNativeLibs → true");
    } else if !new_content.contains("android:extractNativeLibs") {
        // Add it to <application> tag
        new_content = new_content.replace(
            "<application ",
            "<application android:extractNativeLibs=\"true\" ",
        );
        log::info!("Added extractNativeLibs=\"true\" to manifest");
    }

    if new_content != content {
        std::fs::write(&manifest, new_content)?;
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
