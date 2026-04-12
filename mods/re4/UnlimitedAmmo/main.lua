-- mods/UnlimitedAmmo/main.lua v5.0
-- ═══════════════════════════════════════════════════════════════════════
-- UE4SS-style Unlimited Ammo — hooks all gun ammo UFunctions via
-- RegisterPostHook + uses FindFirstOf/FindAllOf for gun discovery.
--
-- v5.0 — Full UE4SS API:
--   RegisterPostHook on VR4GamePlayerGun ammo UFunctions
--   FindFirstOf / FindAllOf for gun instance access
--   Native hooks as backup for stripped C++ ammo checks
-- ═══════════════════════════════════════════════════════════════════════
local TAG = "UnlimitedAmmo"

local state = {
    enabled = true,
}

local saved = ModConfig.Load("UnlimitedAmmo")
if saved then
    if saved.enabled ~= nil then state.enabled = saved.enabled end
end

-- ═══════════════════════════════════════════════════════════════════════
-- UE4SS POST-HOOKS — Override ammo UFunction return values
-- ═══════════════════════════════════════════════════════════════════════

-- VR4GamePlayerGun::AmmoAvailable → force true
RegisterPostHook("/Script/Game.VR4GamePlayerGun:AmmoAvailable", function(self, func, parms)
    if not state.enabled then return end
    WriteU8(parms, 1)
end)

-- VR4GamePlayerBow::AmmoAvailable → force true (bow uses arrows)
RegisterPostHook("/Script/Game.VR4GamePlayerBow:AmmoAvailable", function(self, func, parms)
    if not state.enabled then return end
    WriteU8(parms, 1)
end)

-- VR4GamePlayerGun::UsesAmmo → force true (ensure ammo system stays active)
RegisterPostHook("/Script/Game.VR4GamePlayerGun:UsesAmmo", function(self, func, parms)
    if not state.enabled then return end
    WriteU8(parms, 1)
end)

-- VR4GamePlayerGun::GetCurrentAmmo → 999
RegisterPostHook("/Script/Game.VR4GamePlayerGun:GetCurrentAmmo", function(self, func, parms)
    if not state.enabled then return end
    WriteS32(Offset(parms, 4), 999)
end)

-- VR4GamePlayerGun::GetAmmoReserves → 999
RegisterPostHook("/Script/Game.VR4GamePlayerGun:GetAmmoReserves", function(self, func, parms)
    if not state.enabled then return end
    WriteS32(parms, 999)
end)

-- VR4GamePlayerGun::GetAmmoCapacity → 999
RegisterPostHook("/Script/Game.VR4GamePlayerGun:GetAmmoCapacity", function(self, func, parms)
    if not state.enabled then return end
    WriteS32(parms, 999)
end)

-- VR4GamePlayerGun::GetTotalAvailableAmmo → 999
RegisterPostHook("/Script/Game.VR4GamePlayerGun:GetTotalAvailableAmmo", function(self, func, parms)
    if not state.enabled then return end
    WriteS32(parms, 999)
end)

-- VR4GamePlayerGun::GetWeaponItemAmmo → 999
RegisterPostHook("/Script/Game.VR4GamePlayerGun:GetWeaponItemAmmo", function(self, func, parms)
    if not state.enabled then return end
    WriteS32(parms, 999)
end)

Log(TAG .. ": 8 UE4SS RegisterPostHook on VR4GamePlayerGun/Bow ammo UFunctions")

-- ═══════════════════════════════════════════════════════════════════════
-- NATIVE HOOK BACKUP — catches direct C++ ammo checks (stripped symbols)
-- ═══════════════════════════════════════════════════════════════════════

pcall(function()
    RegisterNativeHook("CheckForOutOfAmmo",
        function(self_ptr) if not state.enabled then return self_ptr end; return self_ptr end,
        nil)
end)

pcall(function()
    RegisterNativeHook("IsAmmoAvailable", nil,
        function(retval) if not state.enabled then return retval end; return 1 end)
end)

local bulletFuncs = {
    "cItemMgr_bulletNumCurrent", "cItemMgr_bulletNum",
    "cItemMgr_bulletNum_ITEM_ID", "cItemMgr_bulletNum_cItem",
    "cItemMgr_bulletNumTotal", "cObjWep_bulletNum",
}
for _, fn in ipairs(bulletFuncs) do
    pcall(function()
        RegisterNativeHook(fn, nil,
            function(retval) if not state.enabled then return retval end; return 999 end)
    end)
end

Log(TAG .. ": Native hooks on bulletNum/IsAmmoAvailable (backup)")

-- ═══════════════════════════════════════════════════════════════════════
-- COMMANDS — with UE4SS gun discovery
-- ═══════════════════════════════════════════════════════════════════════

RegisterCommand("unlimitedammo", function()
    state.enabled = not state.enabled
    ModConfig.Save("UnlimitedAmmo", state)
    Log(TAG .. ": " .. (state.enabled and "ON" or "OFF"))
    Notify(TAG, state.enabled and "ON" or "OFF")
end)

RegisterCommand("ammo_status", function()
    local info = TAG .. ": enabled=" .. tostring(state.enabled)
    -- Use FindAllOf to count active guns
    local guns = FindAllOf("VR4GamePlayerGun")
    if guns then
        info = info .. " | Active guns: " .. #guns
        for i, gun in ipairs(guns) do
            if gun and gun:IsValid() then
                pcall(function()
                    local wtype = gun:GetWeaponType()
                    local wno = gun:GetWeaponNo()
                    if wtype then info = info .. " [" .. tostring(wtype) .. "#" .. tostring(wno) .. "]" end
                end)
            end
        end
    end
    Log(info)
end)

if SharedAPI and SharedAPI.DebugMenu then
    SharedAPI.DebugMenu.RegisterToggle("UnlimitedAmmo", "Unlimited Ammo",
        function() return state.enabled end,
        function(v) state.enabled = v; ModConfig.Save("UnlimitedAmmo", state) end)
end

Log(TAG .. ": v5.0 loaded — UE4SS PostHook ammo overrides + native backup + FindAllOf gun discovery")
