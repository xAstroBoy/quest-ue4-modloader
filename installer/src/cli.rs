// ═══════════════════════════════════════════════════════════════════════
//  CLI — interactive command-line mode
// ═══════════════════════════════════════════════════════════════════════

use crate::{adb, game_db, pipeline, tools_setup};
use anyhow::{bail, Result};
use console::style;
use dialoguer::{Select, Confirm};
use indicatif::{ProgressBar, ProgressStyle};

pub fn run(
    serial_override: Option<String>,
    package_override: Option<String>,
    so_override: Option<String>,
    auto_yes: bool,
) -> Result<()> {
    println!();
    println!("{}", style("╔══════════════════════════════════════════╗").cyan());
    println!("{}", style("║    UE Modloader Installer (CLI)         ║").cyan());
    println!("{}", style("╚══════════════════════════════════════════╝").cyan());
    println!();

    // ── Auto-setup required tools ───────────────────────────────────
    println!("{}", style("Checking required tools...").dim());
    let (_adb, _apktool) = tools_setup::ensure_tools()?;
    println!("{} Tools ready", style("✓").green());

    // ── Find modloader .so ──────────────────────────────────────────
    let so_path = pipeline::find_modloader_so(so_override.as_deref())?;
    println!("{} Found libmodloader.so: {}", style("✓").green(), so_path.display());

    // ── Detect device ───────────────────────────────────────────────
    println!("\n{}", style("Detecting devices...").dim());
    let devices = adb::list_devices()?;
    if devices.is_empty() {
        bail!("No devices connected. Plug in your Quest and enable ADB.");
    }

    let device = if let Some(ref s) = serial_override {
        devices.iter().find(|d| d.serial == *s)
            .ok_or_else(|| anyhow::anyhow!("Device {} not found", s))?
            .clone()
    } else if devices.len() == 1 {
        devices[0].clone()
    } else {
        let labels: Vec<String> = devices.iter()
            .map(|d| format!("{} ({}){}", d.model, d.serial,
                if d.is_quest { " [Quest]" } else { "" }))
            .collect();
        let sel = Select::new()
            .with_prompt("Select device")
            .items(&labels)
            .default(0)
            .interact()?;
        devices[sel].clone()
    };

    println!("{} Device: {} ({}){}", style("✓").green(),
        device.model, device.serial,
        if device.is_quest { " [Quest]" } else { "" });

    // ── Find installed APKs ─────────────────────────────────────────
    let installed = adb::list_installed_apps(&device.serial)?;

    if installed.is_empty() {
        bail!("No installed APK packages found on device.");
    }

    // ── Select package ──────────────────────────────────────────────
    let selected_package = if let Some(ref pkg) = package_override {
        if !installed.iter().any(|a| a.package == pkg.as_str()) {
            bail!("Package is not installed on device: {}", pkg);
        }
        pkg.clone()
    } else if installed.len() == 1 {
        installed[0].package.clone()
    } else {
        let labels: Vec<String> = installed.iter().map(|a| {
            if let Some(g) = game_db::find_by_package(&a.package) {
                format!("{} ({})", g.name, a.package)
            } else {
                a.package.clone()
            }
        }).collect();
        let sel = Select::new()
            .with_prompt("Select package to mod")
            .items(&labels)
            .default(0)
            .interact()?;
        installed[sel].package.clone()
    };

    // Refresh details for chosen package (version/apk path)
    let app = adb::get_installed_app(&device.serial, &selected_package)?
        .ok_or_else(|| anyhow::anyhow!("{} is not installed on device", selected_package))?;

    let app_name = game_db::find_by_package(&selected_package)
        .map(|g| g.name.to_string())
        .unwrap_or_else(|| selected_package.clone());

    println!("{} Target: {} (v{})", style("✓").green(), app_name, app.version);

    // ── Check if already modded ─────────────────────────────────────
    let already = adb::is_modloader_installed(&device.serial, &selected_package)?;
    if already {
        println!("{} Modloader is already installed!", style("⚠").yellow());
        if !auto_yes {
            if !Confirm::new().with_prompt("Reinstall anyway?").default(false).interact()? {
                println!("Aborted.");
                return Ok(());
            }
        } else {
            println!("  --yes: reinstalling.");
        }
    }

    // ── Confirm ─────────────────────────────────────────────────────
    println!();
    println!("{}", style("This will:").bold());
    println!("  1. Pull the APK from the device");
    println!("  2. Inject the modloader into it");
    println!("  3. Sign the patched APK");
    println!("  4. Backup your game data (OBB + saves)");
    println!("  5. Uninstall → Install patched → Restore data");
    println!();

    if !auto_yes {
        if !Confirm::new().with_prompt("Proceed?").default(true).interact()? {
            println!("Aborted.");
            return Ok(());
        }
    }

    // ── Run pipeline ────────────────────────────────────────────────
    println!();
    let pb = ProgressBar::new(10);
    pb.set_style(ProgressStyle::with_template(
        "{spinner:.green} [{bar:30.cyan/dim}] {pos}/{len} {msg}"
    )?.tick_strings(&["⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏","✓"]));

    let pb2 = pb.clone();
    let progress: pipeline::ProgressFn = Box::new(move |step, _total, msg| {
        pb2.set_position(step as u64);
        pb2.set_message(msg.to_string());
    });

    pipeline::install(&device.serial, &selected_package, &app_name, &so_path, Some(progress))?;
    pb.finish_with_message("Done!");

    println!();
    println!("{}", style("════════════════════════════════════════════").green());
    println!("{} {} modloader installed!", style("✅").green(), app_name);
    println!("{}", style("════════════════════════════════════════════").green());
    println!();
    println!("Next steps:");
    println!("  • Launch the game on the Quest");
    println!("  • On first run the modloader generates the Lua SDK");
    println!("  • Deploy mods:  python tools/deploy.py mods");
    println!();

    // Offer to launch
    if auto_yes {
        println!("Use: adb shell monkey -p {} -c android.intent.category.LAUNCHER 1", selected_package);
    } else if Confirm::new().with_prompt("Launch game now?").default(false).interact()? {
        adb::launch(&device.serial, &selected_package)?;
        println!("{} Launched!", style("✓").green());
    }

    Ok(())
}
