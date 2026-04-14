// ═══════════════════════════════════════════════════════════════════════
//  Tools Setup — Auto-download ADB, apktool, and signing tools
//  Downloads from official sources if not found on PATH
// ═══════════════════════════════════════════════════════════════════════

use anyhow::{bail, Context, Result};
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

/// Base directory for installed tools (next to the installer executable)
fn tools_dir() -> PathBuf {
    if let Ok(exe) = std::env::current_exe() {
        exe.parent().unwrap_or(Path::new(".")).join("tools")
    } else {
        PathBuf::from("tools")
    }
}

/// Platform-tools directory (contains adb)
fn platform_tools_dir() -> PathBuf {
    tools_dir().join("platform-tools")
}

// ═══════════════════════════════════════════════════════════════════════
//  ADB Setup — download Android Platform-Tools from Google
// ═══════════════════════════════════════════════════════════════════════

#[cfg(target_os = "windows")]
const PLATFORM_TOOLS_URL: &str = "https://dl.google.com/android/repository/platform-tools-latest-windows.zip";
#[cfg(target_os = "linux")]
const PLATFORM_TOOLS_URL: &str = "https://dl.google.com/android/repository/platform-tools-latest-linux.zip";
#[cfg(target_os = "macos")]
const PLATFORM_TOOLS_URL: &str = "https://dl.google.com/android/repository/platform-tools-latest-darwin.zip";

#[cfg(target_os = "windows")]
const ADB_FILENAME: &str = "adb.exe";
#[cfg(not(target_os = "windows"))]
const ADB_FILENAME: &str = "adb";

/// Check if ADB is available (PATH or local tools dir)
pub fn find_adb() -> Option<PathBuf> {
    // 1. Check PATH
    if let Ok(p) = which::which("adb") {
        return Some(p);
    }
    // 2. Check our tools directory
    let local = platform_tools_dir().join(ADB_FILENAME);
    if local.exists() {
        return Some(local);
    }
    // 3. Check common Windows locations
    #[cfg(target_os = "windows")]
    {
        for dir in [
            r"C:\platform-tools",
            r"C:\Android\platform-tools",
            r"C:\AstroTools\scrcpy",
            r"C:\Rookie Sideloader\platform-tools",
            r"C:\Program Files\Meta Quest Developer Hub\resources\bin",
            r"C:\Program Files (x86)\Android\android-sdk\platform-tools",
        ] {
            let p = PathBuf::from(dir).join("adb.exe");
            if p.exists() { return Some(p); }
        }
        for var in ["ANDROID_HOME", "ANDROID_SDK_ROOT"] {
            if let Ok(sdk) = std::env::var(var) {
                let p = PathBuf::from(&sdk).join("platform-tools").join("adb.exe");
                if p.exists() { return Some(p); }
            }
        }
    }
    None
}

/// Download and extract Android Platform-Tools (contains ADB)
pub fn setup_adb() -> Result<PathBuf> {
    if let Some(p) = find_adb() {
        log::info!("ADB found: {}", p.display());
        return Ok(p);
    }

    log::info!("ADB not found — downloading Android Platform-Tools...");

    let tools = tools_dir();
    fs::create_dir_all(&tools)?;

    let zip_path = tools.join("platform-tools.zip");

    // Download
    download_file(PLATFORM_TOOLS_URL, &zip_path)?;

    // Extract
    log::info!("Extracting platform-tools...");
    extract_zip(&zip_path, &tools)?;

    // Cleanup zip
    let _ = fs::remove_file(&zip_path);

    // Verify
    let adb = platform_tools_dir().join(ADB_FILENAME);
    if !adb.exists() {
        bail!("ADB extraction failed — {} not found", adb.display());
    }

    // Make executable on Unix
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let mut perms = fs::metadata(&adb)?.permissions();
        perms.set_mode(0o755);
        fs::set_permissions(&adb, perms)?;
    }

    log::info!("ADB installed: {}", adb.display());
    Ok(adb)
}

// ═══════════════════════════════════════════════════════════════════════
//  apktool Setup — download from Bitbucket
// ═══════════════════════════════════════════════════════════════════════

const APKTOOL_VERSION: &str = "2.10.0";
const APKTOOL_JAR_URL: &str = "https://bitbucket.org/iBotPeaches/apktool/downloads/apktool_2.10.0.jar";

/// Check if apktool is available
pub fn find_apktool() -> Option<PathBuf> {
    // 1. Check PATH
    if let Ok(p) = which::which("apktool") { return Some(p); }
    if let Ok(p) = which::which("apktool.bat") { return Some(p); }
    // 2. Check our tools directory
    let local_jar = tools_dir().join("apktool.jar");
    if local_jar.exists() { return Some(local_jar); }
    #[cfg(target_os = "windows")]
    {
        let local_bat = tools_dir().join("apktool.bat");
        if local_bat.exists() { return Some(local_bat); }
    }
    // 3. Common locations
    for dir in [r"C:\apktool", r"C:\tools\apktool", r"C:\tools"] {
        let bat = PathBuf::from(dir).join("apktool.bat");
        let jar = PathBuf::from(dir).join("apktool.jar");
        if bat.exists() { return Some(bat); }
        if jar.exists() { return Some(jar); }
    }
    None
}

/// Download apktool.jar (requires Java to run)
pub fn setup_apktool() -> Result<PathBuf> {
    if let Some(p) = find_apktool() {
        log::info!("apktool found: {}", p.display());
        return Ok(p);
    }

    // Check for Java first
    if which::which("java").is_err() {
        bail!(
            "apktool requires Java but Java is not installed.\n\
             Please install Java from https://adoptium.net/ (Temurin JDK 17+)\n\
             Then restart the installer."
        );
    }

    log::info!("apktool not found — downloading v{}...", APKTOOL_VERSION);

    let tools = tools_dir();
    fs::create_dir_all(&tools)?;

    let jar_path = tools.join("apktool.jar");
    download_file(APKTOOL_JAR_URL, &jar_path)?;

    // On Windows, also create a wrapper batch file
    #[cfg(target_os = "windows")]
    {
        let bat_path = tools.join("apktool.bat");
        let bat_content = format!(
            "@echo off\r\njava -jar \"%~dp0apktool.jar\" %*\r\n"
        );
        fs::write(&bat_path, bat_content)?;
        log::info!("apktool installed: {}", bat_path.display());
        return Ok(bat_path);
    }

    #[cfg(not(target_os = "windows"))]
    {
        log::info!("apktool installed: {}", jar_path.display());
        return Ok(jar_path);
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  uber-apk-signer Setup — download from GitHub
// ═══════════════════════════════════════════════════════════════════════

const UBER_SIGNER_VERSION: &str = "1.3.0";
const UBER_SIGNER_URL: &str = "https://github.com/patrickfav/uber-apk-signer/releases/download/v1.3.0/uber-apk-signer-1.3.0.jar";

/// Check if uber-apk-signer is available
pub fn find_uber_signer() -> Option<PathBuf> {
    if let Ok(p) = which::which("uber-apk-signer") { return Some(p); }
    let local = tools_dir().join("uber-apk-signer.jar");
    if local.exists() { return Some(local); }
    for dir in [r"C:\tools", r"C:\apktool", r".", r"tools"] {
        for name in ["uber-apk-signer.jar", "uber-apk-signer-1.3.0.jar"] {
            let p = PathBuf::from(dir).join(name);
            if p.exists() { return Some(p); }
        }
    }
    None
}

/// Download uber-apk-signer (for APK signing without Android SDK)
pub fn setup_uber_signer() -> Result<PathBuf> {
    if let Some(p) = find_uber_signer() {
        log::info!("uber-apk-signer found: {}", p.display());
        return Ok(p);
    }

    // Check for Java first
    if which::which("java").is_err() {
        bail!(
            "uber-apk-signer requires Java but Java is not installed.\n\
             Please install Java from https://adoptium.net/ (Temurin JDK 17+)\n\
             Then restart the installer."
        );
    }

    log::info!("uber-apk-signer not found — downloading v{}...", UBER_SIGNER_VERSION);

    let tools = tools_dir();
    fs::create_dir_all(&tools)?;

    let jar_path = tools.join("uber-apk-signer.jar");
    download_file(UBER_SIGNER_URL, &jar_path)?;

    log::info!("uber-apk-signer installed: {}", jar_path.display());
    Ok(jar_path)
}

// ═══════════════════════════════════════════════════════════════════════
//  Combined setup — ensure all required tools are available
// ═══════════════════════════════════════════════════════════════════════

/// Check and setup all required tools. Returns paths to adb and apktool.
pub fn ensure_tools() -> Result<(PathBuf, PathBuf)> {
    log::info!("Checking required tools...");

    let adb = setup_adb()?;
    let apktool = setup_apktool()?;

    // uber-apk-signer is optional (fallback signing), try to set it up but don't fail
    let _ = setup_uber_signer();

    Ok((adb, apktool))
}

/// Get status of all tools (for GUI display)
#[derive(Debug, Clone)]
pub struct ToolsStatus {
    pub adb: Option<PathBuf>,
    pub apktool: Option<PathBuf>,
    pub uber_signer: Option<PathBuf>,
    pub java: Option<PathBuf>,
    pub zipalign: Option<PathBuf>,
    pub apksigner: Option<PathBuf>,
}

pub fn get_tools_status() -> ToolsStatus {
    ToolsStatus {
        adb: find_adb(),
        apktool: find_apktool(),
        uber_signer: find_uber_signer(),
        java: which::which("java").ok(),
        zipalign: which::which("zipalign").ok(),
        apksigner: which::which("apksigner").ok(),
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  HTTP Download — simple blocking download using std::net
// ═══════════════════════════════════════════════════════════════════════

/// Download a file from URL to local path
fn download_file(url: &str, dest: &Path) -> Result<()> {
    log::info!("Downloading: {}", url);

    // For HTTPS, we need native-tls or rustls. Since we don't have them in deps,
    // shell out to curl/wget/powershell instead.
    download_with_system_tool(url, dest)
}

/// Use system tools (curl, wget, powershell) to download
fn download_with_system_tool(url: &str, dest: &Path) -> Result<()> {
    // Try curl first (available on Windows 10+, Linux, macOS)
    if which::which("curl").is_ok() {
        let status = Command::new("curl")
            .args(["-L", "-o", &dest.to_string_lossy(), url])
            .status()
            .context("curl")?;
        if status.success() && dest.exists() {
            let size = fs::metadata(dest).map(|m| m.len() / 1024).unwrap_or(0);
            log::info!("Downloaded {} KB", size);
            return Ok(());
        }
    }

    // Try wget
    if which::which("wget").is_ok() {
        let status = Command::new("wget")
            .args(["-O", &dest.to_string_lossy(), url])
            .status()
            .context("wget")?;
        if status.success() && dest.exists() {
            let size = fs::metadata(dest).map(|m| m.len() / 1024).unwrap_or(0);
            log::info!("Downloaded {} KB", size);
            return Ok(());
        }
    }

    // Windows: try PowerShell
    #[cfg(target_os = "windows")]
    {
        let ps_cmd = format!(
            "Invoke-WebRequest -Uri '{}' -OutFile '{}' -UseBasicParsing",
            url, dest.display()
        );
        let status = Command::new("powershell")
            .args(["-NoProfile", "-Command", &ps_cmd])
            .status()
            .context("powershell")?;
        if status.success() && dest.exists() {
            let size = fs::metadata(dest).map(|m| m.len() / 1024).unwrap_or(0);
            log::info!("Downloaded {} KB", size);
            return Ok(());
        }
    }

    bail!(
        "Could not download file. Please install curl or wget.\n\
         Or manually download {} to {}",
        url, dest.display()
    )
}

// ═══════════════════════════════════════════════════════════════════════
//  ZIP Extraction
// ═══════════════════════════════════════════════════════════════════════

/// Extract a ZIP file to a directory
fn extract_zip(zip_path: &Path, dest_dir: &Path) -> Result<()> {
    // Try system unzip first (faster, handles large files better)
    if try_system_unzip(zip_path, dest_dir).is_ok() {
        return Ok(());
    }

    // Windows: try PowerShell Expand-Archive
    #[cfg(target_os = "windows")]
    {
        let ps_cmd = format!(
            "Expand-Archive -Path '{}' -DestinationPath '{}' -Force",
            zip_path.display(), dest_dir.display()
        );
        let status = Command::new("powershell")
            .args(["-NoProfile", "-Command", &ps_cmd])
            .status()
            .context("powershell Expand-Archive")?;
        if status.success() {
            return Ok(());
        }
    }

    bail!(
        "Could not extract ZIP file. Please install unzip or 7z.\n\
         Or manually extract {} to {}",
        zip_path.display(), dest_dir.display()
    )
}

fn try_system_unzip(zip_path: &Path, dest_dir: &Path) -> Result<()> {
    // Try unzip
    if which::which("unzip").is_ok() {
        let status = Command::new("unzip")
            .args(["-o", "-q", &zip_path.to_string_lossy(), "-d", &dest_dir.to_string_lossy()])
            .status()?;
        if status.success() { return Ok(()); }
    }

    // Try 7z
    if which::which("7z").is_ok() {
        let status = Command::new("7z")
            .args(["x", "-y", &zip_path.to_string_lossy(), &format!("-o{}", dest_dir.display())])
            .status()?;
        if status.success() { return Ok(()); }
    }

    bail!("No system unzip tool found")
}
