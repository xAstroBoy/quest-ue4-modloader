// ═══════════════════════════════════════════════════════════════════════
//  CLI — interactive command-line mode
// ═══════════════════════════════════════════════════════════════════════

use crate::{adb, game_db, pipeline};
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

    // ── Find installed games ────────────────────────────────────────
    let packages: Vec<&str> = game_db::GAMES.iter().map(|g| g.package).collect();
    let installed = adb::find_installed_games(&device.serial, &packages)?;

    if installed.is_empty() {
        bail!("No supported games found on device.\nSupported: {}",
            game_db::GAMES.iter().map(|g| format!("{} ({})", g.name, g.package)).collect::<Vec<_>>().join(", "));
    }

    // ── Select game ─────────────────────────────────────────────────
    let game = if let Some(ref pkg) = package_override {
        let g = game_db::find_by_package(pkg)
            .ok_or_else(|| anyhow::anyhow!("Unknown package: {}", pkg))?;
        if !installed.iter().any(|a| a.package == pkg.as_str()) {
            bail!("{} is not installed on device", g.name);
        }
        g
    } else if installed.len() == 1 {
        game_db::find_by_package(&installed[0].package).unwrap()
    } else {
        let labels: Vec<String> = installed.iter().map(|a| {
            let g = game_db::find_by_package(&a.package).unwrap();
            format!("{} (v{})", g.name, a.version)
        }).collect();
        let sel = Select::new()
            .with_prompt("Select game to mod")
            .items(&labels)
            .default(0)
            .interact()?;
        game_db::find_by_package(&installed[sel].package).unwrap()
    };

    let app = installed.iter().find(|a| a.package == game.package).unwrap();
    println!("{} Game: {} (v{})", style("✓").green(), game.name, app.version);

    // ── Check if already modded ─────────────────────────────────────
    let already = adb::is_modloader_installed(&device.serial, game.package)?;
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
    let pb = ProgressBar::new(9);
    pb.set_style(ProgressStyle::with_template(
        "{spinner:.green} [{bar:30.cyan/dim}] {pos}/{len} {msg}"
    )?.tick_strings(&["⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏","✓"]));

    let pb2 = pb.clone();
    let progress: pipeline::ProgressFn = Box::new(move |step, _total, msg| {
        pb2.set_position(step as u64);
        pb2.set_message(msg.to_string());
    });

    pipeline::install(&device.serial, game, &so_path, Some(progress))?;
    pb.finish_with_message("Done!");

    println!();
    println!("{}", style("════════════════════════════════════════════").green());
    println!("{} {} modloader installed!", style("✅").green(), game.name);
    println!("{}", style("════════════════════════════════════════════").green());
    println!();
    println!("Next steps:");
    println!("  • Launch the game on the Quest");
    println!("  • On first run the modloader generates the Lua SDK");
    println!("  • Deploy mods:  python tools/deploy.py mods");
    println!();

    // Offer to launch
    if auto_yes {
        println!("Use: adb shell monkey -p {} -c android.intent.category.LAUNCHER 1", game.package);
    } else if Confirm::new().with_prompt("Launch game now?").default(false).interact()? {
        adb::launch(&device.serial, game.package)?;
        println!("{} Launched!", style("✓").green());
    }

    Ok(())
}
