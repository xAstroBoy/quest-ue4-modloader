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
local VERBOSE = true
local function V(...) if VERBOSE then Log(TAG .. " [V] " .. string.format(...)) end end

local function isDefaultObject(obj)
    if not obj then return false end
    local ok, name = pcall(function() return obj:GetName() end)
    return ok and type(name) == "string" and name:sub(1, 9) == "Default__"
end

local function findFirstNonDefault(className)
    local first = nil
    pcall(function() first = FindFirstOf(className) end)
    if first and first:IsValid() and not isDefaultObject(first) then
        return first
    end
    local all = nil
    pcall(function() all = FindAllOf(className) end)
    if all then
        for _, obj in ipairs(all) do
            if obj and obj:IsValid() and not isDefaultObject(obj) then
                return obj
            end
        end
    end
    return nil
end

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
    V("PostHook VR4GamePlayerAmmo:IsReloadInProgress -> false")
    local p = CastParms(parms, "VR4GamePlayerAmmo:IsReloadInProgress")
    if p then p:SetReturnValue(false) end
end)

-- VR4GamePlayerAmmo::IsReloadLingering → false
RegisterPostHook("/Script/Game.VR4GamePlayerAmmo:IsReloadLingering", function(self, func, parms)
    if not state.enabled then return end
    V("PostHook VR4GamePlayerAmmo:IsReloadLingering -> false")
    local p = CastParms(parms, "VR4GamePlayerAmmo:IsReloadLingering")
    if p then p:SetReturnValue(false) end
end)

-- VR4GamePlayerArrow (bow) overrides
RegisterPostHook("/Script/Game.VR4GamePlayerArrow:IsReloadInProgress", function(self, func, parms)
    if not state.enabled then return end
    V("PostHook VR4GamePlayerArrow:IsReloadInProgress -> false")
    local p = CastParms(parms, "VR4GamePlayerArrow:IsReloadInProgress")
    if p then p:SetReturnValue(false) end
end)

RegisterPostHook("/Script/Game.VR4GamePlayerArrow:IsReloadLingering", function(self, func, parms)
    if not state.enabled then return end
    V("PostHook VR4GamePlayerArrow:IsReloadLingering -> false")
    local p = CastParms(parms, "VR4GamePlayerArrow:IsReloadLingering")
    if p then p:SetReturnValue(false) end
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
            V("Native cObjWep_reloadable -> 1")
            return 1
        end)
    Log(TAG .. ": Native hook — cObjWep_reloadable → true")
end)

if sym_InstantReload then
    pcall(function()
        RegisterNativeHook("BeginReload",
            function(self_ptr)
                if not state.enabled then return self_ptr end
                V("Native BeginReload -> InstantReload redirect, self=%s", tostring(self_ptr))
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
    V("toggle: enabled=%s", tostring(state.enabled))
    ModConfig.Save("NoRecharge", state)
    Log(TAG .. ": " .. (state.enabled and "ON" or "OFF"))
    Notify(TAG, state.enabled and "ON" or "OFF")
end)

RegisterCommand("reload_status", function()
    local info = TAG .. ": enabled=" .. tostring(state.enabled)
    -- Find active ammo component
    local ammo = findFirstNonDefault("VR4GamePlayerAmmo")
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
