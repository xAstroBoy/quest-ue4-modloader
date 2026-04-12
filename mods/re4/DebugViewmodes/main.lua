-- mods/DebugViewmodes/main.lua v1.2
-- ═══════════════════════════════════════════════════════════════════════
-- Debug Viewmodes — enables debug viewmodes + CVar debug flags.
-- GameInstance config moved to Patches mod.
-- ═══════════════════════════════════════════════════════════════════════
local TAG = "DebugViewmodes"

local state = { enabled = true }

local saved = ModConfig.Load("DebugViewmodes")
if saved then
    if saved.enabled ~= nil then state.enabled = saved.enabled end
end

-- ═══════════════════════════════════════════════════════════════════════
-- NATIVE HOOKS — AllowDebugViewmodes
-- ═══════════════════════════════════════════════════════════════════════

pcall(function()
    RegisterNativeHook("AllowDebugViewmodes1", nil,
        function(retval)
            if not state.enabled then return retval end
            return 1
        end)
end)

pcall(function()
    RegisterNativeHook("_Z19AllowDebugViewmodesv", nil,
        function(retval)
            if not state.enabled then return retval end
            return 1
        end)
end)

Log(TAG .. ": Native hooks — AllowDebugViewmodes")

-- ═══════════════════════════════════════════════════════════════════════
-- CVAR DEBUG FLAGS
-- ═══════════════════════════════════════════════════════════════════════

pcall(function()
    local cvar_addr = Offset(GetLibBase(), 0x0ABD6F18)
    WriteU32(cvar_addr, 1)
    WriteU32(Offset(cvar_addr, 4), 1)
    Log(TAG .. ": CVar debug flags enabled")
end)

-- GameInstance setup has been moved to Patches mod

-- ═══════════════════════════════════════════════════════════════════════
-- COMMAND
-- ═══════════════════════════════════════════════════════════════════════

RegisterCommand("debugview", function()
    state.enabled = not state.enabled
    ModConfig.Save("DebugViewmodes", state)
    Log(TAG .. ": " .. (state.enabled and "ON" or "OFF"))
    Notify(TAG, state.enabled and "ON" or "OFF")
end)

-- ═══════════════════════════════════════════════════════════════════════
-- DEBUG MENU
-- ═══════════════════════════════════════════════════════════════════════

if SharedAPI and SharedAPI.DebugMenu then
    SharedAPI.DebugMenu.RegisterToggle("DebugViewmodes", "Debug Viewmodes",
        function() return state.enabled end,
        function(v) state.enabled = v; ModConfig.Save("DebugViewmodes", state) end)
end

Log(TAG .. ": v1.2 loaded — viewmodes + CVar (GameInstance → Patches)")
