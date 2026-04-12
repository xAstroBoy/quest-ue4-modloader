-- mods/NoRecharge/main.lua v3.0
-- ═══════════════════════════════════════════════════════════════════════
-- UE4SS-style Instant Reload — hooks reload state UFunctions via
-- RegisterPostHook. Uses FindFirstOf for ammo/gun inspection.
--
-- v3.0 — Full UE4SS API:
--   RegisterPostHook on VR4GamePlayerAmmo/Arrow reload UFunctions
--   FindFirstOf("VR4GamePlayerAmmo") for reload state inspection
--   Native hooks on BeginReload → InstantReload redirect
-- ═══════════════════════════════════════════════════════════════════════
local TAG = "NoRecharge"

local state = {
    enabled = true,
}

local saved = ModConfig.Load("NoRecharge")
if saved then
    if saved.enabled ~= nil then state.enabled = saved.enabled end
end

-- ═══════════════════════════════════════════════════════════════════════
-- UE4SS POST-HOOKS — Override reload state returns
-- ═══════════════════════════════════════════════════════════════════════

-- VR4GamePlayerAmmo::IsReloadInProgress → false (skip reload animation)
RegisterPostHook("/Script/Game.VR4GamePlayerAmmo:IsReloadInProgress", function(self, func, parms)
    if not state.enabled then return end
    WriteU8(parms, 0)
end)

-- VR4GamePlayerAmmo::IsReloadLingering → false
RegisterPostHook("/Script/Game.VR4GamePlayerAmmo:IsReloadLingering", function(self, func, parms)
    if not state.enabled then return end
    WriteU8(parms, 0)
end)

-- VR4GamePlayerArrow (bow) overrides
RegisterPostHook("/Script/Game.VR4GamePlayerArrow:IsReloadInProgress", function(self, func, parms)
    if not state.enabled then return end
    WriteU8(parms, 0)
end)

RegisterPostHook("/Script/Game.VR4GamePlayerArrow:IsReloadLingering", function(self, func, parms)
    if not state.enabled then return end
    WriteU8(parms, 0)
end)

Log(TAG .. ": 4 UE4SS RegisterPostHook on VR4GamePlayerAmmo/Arrow reload UFunctions")

-- ═══════════════════════════════════════════════════════════════════════
-- NATIVE HOOKS — BeginReload → InstantReload redirect
-- ═══════════════════════════════════════════════════════════════════════

local sym_InstantReload = Resolve("InstantReload", 0x066DCA7C)

pcall(function()
    RegisterNativeHook("cObjWep_reloadable", nil,
        function(retval)
            if not state.enabled then return retval end
            return 1
        end)
    Log(TAG .. ": Native hook — cObjWep_reloadable → true")
end)

if sym_InstantReload then
    pcall(function()
        RegisterNativeHook("BeginReload",
            function(self_ptr)
                if not state.enabled then return self_ptr end
                CallNative(sym_InstantReload, "vp", self_ptr)
                return self_ptr
            end, nil)
        Log(TAG .. ": Native hook — BeginReload → InstantReload redirect")
    end)
end

-- ═══════════════════════════════════════════════════════════════════════
-- COMMANDS — UE4SS ammo/gun inspection
-- ═══════════════════════════════════════════════════════════════════════

RegisterCommand("norecharge", function()
    state.enabled = not state.enabled
    ModConfig.Save("NoRecharge", state)
    Log(TAG .. ": " .. (state.enabled and "ON" or "OFF"))
    Notify(TAG, state.enabled and "ON" or "OFF")
end)

RegisterCommand("reload_status", function()
    local info = TAG .. ": enabled=" .. tostring(state.enabled)
    -- Find active ammo component
    local ammo = FindFirstOf("VR4GamePlayerAmmo")
    if ammo and ammo:IsValid() then
        pcall(function()
            local dur = ammo.ReloadDuration
            if dur then info = info .. " | ReloadDuration=" .. tostring(dur) end
        end)
        pcall(function()
            local gun = ammo.OwningGun
            if gun and gun:IsValid() then
                info = info .. " | OwningGun=valid"
                local useMag = gun.UsesMagazines
                if useMag ~= nil then info = info .. " UsesMagazines=" .. tostring(useMag) end
            end
        end)
    end
    Log(info)
end)

if SharedAPI and SharedAPI.DebugMenu then
    SharedAPI.DebugMenu.RegisterToggle("NoRecharge", "Instant Reload",
        function() return state.enabled end,
        function(v) state.enabled = v; ModConfig.Save("NoRecharge", state) end)
end

Log(TAG .. ": v3.0 loaded — UE4SS PostHook reload overrides + native InstantReload redirect + ammo inspection")
