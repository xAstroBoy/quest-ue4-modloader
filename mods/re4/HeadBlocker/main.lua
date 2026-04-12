-- mods/HeadBlocker/main.lua v1.0
-- ═══════════════════════════════════════════════════════════════════════
-- Head Blocker Remover — destroys VR4HeadBlocker actors on spawn
-- via NotifyOnNewObject + K2_DestroyActor.
-- ═══════════════════════════════════════════════════════════════════════
local TAG = "HeadBlocker"

local state = { enabled = true }

local saved = ModConfig.Load("HeadBlocker")
if saved then
    if saved.enabled ~= nil then state.enabled = saved.enabled end
end

-- ═══════════════════════════════════════════════════════════════════════
-- NOTIFY ON NEW OBJECT — Destroy VR4HeadBlocker on spawn
-- ═══════════════════════════════════════════════════════════════════════

local destroyed = 0

NotifyOnNewObject("VR4HeadBlocker", function(obj)
    if not state.enabled then return end
    if not obj:IsValid() then return end

    local ok, err = pcall(function() obj:K2_DestroyActor() end)
    if ok then
        destroyed = destroyed + 1
        Log(TAG .. ": VR4HeadBlocker destroyed (total: " .. destroyed .. ")")
    else
        LogWarn(TAG .. ": K2_DestroyActor failed: " .. tostring(err))
        pcall(function() obj:SetActorHiddenInGame(true) end)
        Log(TAG .. ": HeadBlocker hidden as fallback")
    end
end)

-- ═══════════════════════════════════════════════════════════════════════
-- COMMAND
-- ═══════════════════════════════════════════════════════════════════════

RegisterCommand("headblocker", function()
    state.enabled = not state.enabled
    ModConfig.Save("HeadBlocker", state)
    Log(TAG .. ": " .. (state.enabled and "OFF" or "ON"))
    Notify(TAG, state.enabled and "Blocker OFF" or "Blocker ON")
end)

-- ═══════════════════════════════════════════════════════════════════════
-- DEBUG MENU
-- ═══════════════════════════════════════════════════════════════════════

if SharedAPI and SharedAPI.DebugMenu then
    SharedAPI.DebugMenu.RegisterToggle("HeadBlocker", "Head Blocker Off",
        function() return state.enabled end,
        function(v) state.enabled = v; ModConfig.Save("HeadBlocker", state) end)
end

Log(TAG .. ": v1.0 loaded — VR4HeadBlocker destroy on spawn")
