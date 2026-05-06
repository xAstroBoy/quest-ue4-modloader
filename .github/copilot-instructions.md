# RE4 VR Modloader — Copilot Instructions

> These are **MANDATORY** rules for all code generation in this project.  
> Violating these rules causes real bugs on the device. Follow them exactly.

---

## ⛔ ABSOLUTE RULES — NEVER VIOLATE

### 0. ALWAYS HOT-RELOAD — NEVER RESTART THE GAME TO TEST LUA CHANGES

**MANDATORY: For ALL Lua mod changes, use hot-reload via the bridge. NEVER force-stop or restart the game just to test a Lua change.**

#### Hot-reload workflow (use this EVERY time):
```bash
# Push + reload at runtime — NO restart:
python tools\ue_tool.py --game pfxvr mods --hot
python tools\ue_tool.py --game pfxvr mods ModName --hot

# Or test live via bridge console first (before even deploying):
python tools\ue_tool.py --game pfxvr console
> exec_lua <your lua snippet>
> reload_mod PFX_ModMenu
```

#### Rules:
- ❌ **FORBIDDEN**: `python tools\ue_tool.py --game pfxvr mods` followed by `launch` or `restart` for Lua-only changes
- ❌ **FORBIDDEN**: Saying "deploy and restart to test" for Lua changes
- ✅ **REQUIRED**: `mods --hot` for all Lua mod updates
- ✅ **REQUIRED**: Bridge `exec_lua` to test API calls BEFORE writing mod code
- ✅ **REQUIRED**: Bridge `reload_mod <name>` to hot-reload a single mod after push

#### When a restart IS actually required (rare):
- C++ modloader changes (`libmodloader.so`) — these require `all` + restart
- The game is NOT running at all — then `launch` first, then `mods --hot`
- A hook registration is broken and the mod is in a crash loop

**If you are about to write `launch` or `restart` after a Lua change — STOP. Use `--hot` instead.**

---

### 1. NO RAW MEMORY OFFSETS FOR GAME OBJECTS
**NEVER use `ReadU32`, `WriteU32`, `ReadPtr`, `WriteU8`, `ReadU8`, `Offset()`, `GetAddress()` to read/write UObject properties.**

The ONLY acceptable uses of raw memory functions:
- **Byte patching** (instruction patching in native code)
- **Memory patching** (NOPing instructions, changing opcodes)
- **Reading hook parms** (`ReadU8(parms)` for BndEvt Pressed boolean — this is standard)
- **Symbol resolution** (`Resolve`, `FindSymbol`, `GetLibBase`)

For **ALL** game object properties, use the reflection API:
```lua
obj:Get("PropertyName")           -- read via reflection
obj:Set("PropertyName", value)    -- write via reflection
obj:Call("FunctionName", args...) -- call UFunction via ProcessEvent
obj.PropertyName                  -- dynamic __index (same as Get)
obj.PropertyName = value          -- dynamic __newindex (same as Set)
```

### 2. Call() WITH TABLE ARGS FOR STRUCT PARAMS NOW WORKS
`obj:Call("SetDrawSize", {X=500, Y=2000})` **now correctly fills struct fields via reflection**.

```lua
-- ✅ WORKS: table → struct for Call() params
obj:Call("SetDrawSize", {X=500, Y=2000})

-- ✅ WORKS: LuaUStruct → struct for Call() params
local ds = obj:Get("DrawSize")
ds.X = 500
ds.Y = 2000
obj:Call("SetDrawSize", ds)
```

### 3. STRUCT PROPERTIES ARE NOW LuaUStruct USERDATA
`IntPoint`, `Vector2D`, `FVector`, `FRotator` etc. returned by `:Get()` are **LuaUStruct userdata** with `__index`/`__newindex` metatables.

```lua
-- ✅ WORKS: read struct fields directly
local ds = widget:Get("DrawSize")
ds.X  -- reads X field via reflection
ds.Y  -- reads Y field via reflection

-- ✅ WORKS: write struct fields directly (writes to live UObject memory)
ds.X = 500
ds.Y = 2000

-- ✅ WORKS: Set with a table (fills fields via reflection)
widget:Set("DrawSize", {X=500, Y=2000})

-- ✅ WORKS: Pass struct to Call() as table
obj:Call("SetActorLocation", {X=100, Y=200, Z=300})

-- ✅ WORKS: Pass struct to Call() as LuaUStruct
local pos = obj:Get("Position")
obj:Call("SetActorLocation", pos)

-- Useful methods:
ds:GetTypeName()    -- returns "IntPoint"
ds:GetSize()        -- returns struct size in bytes
ds:Clone()          -- creates an owning copy
ds:CopyFrom({X=1})  -- copies from table
ds:GetFields()      -- returns {X="int32", Y="int32"}
tostring(ds)        -- "UStruct(IntPoint: X=500, Y=2000)"
```

### 4. ENUM API — Named Constants + Memory-Edit UEnum
The modloader exposes ALL game enums via the `Enums` global (lazy-loaded from reflection).

```lua
-- ✅ Named constants via lazy Enums table
Enums.DebugMenuType         -- {NewEnumerator5=0, NewEnumerator0=1, ...}
Enums.ECollisionChannel     -- {ECC_WorldStatic=0, ...}

-- ✅ Lookup functions
local ue = FindEnum("DebugMenuType")
local t  = GetEnumTable("DebugMenuType")
local names = GetEnumNames()

-- ✅ MEMORY-EDIT: Append a new value to the live UEnum TArray
AppendEnumValue("DebugMenuType", "ModsPage", 99)   -- returns true/false

-- After appending, Enums.DebugMenuType auto-refreshes:
Enums.DebugMenuType.ModsPage  -- 99
```

### 5. WIDGET CREATION — CreateWidget() Factory
```lua
local optWidget = CreateWidget("DebugOptionWidget_C")
local pc = FindFirstOf("PlayerController")
local w  = CreateWidget("DebugOptionWidget_C", pc)
parent:Call("AddChild", optWidget)
```

### 6. FText PROPERTIES — Full Read/Write/Call Support
```lua
local text = textBlock:Get("Text")     -- returns Lua string
textBlock:Set("Text", "New Text")
textBlock:Call("SetText", "My Text")
```

### 7. ALWAYS USE SEPARATE pcall BLOCKS
Never put multiple critical operations in one pcall.

```lua
-- WRONG:
pcall(function()
    widget:Call("SetDrawSize", ...)  -- if this fails...
    vbl:Set("MaxVisible", 50)       -- ...this never runs!
end)

-- RIGHT:
pcall(function() widget:Call("SetDrawSize", ...) end)
pcall(function() vbl:Set("MaxVisible", 50) end)
```

### 8. TArray IS 1-INDEXED
All TArray access in Lua uses 1-based indexing. `arr[1]` is the first element.

### 9. PRE-HOOKS CAN BLOCK
Return `"BLOCK"` from a pre-hook callback to prevent the original UFunction from executing.  
Post-hooks STILL fire even when blocked.

### 10. TEST VIA BRIDGE BEFORE DEPLOYING
**MANDATORY: Always test new API calls, hooks, and game state queries via the ADB bridge BEFORE deploying mod code.**

```python
import socket, json

def send(cmd):
    s = socket.socket(); s.settimeout(5)
    s.connect(('127.0.0.1', 19420))
    s.sendall(json.dumps({'cmd': 'exec_lua', 'code': cmd}).encode() + b'\n')
    return json.loads(s.recv(8192).decode())

result = send('local dm = FindFirstOf("DebugMenu_C"); return tostring(dm:Get("ActiveMenu"))')
print(result)
```

```bash
python tools\ue_tool.py console
> ping
> exec_lua local dm = FindFirstOf("DebugMenu_C"); return dm:GetName()
> list_mods
```

### 11. ANALYZE BEFORE CODING
Before writing or modifying any debug menu code, read and understand:
- The SDK class dump for every class involved (in `Current Modloader SDK/Classes/`)
- The deep API analysis (in `docs/DEBUG_MENU_API_ANALYSIS.md`)
- The existing mod code and what it already does

---

## ⛔ CRASH DIAGNOSIS — MANDATORY RULES (NEVER SKIP)

### 12. NEVER DECLARE A CRASH FIXED WITHOUT FRESH EVIDENCE

**You are NOT allowed to say "fixed", "resolved", "should work now", or "no crash" unless ALL of the following are true:**

1. ✅ A fresh `diagnose` was run AFTER the last deploy (DONT RUN IF YOU USED `monitor`)
2. ✅ The crashlog timestamp was checked — it must be NEWER than the last deploy time
3. ✅ The full log was read — not searched, not grepped, not summarized
4. ✅ The game PID is currently running OR you confirmed it exited cleanly
5. ✅ No new tombstones were found

**If you have not done all five, say "I don't have enough evidence — run `python tools\ue_tool.py diagnose` if You haven't used `monitor`"**

### 13. ALWAYS CHECK LOG TIMESTAMPS BEFORE DRAWING ANY CONCLUSION

The `diagnose` and `crashlog` commands print a timestamp header for every log file:
```
──────────────────────────────────────────────────────────────
  modloader_crash.log
  local  : C:\...\logs\modloader_crash.log  (4,821 bytes)
  pulled : 2025-01-15 14:32:07  (3m 12s ago)
  device : 2025-01-15 14:28:44  (6m 35s ago)
  ⚠  LOG IS 6m 35s OLD — may not reflect the current session
──────────────────────────────────────────────────────────────
```

**Rules:**
- If the crashlog is older than the last deploy → it is **stale**. Do not use it as evidence of anything.
- If the crashlog mtime predates the current session → it may be from a previous crash. Do not declare the current build clean.
- If the crashlog is missing entirely → that is **one** data point, not proof of no crash. The game may not have reached the point where it writes the crash log.
- An empty crashlog + no tombstones + PID is alive = actually clean session. This is the only safe conclusion.

### 14. READ THE ENTIRE LOG FILE — NEVER GREP OR SUMMARIZE

**NEVER:**
- Search for individual lines or patterns (`grep "error"`, `grep "crash"`)
- Look only at the first N lines
- Look only at the last N lines
- Summarize what you "expect" the log to say
- Skip reading a log because "it should be fine"

**ALWAYS:**
- Read and process the **entire** log file content, beginning to end
- The `diagnose` command prints the full content — read all of it
- If the log is very long, that is still not a reason to skim it
- Crashes often appear mid-log, not just at the end
- Warning signs appear before the fatal line — you need both

### 15. CRASH DIAGNOSIS WORKFLOW — FOLLOW THIS EXACTLY

When investigating a crash or suspected crash:

```bash
# Step 1: Pull everything fresh
python tools\ue_tool.py diagnose

# Step 2: Read the FULL output — every line matters
# The diagnose command pulls tombstones, crashlog, and main log
# and prints timestamps for all of them

# Step 3: Check if game is running
python tools\ue_tool.py diagnose
# Output includes: "✓ com.Armature.VR4 is RUNNING (pid=XXXX)"
# or              "✗ com.Armature.VR4 is NOT RUNNING"

# Step 4: If you see a tombstone — it IS a crash. Full stop.
# Tombstones = native crash = the game died. Do not rationalize.

# Step 5: If crashlog exists and timestamp is recent — the mod crashed.
# Read the full crashlog content before forming any theory.

# Step 6: If game is NOT running and no logs — something killed it.
# Run monitor to catch the next crash live:
python tools\ue_tool.py monitor
```

### 16. IF THE GAME IS VISIBLY CRASHING — TRUST THE EVIDENCE

If the user says "the game is crashing" or "it's crashing" — **believe them**.
- Do NOT say "the code looks correct"
- Do NOT say "it should be fixed now"
- Do NOT ask them to "try again and see"
- DO run `diagnose`, read the full output, find the actual cause

The user can see the game. You cannot. Their observation overrides any code analysis.

---

## Project Context

### What This Is
- **Multi-game UE modding framework** for Meta Quest 3 (Android ARM64)
- **Supported Games**:
  - **RE4 VR** (com.Armature.VR4) — UE4.25
  - **Pinball FX VR** (com.zenstudios.PFXVRQuest) — UE5.x
- **Custom C++ modloader** (`libmodloader.so`) with sol2 Lua 5.4 + Dobby hooks
- **UE4SS-compatible** Lua API — similar to PC UE4SS but ARM64 Android
- Mods are pure Lua scripts in `mods/<ModName>/main.lua`

### Key Paths
| Path | Description |
|---|---|
| `modloader/` | C++ modloader source (CMake, NDK r23c) |
| `mods/re4/` | RE4 VR Lua mods |
| `mods/pfx/` | Pinball FX VR Lua mods |
| `Current Modloader SDK/` | SDK dump from live game (Classes, Enums, Structs) |
| `tools/` | Python deploy/test tools |
| `tools/bindump/` | **Rust binary analysis tool** (BDMP database) |
| `docs/` | Documentation (LUA_API.md, this file) |
| `logs/` | Tombstones and game logs |
| `logs/sessions/` | Per-session logcat + tombstone captures |
| `Pinball FX VR Patches/` | PFX VR binary analysis dumps + BDMP database |

### Device & Connection
- **Device**: Meta Quest 3 (192.168.1.9, wireless ADB)
- **ADB Serial**: 192.168.1.9:5555
- **Bridge Port**: TCP 127.0.0.1:19420 (ADB forwarded)
- **SSH Key**: `C:\Users\xAstroBoy\.ssh\quest_root`

---

## 🔍 Binary Analysis — Rust Bindump Tool

### ALWAYS USE THIS TOOL FOR OFFSET LOOKUPS
When you need to find function offsets, string references, cross-references, or analyze the game binary, **ALWAYS use the Rust bindump tool**. Never guess offsets.

### Tool Location
```
tools\bindump\target\release\bindump.exe
```

### BDMP Database
```
"Pinball FX VR Patches\bindump.bdmp"    # 7.28 GB, 263K functions, 245K cross-matched
```

### Common Commands
```bash
bindump.exe strings "GEngine" --db "Pinball FX VR Patches\bindump.bdmp"
bindump.exe strxrefs "Create GEngine" --db "Pinball FX VR Patches\bindump.bdmp"
bindump.exe func 0x3FFF768 --extract --db "Pinball FX VR Patches\bindump.bdmp"
bindump.exe grep "GEngine" "c_binja" --db "Pinball FX VR Patches\bindump.bdmp"
bindump.exe search "ProcessEvent" --db "Pinball FX VR Patches\bindump.bdmp"
bindump.exe exports "." --db "Pinball FX VR Patches\bindump.bdmp"
bindump.exe xrefs 0x16774DC --db "Pinball FX VR Patches\bindump.bdmp"
```

### Key Binary Facts (Pinball FX VR)
- **libUnreal.so** is STRIPPED — only `.dynsym` with ~1156 symbols (63 JNI exports)
- **No `.symtab`** — no debug symbols, no function names
- BINJA address offset: **0x400000** (subtract from BINJA addresses to get file offsets)
- All UE5 engine functions must be found via **pattern scanning**, **string xrefs**, or **fallback offsets**

### Offset Discovery Workflow
1. **Find strings**: `strings "FunctionName"` — see if the function name appears in .rodata
2. **Find xrefs**: `strxrefs "string"` — find which functions reference that string
3. **Extract function**: `func 0xADDR --extract` — get full decompilation
4. **Grep for patterns**: `grep "pattern" "c_binja"` or `grep "pattern" "asm_ida"`
5. **Confirm with xref count**: A global like GEngine will have thousands of xrefs

---

## Build & Deploy

### Build Modloader (C++ changes only)
```bash
cd c:\Users\xAstroBoy\Desktop\re4\modloader
.\build.bat
```

### Deploy Commands (tools/ue_tool.py — ALL-IN-ONE)

**Game selection:** Default is RE4 VR. Use `--game pfxvr` (or `-g pfxvr`) for Pinball FX VR.

#### Deploy Commands
```bash
# ✅ HOT-RELOAD (preferred for Lua changes — NO restart needed):
python tools\ue_tool.py mods --hot              # Push all Lua mods + reload via bridge
python tools\ue_tool.py mods ModName --hot      # Push one mod + reload via bridge

# Standard (requires restart — only for C++ changes or first deploy):
python tools\ue_tool.py modloader               # Push libmodloader.so only
python tools\ue_tool.py mods                    # Push all Lua mods only (then restart)
python tools\ue_tool.py mods ModName            # Push a specific mod only (then restart)
python tools\ue_tool.py all                     # Push modloader + all mods
python tools\ue_tool.py all --no-restart        # Push everything but don't force-stop the game
python tools\ue_tool.py all --hot               # Push modloader + hot-reload all mods
```

#### Game Lifecycle Commands
```bash
python tools\ue_tool.py launch            # Kill + relaunch game + ADB forward bridge
python tools\ue_tool.py restart           # Force-stop game (no relaunch)
python tools\ue_tool.py ensure            # Restart only if not already running
python tools\ue_tool.py status            # Show loaded mod versions from device log
python tools\ue_tool.py forward           # Set up ADB port forward for bridge (tcp:19420)
```

#### Log Commands
⚠️ **CRITICAL: `log`, `crashlog`, and `pe_trace` all DELETE the file from device after pulling.**
They archive a timestamped copy to `logs/sessions/` so no data is lost, but the device file is gone.
**If you need to read the log without deleting it, use `livelog` or read from the archived copy.**

```bash
python tools\ue_tool.py log               # Pull + print + DELETE UEModLoader.log
python tools\ue_tool.py livelog           # Live tail UEModLoader.log (SSH, Ctrl+C to stop) — DOES NOT DELETE
python tools\ue_tool.py crashlog          # Pull + print + DELETE modloader_crash.log
python tools\ue_tool.py pe_trace          # Pull + print + DELETE pe_trace.log
python tools\ue_tool.py tombstones        # Pull + print + PURGE all tombstones from device
python tools\ue_tool.py sdk               # Pull SDK dump from device to Current Modloader SDK/
```

#### Diagnosis Commands
```bash
python tools\ue_tool.py diagnose          # Pull tombstones + crashlog + log (all at once)
                                           # Shows timestamps, full content, PID status
                                           # ⚠ ALSO DELETES logs after pulling (archives them)
                                           # ← USE THIS FOR CRASH INVESTIGATION

python tools\ue_tool.py monitor           # Live session monitor: logcat + tombstone watch
python tools\ue_tool.py monitor --once    # Monitor one session then exit
python tools\ue_tool.py monitor --pid-only # Only capture current game PID logs
python tools\ue_tool.py monitor --tag TAG  # Add extra logcat tag filter (repeatable)
python tools\ue_tool.py monitor --no-logcat # Tombstone watch only, skip logcat
```

#### Bridge Console (Live Testing)
```bash
python tools\ue_tool.py console           # Interactive bridge console (TCP 19420)
> ping
> exec_lua local dm = FindFirstOf("DebugMenu_C"); return dm:GetName()
> list_mods
```

### Common Workflows
```bash
# ✅ Lua mod change (NO restart — use this EVERY time):
python tools\ue_tool.py --game pfxvr mods --hot
python tools\ue_tool.py --game pfxvr mods PFX_ModMenu --hot

# ✅ Test a snippet live before deploying:
python tools\ue_tool.py --game pfxvr console
> exec_lua local dm = FindFirstOf("DebugMenu_C"); return dm:GetName()
> reload_mod PFX_ModMenu

# After C++ modloader changes (restart IS required):
cd modloader && .\build.bat
python tools\ue_tool.py all
python tools\ue_tool.py launch

# Investigating a crash (MANDATORY SEQUENCE):
python tools\ue_tool.py diagnose          # pulls + timestamps + FULL content (deletes after)
# Read ALL output. Check timestamps. Find the actual error.

# Watch live for next crash:
python tools\ue_tool.py monitor

# Read log WITHOUT deleting (for repeated checks during a session):
python tools\ue_tool.py livelog           # live tail via SSH

# Pinball FX VR examples:
python tools\ue_tool.py --game pfxvr all
python tools\ue_tool.py --game pfxvr launch
python tools\ue_tool.py --game pfxvr diagnose
python tools\ue_tool.py --game pfxvr monitor --once
```

---

## DebugMenuAPI Architecture

### Key Classes (all in reflection)
| Class | Parent | Access | Description |
|---|---|---|---|
| `DebugMenu_C` | VR4DebugScreenActor | `FindFirstOf("DebugMenu_C")` | Main debug menu actor |
| `DebugMenuWidget_C` | UserWidget | `dm:Get("ParentWidget")` | Widget blueprint with VBoxList |
| `DebugVBoxList_C` | UserWidget | `pw:Get("DebugVBoxList")` | Scrollable list widget |
| `DebugOptionWidget_C` | UserWidget | `ActiveOptionsWidgets[i]` | Individual option entries |
| `DebugMenuInterface_C` | Interface | N/A | Input interface definition |

### Input Flow
1. VR controller input → `VR4PlayerController_BP_C:BndEvt__DebugInput*` (14 events)
2. BndEvt calls → `DebugMenu_C:InputActionScrollUp/Down/Confirm/Back(Pressed)`
3. InputAction functions → modify `CurrentIndex`, call `UpdateOptionHighlight()`, etc.
4. Our mod hooks the BndEvt functions (pre+post) to intercept and handle custom MODS page

### Verified Working API Calls (Bridge-Tested)
```lua
vbl:Set("MaxVisible", 50)
vbl:Set("Selection", 3)
vbl:Set("FirstVisible", 0)
dm:Set("CurrentIndex", 3)
dm:Call("UpdateOptionHighlight")
vbl:Call("SelectionIncremented")
vbl:Call("SelectionDecremented")
vbl:Call("UpdateListView")
dm:Call("ClearWidgets")
dm:Call("CreateActiveOption", "Test")
widget:Call("SetDrawSize", {X=500, Y=2000})
widget:Set("DrawSize", {X=500, Y=2000})
local ds = widget:Get("DrawSize")
ds.X; ds.Y; ds.X = 500
```
---

## Pinball FX VR — Collectible Slot Swap (PE-Trace-Verified)

### Slot Swap Flow (captured via PE trace on live device)
When the user changes a collectible (statue, gadget, poster) in the hub:

1. **Data write**: `slot:Set("m_slotEntry", newEntry)` — sets the entry on `PFXCollectibleSlotComponent`
2. **AC notification**: `AC_CollectibleSlot_C:OnSlotEntryChanged(newEntry)` — fires on the **ActorComponent** (NOT `PFXCollectibleSlotComponent`)
3. **Actor reinit**: `BP_Collectible_SM_Base_C:OnCollectibleActorReinitFromPool()` — reinitializes the visual mesh actor
4. **Entry data**: `BP_Collectible_SM_Base_C:OnEntryDataSet()` (for gadgets/statues) or `BP_Collectible_Poster_Base_C:OnEntryDataSet()` (for posters)
5. **Snap**: `AC_CollectibleSlot_C:InterpSnapToPosition()` — animates into position
6. **Save**: `PFXSaveLoadObserverInterface:OnSaveProfileStarted/Finished` — saves to profile

### ⚠️ CRITICAL: `ChangeSlotEntry()` is a NO-OP
PE trace confirmed: `ChangeSlotEntry` does NOT appear in the trace at all.
`Set("m_slotEntry", newEntry)` is the ONLY way to change slot data — confirmed via bridge.

### Key Classes Involved
| Class | Role | PE Trace Count |
|---|---|---|
| `AC_CollectibleSlot_C` | Actor component on slot — fires `OnSlotEntryChanged` | 12 |
| `BP_Collectible_SM_Base_C` | Static mesh collectible actor (statue/gadget) | 27 (`ReinitFromPool`) |
| `BP_Collectible_Poster_Base_C` | Poster collectible actor | 2 (`OnEntryDataSet`) |
| `PFXMetricEventHolder` | Metrics tracking | 12 |
| `PFXAchievementsManager` | Achievement tracking | 12 |

### Correct Swap Code Pattern
```lua
-- 1. Write the entry data
slot:Set("m_slotEntry", newEntry)

-- 2. Find AC_CollectibleSlot_C on the owner actor
local owner = slot:GetOuter()
local comps = owner:Call("K2_GetComponentsByClass", FindClass("AC_CollectibleSlot_C"))
for i = 1, #comps do
    comps[i]:Call("OnSlotEntryChanged", newEntry)
end

-- 3. Reinit the collectible actor
local actor = slot:Get("m_collectibleActor")
if actor then
    actor:Call("OnCollectibleActorReinitFromPool")
    actor:Call("OnEntryDataSet")
end
```

### 17. DO NOT CREATE OR USE PYTHON VIRTUAL ENVIRONMENTS

**NEVER create, suggest, or require a Python virtual environment (`venv`, `virtualenv`, `pipenv`, `poetry`, etc.).**

#### ❌ FORBIDDEN:
```bash
python -m venv venv
python -m venv .venv
source venv/bin/activate
venv\Scripts\activate
pip install -r requirements.txt

❌ ALSO FORBIDDEN:
Suggesting “use a virtual environment”
Adding requirements.txt for the purpose of isolation
Any workflow that introduces environment setup overhead

✅ REQUIRED:
Use the system Python environment only
Assume all required tools are already available (adb, ssh, sftp, standard library)
If a dependency is missing, provide a direct install command only, no environment setup
RATIONALE:
This project is device-debugging focused, not package-distribution
Virtual environments add unnecessary friction and time waste
All tooling must be immediately runnable with zero setup

### 18. PERFORMANCE PROFILING — MANDATORY LAG DETECTION & ANALYSIS

**YOU MUST actively detect, analyze, and mitigate performance issues (lag, stutter, frame drops) in the modloader and Lua mods.**

This is not optional. Performance regressions are treated as **critical bugs**.

---

### 🔍 REQUIRED CAPABILITIES

#### 1. TRACK RUNTIME EXECUTION COST
You MUST measure:
- Hook execution time
- Lua function execution time
- Per-frame workload (especially tick/update hooks)

#### 2. IDENTIFY LAG SOURCES
When lag is reported or suspected, you MUST:
- Determine which hook, mod, or function is slow
- Measure execution time (not guess)
- Identify frequency (per-frame vs occasional)

#### 3. NO GUESSING — ONLY MEASURED DATA
❌ Forbidden:
- “This might be slow”
- “This could cause lag”
- Blind optimizations without evidence

✅ Required:
- “Hook X takes 12ms per call (called every frame → 720ms/sec)”
- “Function Y allocates tables repeatedly causing GC spikes”

---

### 🧪 TESTING WORKFLOW (MANDATORY)

Before declaring any performance improvement:

1. Run the game with monitor:
```bash
python tools\ue_tool.py monitor

Capture session logs
Use bridge to benchmark:
local t0 = os.clock()
-- code to test
local dt = (os.clock() - t0) * 1000
print("took", dt, "ms")
Compare BEFORE vs AFTER
⚠️ COMMON LAG SOURCES (YOU MUST CHECK THESE)
❌ Per-frame heavy work
Hooks on Tick / Update / Input events doing expensive logic
Rebuilding UI every frame
Calling FindFirstOf() repeatedly
❌ Reflection spam
Excessive Get() / Set() calls in loops
Re-fetching the same UObject every frame instead of caching
❌ Allocation pressure
Creating tables every frame ({} inside loops)
Rebuilding structs instead of mutating existing ones
❌ Logging spam
Printing every frame
Excessive debug output flooding logcat
❌ Widget misuse
Creating widgets repeatedly instead of reusing
Calling UpdateListView too often
🛠 REQUIRED OPTIMIZATION STRATEGIES
1. CACHE EVERYTHING POSSIBLE
-- BAD:
local dm = FindFirstOf("DebugMenu_C") -- every frame

-- GOOD:
if not dm then dm = FindFirstOf("DebugMenu_C") end
2. AVOID PER-FRAME ALLOCATIONS
-- BAD:
obj:Call("SetDrawSize", {X=500, Y=2000}) -- alloc each call

-- GOOD:
if not cachedSize then cachedSize = {X=500, Y=2000} end
obj:Call("SetDrawSize", cachedSize)
3. THROTTLE EXECUTION
local last = 0
Hook("Tick", function()
    local now = os.clock()
    if now - last < 0.1 then return end -- 10Hz instead of 90Hz
    last = now
end)
4. MINIMIZE REFLECTION CALLS
-- BAD:
for i=1,100 do
    obj:Get("Value")
end

-- GOOD:
local v = obj:Get("Value")
for i=1,100 do
    -- use v
end

📊 REQUIRED OUTPUT WHEN ANALYZING LAG

You MUST report findings like this:

Lag Analysis:
- Hook: BndEvt__DebugInputScrollDown
- Frequency: ~90 calls/sec
- Avg time: 3.2 ms
- Total cost: ~288 ms/sec → CRITICAL

Root cause:
- Rebuilding widget list every input tick

Fix:
- Cache widgets
- Only update on index change

Result:
- New avg time: 0.2 ms
- Total cost: ~18 ms/sec
⛔ DO NOT DECLARE PERFORMANCE FIXES WITHOUT EVIDENCE

Same standard as crash diagnosis:

You are NOT allowed to say:

“optimized”
“should be faster”
“lag fixed”

UNLESS:

You measured execution time BEFORE
You measured execution time AFTER
You compared both

Otherwise say:

“No measured performance data — run benchmark via bridge or monitor”

🚀 OPTIONAL (HIGHLY RECOMMENDED)

If possible, integrate:

Per-hook timing instrumentation in C++ modloader
Aggregated stats exposed via bridge (get_stats)
Top slowest hooks/functions list
FINAL RULE

If the game stutters, freezes, or drops frames — THERE IS A MEASURABLE CAUSE.
Your job is to find it, quantify it, and eliminate it, not guess.


## 19. POWERHELL EXECUTION — STRICT, MINIMAL, NO BULLSHIT

**ALL PowerShell commands MUST be simple, deterministic, and free of unnecessary arguments, wrappers, or environment hacks.**

If a command errors due to complexity, that is considered a failure of implementation.

---

### ⛔ ABSOLUTE RULES

#### 1. NO VENV USAGE — EVER
- DO NOT reference `.venv`
- DO NOT use custom Python paths inside virtual environments

```powershell
# ❌ FORBIDDEN
$py = ".venv/Scripts/python.exe"

# ✅ REQUIRED
$py = "python"
2. NO UNNECESSARY ENV SETUP
DO NOT set encoding unless it is PROVEN required
DO NOT use chcp 65001 unless debugging encoding issues
DO NOT set $env:PYTHONIOENCODING
# ❌ FORBIDDEN
chcp 65001
$env:PYTHONIOENCODING='utf-8'

# ✅ REQUIRED
# nothing
3. NO BACKGROUND JOB COMPLEXITY UNLESS ABSOLUTELY REQUIRED

PowerShell jobs (Start-Job) introduce:

race conditions
silent failures
debugging complexity

Use them ONLY if there is NO alternative.

4. NO RESERVED VARIABLE COLLISIONS

PowerShell has built-in variables:

$PID
$PSHOME
$HOST

DO NOT reuse them.

# ❌ FORBIDDEN
$pid = ...

# ✅ REQUIRED
$procId = ...
5. KEEP COMMANDS LINEAR AND DEBUGGABLE

Split logic into clear steps. No mega one-liners.

# ❌ FORBIDDEN (your current script style)
everything chained in one unreadable command

# ✅ REQUIRED
Set-Location "C:\path"

python tools\ue_tool.py launch

Start-Sleep 5

python tools\ue_tool.py monitor --once

python tools\ue_tool.py diagnose
✅ REQUIRED WORKFLOW (NO FAILURE MODE)
SIMPLE, RELIABLE VERSION:
Set-Location "C:\Users\xAstroBoy\Desktop\UE4-5 Quest Modloader"

python tools\ue_tool.py launch

Start-Sleep -Seconds 8

python tools\ue_tool.py monitor --once

python tools\ue_tool.py diagnose
🔥 ZERO-ERROR RULE

If a command:

fails due to encoding
fails due to env vars
fails due to Python path
fails due to PowerShell quirks

REMOVE the complexity — do NOT add more.

🧠 DESIGN PRINCIPLE

If a command cannot be explained in 5 seconds, it is too complex and WILL break.

🚫 FINAL WARNING

Do NOT:

wrap everything in jobs
dynamically build scripts
chain 10 operations in one command
introduce “smart” logic that hides failure
✅ GOAL
You run it
It works
No hidden state
No race conditions
No garbage