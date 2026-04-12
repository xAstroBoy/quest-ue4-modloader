mod adb;
mod cli;
mod game_db;
mod gui;
mod pipeline;
mod signer;
mod smali;

use clap::Parser;

#[derive(Parser)]
#[command(name = "ue-modloader-installer")]
#[command(about = "UE Modloader Installer — Inject modloader into Quest APKs")]
struct Args {
    /// Run in CLI mode instead of GUI
    #[arg(long)]
    cli: bool,

    /// Target game package (e.g. com.Armature.VR4) — CLI only
    #[arg(long)]
    package: Option<String>,

    /// ADB device serial — CLI only
    #[arg(long, short)]
    serial: Option<String>,

    /// Path to prebuilt libmodloader.so — defaults to ./libmodloader.so
    #[arg(long)]
    modloader_so: Option<String>,

    /// Skip all confirmation prompts (auto-yes)
    #[arg(long, short = 'y')]
    yes: bool,
}

fn main() -> anyhow::Result<()> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info"))
        .format_timestamp(None)
        .init();

    let args = Args::parse();

    if args.cli {
        cli::run(args.serial, args.package, args.modloader_so, args.yes)?;
    } else {
        gui::run(args.modloader_so)?;
    }

    Ok(())
}
