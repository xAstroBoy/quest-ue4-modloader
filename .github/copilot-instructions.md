# RE4 VR Modloader — Copilot Instructions

> These are **MANDATORY** rules for all code generation in this project.  
> Violating these rules causes real bugs on the device. Follow them exactly.

---

## ⛔ ABSOLUTE RULES — NEVER VIOLATE

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
The modloader's `Call()` serializes Lua tables into UE4 struct parameters by walking the struct's FProperty list.

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
You can also **memory-edit** UEnum objects to append custom values.

```lua
-- ✅ Named constants via lazy Enums table (auto-populated from UE4 reflection)
Enums.DebugMenuType         -- {NewEnumerator5=0, NewEnumerator0=1, ...}
Enums.ECollisionChannel     -- {ECC_WorldStatic=0, ...}

-- ✅ Lookup functions
local ue = FindEnum("DebugMenuType")           -- returns UEnum* lightuserdata
local t  = GetEnumTable("DebugMenuType")       -- returns {Name=Value, ...} table
local names = GetEnumNames()                   -- returns {"DebugMenuType", "ECollisionChannel", ...}

-- ✅ MEMORY-EDIT: Append a new value to the live UEnum TArray
-- This edits the actual UEnum object in memory (data ptr, count, capacity).
-- Creates a real FName via FName::Init and writes it as a TPair<FName,int64>.
AppendEnumValue("DebugMenuType", "ModsPage", 99)   -- returns true/false

-- After appending, Enums.DebugMenuType auto-refreshes:
Enums.DebugMenuType.ModsPage  -- 99

-- ⚠️ IMPORTANT: Appending to UEnum updates REFLECTION METADATA only.
-- It does NOT create new Blueprint logic. The game's switch/branch per
-- enum value is compiled. New values need explicit handling in hook code.
-- Example: DebugMenuAPI hooks NewMenu() and handles AM=99 itself.

-- ✅ Works with UEnumMethods too:
UEnumMethods.GetNameByValue(ue, 99)  -- "ModsPage" (after append)
UEnumMethods.ForEachName(ue, function(name, value) ... end)
```

### 5. WIDGET CREATION — CreateWidget() Factory
`CreateWidget(className)` creates a proper UMG widget via `WidgetBlueprintLibrary::Create`.
This handles Blueprint widget initialization, widget trees, and Slate setup properly.

```lua
-- ✅ Create any UserWidget subclass (Blueprint or native)
local optWidget = CreateWidget("DebugOptionWidget_C")
local textBlock = CreateWidget("TextBlock")
local vbox      = CreateWidget("VerticalBox")
local img       = CreateWidget("Image")

-- ✅ With owning player (optional)
local pc = FindFirstOf("PlayerController")
local w  = CreateWidget("DebugOptionWidget_C", pc)

-- ✅ Add widgets to containers via reflection
local parent = someVerticalBox  -- any PanelWidget
parent:Call("AddChild", optWidget)

-- ✅ Works with DebugVBoxList_C for debug menu:
local dm  = FindFirstOf("DebugMenu_C")
local pw  = dm:Get("ParentWidget")
local vbl = pw:Get("DebugVBoxList")
local pvb = vbl:Get("ParentVBox")     -- the actual VerticalBox
pvb:Call("AddChildToVerticalBox", optWidget) -- returns VerticalBoxSlot
```

### 6. FText PROPERTIES — Full Read/Write/Call Support
FText properties (like `TextBlock.Text`) are now fully supported.
Reading returns a Lua string, writing accepts a Lua string.

```lua
-- ✅ Read FText property → returns Lua string
local text = textBlock:Get("Text")     -- "Hello World"

-- ✅ Write FText property from string
textBlock:Set("Text", "New Text")

-- ✅ Pass FText as Call() parameter
textBlock:Call("SetText", "My Text")   -- FText param auto-constructed

-- ✅ Works with DebugOptionWidget_C text fields:
local opt = CreateWidget("DebugOptionWidget_C")
opt:Set("OptionName", "My Option")     -- FString property
opt:Call("Setup")                      -- Updates visual text from OptionName
```

### 7. ALWAYS USE SEPARATE pcall BLOCKS
Never put multiple critical operations in one pcall. If one fails, everything after it is skipped.

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
Always test new API calls via the ADB bridge `exec_lua` command before putting them in mod code.  
Use `python tools\deploy.py console` → `exec_lua <code>` to validate.

### 11. ANALYZE BEFORE CODING
Before writing or modifying any debug menu code, read and understand:
- The SDK class dump for every class involved (in `Current Modloader SDK/Classes/`)
- The deep API analysis (in `docs/DEBUG_MENU_API_ANALYSIS.md`)
- The existing mod code and what it already does

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
The tool uses a merged IDA+BINJA binary database for cross-view analysis:
```
"Pinball FX VR Patches\bindump.bdmp"    # 7.28 GB, 263K functions, 245K cross-matched
```

### Common Commands
```bash
# Search for strings in the binary
bindump.exe strings "GEngine" --db "Pinball FX VR Patches\bindump.bdmp"

# Find string cross-references (which functions reference a string)
bindump.exe strxrefs "Create GEngine" --db "Pinball FX VR Patches\bindump.bdmp"

# Get function details (decompilation, assembly, xrefs)
bindump.exe func 0x3FFF768 --extract --db "Pinball FX VR Patches\bindump.bdmp"

# Grep through decompiled C or assembly for patterns
bindump.exe grep "GEngine" "c_binja" --db "Pinball FX VR Patches\bindump.bdmp"
bindump.exe grep "qword_73CF" "asm_ida" --db "Pinball FX VR Patches\bindump.bdmp"

# Search for function by name/pattern
bindump.exe search "ProcessEvent" --db "Pinball FX VR Patches\bindump.bdmp"

# List exports from the binary
bindump.exe exports "." --db "Pinball FX VR Patches\bindump.bdmp"

# Get database info
bindump.exe info --db "Pinball FX VR Patches\bindump.bdmp"

# View cross-references for a function
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

### Deploy Commands (tools/deploy.py)
```bash
# RE4 VR (default game):
python tools\deploy.py mods              # Push all Lua mods
python tools\deploy.py modloader         # Push libmodloader.so
python tools\deploy.py all               # Push modloader + all mods
python tools\deploy.py log               # Pull UEModLoader.log
python tools\deploy.py tombstones        # Pull & purge crash tombstones
python tools\deploy.py launch            # Kill + relaunch game
python tools\deploy.py console           # Interactive bridge console

# Pinball FX VR (use --game pfxvr):
python tools\deploy.py --game pfxvr mods
python tools\deploy.py --game pfxvr all
python tools\deploy.py --game pfxvr log
python tools\deploy.py --game pfxvr launch
python tools\deploy.py --game pfxvr console
python tools\deploy.py --game pfxvr sdk    # Pull SDK dump from device
```

### Common Workflow
```bash
# Mod-only change (no C++ rebuild needed):
python tools\deploy.py mods
python tools\deploy.py launch

# After C++ modloader changes:
cd modloader && .\build.bat
python tools\deploy.py all
python tools\deploy.py launch

# Debug after crash:
python tools\deploy.py tombstones
python tools\deploy.py log
```

### Bridge Console (Live Testing)
```bash
python tools\deploy.py console
> ping
> exec_lua local dm = FindFirstOf("DebugMenu_C"); return dm:GetName()
> debugmenu_status
> list_mods
```

### Python Test Script Pattern
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
-- These ALL work correctly:
vbl:Set("MaxVisible", 50)              -- ✅ Confirmed 5→50
vbl:Set("Selection", 3)                -- ✅ Direct property write
vbl:Set("FirstVisible", 0)             -- ✅ Direct property write
dm:Set("CurrentIndex", 3)              -- ✅ Direct property write
dm:Call("UpdateOptionHighlight")        -- ✅ Visual highlight updates
vbl:Call("SelectionIncremented")        -- ✅ Selection 0→1
vbl:Call("SelectionDecremented")        -- ✅ Selection decrements
vbl:Call("UpdateListView")              -- ✅ Refreshes visibility/scrolling
dm:Call("ClearWidgets")                 -- ✅ Clears all option widgets
dm:Call("CreateActiveOption", "Test")   -- ✅ Creates new option widget
```

### NOW-FIXED API Calls (previously broken, now working)
```lua
-- These ALL work now with LuaUStruct support:
widget:Call("SetDrawSize", {X=500, Y=2000})  -- ✅ Table → struct via reflection
widget:Set("DrawSize", {X=500, Y=2000})      -- ✅ Table → struct via reflection
local ds = widget:Get("DrawSize")
ds.X                                          -- ✅ LuaUStruct __index
ds.Y                                          -- ✅ LuaUStruct __index
ds.X = 500                                    -- ✅ LuaUStruct __newindex (writes to live UObject memory)
```
