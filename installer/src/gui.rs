// ═══════════════════════════════════════════════════════════════════════
//  GUI — simple egui wizard: detect → select → install → done
// ═══════════════════════════════════════════════════════════════════════

use crate::{adb, game_db, pipeline};
use eframe::egui;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::thread;

pub fn run(so_override: Option<String>) -> anyhow::Result<()> {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([520.0, 480.0])
            .with_min_inner_size([520.0, 480.0])
            .with_title("UE Modloader Installer"),
        ..Default::default()
    };
    eframe::run_native(
        "UE Modloader Installer",
        options,
        Box::new(move |_cc| Ok(Box::new(App::new(so_override)))),
    ).map_err(|e| anyhow::anyhow!("{}", e))
}

// ── State ───────────────────────────────────────────────────────────────

#[derive(Clone, PartialEq)]
enum Phase {
    Detecting,
    Ready,
    Installing,
    Done,
    Error,
}

struct App {
    phase: Phase,
    so_override: Option<String>,
    so_path: Option<PathBuf>,
    so_error: Option<String>,

    // Device
    devices: Vec<adb::Device>,
    selected_device: usize,
    device_error: Option<String>,

    // Games
    installed_games: Vec<(game_db::GameProfile, adb::InstalledApp)>,
    selected_game: usize,
    already_modded: bool,

    // Install progress
    progress: Arc<Mutex<ProgressState>>,
}

#[derive(Clone)]
struct ProgressState {
    step: u32,
    total: u32,
    message: String,
    done: bool,
    error: Option<String>,
}

impl App {
    fn new(so_override: Option<String>) -> Self {
        Self {
            phase: Phase::Detecting,
            so_override,
            so_path: None,
            so_error: None,
            devices: Vec::new(),
            selected_device: 0,
            device_error: None,
            installed_games: Vec::new(),
            selected_game: 0,
            already_modded: false,
            progress: Arc::new(Mutex::new(ProgressState {
                step: 0, total: 9, message: String::new(), done: false, error: None,
            })),
        }
    }

    fn detect(&mut self) {
        // Find .so
        match pipeline::find_modloader_so(self.so_override.as_deref()) {
            Ok(p) => { self.so_path = Some(p); self.so_error = None; }
            Err(e) => { self.so_error = Some(e.to_string()); }
        }

        // Find devices
        match adb::list_devices() {
            Ok(devs) => {
                if devs.is_empty() {
                    self.device_error = Some("No devices connected. Plug in your Quest.".into());
                } else {
                    self.device_error = None;
                    self.devices = devs;
                    self.selected_device = 0;
                    self.scan_games();
                }
            }
            Err(e) => { self.device_error = Some(e.to_string()); }
        }

        if self.so_error.is_none() && self.device_error.is_none() && !self.installed_games.is_empty() {
            self.phase = Phase::Ready;
        }
    }

    fn scan_games(&mut self) {
        if self.devices.is_empty() { return; }
        let serial = &self.devices[self.selected_device].serial;
        let pkgs: Vec<&str> = game_db::GAMES.iter().map(|g| g.package).collect();
        match adb::find_installed_games(serial, &pkgs) {
            Ok(apps) => {
                self.installed_games = apps.iter().filter_map(|a| {
                    game_db::find_by_package(&a.package).map(|g| (g.clone(), a.clone()))
                }).collect();
                self.selected_game = 0;
                self.check_modded();
            }
            Err(e) => { self.device_error = Some(format!("Scan error: {}", e)); }
        }
    }

    fn check_modded(&mut self) {
        if self.installed_games.is_empty() || self.devices.is_empty() { return; }
        let serial = &self.devices[self.selected_device].serial;
        let pkg = &self.installed_games[self.selected_game].0.package;
        self.already_modded = adb::is_modloader_installed(serial, pkg).unwrap_or(false);
    }

    fn start_install(&mut self) {
        self.phase = Phase::Installing;
        let serial = self.devices[self.selected_device].serial.clone();
        let game = self.installed_games[self.selected_game].0.clone();
        let so = self.so_path.clone().unwrap();
        let progress = self.progress.clone();

        // Reset progress
        {
            let mut p = progress.lock().unwrap();
            p.step = 0;
            p.total = 9;
            p.message = "Starting...".into();
            p.done = false;
            p.error = None;
        }

        thread::spawn(move || {
            let prog = progress.clone();
            let cb: pipeline::ProgressFn = Box::new(move |step, total, msg| {
                let mut p = prog.lock().unwrap();
                p.step = step;
                p.total = total;
                p.message = msg.to_string();
            });

            match pipeline::install(&serial, &game, &so, Some(cb)) {
                Ok(()) => {
                    let mut p = progress.lock().unwrap();
                    p.done = true;
                    p.message = format!("{} installed successfully!", game.name);
                }
                Err(e) => {
                    let mut p = progress.lock().unwrap();
                    p.error = Some(format!("{:#}", e));
                }
            }
        });
    }
}

impl eframe::App for App {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        // Auto-detect on first frame
        if self.phase == Phase::Detecting && self.devices.is_empty() && self.device_error.is_none() {
            self.detect();
        }

        // Poll progress during install
        if self.phase == Phase::Installing {
            let p = self.progress.lock().unwrap().clone();
            if p.done { self.phase = Phase::Done; }
            if p.error.is_some() { self.phase = Phase::Error; }
            ctx.request_repaint();
        }

        egui::CentralPanel::default().show(ctx, |ui| {
            ui.vertical_centered(|ui| {
                ui.heading("🎮 UE Modloader Installer");
                ui.add_space(8.0);
            });

            ui.separator();
            ui.add_space(8.0);

            match self.phase {
                Phase::Detecting => self.ui_detecting(ui),
                Phase::Ready => self.ui_ready(ui, ctx),
                Phase::Installing => self.ui_installing(ui),
                Phase::Done => self.ui_done(ui),
                Phase::Error => self.ui_error(ui),
            }
        });
    }
}

impl App {
    fn ui_detecting(&mut self, ui: &mut egui::Ui) {
        // Show errors
        if let Some(ref e) = self.so_error {
            ui.colored_label(egui::Color32::RED, format!("❌ {}", e));
            ui.add_space(8.0);
        }
        if let Some(ref e) = self.device_error {
            ui.colored_label(egui::Color32::RED, format!("❌ {}", e));
            ui.add_space(8.0);
        }
        if self.installed_games.is_empty() && self.device_error.is_none() && !self.devices.is_empty() {
            ui.colored_label(egui::Color32::YELLOW, "⚠ No supported games found on device.");
            ui.add_space(8.0);
        }

        if ui.button("🔄 Refresh").clicked() {
            self.device_error = None;
            self.so_error = None;
            self.detect();
        }
    }

    fn ui_ready(&mut self, ui: &mut egui::Ui, _ctx: &egui::Context) {
        // .so path
        if let Some(ref p) = self.so_path {
            ui.horizontal(|ui| {
                ui.label("✅ Modloader:");
                ui.monospace(p.file_name().unwrap_or_default().to_string_lossy().to_string());
            });
        }

        ui.add_space(4.0);

        // Device selector
        let dev_labels: Vec<String> = self.devices.iter()
            .map(|d| format!("{} ({}){}", d.model, d.serial,
                if d.is_quest { " [Quest]" } else { "" }))
            .collect();
        let old_dev = self.selected_device;
        ui.horizontal(|ui| {
            ui.label("📱 Device:");
            egui::ComboBox::from_id_salt("device")
                .selected_text(&dev_labels[self.selected_device])
                .show_ui(ui, |ui| {
                    for (i, label) in dev_labels.iter().enumerate() {
                        ui.selectable_value(&mut self.selected_device, i, label);
                    }
                });
        });
        if self.selected_device != old_dev {
            self.scan_games();
        }

        ui.add_space(4.0);

        // Game selector
        if self.installed_games.is_empty() {
            ui.colored_label(egui::Color32::YELLOW, "No supported games on this device.");
        } else {
            let game_labels: Vec<String> = self.installed_games.iter()
                .map(|(g, a)| format!("{} (v{})", g.name, a.version))
                .collect();
            let old_game = self.selected_game;
            ui.horizontal(|ui| {
                ui.label("🎮 Game:");
                egui::ComboBox::from_id_salt("game")
                    .selected_text(&game_labels[self.selected_game])
                    .show_ui(ui, |ui| {
                        for (i, label) in game_labels.iter().enumerate() {
                            ui.selectable_value(&mut self.selected_game, i, label);
                        }
                    });
            });
            if self.selected_game != old_game {
                self.check_modded();
            }

            ui.add_space(8.0);

            if self.already_modded {
                ui.colored_label(egui::Color32::YELLOW, "⚠ Modloader already installed — will reinstall.");
            }

            ui.add_space(12.0);

            // What it will do
            ui.group(|ui| {
                ui.label("This will:");
                ui.label("  1. Pull the APK from device");
                ui.label("  2. Inject modloader into it");
                ui.label("  3. Sign the patched APK");
                ui.label("  4. Backup game data → Install → Restore");
            });

            ui.add_space(12.0);

            ui.vertical_centered(|ui| {
                let btn = egui::Button::new(
                    egui::RichText::new("⚡ Install Modloader")
                        .size(18.0)
                        .strong()
                ).min_size(egui::vec2(250.0, 40.0));
                if ui.add(btn).clicked() {
                    self.start_install();
                }
            });
        }

        ui.add_space(8.0);
        if ui.button("🔄 Refresh").clicked() {
            self.detect();
        }
    }

    fn ui_installing(&self, ui: &mut egui::Ui) {
        let p = self.progress.lock().unwrap().clone();
        ui.add_space(20.0);
        ui.vertical_centered(|ui| {
            ui.heading("Installing...");
            ui.add_space(12.0);
            ui.add(egui::ProgressBar::new(p.step as f32 / p.total as f32)
                .text(format!("{}/{} — {}", p.step, p.total, p.message))
                .animate(true));
            ui.add_space(12.0);
            ui.spinner();
        });
    }

    fn ui_done(&mut self, ui: &mut egui::Ui) {
        let p = self.progress.lock().unwrap().clone();
        ui.add_space(20.0);
        ui.vertical_centered(|ui| {
            ui.heading(egui::RichText::new("✅ Success!").color(egui::Color32::GREEN).size(24.0));
            ui.add_space(8.0);
            ui.label(&p.message);
            ui.add_space(16.0);
            ui.label("Next steps:");
            ui.label("  • Launch the game on the Quest");
            ui.label("  • First run generates the Lua SDK");
            ui.label("  • Deploy mods: python tools/deploy.py mods");
            ui.add_space(16.0);

            ui.horizontal(|ui| {
                if ui.button("🚀 Launch Game").clicked() {
                    let serial = &self.devices[self.selected_device].serial;
                    let pkg = &self.installed_games[self.selected_game].0.package;
                    let _ = adb::launch(serial, pkg);
                }
                if ui.button("↩ Back").clicked() {
                    self.phase = Phase::Ready;
                }
            });
        });
    }

    fn ui_error(&mut self, ui: &mut egui::Ui) {
        let p = self.progress.lock().unwrap().clone();
        ui.add_space(20.0);
        ui.vertical_centered(|ui| {
            ui.heading(egui::RichText::new("❌ Error").color(egui::Color32::RED).size(24.0));
            ui.add_space(8.0);
            if let Some(ref e) = p.error {
                egui::ScrollArea::vertical().max_height(200.0).show(ui, |ui| {
                    ui.monospace(e);
                });
            }
            ui.add_space(16.0);
            if ui.button("↩ Back").clicked() {
                self.phase = Phase::Ready;
            }
        });
    }
}
