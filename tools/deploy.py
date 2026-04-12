#!/usr/bin/env python3
"""
UE Modloader Deploy Tool — SFTP-first, ADB fallback.

Usage:
    python deploy.py modloader              Push libmodloader.so to device
    python deploy.py mods                   Push all mods to device
    python deploy.py mods Patches           Push specific mod(s)
    python deploy.py all                    Push modloader + all mods
    python deploy.py log                    Pull UEModLoader.log
    python deploy.py pe_trace               Pull pe_trace.log
    python deploy.py sdk                    Pull generated SDK from device
    python deploy.py tombstones             Pull & purge tombstones
    python deploy.py restart                Force-stop the game
    python deploy.py launch                 Kill + relaunch the game
    python deploy.py ensure                 Ensure game is running (restart if needed)
    python deploy.py status                 Show mod versions from log
    python deploy.py forward                Set up adb port forwarding (tcp:19420)
    python deploy.py console                Interactive ADB bridge console

Game selection (pick one):
    --game re4                              Target RE4 VR (default)
    --game pfxvr                            Target Pinball FX VR
    GAME=pfxvr python deploy.py modloader   Via env var

Env vars (optional):
    QUEST_IP        Device IP (default: 192.168.1.9)
    QUEST_SSH_KEY   SSH key path (default: ~/.ssh/quest_root)
    ADB_SERIAL      ADB serial (default: auto-detect or $QUEST_IP:5555)
    GAME            Target game: re4, pfxvr (default: auto-detect)
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

# ═══════════════════════════════════════════════════════════════════════
# CONFIG — all derived from env vars or defaults
# ═══════════════════════════════════════════════════════════════════════

QUEST_IP = os.environ.get("QUEST_IP", "192.168.1.9")
SSH_KEY = os.environ.get("QUEST_SSH_KEY",
    str(Path.home() / ".ssh" / "quest_root"))
ADB_SERIAL = os.environ.get("ADB_SERIAL", "")  # will auto-detect if empty

# ─── Multi-game profiles ──────────────────────────────────────────────
# Each game has: pkg, activity, apk_lib_path (auto-detected at runtime)
GAME_PROFILES = {
    "re4": {
        "pkg": "com.Armature.VR4",
        "activity": "com.epicgames.ue4.GameActivity",
        "mods_subdir": "re4",
    },
    "pfxvr": {
        "pkg": "com.zenstudios.PFXVRQuest",
        "activity": "com.epicgames.unreal.GameActivity",
        "mods_subdir": "pfx",
    },
}

# Select game from env var GAME (default: auto-detect running game, else re4)
_game_key = os.environ.get("GAME", "").lower()

def _detect_game() -> str:
    """Auto-detect which game is installed/running on device."""
    global _game_key
    if _game_key and _game_key in GAME_PROFILES:
        return _game_key
    # Try to detect via ADB — check which package exists
    for key, prof in GAME_PROFILES.items():
        try:
            r = subprocess.run(
                ["adb", "shell", f"pm path {prof['pkg']}"],
                capture_output=True, text=True, timeout=5, check=False
            )
            if r.returncode == 0 and r.stdout.strip():
                _game_key = key
                return key
        except Exception:
            pass
    return "re4"  # fallback


def _find_apk_lib_path(pkg: str) -> str:
    """Find the APK lib/arm64 path for a package via root SSH."""
    try:
        r = subprocess.run(
            ["ssh", "-i", SSH_KEY, "-oConnectTimeout=3",
             f"root@{QUEST_IP}",
             f"find /data/app -path '*{pkg}*' -name 'lib' -type d 2>/dev/null"],
            capture_output=True, text=True, timeout=10, check=False
        )
        if r.returncode == 0 and r.stdout.strip():
            lib_dir = r.stdout.strip().splitlines()[0]
            return f"{lib_dir}/arm64"
    except Exception:
        pass
    # Hardcoded fallbacks
    fallbacks = {
        "com.Armature.VR4": (
            "/data/app/~~oJ6qrGbx0E_HVDUVWw-U2g==/"
            "com.Armature.VR4-E18UVaMhj3ERp6JCKpdrSQ==/lib/arm64"
        ),
        "com.zenstudios.PFXVRQuest": (
            "/data/app/~~UsV39PPZPhjfO7ZUs03PuA==/"
            "com.zenstudios.PFXVRQuest-FijRqQ-2zYyCGfeurQ0y4A==/lib/arm64"
        ),
    }
    return fallbacks.get(pkg, f"/data/app/{pkg}/lib/arm64")


def _init_game_config(game_key: str | None = None):
    """Initialize global config vars for the selected game."""
    global PKG, APK_LIB_PATH, MODS_DEVICE, LOG_DEVICE, GAME_ACTIVITY, MODS_DIR
    key = game_key or _detect_game()
    prof = GAME_PROFILES.get(key, GAME_PROFILES["re4"])
    PKG = prof["pkg"]
    GAME_ACTIVITY = prof["activity"]
    APK_LIB_PATH = _find_apk_lib_path(PKG)
    MODS_DEVICE = f"/storage/emulated/0/Android/data/{PKG}/files/mods"
    LOG_DEVICE = f"/storage/emulated/0/Android/data/{PKG}/files/UEModLoader.log"
    # Use game-specific mods subdirectory if configured
    subdir = prof.get("mods_subdir")
    if subdir:
        MODS_DIR = PROJECT_ROOT / "mods" / subdir
    else:
        MODS_DIR = PROJECT_ROOT / "mods"


# These will be set by _init_game_config() in main()
PKG = ""
APK_LIB_PATH = ""
MODS_DEVICE = ""
LOG_DEVICE = ""
GAME_ACTIVITY = ""
TOMBSTONE_DIR = "/data/tombstones"

# Project paths — relative to this script's parent dir (project root)
PROJECT_ROOT = Path(__file__).resolve().parent.parent
MODLOADER_SO = PROJECT_ROOT / "modloader" / "build" / "libmodloader.so"
MODS_DIR = PROJECT_ROOT / "mods"
LOGS_DIR = PROJECT_ROOT / "logs"


def sftp_batch(commands: list[str]) -> bool:
    """Run SFTP batch commands. Returns True on success."""
    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".sftp", delete=False
    ) as f:
        f.write("\n".join(commands) + "\nbye\n")
        batch_file = f.name

    try:
        result = subprocess.run(
            [
                "sftp",
                "-i", SSH_KEY,
                "-oBatchMode=yes",
                "-oConnectTimeout=5",
                "-b", batch_file,
                f"root@{QUEST_IP}",
            ],
            capture_output=True,
            text=True,
            timeout=60,
        )
        if result.returncode == 0:
            return True
        print(f"  [SFTP FAIL] {result.stderr.strip()}")
        return False
    except FileNotFoundError:
        print("  [SFTP FAIL] sftp command not found")
        return False
    except subprocess.TimeoutExpired:
        print("  [SFTP FAIL] Connection timed out")
        return False
    finally:
        os.unlink(batch_file)




def get_adb_serial() -> str:
    """Return a usable adb serial. If ADB_SERIAL env is set and reachable
    return it; otherwise query `adb devices` and pick the first line.
    Cache the result globally to avoid repeated calls."""
    global ADB_SERIAL
    if ADB_SERIAL:
        # test if this serial is connected
        try:
            r = subprocess.run(["adb", "-s", ADB_SERIAL, "get-state"],
                               capture_output=True, text=True, check=False)
            if r.returncode == 0 and r.stdout.strip() in ("device", "unauthorized", "recovery"):
                return ADB_SERIAL
        except FileNotFoundError:
            pass
        # not reachable, fall through to autodetect
    # list devices
    try:
        r = subprocess.run(["adb", "devices"], capture_output=True, text=True, check=False)
    except FileNotFoundError:
        return ""  # adb not installed
    for line in r.stdout.strip().splitlines()[1:]:
        parts = line.strip().split()
        if len(parts) >= 2 and parts[1] == "device":
            ADB_SERIAL = parts[0]
            return ADB_SERIAL
    return ""


def adb(*args: str, check: bool = True) -> subprocess.CompletedProcess:
    """Run ADB command with device serial."""
    serial = get_adb_serial()
    cmd = ["adb"] + (["-s", serial] if serial else []) + list(args)
    return subprocess.run(cmd, capture_output=True, text=True, check=check)


def adb_shell(shell_cmd: str, check: bool = True) -> subprocess.CompletedProcess:
    """Run ADB shell command (with su)."""
    return adb("shell", f"su -c '{shell_cmd}'", check=check)


def adb_forward() -> bool:
    """Ensure local port 19420 is forwarded to device. Returns True if set.
    This is idempotent and runs silently if already forwarded."""
    try:
        r = adb("forward", "tcp:19420", "tcp:19420", check=False)
        if r.returncode == 0:
            return True
        print(f"  [ADB FAIL] forward: {r.stderr.strip()}")
        return False
    except Exception as e:
        print(f"  [ADB FAIL] forward: {e}")
        return False



def adb_push_fallback(local: Path, remote: str) -> bool:
    """ADB push as fallback when SFTP fails. Uses /data/local/tmp staging.
    Tries cp, then dd, then scp as root for incremental-fs."""
    tmp = f"/data/local/tmp/{local.name}"
    try:
        r = adb("push", str(local), tmp, check=False)
        if r.returncode != 0:
            print(f"  [ADB FAIL] push: {r.stderr.strip()}")
            return False

        # Try cp first (works on normal filesystems)
        r2 = adb_shell(f"cp {tmp} {remote}", check=False)
        if r2.returncode == 0:
            adb_shell(f"rm {tmp}", check=False)
            return True
        print(f"  [ADB] cp failed, trying dd...")

        # Try dd (may bypass some permission checks)
        r3 = adb_shell(f"dd if={tmp} of={remote} 2>/dev/null && chmod 755 {remote}", check=False)
        if r3.returncode == 0:
            adb_shell(f"rm {tmp}", check=False)
            return True
        print(f"  [ADB] dd failed, trying scp via root SSH...")

        # Try scp from localhost as root (SFTP already failed, try raw scp)
        try:
            scp_r = subprocess.run(
                ["ssh", "-i", SSH_KEY, "-oConnectTimeout=3",
                 f"root@{QUEST_IP}",
                 f"cp {tmp} '{remote}' 2>&1 || "
                 f"dd if={tmp} of='{remote}' 2>/dev/null && chmod 755 '{remote}'"],
                capture_output=True, text=True, timeout=15, check=False
            )
            if scp_r.returncode == 0:
                adb_shell(f"rm {tmp}", check=False)
                return True
        except Exception:
            pass

        print(f"  [ADB FAIL] all copy methods failed for {remote}")
        adb_shell(f"rm {tmp}", check=False)
        return False
    except Exception as e:
        print(f"  [ADB FAIL] {e}")
        return False


def push_file(local: Path, remote: str) -> bool:
    """Push a file — SFTP first, ADB fallback, root dd last resort."""
    local_posix = str(local).replace("\\", "/")
    print(f"  {local.name} → {remote}")

    # Try SFTP first (quote paths for spaces)
    if sftp_batch([f'put "{local_posix}" "{remote}"']):
        print(f"  ✓ SFTP OK")
        return True

    # ADB fallback
    print(f"  ⚠ SFTP failed — trying ADB push...")
    if adb_push_fallback(local, remote):
        print(f"  ✓ ADB OK")
        return True

    print(f"  ✗ FAILED — could not push {local.name}")
    return False


# ═══════════════════════════════════════════════════════════════════════
# COMMANDS
# ═══════════════════════════════════════════════════════════════════════

def cmd_forward():
    """Run adb forward tcp:19420 tcp:19420 and report result."""
    print("[DEPLOY] setting up ADB forward for command bridge...")
    if adb_forward():
        print("  ✓ ADB forward established")
        return True
    else:
        print("  ✗ failed to set up ADB forward")
        return False


def cmd_console():
    """Launch an interactive console to the ADB bridge port.
    Fully synchronous — no reader thread, no netcat, no races.
    Exit with Ctrl-D, Ctrl-C, or 'quit'.
    """
    import json
    import socket
    import time

    print("[DEPLOY] opening ADB bridge console")

    # ensure game process is running; if not, launch it
    rpid = adb("shell", f"su -c 'pidof {PKG}'", check=False)
    if not rpid.stdout.strip():
        print("  ⚠ game not running, launching now...")
        cmd_launch()
        time.sleep(6)
    if not adb_forward():
        print("  ⚠ could not establish ADB forward")

    def recv_line(sock, timeout=3.0):
        """Read one newline-delimited response line. Returns str or None."""
        sock.settimeout(timeout)
        buf = b""
        try:
            while True:
                ch = sock.recv(1)
                if not ch:
                    return None
                if ch == b"\n":
                    break
                buf += ch
        except socket.timeout:
            if buf:
                return buf.decode("utf-8", errors="ignore")
            return None
        except OSError:
            return None
        finally:
            sock.settimeout(None)
        return buf.decode("utf-8", errors="ignore")

    def pretty(raw):
        """Pretty-print a JSON response line."""
        try:
            obj = json.loads(raw)
            return json.dumps(obj, indent=2)
        except (json.JSONDecodeError, TypeError):
            return raw

    def send_and_recv(sock, payload, timeout=3.0):
        """Send a JSON payload, receive and print the response."""
        try:
            sock.sendall((payload + "\n").encode("utf-8"))
        except OSError as e:
            print(f"  ✗ send failed: {e}")
            return False
        resp = recv_line(sock, timeout)
        if resp is None:
            print("  ⚠ no response (timeout)")
            return True
        print(pretty(resp))
        return True

    # connect
    try:
        s = socket.create_connection(("127.0.0.1", 19420), timeout=5)
    except (OSError, socket.timeout) as e:
        print(f"  ✗ connection failed: {e}")
        print("    Is the game running? Try: python deploy.py launch")
        return False

    print("  ✓ connected to ADB bridge (127.0.0.1:19420)")

    # ping handshake
    if not send_and_recv(s, json.dumps({"cmd": "ping"}), timeout=2.0):
        s.close()
        return False

    # main input loop
    HELP_TEXT = (
        "  Commands: list_mods, reload_mod <name>, load_mod <name>,\n"
        "    exec_lua <code>, list_hooks, dump_sdk, mount_pak <path>,\n"
        "    list_paks, log_tail [N], get_stats, find_object <name>,\n"
        "    find_class <name>, object_count, dump_symbols,\n"
        "    pe_trace_start [filter], pe_trace_stop, pe_trace_top [N],\n"
        "    pe_trace_dump, pe_trace_clear, pe_trace_status, ping, help\n"
        "  Type command name or raw JSON. Ctrl-C / quit to exit."
    )
    print(HELP_TEXT)

    try:
        while True:
            try:
                raw = input("bridge> ")
            except EOFError:
                break
            text = raw.strip()
            if not text:
                continue
            if text.lower() in ("quit", "exit", "q"):
                break
            if text.lower() in ("help", "?", "commands"):
                print(HELP_TEXT)
                continue

            # build JSON payload
            if text.lstrip().startswith("{"):
                payload = text
            else:
                parts = text.split(None, 1)
                cmd_name = parts[0]
                cmd_arg = parts[1] if len(parts) > 1 else None
                obj = {"cmd": cmd_name}
                if cmd_arg is not None:
                    # try to figure out the right arg key
                    if cmd_name in ("reload_mod", "load_mod"):
                        obj["name"] = cmd_arg
                    elif cmd_name == "exec_lua":
                        obj["code"] = cmd_arg
                    elif cmd_name in ("find_object", "find_class"):
                        obj["name"] = cmd_arg
                    elif cmd_name == "mount_pak":
                        obj["path"] = cmd_arg
                    elif cmd_name in ("log_tail", "pe_trace_top"):
                        try:
                            obj["lines"] = int(cmd_arg)
                        except ValueError:
                            obj["lines"] = 50
                    elif cmd_name == "pe_trace_start":
                        obj["filter"] = cmd_arg
                    else:
                        obj["args"] = cmd_arg
                payload = json.dumps(obj)

            # longer timeout for dump commands
            timeout = 10.0 if any(k in text for k in ("dump_sdk", "dump_symbols", "exec_lua")) else 3.0
            if not send_and_recv(s, payload, timeout=timeout):
                break
    except KeyboardInterrupt:
        pass

    print("\n  ✓ console closed")
    s.close()
    return True


def _tmpfs_overlay_deploy(local_so: Path) -> bool:
    """Deploy .so via tmpfs overlay when incremental-fs blocks direct writes.
    Steps: SCP .so to /data/local/tmp, backup originals, mount tmpfs, restore + add."""
    remote_tmp = f"/data/local/tmp/{local_so.name}"
    backup_dir = "/data/local/tmp/pfxvr_backup"
    script = f"""#!/system/bin/sh
set -e
LIB_DIR=$(find /data/app -path '*{PKG}*/lib/arm64' -type d 2>/dev/null | head -1)
[ -z "$LIB_DIR" ] && echo "ERROR: lib dir not found" && exit 1
echo "LIB_DIR=$LIB_DIR"

# Check if tmpfs already mounted (re-deploy case)
if mount | grep -q "$LIB_DIR.*tmpfs"; then
    echo "tmpfs already mounted — copying directly"
    cp {remote_tmp} "$LIB_DIR/{local_so.name}"
    chmod 755 "$LIB_DIR/{local_so.name}"
    ls -la "$LIB_DIR/{local_so.name}"
    echo "DONE — updated {local_so.name} on existing tmpfs"
    exit 0
fi

# Fresh mount: backup, mount tmpfs, restore + add
mkdir -p {backup_dir}
for f in "$LIB_DIR"/*.so; do
    fname=$(basename "$f")
    [ ! -f "{backup_dir}/$fname" ] && cp "$f" "{backup_dir}/$fname"
done
mount -t tmpfs tmpfs "$LIB_DIR"
for f in {backup_dir}/*.so; do
    cp "$f" "$LIB_DIR/$(basename $f)"
done
cp {remote_tmp} "$LIB_DIR/{local_so.name}"
chmod 755 "$LIB_DIR"/*.so
ls -la "$LIB_DIR/{local_so.name}"
echo "DONE — {local_so.name} deployed via tmpfs overlay"
"""
    # SCP the .so to device
    print("  [TMPFS] SCP .so to device staging...")
    try:
        r = subprocess.run(
            ["scp", "-i", SSH_KEY, str(local_so), f"root@{QUEST_IP}:{remote_tmp}"],
            capture_output=True, text=True, timeout=60, check=False
        )
        if r.returncode != 0:
            print(f"  [TMPFS FAIL] SCP: {r.stderr.strip()}")
            return False
    except Exception as e:
        print(f"  [TMPFS FAIL] SCP: {e}")
        return False

    # Run the overlay script via SSH
    print("  [TMPFS] Running overlay deploy via SSH...")
    try:
        r = subprocess.run(
            ["ssh", "-i", SSH_KEY, f"root@{QUEST_IP}", script],
            capture_output=True, text=True, timeout=30, check=False
        )
        for line in (r.stdout + r.stderr).strip().splitlines():
            print(f"  [TMPFS] {line}")
        if r.returncode == 0 and "DONE" in r.stdout:
            return True
        return False
    except Exception as e:
        print(f"  [TMPFS FAIL] SSH: {e}")
        return False


def cmd_modloader():
    """Push libmodloader.so to device."""
    print("[DEPLOY] Pushing modloader...")
    if not MODLOADER_SO.exists():
        print(f"  ✗ Not found: {MODLOADER_SO}")
        print(f"    Run build.bat first!")
        return False
    size_mb = MODLOADER_SO.stat().st_size / (1024 * 1024)
    print(f"  Size: {size_mb:.1f} MB")

    # Try SFTP direct first
    ok = push_file(MODLOADER_SO, f"{APK_LIB_PATH}/libmodloader.so")

    # If direct push failed, try tmpfs overlay (for incremental-fs)
    if not ok:
        print("  ⚠ Direct push failed — trying tmpfs overlay deploy...")
        ok = _tmpfs_overlay_deploy(MODLOADER_SO)

    if ok:
        print("  ✓ Modloader deployed")
        print("[INFO] setting up ADB forward for command bridge...")
        adb_forward()
    return ok


def cmd_mods(mod_names: list[str] | None = None):
    """Push Lua mods to device."""
    if not MODS_DIR.exists():
        print(f"  ✗ Mods directory not found: {MODS_DIR}")
        return False

    if mod_names:
        mods = []
        for name in mod_names:
            mod_path = MODS_DIR / name
            if not mod_path.exists():
                print(f"  ✗ Mod not found: {name}")
                continue
            mods.append(mod_path)
    else:
        mods = sorted([
            d for d in MODS_DIR.iterdir()
            if d.is_dir() and (d / "main.lua").exists()
        ])

    if not mods:
        print("  No mods to push")
        return False

    # ensure bridge is accessible when mods are updated
    print("[INFO] ensuring ADB forward for command bridge...")
    adb_forward()

    print(f"[DEPLOY] Pushing {len(mods)} mod(s)...")

    # Build SFTP batch for all mods at once (faster than individual connections)
    sftp_cmds = []
    for mod in mods:
        lua_file = mod / "main.lua"
        local_posix = str(lua_file).replace("\\", "/")
        remote = f"{MODS_DEVICE}/{mod.name}/main.lua"
        sftp_cmds.append(f'put "{local_posix}" "{remote}"')
        print(f"  {mod.name}/main.lua")

    if sftp_batch(sftp_cmds):
        print(f"  ✓ SFTP OK — {len(mods)} mod(s) pushed")
        return True

    # ADB fallback — push one by one
    print(f"  ⚠ SFTP batch failed — falling back to ADB push...")
    ok = 0
    for mod in mods:
        lua_file = mod / "main.lua"
        remote = f"{MODS_DEVICE}/{mod.name}/main.lua"
        if adb_push_fallback(lua_file, remote):
            ok += 1
        else:
            print(f"  ✗ Failed: {mod.name}")
    print(f"  {'✓' if ok == len(mods) else '⚠'} ADB: {ok}/{len(mods)} pushed")
    return ok == len(mods)


def cmd_all():
    """Push modloader + all mods."""
    r1 = cmd_modloader()
    r2 = cmd_mods()
    return r1 and r2


def cmd_restart():
    """Force-stop the game."""
    print(f"[RESTART] Stopping {PKG}...")
    try:
        adb("shell", f"am force-stop {PKG}", check=False)
        print("  ✓ Game stopped")
        return True
    except Exception as e:
        print(f"  ✗ {e}")
        return False


def cmd_ensure():
    """Ensure the game is running.  If not running, start it; if already
    running, kill and restart it to guarantee a fresh process."""
    # if pid exists we still restart to give a clean state
    print(f"[ENSURE] ensuring {PKG} process is running (will restart if needed)")
    return cmd_launch()


def cmd_launch():
    """Kill the game, then launch it fresh."""
    import time

    print(f"[LAUNCH] Killing {PKG}...")
    adb("shell", f"am force-stop {PKG}", check=False)
    adb("shell", f"su -c 'pkill -9 -f {PKG}'", check=False)
    adb("shell", f"su -c 'killall -9 {PKG}'", check=False)
    # Kill any remaining PIDs
    r = adb("shell", f"su -c 'pidof {PKG}'", check=False)
    if r.stdout.strip():
        for pid in r.stdout.strip().split():
            adb("shell", f"su -c 'kill -9 {pid}'", check=False)
    adb("shell", f"am kill {PKG}", check=False)
    print("  ✓ App killed")

    time.sleep(1)

    print(f"[LAUNCH] Starting {PKG}...")
    activity = f"{PKG}/{GAME_ACTIVITY}"
    r = adb("shell", f"am start -n {activity}", check=False)
    if r.stderr and "Error" in r.stderr:
        print(f"  ✗ Launch error: {r.stderr.strip()}")
        return False
    print("  ✓ App launched")

    time.sleep(2)

    # Check if PID appeared
    r = adb("shell", f"su -c 'pidof {PKG}'", check=False)
    if r.stdout.strip():
        print(f"  ✓ Running (PID: {r.stdout.strip().split()[0]})")
    else:
        print("  ⚠ PID not found yet — app may still be starting")

    # Forward ADB bridge port
    adb("forward", "tcp:19420", "tcp:19420", check=False)
    print("  ✓ ADB bridge forwarded (port 19420)")

    return True


def cmd_log():
    """Pull UEModLoader.log from device."""
    LOGS_DIR.mkdir(exist_ok=True)
    local = LOGS_DIR / "UEModLoader.log"
    local_posix = str(local).replace("\\", "/")
    print(f"[LOG] Pulling UEModLoader.log...")

    if sftp_batch([f'get "{LOG_DEVICE}" "{local_posix}"']):
        print(f"  ✓ Saved to {local}")
        return True

    # ADB fallback
    r = adb("pull", LOG_DEVICE, str(local), check=False)
    if r.returncode == 0:
        print(f"  ✓ Saved to {local} (via ADB)")
        return True
    print(f"  ✗ Failed to pull log")
    return False


def cmd_tombstones():
    """Pull and purge all tombstones."""
    LOGS_DIR.mkdir(exist_ok=True)
    print("[TOMBSTONES] Checking for crash data...")

    # List tombstones via ADB shell (need su for /data/tombstones)
    r = adb_shell(f"ls {TOMBSTONE_DIR}/ 2>/dev/null", check=False)
    if r.returncode != 0 or not r.stdout.strip():
        print("  No tombstones found")
        return True

    files = [
        f.strip() for f in r.stdout.strip().split("\n")
        if f.strip().startswith("tombstone_") and not f.strip().endswith(".lock")
        and not f.strip().endswith(".pb")
    ]

    if not files:
        print("  No tombstones found")
        return True

    print(f"  Found {len(files)} tombstone(s)")

    # Pull each via SFTP, then purge
    for fname in files:
        remote = f"{TOMBSTONE_DIR}/{fname}"
        local = LOGS_DIR / fname
        local.parent.mkdir(exist_ok=True)
        local_posix = str(local).replace("\\", "/")

        pulled = sftp_batch([f'get "{remote}" "{local_posix}"'])
        if not pulled:
            r = adb_shell(f"cat {remote}", check=False)
            if r.returncode == 0:
                local.write_text(r.stdout)
                pulled = True

        if pulled:
            print(f"  ✓ Pulled {fname} → {local}")
            # Purge from device
            adb_shell(f"rm {remote} {remote}.pb {remote}.lock {remote}.pb.lock 2>/dev/null", check=False)
            print(f"  ✓ Purged {fname} from device")
        else:
            print(f"  ✗ Failed to pull {fname}")

    return True


def cmd_pe_trace():
    """Pull pe_trace.log from device."""
    LOGS_DIR.mkdir(exist_ok=True)
    local = LOGS_DIR / "pe_trace.log"
    local_posix = str(local).replace("\\", "/")
    pe_trace_device = f"/storage/emulated/0/Android/data/{PKG}/files/pe_trace.log"
    print("[PE_TRACE] Pulling pe_trace.log...")

    if sftp_batch([f'get "{pe_trace_device}" "{local_posix}"']):
        print(f"  ✓ Saved to {local}")
        return True

    r = adb("pull", pe_trace_device, str(local), check=False)
    if r.returncode == 0:
        print(f"  ✓ Saved to {local} (via ADB)")
        return True
    print(f"  ✗ Failed to pull pe_trace.log")
    return False


def cmd_sdk():
    """Pull the generated SDK from device."""
    sdk_device = f"/storage/emulated/0/Android/data/{PKG}/files/SDK"
    local_sdk = PROJECT_ROOT / "Current Modloader SDK" / "SDK"
    local_sdk.mkdir(parents=True, exist_ok=True)
    local_posix = str(local_sdk).replace("\\", "/")
    print(f"[SDK] Pulling SDK from device...")

    # Use adb pull for recursive directory copy
    r = adb("pull", sdk_device + "/", str(local_sdk), check=False)
    if r.returncode == 0:
        print(f"  ✓ SDK pulled to {local_sdk}")
        return True
    print(f"  ✗ Failed to pull SDK")
    return False


def cmd_status():
    """Show loaded mod versions from log."""
    print("[STATUS] Reading mod versions from device log...")
    r = adb("shell", f"grep 'loaded' {LOG_DEVICE}", check=False)
    if r.returncode != 0:
        print("  ✗ Could not read log")
        return False

    import re
    for line in r.stdout.strip().split("\n"):
        m = re.search(r'\[LUA\s*\]\s+(\w+):.*?(v[\d.]+)\s+loaded', line)
        if m:
            print(f"  {m.group(1):25s} {m.group(2)}")

    # Check crash guards
    r2 = adb("shell", f"grep 'crash guard' {LOG_DEVICE}", check=False)
    if r2.returncode == 0 and r2.stdout.strip():
        guards = r2.stdout.strip().split("\n")
        print(f"\n  Crash guards: {len([g for g in guards if 'installed' in g.lower()])}")
        recoveries = [g for g in guards if "RECOVERY" in g]
        if recoveries:
            print(f"  Crash recoveries: {len(recoveries)}")

    return True


# ═══════════════════════════════════════════════════════════════════════
# MAIN
# ═══════════════════════════════════════════════════════════════════════

def main():
    parser = argparse.ArgumentParser(
        description="UE Modloader Deploy Tool — SFTP-first, ADB fallback"
    )
    parser.add_argument(
        "command",
        choices=["modloader", "mods", "all", "log", "pe_trace", "sdk",
                 "tombstones", "restart", "launch", "ensure", "status",
                 "forward", "console"],
        help="What to deploy/pull"
    )
    parser.add_argument(
        "args", nargs="*",
        help="Additional args (mod names for 'mods' command)"
    )
    parser.add_argument(
        "--no-restart", action="store_true",
        help="Don't force-stop the game after deploy"
    )
    parser.add_argument(
        "--game", "-g",
        choices=list(GAME_PROFILES.keys()),
        default=os.environ.get("GAME", "").lower() or None,
        help="Target game (default: auto-detect). Can also set GAME env var."
    )
    args = parser.parse_args()

    # Initialize game config BEFORE any commands
    _init_game_config(args.game)

    game_label = {
        "com.Armature.VR4": "RE4 VR",
        "com.zenstudios.PFXVRQuest": "Pinball FX VR",
    }.get(PKG, PKG)

    print(f"═══ UE Modloader Deploy Tool ═══")
    print(f"  Game:    {game_label} ({PKG})")
    print(f"  Device:  {QUEST_IP}")
    print(f"  ADB:     {get_adb_serial()}")
    print(f"  SSH Key: {SSH_KEY}")
    print(f"  Lib:     {APK_LIB_PATH}")
    print()

    ok = True

    if args.command == "modloader":
        ok = cmd_modloader()
        if ok and not args.no_restart:
            cmd_restart()

    elif args.command == "mods":
        ok = cmd_mods(args.args if args.args else None)
        if ok and not args.no_restart:
            cmd_restart()

    elif args.command == "all":
        ok = cmd_all()
        if ok and not args.no_restart:
            cmd_restart()

    elif args.command == "log":
        ok = cmd_log()

    elif args.command == "pe_trace":
        ok = cmd_pe_trace()

    elif args.command == "sdk":
        ok = cmd_sdk()

    elif args.command == "tombstones":
        ok = cmd_tombstones()

    elif args.command == "restart":
        ok = cmd_restart()

    elif args.command == "launch":
        ok = cmd_launch()

    elif args.command == "ensure":
        ok = cmd_ensure()

    elif args.command == "status":
        ok = cmd_status()

    elif args.command == "forward":
        ok = cmd_forward()

    elif args.command == "console":
        ok = cmd_console()

    print()
    if ok:
        print("✓ Done")
    else:
        print("✗ Completed with errors")
        sys.exit(1)


if __name__ == "__main__":
    main()
