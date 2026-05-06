-- mods/Patches/main.lua v8.1
-- ═══════════════════════════════════════════════════════════════════════
-- GameInstance Config — Debug mode + quick start on init
--
-- v8.0 — CRASH GUARDS REMOVED (moved to C++ modloader):
--   • UpdateViewTargetInternal crash: now caught by dispatch_full()
--     sigsetjmp/siglongjmp safe-call guard in native_hooks.cpp.
--     Handles dangling pointers AND null targets automatically.
--   • itemInfo NULL crash: now caught by safe-call guard +
--     null-safe ReadXXX/WriteXXX/Offset in lua_bindings.cpp.
--
--   ALL hooked native function crashes are automatically recovered
--   by the modloader's C++ crash guard — no Lua patches needed.
-- ═══════════════════════════════════════════════════════════════════════
local TAG = "Patches"
local VERBOSE = true
local function V(...) if VERBOSE then Log(TAG .. " [V] " .. string.format(...)) end end

local state = { enabled = true }

local saved = ModConfig.Load("Patches")
if saved then
    if saved.enabled ~= nil then state.enabled = saved.enabled end
end

-- ═══════════════════════════════════════════════════════════════════════
-- GAMEINST SETUP — Debug mode + quick start on init
-- ═══════════════════════════════════════════════════════════════════════

RegisterHook("/Script/Game.GameInstance:ReceiveInit", function(Context)
    V("ReceiveInit hook fired")
    if not state.enabled then return end
    local obj = Context:get()
    if not obj or not obj:IsValid() then return end

    local sym_SetDebugMode  = Resolve("SetDebugMode",  0x067DF4F4)
    local sym_SetQuickStart = Resolve("SetQuickStart", 0x067DF654)

    if sym_SetDebugMode then
        V("Calling SetDebugMode on GameInstance")
        pcall(function() CallNative(sym_SetDebugMode, "vpi", obj, 1) end)
    end
    if sym_SetQuickStart then
        V("Calling SetQuickStart on GameInstance")
        pcall(function() CallNative(sym_SetQuickStart, "vpi", obj, 1) end)
    end

    V("Setting NewWeaponWheel=true, ShouldLoadTheatreBox=false")
    pcall(function() obj:Set("NewWeaponWheel", true) end)
    pcall(function() obj:Set("ShouldLoadTheatreBox", false) end)

    Log(TAG .. ": GameInstance configured — debug on, quick start, theatre box OFF")
end)
Log(TAG .. ": RegisterHook — GameInstance:ReceiveInit")

-- ═══════════════════════════════════════════════════════════════════════
-- COMMANDS
-- ═══════════════════════════════════════════════════════════════════════

RegisterCommand("patches", function()
    V("patches command — toggling enabled from %s", tostring(state.enabled))
    state.enabled = not state.enabled
    ModConfig.Save("Patches", state)
    Log(TAG .. ": " .. (state.enabled and "ON" or "OFF"))
    Notify(TAG, state.enabled and "ON" or "OFF")
end)

-- ═══════════════════════════════════════════════════════════════════════
-- DEBUG MENU
-- ═══════════════════════════════════════════════════════════════════════

if SharedAPI and SharedAPI.DebugMenu then
    SharedAPI.DebugMenu.RegisterToggle("Patches", "GameInstance Config",
        function() return state.enabled end,
        function(v) state.enabled = v; ModConfig.Save("Patches", state) end)
end

Log(TAG .. ": v8.1 loaded — GameInstance config (crash guards in C++ modloader)")
