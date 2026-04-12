-- mods/AshleyArmor/main.lua v1.0
-- ═══════════════════════════════════════════════════════════════════════
-- Ashley Armor — blocks all damage to Ashley via RegisterPreHook
-- on Bio4Utils:HurtAshley.
-- ═══════════════════════════════════════════════════════════════════════
local TAG = "AshleyArmor"

local state = { enabled = true }

local saved = ModConfig.Load("AshleyArmor")
if saved then
    if saved.enabled ~= nil then state.enabled = saved.enabled end
end

-- ═══════════════════════════════════════════════════════════════════════
-- UE4SS PRE-HOOK — Block Ashley damage
-- ═══════════════════════════════════════════════════════════════════════

RegisterPreHook("/Script/Game.Bio4Utils:HurtAshley", function(self, func, parms)
    if not state.enabled then return end
    return "BLOCK"
end)

Log(TAG .. ": RegisterPreHook — Bio4Utils:HurtAshley")

-- ═══════════════════════════════════════════════════════════════════════
-- COMMAND
-- ═══════════════════════════════════════════════════════════════════════

RegisterCommand("ashleyarmor", function()
    state.enabled = not state.enabled
    ModConfig.Save("AshleyArmor", state)
    Log(TAG .. ": " .. (state.enabled and "ON" or "OFF"))
    Notify(TAG, state.enabled and "ON" or "OFF")
end)

-- ═══════════════════════════════════════════════════════════════════════
-- DEBUG MENU
-- ═══════════════════════════════════════════════════════════════════════

if SharedAPI and SharedAPI.DebugMenu then
    SharedAPI.DebugMenu.RegisterToggle("AshleyArmor", "Ashley Armor",
        function() return state.enabled end,
        function(v) state.enabled = v; ModConfig.Save("AshleyArmor", state) end)
end

Log(TAG .. ": v1.0 loaded — Ashley invincibility")
