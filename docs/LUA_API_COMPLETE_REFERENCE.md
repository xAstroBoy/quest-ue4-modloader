# Complete Lua API Reference — UE Modloader

> Auto-generated from C++ source analysis of `modloader/src/lua/*.cpp`  
> Every Lua-visible symbol registered by the modloader.

---

## Table of Contents

- [Complete Lua API Reference — UE Modloader](#complete-lua-api-reference--ue-modloader)
  - [Table of Contents](#table-of-contents)
  - [Global Functions](#global-functions)
    - [Logging](#logging)
    - [Object Finding \& Loading](#object-finding--loading)
      - [RebuildClass Return Table Methods](#rebuildclass-return-table-methods)
    - [Object Construction \& Destruction](#object-construction--destruction)
    - [Reflection / Introspection](#reflection--introspection)
    - [ProcessEvent Hooks](#processevent-hooks)
    - [HarmonyX-style Hook API](#harmonyx-style-hook-api)
    - [Native Hooks](#native-hooks)
    - [Cast \& Type Checking](#cast--type-checking)
    - [Enum API](#enum-api)
    - [Memory Read/Write](#memory-readwrite)
    - [Symbol Resolution](#symbol-resolution)
    - [Delayed Actions \& Async](#delayed-actions--async)
    - [Console \& Cheats](#console--cheats)
    - [View Modes \& Stats](#view-modes--stats)
    - [ProcessEvent Trace](#processevent-trace)
    - [SDK / Debug Dump](#sdk--debug-dump)
    - [File I/O](#file-io)
    - [Misc Utilities](#misc-utilities)
    - [UE4SS Lifecycle Hooks](#ue4ss-lifecycle-hooks)
    - [Thread Functions](#thread-functions)
    - [Shared Variables](#shared-variables)
    - [UObject Extension Functions](#uobject-extension-functions)
  - [Usertypes](#usertypes)
    - [UObject](#uobject)
      - [Methods](#methods)
      - [Dynamic Access (via \_\_index / \_\_newindex)](#dynamic-access-via-__index--__newindex)
      - [Metamethods](#metamethods)
    - [UStruct (LuaUStruct)](#ustruct-luaustruct)
      - [Methods](#methods-1)
      - [Dynamic Access (via \_\_index / \_\_newindex)](#dynamic-access-via-__index--__newindex-1)
      - [Metamethods](#metamethods-1)
    - [TArray (LuaTArray)](#tarray-luatarray)
      - [Methods](#methods-2)
      - [Indexing](#indexing)
      - [Metamethods](#metamethods-2)
    - [TMap (LuaTMap)](#tmap-luatmap)
      - [Methods](#methods-3)
      - [Metamethods](#metamethods-3)
    - [FName (LuaFName)](#fname-luafname)
      - [Constructors](#constructors)
      - [Methods](#methods-4)
      - [Metamethods](#metamethods-4)
    - [FText (LuaFText)](#ftext-luaftext)
      - [Constructors](#constructors-1)
      - [Methods](#methods-5)
      - [Metamethods](#metamethods-5)
    - [FString (LuaFString)](#fstring-luafstring)
      - [Constructors](#constructors-2)
      - [Methods](#methods-6)
      - [Metamethods](#metamethods-6)
    - [RemoteUnrealParam](#remoteunrealparam)
      - [Methods](#methods-7)
      - [Metamethods](#metamethods-7)
    - [FWeakObjectPtr](#fweakobjectptr)
      - [Methods](#methods-8)
      - [Metamethods](#metamethods-8)
    - [FOutputDevice](#foutputdevice)
      - [Methods](#methods-9)
      - [Metamethods](#metamethods-9)
    - [ThreadId](#threadid)
      - [Methods](#methods-10)
      - [Metamethods](#metamethods-10)
    - [LuaParmsAccessor](#luaparmsaccessor)
      - [Methods](#methods-11)
      - [Dynamic Access (via \_\_index / \_\_newindex)](#dynamic-access-via-__index--__newindex-2)
      - [Metamethods](#metamethods-11)
  - [Global Tables / Namespaces](#global-tables--namespaces)
    - [Enums (lazy metatable)](#enums-lazy-metatable)
    - [EFindName](#efindname)
    - [PropertyTypes](#propertytypes)
    - [EObjectFlags](#eobjectflags)
    - [EInternalObjectFlags](#einternalobjectflags)
    - [Key](#key)
    - [ModifierKey](#modifierkey)
    - [ModKeys](#modkeys)
    - [ModConfig](#modconfig)
    - [UClassMethods](#uclassmethods)
    - [UFunctionMethods](#ufunctionmethods)
    - [UEnumMethods](#uenummethods)
  - [Global Variables](#global-variables)
  - [Symbol Count Summary](#symbol-count-summary)
    - [Source File Breakdown](#source-file-breakdown)

---

## Global Functions

### Logging
*Source: `lua_bindings.cpp`*

| Function | Signature | Return | Description |
|---|---|---|---|
| `Log` | `(msg: string)` | void | Log info message |
| `LogWarn` | `(msg: string)` | void | Log warning message |
| `LogError` | `(msg: string)` | void | Log error message |
| `print` | `(...)` | void | UE4SS-style print — variadic, tab-separated, supports all types |
| `Notify` | `(title: string, body: string, ...)` | void | Send notification |

### Object Finding & Loading
*Source: `lua_bindings.cpp`, `lua_ue4ss_globals.cpp`*

| Function | Signature | Return | Description |
|---|---|---|---|
| `FindClass` | `(name: string)` | UObject\|nil | Find a UClass by short name |
| `FindObject` | `(name: string)` | UObject\|nil | Find a UObject by name |
| `StaticFindObject` | `(path: string)` | UObject\|nil | Find a UObject by full path |
| `StaticFindObjectEx` | `(classObj, outerObj, name: string, exact?: bool)` | UObject\|nil | Extended find with class/outer filter |
| `StaticLoadClass` | `(baseName: string, path: string)` | UObject\|nil | Load a UClass by path |
| `StaticLoadObject` | `(baseName: string, path: string)` | UObject\|nil | Load a UObject by path |
| `FindFirstOf` | `(className: string)` | UObject\|nil | Find first live instance of class |
| `FindAllOf` | `(className: string)` | table\|nil | Find all live instances of class (1-indexed array) |
| `FindObjects` | `(numToFind?: int, className: string, reqFlags?: int, banFlags?: int)` | table\|nil | UE4SS-style search with flag filters |
| `LoadAsset` | `(path: string)` | UObject\|nil | Load an asset via StaticLoadObject |
| `LoadExport` | `(path: string)` | UObject\|nil | Alias for LoadAsset |
| `GetCDO` | `(className: string)` | UObject\|nil | Get the Class Default Object |
| `GetWorldContext` | `()` | UObject\|nil | Get the current UWorld |
| `FindEnum` | `(name: string)` | lightuserdata\|nil | Find a UEnum by name (returns UEnum*) |
| `FindStruct` | `(name: string)` | UObject\|nil | Find a UScriptStruct by name |
| `UObjectFromPtr` | `(ptr: lightuserdata)` | UObject\|nil | Wrap a raw pointer as a LuaUObject |
| `RebuildClass` | `(name: string)` | table\|nil | Rebuild class reflection → returns class descriptor table |
| `ForEachUObject` | `(callback: function)` | void | Iterate all UObjects in GUObjectArray; callback(UObject) |
| `GetKnownClasses` | `()` | table | Get table of all known class names |

#### RebuildClass Return Table Methods
The table returned by `RebuildClass(name)` has these fields/methods:
- `__name` — string class name
- `__parent` — string parent class name
- `__raw` — lightuserdata (UClass*)
- `GetInstance(index?: int)` → UObject — get instance by index (or first)
- `GetAllInstances()` → table of UObject
- `InstanceCount()` → int
- `HasProp(propName: string)` → bool
- `HasFunc(funcName: string)` → bool
- `Properties()` → table of `{name, offset, type, size}`
- `Functions()` → table of `{name, flags, parms_size, num_parms}`
- `HookProp(propName, callback)` → uint64 hookId
- `HookFunc(funcName, preFn, postFn)` → uint64 hookId

### Object Construction & Destruction
*Source: `lua_bindings.cpp`, `lua_cast.cpp`*

| Function | Signature | Return | Description |
|---|---|---|---|
| `ConstructObject` | `(classArg: UObject\|string, outer?: UObject, name?: string)` | UObject\|nil | StaticConstructObject_Internal wrapper |
| `NewObject` | `(className: string, outer?: UObject)` | UObject\|nil | Create new UObject via StaticConstructObject |
| `CreateWidget` | `(classArg: string\|UObject, owner?: UObject)` | UObject\|nil | Create UMG widget via WidgetBlueprintLibrary::Create |
| `SpawnActor` | `(className: string, location?: table, rotation?: table)` | UObject\|nil | Spawn actor in world (falls back to Summon) |
| `DestroyActor` | `(obj: UObject)` | bool | Destroy an actor via K2_DestroyActor |
| `MarkPendingKill` | `(obj: UObject)` | bool | Set RF_BeginDestroyed flag |
| `CreateInvalidObject` | `()` | UObject | Create a null-wrapped UObject |

### Reflection / Introspection
*Source: `lua_cast.cpp`*

| Function | Signature | Return | Description |
|---|---|---|---|
| `GetClassHierarchy` | `(obj: UObject)` | table | Returns `{"ChildClass", "ParentClass", ..., "Object"}` |
| `GetAllProperties` | `(obj: UObject\|className: string)` | table | Dump all properties `{name, type, type_name, offset, size}` |
| `GetAllFunctions` | `(obj: UObject\|className: string)` | table | Dump all functions `{name, flags, num_params, params=[...]}` |

### ProcessEvent Hooks
*Source: `lua_bindings.cpp`*

| Function | Signature | Return | Description |
|---|---|---|---|
| `RegisterHook` | `(path: string, callback: function)` | (preId: uint64, postId: uint64) | UE4SS-style hook; callback receives (Context, ...) where `Context:get()` → self |
| `RegisterPreHook` | `(path: string, callback: function)` | uint64 | Pre-hook only; return "BLOCK" to prevent original |
| `RegisterPostHook` | `(path: string, callback: function)` | uint64 | Post-hook only |
| `RegisterProcessEventHook` | `(className: string, ...)` | void | Register ProcessEvent-level hook for a class |
| `UnregisterHook` | `(id: uint64)` or `(path, preId, postId)` | void | Unregister a hook by ID(s) |
| `NotifyOnNewObject` | `(className: string, callback: function)` | uint64 | Fires callback(newObj) when an instance of className spawns |

### HarmonyX-style Hook API
*Source: `lua_cast.cpp`*

| Function | Signature | Return | Description |
|---|---|---|---|
| `Hook` | `(funcPath: string, opts: table)` | table | HarmonyX-style hook. `opts = {Prefix=fn, Postfix=fn}`. Returns `{PreId, PostId, Unhook=fn}` |
| `CastParms` | `(parms: lightuserdata, funcPath: string)` | LuaParmsAccessor\|nil | Cast raw parms pointer to named parameter accessor |

### Native Hooks
*Source: `lua_bindings.cpp`*

| Function | Signature | Return | Description |
|---|---|---|---|
| `RegisterNativeHook` | `(symbolName: string, preFn, postFn, sig?: string)` | bool | Hook a native C++ function by symbol name |
| `RegisterNativeHookAt` | `(addr: lightuserdata, hookName: string, preFn, postFn, sig?: string)` | bool | Hook at an absolute address |
| `CallNative` | `(addr: lightuserdata, sig: string, ...)` | varies | Call a native function at address with ARM64 ABI |
| `CallNativeBySymbol` | `(symbol: string, sig: string, ...)` | varies | Call a native function by symbol with ARM64 ABI |

### Cast & Type Checking
*Source: `lua_cast.cpp`*

| Function | Signature | Return | Description |
|---|---|---|---|
| `Cast` | `(obj: UObject, className: string)` | UObject\|nil | Validate IsA, return same object or nil |
| `UnsafeCast` | `(obj: UObject\|lightuserdata)` | UObject\|nil | Wrap as UObject without IsA validation |
| `IsA` | `(obj: UObject, className: string)` | bool | Check if object inherits from class |

### Enum API
*Source: `lua_enum_ext.cpp`*

| Function | Signature | Return | Description |
|---|---|---|---|
| `FindEnum` | `(name: string)` | lightuserdata\|nil | Find UEnum* by name |
| `GetEnumTable` | `(name: string)` | table\|nil | Get `{ValueName=intValue, ...}` for an enum |
| `GetEnumNames` | `()` | table | Get array of all enum names |
| `AppendEnumValue` | `(enumName: string, valueName: string, value: int64)` | bool | Memory-edit UEnum to add new value |

### Memory Read/Write
*Source: `lua_bindings.cpp`*

| Function | Signature | Return | Description |
|---|---|---|---|
| `ReadU8` | `(addr: lightuserdata)` | int | Read unsigned 8-bit |
| `ReadU16` | `(addr: lightuserdata)` | int | Read unsigned 16-bit |
| `ReadU32` | `(addr: lightuserdata)` | uint32 | Read unsigned 32-bit |
| `ReadU64` | `(addr: lightuserdata)` | double | Read unsigned 64-bit (as double) |
| `ReadS32` | `(addr: lightuserdata)` | int32 | Read signed 32-bit |
| `ReadF32` | `(addr: lightuserdata)` | float | Read 32-bit float |
| `ReadF64` | `(addr: lightuserdata)` | double | Read 64-bit double |
| `ReadPtr` | `(addr: lightuserdata)` | lightuserdata | Read pointer |
| `ReadFloat` | `(addr: lightuserdata)` | float | Alias for ReadF32 |
| `ReadPointer` | `(addr: lightuserdata)` | lightuserdata | Alias for ReadPtr |
| `WriteU8` | `(addr: lightuserdata, val: int)` | void | Write unsigned 8-bit |
| `WriteU16` | `(addr: lightuserdata, val: int)` | void | Write unsigned 16-bit |
| `WriteU32` | `(addr: lightuserdata, val: uint32)` | void | Write unsigned 32-bit |
| `WriteU64` | `(addr: lightuserdata, val: double)` | void | Write unsigned 64-bit |
| `WriteS32` | `(addr: lightuserdata, val: int32)` | void | Write signed 32-bit |
| `WriteF32` | `(addr: lightuserdata, val: float)` | void | Write 32-bit float |
| `WriteF64` | `(addr: lightuserdata, val: double)` | void | Write 64-bit double |
| `WriteFloat` | `(addr: lightuserdata, val: float)` | void | Alias for WriteF32 |
| `WritePtr` | `(addr: lightuserdata, val: lightuserdata)` | void | Write pointer |
| `AllocateMemory` | `(size: int)` | lightuserdata | Allocate raw memory (use FreeMemory to release) |
| `FreeMemory` | `(addr: uintptr)` | void | Free memory allocated by AllocateMemory |

### Symbol Resolution
*Source: `lua_bindings.cpp`*

| Function | Signature | Return | Description |
|---|---|---|---|
| `GetLibBase` | `()` | lightuserdata | Get base address of libUnreal.so |
| `FindSymbol` | `(name: string)` | lightuserdata\|nil | Find a symbol by name via dlsym |
| `Resolve` | `(name: string, fallbackOffset?: int64)` | lightuserdata\|nil | Resolve symbol or use fallback offset from lib base |
| `FindPattern` | `(pattern: string)` | lightuserdata\|nil | Scan for byte pattern in memory |
| `Offset` | `(base: lightuserdata, offset: int64)` | lightuserdata | Pointer arithmetic: base + offset |

### Delayed Actions & Async
*Source: `lua_delayed_actions.cpp`, `lua_bindings.cpp`*

| Function | Signature | Return | Description |
|---|---|---|---|
| `ExecuteWithDelay` | `(delayMs: double, callback: function)` | uint64 handle | Execute once after delay on game thread |
| `ExecuteAsync` | `(callback: function)` | uint64 handle | Execute on next tick (0ms, one-shot) |
| `LoopAsync` | `(delayMs: double, callback: function)` | uint64 handle | Execute repeatedly every delayMs |
| `ExecuteInGameThread` | `(callback: function)` | void | Queue callback for game thread execution |
| `ExecuteInGameThreadWithDelay` | `(delayMs: double, callback: function)` | uint64 handle | Execute once after delay, game thread |
| `LoopInGameThread` | `(delayMs: double, callback: function)` | uint64 handle | Loop on game thread every delayMs |
| `ExecuteWithDelayFrames` | `(frames: int, callback: function)` | uint64 handle | Execute once after N frames |
| `LoopAsyncFrames` | `(frames: int, callback: function)` | uint64 handle | Loop every N frames |
| `LoopInGameThreadFrames` | `(frames: int, callback: function)` | uint64 handle | Loop on game thread every N frames |
| `ExecuteInGameThreadWithDelayFrames` | `(frames: int, callback: function)` | uint64 handle | Execute once after N frames, game thread |
| `CancelDelayedAction` | `(handle: uint64)` | void | Cancel a delayed/loop action |
| `IsDelayedActionValid` | `(handle: uint64)` | bool | Check if handle is still active |
| `CallAsync` | `(obj: UObject, funcArg: string\|table)` | bool | Call UFunction(s) on background thread |
| `EnableCheatsAsync` | `(pc?: UObject)` | bool | Construct CheatManager on bg thread |

### Console & Cheats
*Source: `lua_console.cpp`*

| Function | Signature | Return | Description |
|---|---|---|---|
| `ConsoleCommand` | `(cmd: string)` | string | Execute a UE console command |
| `ConsoleCommands` | `(...)` | void | Execute multiple console commands |
| `ExecConsoleCommand` | `(cmdString: string)` | string | Execute console command (alternative) |
| `DumpConsoleCommands` | `()` | table | List all registered console commands |
| `GetCheatManager` | `()` | UObject\|nil | Get the CheatManager instance |
| `HasCheatManager` | `()` | bool | Check if CheatManager exists |
| `CheatGod` | `()` | bool | Toggle god mode |
| `CheatFly` | `()` | bool | Toggle fly mode |
| `CheatGhost` | `()` | bool | Toggle ghost mode |
| `CheatWalk` | `()` | bool | Toggle walk mode |
| `CheatTeleport` | `()` | bool | Teleport to crosshair |
| `CheatSlomo` | `(rate: float)` | bool | Set time dilation |
| `CheatChangeSize` | `(factor: float)` | bool | Change player scale |
| `CheatDamageTarget` | `(amount: float)` | bool | Damage looked-at target |
| `CheatDestroyTarget` | `()` | bool | Destroy looked-at target |
| `CheatSummon` | `(className: string)` | bool | Summon actor |
| `CheatPause` | `()` | bool | Toggle pause |
| `CheatToggleDebugCamera` | `()` | bool | Toggle debug camera |
| `GetCVar` | `(name: string)` | string | Get a console variable value |
| `SetCVar` | `(name: string, value: string)` | bool | Set a console variable |
| `GetCrashStats` | `()` | table | Get crash statistics |
| `GetCrashLogPath` | `()` | string | Get crash log file path |
| `GetLogPath` | `()` | string | Get log file path |
| `GetEngineVersion` | `()` | string | Get UE engine version string |
| `GetGameInfo` | `()` | table | Get game info (package, version, etc.) |
| `GetSandboxPaths` | `()` | table | Get all sandbox/data paths |
| `DumpAllFunctions` | `(filter?: string)` | string | Dump all UFunction names (optional filter) |
| `DumpAllProperties` | `(filter?: string)` | string | Dump all FProperty names (optional filter) |

### View Modes & Stats
*Source: `lua_console.cpp`*

| Function | Signature | Return | Description |
|---|---|---|---|
| `SetViewMode` | `(mode: string)` | bool | Set rendering view mode |
| `ViewModeWireframe` | `()` | bool | Switch to wireframe |
| `ViewModeLit` | `()` | bool | Switch to lit |
| `ViewModeUnlit` | `()` | bool | Switch to unlit |
| `ViewModeDetailLighting` | `()` | bool | Switch to detail lighting |
| `ViewModeLightingOnly` | `()` | bool | Switch to lighting only |
| `ViewModeShaderComplexity` | `()` | bool | Switch to shader complexity |
| `ViewModeCollision` | `()` | bool | Switch to collision view |
| `SetShowFlag` | `(flagName: string, value: int)` | bool | Set a show flag |
| `ToggleStat` | `(statName: string)` | bool | Toggle a stat display |
| `StatFPS` | `()` | bool | Toggle FPS stat |
| `StatUnit` | `()` | bool | Toggle unit stat |
| `StatUnitGraph` | `()` | bool | Toggle unit graph |
| `StatEngine` | `()` | bool | Toggle engine stat |
| `StatGame` | `()` | bool | Toggle game stat |

### ProcessEvent Trace
*Source: `lua_console.cpp`*

| Function | Signature | Return | Description |
|---|---|---|---|
| `PETraceStart` | `(filter?: string)` | void | Start tracing ProcessEvent calls |
| `PETraceStop` | `()` | void | Stop tracing |
| `PETraceClear` | `()` | void | Clear trace data |
| `PETraceIsActive` | `()` | bool | Check if tracing is active |
| `PETraceStatus` | `()` | string | Get trace status info |
| `PETraceTop` | `(n?: int)` | string | Get top N most-called functions |
| `PETraceTopByTime` | `(n?: int)` | string | Get top N by cumulative time |
| `PETraceDump` | `()` | string | Dump all trace data |
| `PETraceDetailed` | `(enable: bool)` | void | Enable/disable detailed tracing |
| `PETraceWatch` | `(pattern: string, callback: function)` | uint32 watchId | Watch for specific PE calls |
| `PETraceUnwatch` | `(watchId: uint32)` | void | Remove a watch |
| `PETraceClearWatches` | `()` | void | Clear all watches |

### SDK / Debug Dump
*Source: `lua_bindings.cpp`*

| Function | Signature | Return | Description |
|---|---|---|---|
| `DumpSDK` | `()` | void | Dump SDK to files (Classes, Enums, Structs) |
| `RedumpSDK` | `()` | void | Force re-dump SDK |
| `ForceRedump` | `()` | void | Force immediate redump |
| `DumpSymbols` | `(customPath?: string)` | int | Dump dynamic symbols; returns count |
| `DumpGNames` | `(customPath?: string)` | int | Dump FName table; returns count |
| `DumpGObjects` | `(customPath?: string)` | int | Dump GUObjectArray; returns count |
| `GetObjectCount` | `()` | int32 | Total objects in GUObjectArray |
| `GetLiveObjectCount` | `()` | int32 | Count of live (non-null) objects |
| `GetSDKCounts` | `()` | table | Returns `{classes=N, structs=N, enums=N, functions=N}` |
| `MountPak` | `(pakName: string)` | bool | Mount a .pak file |

### File I/O
*Source: `lua_bindings.cpp`*

| Function | Signature | Return | Description |
|---|---|---|---|
| `ReadTextFile` | `(path: string)` | string\|nil | Read a text file |
| `WriteTextFile` | `(path: string, content: string)` | bool | Write a text file |
| `FileExists` | `(path: string)` | bool | Check if file exists |
| `GetModDir` | `(modName: string)` | string | Get mod's directory path |
| `GetDataDir` | `()` | string | Get the data directory path |

### Misc Utilities
*Source: `lua_bindings.cpp`, `lua_ue4ss_globals.cpp`*

| Function | Signature | Return | Description |
|---|---|---|---|
| `ToHex` | `(addr: lightuserdata)` | string | Format pointer as hex string |
| `IsNull` | `(addr: lightuserdata)` | bool | Check if pointer is null |
| `IsValidPtr` | `(addr: lightuserdata)` | bool | Check if pointer is valid (mapped) |
| `FNameToString` | `(index: int32)` | string | Resolve FName index to string |
| `RegisterCommand` | `(name: string, callback: function)` | void | Register an ADB bridge command |
| `RegisterMenuEntry` | `(id: string, label: string, type: string)` | void | Register debug menu entry |
| `RegisterKeyBind` | `(keyCode: int, callback: function)` | uint64 | Register key binding (Quest: ADB trigger) |
| `RegisterKeyBindWithModifiers` | `(keyCode: int, modifiers: int, callback: function)` | uint64 | Register key bind with modifiers |
| `IsKeyBindPressed` | `(keyCode: int)` | bool | Always false on Quest |
| `RegisterConsoleCommandHandler` | `(cmdName: string, callback: function)` | void | Register console command handler |

### UE4SS Lifecycle Hooks
*Source: `lua_ue4ss_globals.cpp`*

| Function | Signature | Return | Description |
|---|---|---|---|
| `RegisterBeginPlayPreHook` | `(callback: function)` | uint64 | Pre-hook on ReceiveBeginPlay/BeginPlay |
| `RegisterBeginPlayPostHook` | `(callback: function)` | uint64 | Post-hook on ReceiveBeginPlay/BeginPlay |
| `RegisterInitGameStatePreHook` | `(callback: function)` | uint64 | Pre-hook on InitGameState |
| `RegisterInitGameStatePostHook` | `(callback: function)` | uint64 | Post-hook on InitGameState |
| `RegisterLoadMapPreHook` | `(callback: function)` | uint64 | Pre-hook on LoadMap/ServerTravel |
| `RegisterLoadMapPostHook` | `(callback: function)` | uint64 | Post-hook on LoadMap/ServerTravel |
| `RegisterProcessConsoleExecPreHook` | `(callback: function)` | uint64 | Pre-hook on ProcessConsoleExec |
| `RegisterProcessConsoleExecPostHook` | `(callback: function)` | uint64 | Post-hook on ProcessConsoleExec |
| `RegisterCallFunctionByNameWithArgumentsPreHook` | `(callback: function)` | uint64 | Pre-hook |
| `RegisterCallFunctionByNameWithArgumentsPostHook` | `(callback: function)` | uint64 | Post-hook |
| `RegisterULocalPlayerExecPreHook` | `(callback: function)` | uint64 | Pre-hook on ULocalPlayer::Exec |
| `RegisterULocalPlayerExecPostHook` | `(callback: function)` | uint64 | Post-hook on ULocalPlayer::Exec |

### Thread Functions
*Source: `lua_ue4ss_globals.cpp`*

| Function | Signature | Return | Description |
|---|---|---|---|
| `GetCurrentThreadId` | `()` | ThreadId | Get current thread's ID |
| `GetGameThreadId` | `()` | ThreadId | Get game thread's ID |
| `IsInGameThread` | `()` | bool | Check if currently on game thread |

### Shared Variables
*Source: `lua_ue4ss_globals.cpp`*

| Function | Signature | Return | Description |
|---|---|---|---|
| `SetSharedVariable` | `(name: string, value: any)` | void | Set a cross-mod shared variable |
| `GetSharedVariable` | `(name: string)` | any\|nil | Get a cross-mod shared variable |

### UObject Extension Functions
*Source: `lua_ue4ss_types.cpp` — free functions that operate on UObject*

| Function | Signature | Return | Description |
|---|---|---|---|
| `UObject_GetFName` | `(obj: UObject)` | FName | Get FName of object |
| `UObject_GetOuter` | `(obj: UObject)` | UObject\|nil | Get outer object |
| `UObject_IsA` | `(obj: UObject, classArg: string\|UObject)` | bool | Type check |
| `UObject_HasAllFlags` | `(obj: UObject, flags: int32)` | bool | Check all flags set |
| `UObject_HasAnyFlags` | `(obj: UObject, flags: int32)` | bool | Check any flags set |
| `UObject_GetWorld` | `(obj: UObject)` | UObject\|nil | Get world |
| `UObject_type` | `(obj: UObject)` | string | Get UE type string ("UClass", "AActor", etc.) |
| `UObject_GetPropertyValue` | `(obj: UObject, propName: string)` | RemoteUnrealParam | Get typed property wrapper |

---

## Usertypes

### UObject
*Source: `lua_uobject.cpp` — `lua.new_usertype<LuaUObject>("UObject")`*

The core wrapper for all UE4 UObject instances. No constructor (objects come from Find/Create functions).

#### Methods

| Method | Signature | Return | Description |
|---|---|---|---|
| `IsValid` | `()` | bool | Check if pointer is valid |
| `GetName` | `()` | string | Short name (e.g. "BP_Player_C_0") |
| `GetFullName` | `()` | string | Full path name |
| `GetClass` | `()` | UObject | The UClass of this object |
| `GetClassName` | `()` | string | Short class name |
| `GetAddress` | `()` | lightuserdata | Raw pointer |
| `ToHex` | `()` | string | Address as hex string |
| `GetOuter` | `()` | UObject\|nil | Outer/owner object |
| `IsA` | `(classArg: string\|UObject)` | bool | Inheritance check |
| `Cast` | `(className: string)` | UObject\|nil | IsA + return self or nil |
| `GetFName` | `()` | string | Same as GetName |
| `Get` | `(propName: string)` | any | Read property via reflection |
| `Set` | `(propName: string, value: any)` | bool | Write property via reflection |
| `GetProp` | `(propName: string)` | any | Alias for Get |
| `SetProp` | `(propName: string, value: any)` | bool | Alias for Set |
| `Call` | `(funcName: string, ...)` | any | Call UFunction via ProcessEvent |
| `CallFunc` | `(funcName: string, ...)` | any | Alias for Call |
| `CallBg` | `(funcName: string, ...)` | bool | Call on background thread |
| `CallBgRaw` | `(funcName: string, params: lightuserdata, len: int)` | bool | Call with raw param buffer, bg thread |
| `CallRaw` | `(funcName: string, params: lightuserdata, len: int)` | bool | Call with raw param buffer, sync |
| `HookProp` | `(propName: string, callback: function)` | uint64 | Per-instance property hook |
| `HookFunc` | `(funcName: string, pre?: function, post?: function)` | uint64 | Per-instance function hook |

#### Dynamic Access (via __index / __newindex)
- `obj.PropertyName` — reads property (same as `obj:Get("PropertyName")`)
- `obj.PropertyName = value` — writes property (same as `obj:Set("PropertyName", value)`)
- `obj:FunctionName(args)` — calls UFunction (same as `obj:Call("FunctionName", args)`)

#### Metamethods
- `__eq` — pointer equality
- `__tostring` — `"UObject(Name @ 0xADDR)"`

---

### UStruct (LuaUStruct)
*Source: `lua_ustruct.cpp` — `lua.new_usertype<LuaUStruct>("UStruct")`*

Returned by `obj:Get("StructProperty")`. Represents a live UE4 struct (FVector, FRotator, IntPoint, etc.).

#### Methods

| Method | Signature | Return | Description |
|---|---|---|---|
| `IsValid` | `()` | bool | Check if struct data is valid |
| `GetTypeName` | `()` | string | Struct type name (e.g. "IntPoint") |
| `GetSize` | `()` | int32 | Struct size in bytes |
| `IsOwning` | `()` | bool | True if this is an owning copy |
| `Clone` | `()` | UStruct | Create an independent copy |
| `CopyFrom` | `(source: UStruct\|table)` | bool | Copy data from another struct or table |
| `GetFields` | `()` | table | Returns `{fieldName = "typeString", ...}` |

#### Dynamic Access (via __index / __newindex)
- `struct.X` — reads field X
- `struct.X = 100` — writes field X (writes to live UObject memory if non-owning)

#### Metamethods
- `__tostring` — `"UStruct(IntPoint: X=500, Y=2000)"`

---

### TArray (LuaTArray)
*Source: `lua_tarray.cpp` — `lua.new_usertype<LuaTArray>("TArray")`*

Returned by `obj:Get("ArrayProperty")`. **1-indexed** (Lua convention).

#### Methods

| Method | Signature | Return | Description |
|---|---|---|---|
| `GetArrayNum` / `Num` | `()` | int | Number of elements |
| `GetArrayMax` | `()` | int | Allocated capacity |
| `GetArrayAddress` | `()` | uintptr | Address of the TArray struct |
| `GetArrayDataAddress` | `()` | uintptr | Address of data buffer |
| `IsEmpty` | `()` | bool | True if Num == 0 |
| `IsValid` | `()` | bool | True if array pointer is valid |
| `ForEach` | `(callback: function)` | void | Iterate: `callback(index, element)`. Return true to break. |
| `Empty` / `Clear` | `()` | void | Set Num to 0 (does NOT free memory) |
| `Add` | `(value: any)` | int | Append element (requires Num < Max). Returns new 1-based index or 0. |
| `AddFName` | `(value: string\|FName\|int)` | int | Specialized FName append |
| `RemoveAt` | `(index: int)` | bool | Remove at 1-based index, shifts rest down |
| `Insert` | `(index: int, value: any)` | bool | Insert at 1-based index, shifts rest up |
| `Swap` | `(idx1: int, idx2: int)` | bool | Swap two elements |
| `Contains` | `(target: any)` | bool | Check if array contains value (string comparison) |
| `IndexOf` | `(target: any)` | int | Find 1-based index of value, or 0 |
| `ToTable` | `()` | table | Convert to Lua table |

#### Indexing
- `arr[1]` — first element (1-indexed)
- `arr[i] = value` — write element at 1-indexed position
- `#arr` — length

#### Metamethods
- `__len` — element count
- `__tostring` — `"TArray(Num=5, Max=16)"`

---

### TMap (LuaTMap)
*Source: `lua_tarray.cpp` — `lua.new_usertype<LuaTMap>("TMap")`*

Returned by `obj:Get("MapProperty")`.

#### Methods

| Method | Signature | Return | Description |
|---|---|---|---|
| `IsValid` | `()` | bool | Check if map pointer is valid |
| `Num` | `()` | int | Count of valid entries |
| `IsEmpty` | `()` | bool | True if no valid entries |
| `ForEach` | `(callback: function)` | void | Iterate: `callback(key, value)`. Return true to break. |
| `GetByKey` | `(key: any)` | any\|nil | Find value by key (string comparison) |
| `SetByKey` | `(key: any, value: any)` | bool | Overwrite value for existing key |
| `ContainsKey` | `(key: any)` | bool | Check if key exists |
| `Keys` | `()` | table | Array of all keys |
| `Values` | `()` | table | Array of all values |
| `ToTable` | `()` | table | Array of `{key=..., value=...}` pairs |
| `GetDebugInfo` | `()` | string | Layout details for debugging |
| `PeekElement` | `(index: int)` | string | Hex dump of raw element (1-indexed) |

#### Metamethods
- `__len` — valid entry count
- `__tostring` — `"TMap(Num=3)"`

---

### FName (LuaFName)
*Source: `lua_types.cpp` — `lua.new_usertype<LuaFName>("FName")`*

#### Constructors
- `FName()` — empty
- `FName(name: string)` — resolve via FName pool (FNAME_Add)
- `FName(name: string, findType: int)` — 0=FNAME_Find, 1=FNAME_Add
- `FName(index: int32)` — from ComparisonIndex
- `FName(index: int32, findType: int)` — from ComparisonIndex

Also available as global function: `FName(...)` (same signatures)

#### Methods

| Method | Signature | Return | Description |
|---|---|---|---|
| `ToString` | `()` | string | Resolve to string |
| `GetComparisonIndex` | `()` | int32 | Get ComparisonIndex |
| `IsValid` | `()` | bool | Non-zero index or number |

#### Metamethods
- `__tostring` — string representation
- `__eq` — compare by ComparisonIndex

---

### FText (LuaFText)
*Source: `lua_types.cpp` — `lua.new_usertype<LuaFText>("FText")`*

#### Constructors
- `FText()` — empty
- `FText(str: string)` — from string

Also available as global function: `FText(str: string)`

#### Methods

| Method | Signature | Return | Description |
|---|---|---|---|
| `ToString` | `()` | string | Get text content |

#### Metamethods
- `__tostring` — text content

---

### FString (LuaFString)
*Source: `lua_types.cpp` — `lua.new_usertype<LuaFString>("FString")`*

#### Constructors
- `FString()` — empty
- `FString(str: string)` — from string

#### Methods

| Method | Signature | Return | Description |
|---|---|---|---|
| `ToString` | `()` | string | Get string content |
| `Empty` / `Clear` | `()` | void | Clear content |
| `Len` | `()` | int | String length |
| `IsEmpty` | `()` | bool | True if empty |
| `Append` | `(str: string)` | void | Append string |
| `Find` | `(search: string)` | int\|nil | Find substring (1-based index), nil if not found |
| `StartsWith` | `(prefix: string)` | bool | Prefix check |
| `EndsWith` | `(suffix: string)` | bool | Suffix check |
| `ToUpper` | `()` | FString | Uppercase copy |
| `ToLower` | `()` | FString | Lowercase copy |

#### Metamethods
- `__tostring` — string content
- `__len` — length
- `__concat` — concatenation
- `__eq` — string equality

---

### RemoteUnrealParam
*Source: `lua_types.cpp` — `lua.new_usertype<LuaRemoteUnrealParam>("RemoteUnrealParam")`*

Wraps typed access to hook callback parameter memory. No constructor.

#### Methods

| Method | Signature | Return | Description |
|---|---|---|---|
| `get` / `Get` | `()` | any | Read the parameter value |
| `set` | `(value: any)` | void | Write the parameter value |
| `type` | `()` | string | Type name ("UObject", "bool", "int32", "float", "FName", etc.) |

#### Metamethods
- `__tostring` — `"RemoteUnrealParam(typeName)"`

---

### FWeakObjectPtr
*Source: `lua_types.cpp` — `lua.new_usertype<LuaFWeakObjectPtr>("FWeakObjectPtr")`*

No constructor.

#### Methods

| Method | Signature | Return | Description |
|---|---|---|---|
| `get` / `Get` | `()` | UObject\|nil | Resolve weak reference |
| `IsValid` | `()` | bool | Check if reference is still valid |

#### Metamethods
- `__tostring` — `"FWeakObjectPtr(valid)"` or `"FWeakObjectPtr(invalid)"`

---

### FOutputDevice
*Source: `lua_types.cpp` — `lua.new_usertype<LuaFOutputDevice>("FOutputDevice")`*

No constructor.

#### Methods

| Method | Signature | Return | Description |
|---|---|---|---|
| `Log` | `(msg: string)` | void | Log a message through UE4's output device |

#### Metamethods
- `__tostring` — `"FOutputDevice"`

---

### ThreadId
*Source: `lua_types.cpp` — `lua.new_usertype<LuaThreadId>("ThreadId")`*

No constructor. Obtained via `GetCurrentThreadId()` / `GetGameThreadId()`.

#### Methods

| Method | Signature | Return | Description |
|---|---|---|---|
| `ToString` | `()` | string | Thread ID as string |

#### Metamethods
- `__tostring` — thread ID string
- `__eq` — compare thread IDs

---

### LuaParmsAccessor
*Source: `lua_cast.cpp` — `lua.new_usertype<LuaParmsAccessor>("LuaParmsAccessor")`*

Created by `CastParms(parms, "ClassName:FuncName")`. Provides named access to UFunction parameters.

#### Methods

| Method | Signature | Return | Description |
|---|---|---|---|
| `GetParamNames` | `()` | table | Array of `{name, type, offset, size, is_return, is_out, is_const}` |
| `GetReturnValue` | `()` | any | Read the return value |
| `SetReturnValue` | `(value: any)` | bool | Write the return value |
| `ToTable` | `()` | table | All params as `{ParamName = value, ...}` |
| `GetAddress` | `()` | uintptr | Raw parms buffer address |

#### Dynamic Access (via __index / __newindex)
- `parms.DamageAmount` — read parameter by name
- `parms.DamageAmount = 0` — write parameter by name

#### Metamethods
- `__tostring` — `"ParmsAccessor(ClassName:FuncName @ 0xADDR)"`

---

## Global Tables / Namespaces

### Enums (lazy metatable)
*Source: `lua_enum_ext.cpp`*

`Enums` is a global table with a `__index` metatable that lazy-loads enum tables from UE4 reflection.

```lua
Enums.DebugMenuType         -- {NewEnumerator5=0, NewEnumerator0=1, ...}
Enums.ECollisionChannel     -- {ECC_WorldStatic=0, ...}
```

### EFindName
*Source: `lua_enums.cpp`*

| Key | Value |
|---|---|
| `FNAME_Find` | 0 |
| `FNAME_Add` | 1 |

### PropertyTypes
*Source: `lua_enums.cpp`*

| Key | Value |
|---|---|
| `ObjectProperty` | 0 |
| `ObjectPtrProperty` | 1 |
| `Int8Property` | 2 |
| `Int16Property` | 3 |
| `IntProperty` | 4 |
| `Int64Property` | 5 |
| `ByteProperty` | 6 |
| `UInt16Property` | 7 |
| `UInt32Property` | 8 |
| `UInt64Property` | 9 |
| `NameProperty` | 10 |
| `FloatProperty` | 11 |
| `DoubleProperty` | 12 |
| `BoolProperty` | 13 |
| `StrProperty` | 14 |
| `TextProperty` | 15 |
| `ClassProperty` | 16 |
| `SoftClassProperty` | 17 |
| `SoftObjectProperty` | 18 |
| `WeakObjectProperty` | 19 |
| `LazyObjectProperty` | 20 |
| `StructProperty` | 21 |
| `EnumProperty` | 22 |
| `ArrayProperty` | 23 |
| `MapProperty` | 24 |
| `SetProperty` | 25 |
| `InterfaceProperty` | 26 |
| `DelegateProperty` | 27 |
| `MulticastDelegateProperty` | 28 |
| `MulticastInlineDelegateProperty` | 29 |
| `MulticastSparseDelegateProperty` | 30 |
| `FieldPathProperty` | 31 |

### EObjectFlags
*Source: `lua_enums.cpp`*

| Key | Value |
|---|---|
| `RF_NoFlags` | 0x00000000 |
| `RF_Public` | 0x00000001 |
| `RF_Standalone` | 0x00000002 |
| `RF_MarkAsNative` | 0x00000004 |
| `RF_Transactional` | 0x00000008 |
| `RF_ClassDefaultObject` | 0x00000010 |
| `RF_ArchetypeObject` | 0x00000020 |
| `RF_Transient` | 0x00000040 |
| `RF_MarkAsRootSet` | 0x00000080 |
| `RF_TagGarbageTemp` | 0x00000100 |
| `RF_NeedInitialization` | 0x00000200 |
| `RF_NeedLoad` | 0x00000400 |
| `RF_KeepForCooker` | 0x00000800 |
| `RF_NeedPostLoad` | 0x00001000 |
| `RF_NeedPostLoadSubobjects` | 0x00002000 |
| `RF_NewerVersionExists` | 0x00004000 |
| `RF_BeginDestroyed` | 0x00008000 |
| `RF_FinishDestroyed` | 0x00010000 |
| `RF_BeingRegenerated` | 0x00020000 |
| `RF_DefaultSubObject` | 0x00040000 |
| `RF_WasLoaded` | 0x00080000 |
| `RF_TextExportTransient` | 0x00100000 |
| `RF_LoadCompleted` | 0x00200000 |
| `RF_InheritableComponentTemplate` | 0x00400000 |
| `RF_DuplicateTransient` | 0x00800000 |
| `RF_StrongRefOnFrame` | 0x01000000 |
| `RF_NonPIEDuplicateTransient` | 0x02000000 |
| `RF_Dynamic` | 0x04000000 |
| `RF_WillBeLoaded` | 0x08000000 |

### EInternalObjectFlags
*Source: `lua_enums.cpp`*

| Key | Value |
|---|---|
| `None` | 0 |
| `ReachableInCluster` | 1 << 23 |
| `ClusterRoot` | 1 << 24 |
| `Native` | 1 << 25 |
| `Async` | 1 << 26 |
| `AsyncLoading` | 1 << 27 |
| `Unreachable` | 1 << 28 |
| `PendingKill` | 1 << 29 |
| `RootSet` | 1 << 30 |
| `GarbageCollectionKeepFlags` | Native \| Async |
| `AllFlags` | 0x7F800000 |

### Key
*Source: `lua_enums.cpp`*

Windows VK codes for keyboard keys (A-Z, 0-9, F1-F12, arrows, numpad, etc.). On Quest/Android these don't map to physical keys but are provided for UE4SS mod compatibility. See source for full table (~100 keys).

### ModifierKey
*Source: `lua_enums.cpp`*

| Key | Value |
|---|---|
| `SHIFT` | 0x01 |
| `CONTROL` | 0x02 |
| `ALT` | 0x04 |

### ModKeys
*Source: `lua_enums.cpp`*

Alias for ModifierKey (same values).

### ModConfig
*Source: `lua_bindings.cpp` — `lua.create_named_table("ModConfig")`*

Per-mod JSON configuration persistence.

| Method | Signature | Return | Description |
|---|---|---|---|
| `ModConfig.Load` | `(modName: string)` | table\|nil | Load config.json for mod |
| `ModConfig.Save` | `(modName: string, data: table)` | bool | Save config.json for mod |
| `ModConfig.GetPath` | `(modName: string)` | string | Get config file path |

### UClassMethods
*Source: `lua_ue4ss_types.cpp` — `lua.create_named_table("UClassMethods")`*

Helper functions for UClass objects.

| Method | Signature | Return | Description |
|---|---|---|---|
| `GetCDO` | `(classObj: UObject)` | UObject\|nil | Get Class Default Object |
| `IsChildOf` | `(classObj: UObject, parentObj: UObject)` | bool | Inheritance check |
| `GetSuperStruct` | `(classObj: UObject)` | UObject\|nil | Get parent class |
| `ForEachFunction` | `(classObj: UObject, callback: function)` | void | Iterate functions; return true to break |
| `ForEachProperty` | `(classObj: UObject, callback: function)` | void | Iterate properties; return true to break |

### UFunctionMethods
*Source: `lua_ue4ss_types.cpp` — `lua.create_named_table("UFunctionMethods")`*

| Method | Signature | Return | Description |
|---|---|---|---|
| `GetFunctionFlags` | `(funcObj: UObject)` | uint32 | Get function flags |
| `SetFunctionFlags` | `(funcObj: UObject, flags: uint32)` | void | Set function flags |
| `GetNumParms` | `(funcObj: UObject)` | int | Parameter count |
| `GetParmsSize` | `(funcObj: UObject)` | int | Total parameter buffer size |
| `GetNativeFunc` | `(funcObj: UObject)` | lightuserdata | Native function pointer |

### UEnumMethods
*Source: `lua_ue4ss_types.cpp` — `lua.create_named_table("UEnumMethods")`*

| Method | Signature | Return | Description |
|---|---|---|---|
| `GetNameByValue` | `(enumObj: UObject, value: int64)` | string | Look up name for enum value |
| `GetValueByName` | `(enumObj: UObject, name: string)` | number\|nil | Look up value for enum name |
| `ForEachName` | `(enumObj: UObject, callback: function)` | void | Iterate `(name, value)` pairs; return true to break |
| `NumEnums` | `(enumObj: UObject)` | int | Total enum entry count |

---

## Global Variables

*Source: `lua_ue4ss_globals.cpp`*

| Variable | Type | Value | Description |
|---|---|---|---|
| `UE4SS_VERSION_MAJOR` | int | 3 | UE4SS compatibility version major |
| `UE4SS_VERSION_MINOR` | int | 0 | UE4SS compatibility version minor |
| `UE4SS_VERSION_HOTFIX` | int | 0 | UE4SS compatibility version hotfix |
| `MODLOADER_VERSION` | string | "3.0.0-arm64" | Modloader version string |

---

## Symbol Count Summary

| Category | Count |
|---|---|
| **Global Functions** | ~140 |
| **Usertypes** | 12 |
| **Global Tables/Namespaces** | 10 |
| **Global Variables** | 4 |
| **Total Lua-visible symbols** | **~166** |

### Source File Breakdown

| File | What it registers |
|---|---|
| `lua_bindings.cpp` | Core globals: Log, Find*, Construct*, Hook, Memory R/W, SDK dump, File I/O, ModConfig |
| `lua_ue4ss_globals.cpp` | UE4SS compat: print, FindObjects, LoadAsset, Register*Hook lifecycle, threads, shared vars, StaticFindObjectEx, FNameToString, FindStruct |
| `lua_ue4ss_types.cpp` | UObject extension fns, UClassMethods, UFunctionMethods, UEnumMethods tables |
| `lua_cast.cpp` | Cast, IsA, CastParms, LuaParmsAccessor, Hook(), GetAllProperties/Functions, SpawnActor, NewObject, DestroyActor |
| `lua_uobject.cpp` | UObject usertype (Get/Set/Call + __index/__newindex dynamic access) |
| `lua_ustruct.cpp` | UStruct usertype (Clone, CopyFrom, GetFields + __index/__newindex) |
| `lua_tarray.cpp` | TArray usertype (Add, RemoveAt, Insert, ForEach, etc.) + TMap usertype |
| `lua_types.cpp` | FName, FText, FString, RemoteUnrealParam, FWeakObjectPtr, FOutputDevice, ThreadId |
| `lua_enums.cpp` | EFindName, PropertyTypes, EObjectFlags, EInternalObjectFlags, Key, ModifierKey, ModKeys |
| `lua_enum_ext.cpp` | FindEnum, GetEnumTable, GetEnumNames, AppendEnumValue, lazy Enums table |
| `lua_delayed_actions.cpp` | ExecuteWithDelay, LoopAsync, frame-based variants, Cancel/IsValid |
| `lua_console.cpp` | Console commands, cheats, view modes, stats, PE trace, GetCVar/SetCVar, engine info |
