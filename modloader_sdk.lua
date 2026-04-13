---@meta
-- ═══════════════════════════════════════════════════════════════════════════
-- UE4-5 Quest Modloader — EmmyLua/LuaLS Type Annotations
-- Generated for IDE autocomplete, type checking, and documentation.
-- Place in your workspace root alongside your mod scripts.
-- ═══════════════════════════════════════════════════════════════════════════

-- ╔═════════════════════════════════════════════════════════════════════════╗
-- ║  GLOBAL VARIABLES                                                       ║
-- ╚═════════════════════════════════════════════════════════════════════════╝

---@type integer Major version of UE4SS compatibility layer
UE4SS_VERSION_MAJOR = 0
---@type integer Minor version of UE4SS compatibility layer
UE4SS_VERSION_MINOR = 0
---@type integer Hotfix version of UE4SS compatibility layer
UE4SS_VERSION_HOTFIX = 0
---@type string Modloader version string
MODLOADER_VERSION = ""

-- ╔═════════════════════════════════════════════════════════════════════════╗
-- ║  USERTYPES                                                              ║
-- ╚═════════════════════════════════════════════════════════════════════════╝

---@class UObject
---Wrapped UObject pointer. Access properties via `obj.PropertyName` or `obj:Get("PropertyName")`.
---Write properties via `obj.PropertyName = value` or `obj:Set("PropertyName", value)`.
---Call UFunctions via `obj:Call("FunctionName", ...)`.
---@field ptr lightuserdata Raw UObject pointer (internal)
local UObject = {}

---Get a property value by name (via reflection)
---@param name string Property name
---@return any
function UObject:Get(name) end

---Set a property value by name (via reflection)
---@param name string Property name
---@param value any Value to write
---@return boolean success
function UObject:Set(name, value) end

---Call a UFunction on this object via ProcessEvent
---@param funcName string UFunction name
---@param ... any Function arguments (primitives, tables for structs, UObjects)
---@return any returnValue Return value from the function, or nil
function UObject:Call(funcName, ...) end

---Queue a UFunction call on the game thread (fire-and-forget, throttled)
---@param funcName string UFunction name
---@param ... any Function arguments
---@return boolean dispatched
function UObject:CallBg(funcName, ...) end

---Check if this UObject pointer is still valid
---@return boolean
function UObject:IsValid() end

---Get the short name (e.g. "PlayerController_C_0")
---@return string
function UObject:GetName() end

---Get the full object path (e.g. "/Game/Maps/Level.Level:PersistentLevel.PlayerController_C_0")
---@return string
function UObject:GetFullName() end

---Get the UClass of this object (as UObject wrapper)
---@return UObject
function UObject:GetClass() end

---Get the class name string
---@return string
function UObject:GetClassName() end

---Get the raw address of this UObject
---@return integer
function UObject:GetAddress() end

---@class UStruct
---Typed struct field access via reflection. Returned by `obj:Get("StructProp")`.
---Access fields: `struct.X`, `struct.Y`. Write: `struct.X = 100`.
local UStruct = {}

---Check if struct data and type are valid
---@return boolean
function UStruct:IsValid() end

---Get the UE type name (e.g. "FVector", "IntPoint", "FRotator")
---@return string
function UStruct:GetTypeName() end

---Get the struct size in bytes
---@return integer
function UStruct:GetSize() end

---Check if this struct owns its data (owning copy vs live reference)
---@return boolean
function UStruct:IsOwning() end

---Create a deep copy of this struct (owning)
---@return UStruct
function UStruct:Clone() end

---Copy data from another struct or table into this struct
---@param source UStruct|table Source data
---@return boolean success
function UStruct:CopyFrom(source) end

---Get all field names and their type strings
---@return table<string, string> {FieldName = "TypeName", ...}
function UStruct:GetFields() end

---@class TArray
---UE4 TArray wrapper. 1-indexed access: `arr[1]`, `arr[2]`, etc.
---Write elements: `arr[1] = newValue`. Length: `#arr`.
local TArray = {}

---Get the number of elements
---@return integer
function TArray:GetArrayNum() end

---@return integer
function TArray:Num() end

---Get the capacity (max allocated slots)
---@return integer
function TArray:GetArrayMax() end

---Get the raw address of the TArray struct
---@return integer
function TArray:GetArrayAddress() end

---Get the raw address of the data pointer
---@return integer
function TArray:GetArrayDataAddress() end

---Check if the array has zero elements
---@return boolean
function TArray:IsEmpty() end

---Check if the array pointer is valid
---@return boolean
function TArray:IsValid() end

---Iterate all elements. Callback receives (1-based index, element). Return true to break.
---@param callback fun(index: integer, element: any): boolean?
function TArray:ForEach(callback) end

---Set all element count to 0 (does NOT free memory)
function TArray:Empty() end

---Set all element count to 0 (does NOT free memory). Alias for Empty().
function TArray:Clear() end

---Append an element. Requires Num < Max (pre-allocated capacity). Returns new 1-based index or 0 on failure.
---@param value any
---@return integer newIndex 1-based index of new element, or 0
function TArray:Add(value) end

---Append an FName element. Takes string or LuaFName. Returns 1-based index or 0.
---@param value string|FName
---@return integer
function TArray:AddFName(value) end

---Remove element at 1-based index, shifts remaining down. Returns true if removed.
---@param index integer 1-based index
---@return boolean
function TArray:RemoveAt(index) end

---Insert value at 1-based index, shifts existing up. Requires capacity. Returns true if inserted.
---@param index integer 1-based index
---@param value any
---@return boolean
function TArray:Insert(index, value) end

---Swap two elements by 1-based indices.
---@param idx1 integer 1-based index
---@param idx2 integer 1-based index
---@return boolean
function TArray:Swap(idx1, idx2) end

---Check if the array contains a value (by string comparison)
---@param target any
---@return boolean
function TArray:Contains(target) end

---Find the 1-based index of the first matching value, or 0 if not found
---@param target any
---@return integer index 1-based, or 0
function TArray:IndexOf(target) end

---Convert entire array to a Lua table
---@return any[]
function TArray:ToTable() end

---@class TMap
---UE4 TMap wrapper. Sparse array iteration with hash table.
local TMap = {}

---Check if the map pointer is valid
---@return boolean
function TMap:IsValid() end

---Iterate all key-value pairs. Callback receives (key, value). Return true to break.
---@param callback fun(key: any, value: any): boolean?
function TMap:ForEach(callback) end

---Set an existing key's value. Does NOT add new entries. Returns true if key found.
---@param key any
---@param value any
---@return boolean
function TMap:SetByKey(key, value) end

---Get a value by key, or nil if not found
---@param key any
---@return any?
function TMap:GetByKey(key) end

---Get all keys as a Lua table
---@return any[]
function TMap:Keys() end

---Get all values as a Lua table
---@return any[]
function TMap:Values() end

---Get all entries as {key=..., value=...} pairs
---@return {key: any, value: any}[]
function TMap:ToTable() end

---Check if a key exists in the map
---@param key any
---@return boolean
function TMap:ContainsKey(key) end

---Get the number of valid entries
---@return integer
function TMap:Num() end

---Check if the map has no valid entries
---@return boolean
function TMap:IsEmpty() end

---Get internal debug info string
---@return string
function TMap:GetDebugInfo() end

---@class FName
---UE4 FName wrapper (ComparisonIndex + Number)
---@field comparison_index integer
---@field number integer
local FName = {}

---Convert to string representation
---@return string
function FName:to_string() end

---@class FText
---UE4 FText wrapper
local FText = {}

---Convert to string
---@return string
function FText:to_string() end

---@class FString
---UE4 FString wrapper
local FString = {}

---Convert to string
---@return string
function FString:to_string() end

-- ╔═════════════════════════════════════════════════════════════════════════╗
-- ║  LOGGING                                                                ║
-- ╚═════════════════════════════════════════════════════════════════════════╝

---Log an info message
---@param msg string
function Log(msg) end

---Log a warning message
---@param msg string
function LogWarn(msg) end

---Log an error message
---@param msg string
function LogError(msg) end

---Print to log (UE4SS-compatible, supports multiple args)
---@param ... any
function print(...) end

-- ╔═════════════════════════════════════════════════════════════════════════╗
-- ║  OBJECT FINDING & CONSTRUCTION                                          ║
-- ╚═════════════════════════════════════════════════════════════════════════╝

---Find the first instance of a class by short name (e.g. "PlayerController_C")
---@param className string Short class name
---@return UObject?
function FindFirstOf(className) end

---Find all instances of a class by short name
---@param className string Short class name
---@return UObject[]?
function FindAllOf(className) end

---Find a UClass by name. Returns UObject wrapping the UClass.
---@param className string
---@return UObject?
function FindClass(className) end

---Find an object by name (searches GUObjectArray)
---@param name string
---@return UObject?
function FindObject(name) end

---Find object by full path (StaticFindObject)
---@param path string Full object path
---@return UObject?
function StaticFindObject(path) end

---Load a UClass by base class name and path
---@param baseClassName string
---@param path string
---@return UObject?
function StaticLoadClass(baseClassName, path) end

---Load an object by base class name and path
---@param baseClassName string
---@param path string
---@return UObject?
function StaticLoadObject(baseClassName, path) end

---Get the Class Default Object for a class
---@param className string
---@return UObject?
function GetCDO(className) end

---Get the current UWorld context
---@return UObject?
function GetWorldContext() end

---Construct a new UObject via StaticConstructObject
---@param className string Class name or path
---@param outer? UObject Optional outer object
---@param name? string Optional object name
---@return UObject?
function ConstructObject(className, outer, name) end

---Create a new UObject via NewObject (simpler than ConstructObject)
---@param className string
---@param outer? UObject
---@return UObject?
function NewObject(className, outer) end

---Create a UMG widget via WidgetBlueprintLibrary::Create
---@param className string|UObject Widget class name or UClass
---@param owner? UObject Owning player controller
---@return UObject?
function CreateWidget(className, owner) end

---Spawn an actor in the current world
---@param className string
---@param location? {X: number, Y: number, Z: number}
---@param rotation? {Pitch: number, Yaw: number, Roll: number}
---@return UObject?
function SpawnActor(className, location, rotation) end

---Destroy an actor
---@param actor UObject
---@return boolean
function DestroyActor(actor) end

---Mark an object as pending kill
---@param obj UObject
---@return boolean
function MarkPendingKill(obj) end

---Create an invalid/null UObject wrapper
---@return UObject
function CreateInvalidObject() end

---Wrap a raw pointer into a UObject wrapper
---@param ptr lightuserdata
---@return UObject?
function UObjectFromPtr(ptr) end

---Find objects by class with optional flag filtering (UE4SS-compatible)
---@param numToFind? integer Max objects to return
---@param className string
---@param requiredFlags? integer
---@param bannedFlags? integer
---@return UObject[]?
function FindObjects(numToFind, className, requiredFlags, bannedFlags) end

---Load an asset by path
---@param path string
---@return UObject?
function LoadAsset(path) end

---Load an exported object by path
---@param path string
---@return UObject?
function LoadExport(path) end

-- ╔═════════════════════════════════════════════════════════════════════════╗
-- ║  HOOKS & CALLBACKS                                                      ║
-- ╚═════════════════════════════════════════════════════════════════════════╝

---Register a pre+post hook on a UFunction path. Returns (preId, postId).
---@param funcPath string "ClassName:FunctionName" format
---@param callback fun(self: UObject, ...: any): string? Return "BLOCK" to prevent execution
---@return integer preId, integer postId
function RegisterHook(funcPath, callback) end

---Register only a pre-hook on a UFunction path
---@param funcPath string "ClassName:FunctionName"
---@param callback fun(self: UObject, ...: any): string?
---@return integer hookId
function RegisterPreHook(funcPath, callback) end

---Register only a post-hook on a UFunction path
---@param funcPath string "ClassName:FunctionName"
---@param callback fun(self: UObject, ...: any)
---@return integer hookId
function RegisterPostHook(funcPath, callback) end

---Register a ProcessEvent-level hook for a class
---@param className string
---@param funcName string
---@param preCallback? fun(obj: UObject, funcName: string, parms: lightuserdata): string?
---@param postCallback? fun(obj: UObject, funcName: string, parms: lightuserdata)
function RegisterProcessEventHook(className, funcName, preCallback, postCallback) end

---Unregister a hook by ID
---@param id integer
function UnregisterHook(id) end

---HarmonyX-style Hook API with Prefix/Postfix callbacks
---@param funcPath string "ClassName:FunctionName"
---@param opts {Prefix?: fun(instance: UObject, parms: table): string?, Postfix?: fun(instance: UObject, parms: table, result: any)}
---@return table hookInfo {id, Unhook: function}
function Hook(funcPath, opts) end

---Register a native (C++) function hook by symbol name
---@param symbolName string
---@param preCallback? fun(...)
---@param postCallback? fun(...)
---@param signature? string Function signature for parameter parsing
---@return boolean success
function RegisterNativeHook(symbolName, preCallback, postCallback, signature) end

---Register a native hook at a specific address
---@param addr lightuserdata
---@param hookName string
---@param preCallback? fun(...)
---@param postCallback? fun(...)
---@param signature? string
---@return boolean success
function RegisterNativeHookAt(addr, hookName, preCallback, postCallback, signature) end

---Register a callback for when new objects of a class are created
---@param classNameOrObj string|UObject
---@param callback fun(newObj: UObject)
---@return integer hookId
function NotifyOnNewObject(classNameOrObj, callback) end

-- ╔═════════════════════════════════════════════════════════════════════════╗
-- ║  CAST & INTROSPECTION                                                   ║
-- ╚═════════════════════════════════════════════════════════════════════════╝

---Cast a UObject to a specific class type (validates class hierarchy)
---@param obj UObject
---@param className string
---@return UObject?
function Cast(obj, className) end

---Unsafe cast — wraps any lightuserdata as UObject without validation
---@param obj lightuserdata|UObject
---@return UObject?
function UnsafeCast(obj) end

---Check if a UObject is an instance of a class
---@param obj UObject
---@param className string
---@return boolean
function IsA(obj, className) end

---Get the full class hierarchy as a table of class names
---@param obj UObject
---@return string[]
function GetClassHierarchy(obj) end

---Get all properties of an object as {name, type, offset, size, ...} entries
---@param obj UObject
---@return table[]
function GetAllProperties(obj) end

---Get all UFunctions of an object as {name, params, ...} entries
---@param obj UObject
---@return table[]
function GetAllFunctions(obj) end

---Cast a ProcessEvent parms buffer into named fields
---@param parms lightuserdata Raw parms pointer from hook
---@param funcPath string "ClassName:FunctionName"
---@return table parsedParms {ParamName = value, ...}
function CastParms(parms, funcPath) end

---Rebuild class reflection data for a given class name
---@param className string
---@return table? classInfo
function RebuildClass(className) end

-- ╔═════════════════════════════════════════════════════════════════════════╗
-- ║  ENUM API                                                               ║
-- ╚═════════════════════════════════════════════════════════════════════════╝

---Global table of all game enums. Access: `Enums.ECollisionChannel.ECC_WorldStatic`
---@type table<string, table<string, integer>>
Enums = {}

---Find a UEnum by name (returns lightuserdata)
---@param name string
---@return lightuserdata?
function FindEnum(name) end

---Get enum values as a Lua table {Name = Value, ...}
---@param name string
---@return table<string, integer>?
function GetEnumTable(name) end

---Get all known enum names
---@return string[]
function GetEnumNames() end

---Append a new value to a live UEnum object in memory
---@param enumName string
---@param valueName string
---@param value integer
---@return boolean success
function AppendEnumValue(enumName, valueName, value) end

-- ╔═════════════════════════════════════════════════════════════════════════╗
-- ║  TYPE CONSTRUCTORS                                                      ║
-- ╚═════════════════════════════════════════════════════════════════════════╝

---Create an FName from string or comparison index
---@param nameOrIndex string|integer
---@return FName
function FName(nameOrIndex) end

---Create an FText from string
---@param str string
---@return FText
function FText(str) end

-- ╔═════════════════════════════════════════════════════════════════════════╗
-- ║  DELAYED ACTIONS & GAME THREAD                                          ║
-- ╚═════════════════════════════════════════════════════════════════════════╝

---Execute a function after a delay (milliseconds)
---@param delayMs number
---@param callback fun()
---@return integer handle
function ExecuteWithDelay(delayMs, callback) end

---Execute a function on the next tick (async)
---@param callback fun()
---@return integer handle
function ExecuteAsync(callback) end

---Loop a function at interval (milliseconds). Return true from callback to stop.
---@param delayMs number
---@param callback fun(): boolean?
---@return integer handle
function LoopAsync(delayMs, callback) end

---Execute on game thread after delay
---@param delayMs number
---@param callback fun()
---@return integer handle
function ExecuteInGameThreadWithDelay(delayMs, callback) end

---Loop on game thread at interval
---@param delayMs number
---@param callback fun(): boolean?
---@return integer handle
function LoopInGameThread(delayMs, callback) end

---Execute after N frames
---@param frames integer
---@param callback fun()
---@return integer handle
function ExecuteWithDelayFrames(frames, callback) end

---Loop every N frames
---@param frames integer
---@param callback fun(): boolean?
---@return integer handle
function LoopAsyncFrames(frames, callback) end

---Loop on game thread every N frames
---@param frames integer
---@param callback fun(): boolean?
---@return integer handle
function LoopInGameThreadFrames(frames, callback) end

---Execute on game thread after N frames delay
---@param frames integer
---@param callback fun()
---@return integer handle
function ExecuteInGameThreadWithDelayFrames(frames, callback) end

---Cancel a pending delayed action by handle
---@param handle integer
function CancelDelayedAction(handle) end

---Check if a delayed action handle is still active
---@param handle integer
---@return boolean
function IsDelayedActionValid(handle) end

---Queue a function to run on the game thread (next PE tick)
---@param callback fun()
function ExecuteInGameThread(callback) end

---Queue a UFunction call to execute on the game thread (fire-and-forget)
---@param obj UObject
---@param funcName string|fun()
---@return boolean dispatched
function CallAsync(obj, funcName) end

-- ╔═════════════════════════════════════════════════════════════════════════╗
-- ║  MEMORY ACCESS (for byte/instruction patching ONLY)                     ║
-- ╚═════════════════════════════════════════════════════════════════════════╝

---@param addr lightuserdata
---@return integer
function ReadU8(addr) end
---@param addr lightuserdata
---@return integer
function ReadU16(addr) end
---@param addr lightuserdata
---@return integer
function ReadU32(addr) end
---@param addr lightuserdata
---@return number
function ReadU64(addr) end
---@param addr lightuserdata
---@return number
function ReadF32(addr) end
---@param addr lightuserdata
---@return number
function ReadF64(addr) end
---@param addr lightuserdata
---@return lightuserdata
function ReadPtr(addr) end
---@param addr lightuserdata
---@return integer
function ReadS32(addr) end
---@param addr lightuserdata
---@return number
function ReadFloat(addr) end
---@param addr lightuserdata
---@return lightuserdata
function ReadPointer(addr) end

---@param addr lightuserdata
---@param val integer
function WriteU8(addr, val) end
---@param addr lightuserdata
---@param val integer
function WriteU16(addr, val) end
---@param addr lightuserdata
---@param val integer
function WriteU32(addr, val) end
---@param addr lightuserdata
---@param val number
function WriteU64(addr, val) end
---@param addr lightuserdata
---@param val number
function WriteF32(addr, val) end
---@param addr lightuserdata
---@param val number
function WriteF64(addr, val) end
---@param addr lightuserdata
---@param val lightuserdata
function WritePtr(addr, val) end
---@param addr lightuserdata
---@param val integer
function WriteS32(addr, val) end
---@param addr lightuserdata
---@param val number
function WriteFloat(addr, val) end

---Get the base address of the main game library (libUE4.so / libUnreal.so)
---@return lightuserdata
function GetLibBase() end

---Find a dynamic symbol by name
---@param name string
---@return lightuserdata?
function FindSymbol(name) end

---Resolve a symbol by name with optional fallback offset
---@param name string
---@param fallbackOffset? integer
---@return lightuserdata?
function Resolve(name, fallbackOffset) end

---Find a byte pattern in the game binary
---@param pattern string IDA-style pattern (e.g. "48 8B 05 ?? ?? ?? ??")
---@return lightuserdata?
function FindPattern(pattern) end

---Calculate offset from base pointer
---@param base lightuserdata
---@param offset integer
---@return lightuserdata
function Offset(base, offset) end

---Allocate raw memory
---@param size integer
---@return lightuserdata
function AllocateMemory(size) end

---Free raw memory at address
---@param addr integer Address as number
function FreeMemory(addr) end

---Convert pointer to hex string
---@param addr lightuserdata
---@return string
function ToHex(addr) end

---Check if a pointer is null
---@param addr lightuserdata
---@return boolean
function IsNull(addr) end

---Check if a pointer is valid (mapped and readable)
---@param addr lightuserdata
---@return boolean
function IsValidPtr(addr) end

-- ╔═════════════════════════════════════════════════════════════════════════╗
-- ║  CONSOLE & CHEATS                                                       ║
-- ╚═════════════════════════════════════════════════════════════════════════╝

---Execute a console command string
---@param cmd string
---@return string result
function ConsoleCommand(cmd) end

---Execute multiple console commands
---@param ... string
function ConsoleCommands(...) end

---Execute a console command via UEngine::Exec
---@param cmdString string
---@return string result
function ExecConsoleCommand(cmdString) end

---Get the CheatManager UObject
---@return UObject?
function GetCheatManager() end

---Check if a CheatManager is available
---@return boolean
function HasCheatManager() end

---Enable cheats asynchronously
---@param playerController? UObject
---@return boolean success
function EnableCheatsAsync(playerController) end

---@return boolean success
function CheatGod() end
---@return boolean success
function CheatFly() end
---@return boolean success
function CheatGhost() end
---@return boolean success
function CheatWalk() end
---@return boolean success
function CheatTeleport() end
---@param rate number Slomo rate (1.0 = normal)
---@return boolean success
function CheatSlomo(rate) end
---@param factor number Size factor
---@return boolean success
function CheatChangeSize(factor) end
---@param amount number Damage amount
---@return boolean success
function CheatDamageTarget(amount) end
---@return boolean success
function CheatDestroyTarget() end
---@param className string Actor class to summon
---@return boolean success
function CheatSummon(className) end
---@return boolean success
function CheatPause() end
---@return boolean success
function CheatToggleDebugCamera() end

---Set view mode (Wireframe, Lit, Unlit, etc.)
---@param mode string
---@return boolean success
function SetViewMode(mode) end
---@return boolean success
function ViewModeWireframe() end
---@return boolean success
function ViewModeLit() end
---@return boolean success
function ViewModeUnlit() end
---@return boolean success
function ViewModeDetailLighting() end
---@return boolean success
function ViewModeLightingOnly() end
---@return boolean success
function ViewModeShaderComplexity() end
---@return boolean success
function ViewModeCollision() end

---Set a show flag
---@param flagName string
---@param value integer
---@return boolean success
function SetShowFlag(flagName, value) end

---Toggle an engine stat display
---@param statName string
---@return boolean success
function ToggleStat(statName) end

---@return boolean success
function StatFPS() end
---@return boolean success
function StatUnit() end
---@return boolean success
function StatUnitGraph() end
---@return boolean success
function StatEngine() end
---@return boolean success
function StatGame() end

---Set a console variable
---@param name string CVar name
---@param value string CVar value
---@return boolean success
function SetCVar(name, value) end

---Get a console variable value
---@param name string CVar name
---@return string value
function GetCVar(name) end

-- ╔═════════════════════════════════════════════════════════════════════════╗
-- ║  PROCESSEVENT TRACE                                                     ║
-- ╚═════════════════════════════════════════════════════════════════════════╝

---Start recording ProcessEvent calls (optional class/func filter)
---@param filter? string
function PETraceStart(filter) end
function PETraceStop() end
function PETraceClear() end
---@return boolean
function PETraceIsActive() end
---@return string
function PETraceStatus() end
---@param n? integer Top N by call count
---@return string
function PETraceTop(n) end
---@param n? integer Top N by total time
---@return string
function PETraceTopByTime(n) end
---@return string
function PETraceDump() end
---@param enable boolean
function PETraceDetailed(enable) end
---Watch for specific PE calls by pattern. Callback(className, funcName) per call.
---@param pattern string
---@param callback fun(className: string, funcName: string)
---@return integer watchId
function PETraceWatch(pattern, callback) end
---@param watchId integer
function PETraceUnwatch(watchId) end
function PETraceClearWatches() end

-- ╔═════════════════════════════════════════════════════════════════════════╗
-- ║  SDK DUMP & INTROSPECTION                                               ║
-- ╚═════════════════════════════════════════════════════════════════════════╝

---Dump the SDK to disk (classes, enums, structs)
function DumpSDK() end
---Re-dump the SDK (incremental)
function RedumpSDK() end
---Force full re-dump from scratch
function ForceRedump() end
---Get total object count in GUObjectArray
---@return integer
function GetObjectCount() end
---Get live (non-null) object count
---@return integer
function GetLiveObjectCount() end
---Get SDK counts {classes, enums, structs, functions}
---@return table
function GetSDKCounts() end
---Dump dynamic symbol table
---@param customPath? string
---@return integer count
function DumpSymbols(customPath) end
---Dump GNames table
---@param customPath? string
---@return integer count
function DumpGNames(customPath) end
---Dump GObjects table
---@param customPath? string
---@return integer count
function DumpGObjects(customPath) end
---Iterate all objects in GUObjectArray. Callback(obj) — return true to break.
---@param callback fun(obj: UObject): boolean?
function ForEachUObject(callback) end
---Get all rebuilt class names
---@return string[]
function GetKnownClasses() end

---Dump all functions matching optional filter
---@param filter? string
---@return string
function DumpAllFunctions(filter) end

---Dump all properties matching optional filter
---@param filter? string
---@return string
function DumpAllProperties(filter) end

---Dump console commands
---@return table
function DumpConsoleCommands() end

-- ╔═════════════════════════════════════════════════════════════════════════╗
-- ║  FILE I/O                                                               ║
-- ╚═════════════════════════════════════════════════════════════════════════╝

---Read a text file from the sandbox
---@param path string
---@return string?
function ReadTextFile(path) end

---Write a text file to the sandbox
---@param path string
---@param content string
---@return boolean success
function WriteTextFile(path, content) end

---Check if a file exists in the sandbox
---@param path string
---@return boolean
function FileExists(path) end

---Get the mod directory path for a given mod name
---@param modName string
---@return string
function GetModDir(modName) end

---Get the data directory path
---@return string
function GetDataDir() end

-- ╔═════════════════════════════════════════════════════════════════════════╗
-- ║  PAK MOUNTING                                                           ║
-- ╚═════════════════════════════════════════════════════════════════════════╝

---Mount a .pak file from the mods directory
---@param pakName string Pak filename
---@return boolean success
function MountPak(pakName) end

-- ╔═════════════════════════════════════════════════════════════════════════╗
-- ║  GAME INFO                                                              ║
-- ╚═════════════════════════════════════════════════════════════════════════╝

---Get game/engine info table
---@return table {package_name, engine_version, ...}
function GetGameInfo() end

---Get sandbox file paths
---@return table {data_dir, mod_dir, log_dir, ...}
function GetSandboxPaths() end

---Get the engine version string
---@return string
function GetEngineVersion() end

---Get crash statistics
---@return table {total_crashes, last_crash, ...}
function GetCrashStats() end

---Get the crash log file path
---@return string
function GetCrashLogPath() end

---Get the main log file path
---@return string
function GetLogPath() end

-- ╔═════════════════════════════════════════════════════════════════════════╗
-- ║  NOTIFICATIONS                                                          ║
-- ╚═════════════════════════════════════════════════════════════════════════╝

---Send a notification (Android toast)
---@param title string
---@param body string
---@param duration? number Duration in seconds
function Notify(title, body, duration) end

-- ╔═════════════════════════════════════════════════════════════════════════╗
-- ║  UE4SS COMPATIBILITY — LIFECYCLE HOOKS                                  ║
-- ╚═════════════════════════════════════════════════════════════════════════╝

---@param callback fun(context: UObject)
---@return integer hookId
function RegisterBeginPlayPreHook(callback) end
---@param callback fun(context: UObject)
---@return integer hookId
function RegisterBeginPlayPostHook(callback) end
---@param callback fun(context: UObject)
---@return integer hookId
function RegisterInitGameStatePreHook(callback) end
---@param callback fun(context: UObject)
---@return integer hookId
function RegisterInitGameStatePostHook(callback) end
---@param callback fun(context: UObject)
---@return integer hookId
function RegisterLoadMapPreHook(callback) end
---@param callback fun(context: UObject)
---@return integer hookId
function RegisterLoadMapPostHook(callback) end
---@param callback fun(context: UObject, cmd: string, executor: UObject, ar: UObject): boolean?
---@return integer hookId
function RegisterProcessConsoleExecPreHook(callback) end
---@param callback fun(context: UObject, cmd: string, executor: UObject, ar: UObject)
---@return integer hookId
function RegisterProcessConsoleExecPostHook(callback) end
---@param callback fun(context: UObject, name: string, args: string, ar: UObject, executor: UObject): boolean?
---@return integer hookId
function RegisterCallFunctionByNameWithArgumentsPreHook(callback) end
---@param callback fun(context: UObject, name: string, args: string, ar: UObject, executor: UObject)
---@return integer hookId
function RegisterCallFunctionByNameWithArgumentsPostHook(callback) end
---@param callback fun(player: UObject, cmd: string, ar: UObject): boolean?
---@return integer hookId
function RegisterULocalPlayerExecPreHook(callback) end
---@param callback fun(player: UObject, cmd: string, ar: UObject)
---@return integer hookId
function RegisterULocalPlayerExecPostHook(callback) end

---Register a key bind (VR controller)
---@param keyCode integer
---@param callback fun()
---@return integer hookId
function RegisterKeyBind(keyCode, callback) end

---Register a key bind with modifier keys
---@param keyCode integer
---@param modifiers integer
---@param callback fun()
---@return integer hookId
function RegisterKeyBindWithModifiers(keyCode, modifiers, callback) end

---Check if a key is currently pressed
---@param keyCode integer
---@return boolean
function IsKeyBindPressed(keyCode) end

---Register a named console command handler
---@param cmdName string
---@param callback fun(fullCmd: string, args: string[]): boolean
function RegisterConsoleCommandHandler(cmdName, callback) end

---Register a bridge command handler (accessible via ADB bridge)
---@param name string
---@param callback fun(args: table): any
function RegisterCommand(name, callback) end

---Register a menu entry for debug menu integration
---@param id string
---@param label string
---@param callback fun()
function RegisterMenuEntry(id, label, callback) end

-- ╔═════════════════════════════════════════════════════════════════════════╗
-- ║  THREADING                                                              ║
-- ╚═════════════════════════════════════════════════════════════════════════╝

---Get the current thread ID
---@return integer
function GetCurrentThreadId() end

---Get the game thread ID
---@return integer
function GetGameThreadId() end

---Check if currently on the game thread
---@return boolean
function IsInGameThread() end

-- ╔═════════════════════════════════════════════════════════════════════════╗
-- ║  SHARED VARIABLES (cross-mod communication)                             ║
-- ╚═════════════════════════════════════════════════════════════════════════╝

---Set a shared variable accessible across mods
---@param name string
---@param value any
function SetSharedVariable(name, value) end

---Get a shared variable
---@param name string
---@return any
function GetSharedVariable(name) end

-- ╔═════════════════════════════════════════════════════════════════════════╗
-- ║  NATIVE CALLS                                                           ║
-- ╚═════════════════════════════════════════════════════════════════════════╝

---Call a native function at a given address with signature
---@param addr lightuserdata
---@param sig string Signature string
---@param ... any
---@return any
function CallNative(addr, sig, ...) end

---Call a native function by symbol name with signature
---@param symbol string
---@param sig string
---@param ... any
---@return any
function CallNativeBySymbol(symbol, sig, ...) end

-- ╔═════════════════════════════════════════════════════════════════════════╗
-- ║  UE4SS COMPAT — UObject free functions                                  ║
-- ╚═════════════════════════════════════════════════════════════════════════╝

---Get FName of a UObject
---@param obj UObject
---@return FName?
function UObject_GetFName(obj) end

---Get the outer object of a UObject
---@param obj UObject
---@return UObject?
function UObject_GetOuter(obj) end

---Check if UObject is an instance of a class (free function form)
---@param obj UObject
---@param classNameOrObj string|UObject
---@return boolean
function UObject_IsA(obj, classNameOrObj) end

---Check if UObject has all specified internal flags
---@param obj UObject
---@param flags integer
---@return boolean
function UObject_HasAllFlags(obj, flags) end

---Check if UObject has any of the specified internal flags
---@param obj UObject
---@param flags integer
---@return boolean
function UObject_HasAnyFlags(obj, flags) end

---Get the UWorld associated with a UObject
---@param obj UObject
---@return UObject?
function UObject_GetWorld(obj) end

---Get the type name string of a UObject
---@param obj UObject
---@return string
function UObject_type(obj) end

---Get a property value from a UObject (free function form)
---@param obj UObject
---@param propName string
---@return any
function UObject_GetPropertyValue(obj, propName) end

-- ╔═════════════════════════════════════════════════════════════════════════╗
-- ║  GLOBAL TABLES — ENUM CONSTANTS                                         ║
-- ╚═════════════════════════════════════════════════════════════════════════╝

---EFindName constants for FName lookup
---@type {FNAME_Find: integer, FNAME_Add: integer}
EFindName = {}

---PropertyType classification IDs
---@type table<string, integer>
PropertyTypes = {}

---EObjectFlags constants
---@type table<string, integer>
EObjectFlags = {}

---EInternalObjectFlags constants
---@type table<string, integer>
EInternalObjectFlags = {}

---Key code constants (VR controller / keyboard)
---@type table<string, integer>
Key = {}

---Modifier key constants
---@type table<string, integer>
ModifierKey = {}

---@type table<string, integer>
ModKeys = {}

---UClassMethods utility functions
---@type table
UClassMethods = {}

---UFunctionMethods utility functions
---@type table
UFunctionMethods = {}

---UEnumMethods utility functions
---@type table
UEnumMethods = {}

---ModConfig — per-mod persistent configuration
---@type table
ModConfig = {}
