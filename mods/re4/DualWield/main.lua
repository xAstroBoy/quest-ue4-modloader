-- mods/DualWield/main.lua v4.1
-- ═══════════════════════════════════════════════════════════════════════
-- UE4SS-enhanced Dual Wield — grab any weapon in both hands.
--
-- v4.0 — UE4SS enhancements:
--   FindFirstOf("VR4GamePlayerPawn") for hand state inspection
--   FindAllOf("VR4GamePlayerGun") for active gun listing
--   RegisterHook on EquipProp for equip monitoring
--   Native Dobby hooks on IsPresentOnBody/CanGrab/IsGrabAllowed
-- ═══════════════════════════════════════════════════════════════════════
local TAG = "DualWield"

local state = {
    enabled = true,
}

local saved = ModConfig.Load("DualWield")
if saved then
    if saved.enabled ~= nil then state.enabled = saved.enabled end
end

-- ═══════════════════════════════════════════════════════════════════════
-- UE4SS HOOK — Monitor equip events
-- ═══════════════════════════════════════════════════════════════════════

RegisterHook("/Script/Game.VR4GamePlayerPawn:EquipProp", function(Context, Parms)
    if not state.enabled then return end
    local self = Context:get()
    if self and self:IsValid() then
        pcall(function()
            local slot = self:GetCurrentEquippedPropSlot()
            local hand = self:GetWeaponHand()
            if slot and hand then
                Log(TAG .. ": EquipProp slot=" .. tostring(slot)
                    .. " hand=" .. tostring(hand))
            end
        end)
    end
end)
Log(TAG .. ": RegisterHook — VR4GamePlayerPawn:EquipProp (monitor)")

-- ═══════════════════════════════════════════════════════════════════════
-- NATIVE DOBBY HOOKS — Allow grabbing duplicates
-- ═══════════════════════════════════════════════════════════════════════

local sym_IsPresentOnBody = Resolve("IsPresentOnBody", 0x048F1A8)
if sym_IsPresentOnBody then
    pcall(function()
        RegisterNativeHook("IsPresentOnBody", nil,
            function(retval)
                if not state.enabled then return retval end
                return 1
            end)
        Log(TAG .. ": Native hook — IsPresentOnBody → true (allow duplicates)")
    end)
else
    LogWarn(TAG .. ": IsPresentOnBody not found — trying ELF patterns")
    local alt_names = {
        "_ZNK19UVR4GamePlayerProp15IsPresentOnBodyEv",
        "_ZNK18VR4GamePlayerProp15IsPresentOnBodyEv",
        "_ZN19UVR4GamePlayerProp15IsPresentOnBodyEv",
        "_ZN18VR4GamePlayerProp15IsPresentOnBodyEv",
    }
    local found = false
    for _, sym_name in ipairs(alt_names) do
        local addr = FindSymbol(sym_name)
        if addr then
            pcall(function()
                RegisterNativeHook(sym_name, nil,
                    function(retval)
                        if not state.enabled then return retval end
                        return 1
                    end)
                Log(TAG .. ": Native hook via mangled: " .. sym_name)
            end)
            found = true
            break
        end
    end
    if not found then LogWarn(TAG .. ": IsPresentOnBody resolution failed") end
end

local sym_CanGrab = Resolve("VR4GamePlayerProp_CanGrab")
if sym_CanGrab then
    pcall(function()
        RegisterNativeHook("VR4GamePlayerProp_CanGrab", nil,
            function(retval) if not state.enabled then return retval end; return 1 end)
        Log(TAG .. ": Native hook — CanGrab → true")
    end)
end

local sym_IsGrabAllowed = Resolve("VR4GamePlayerProp_IsGrabAllowed")
if sym_IsGrabAllowed then
    pcall(function()
        RegisterNativeHook("VR4GamePlayerProp_IsGrabAllowed", nil,
            function(retval) if not state.enabled then return retval end; return 1 end)
        Log(TAG .. ": Native hook — IsGrabAllowed → true")
    end)
end

local sym_IsPropGrabbable = Resolve("VR4GamePlayerPropHolster_IsPropGrabbable")
if sym_IsPropGrabbable then
    pcall(function()
        RegisterNativeHook("VR4GamePlayerPropHolster_IsPropGrabbable", nil,
            function(retval) if not state.enabled then return retval end; return 1 end)
        Log(TAG .. ": Native hook — IsPropGrabbable → true")
    end)
end

-- ═══════════════════════════════════════════════════════════════════════
-- COMMANDS — UE4SS enhanced gun/hand status
-- ═══════════════════════════════════════════════════════════════════════

RegisterCommand("dualwield", function()
    state.enabled = not state.enabled
    ModConfig.Save("DualWield", state)
    Log(TAG .. ": " .. (state.enabled and "ON" or "OFF"))
    Notify(TAG, state.enabled and "ON" or "OFF")
end)

RegisterCommand("dualwield_status", function()
    local info = TAG .. ": enabled=" .. tostring(state.enabled)
    -- List all active gun instances via UE4SS
    local guns = FindAllOf("VR4GamePlayerGun")
    if guns then
        info = info .. " | Active guns: " .. #guns
        for i, gun in ipairs(guns) do
            if gun and gun:IsValid() then
                pcall(function()
                    local wtype = gun:GetWeaponType()
                    local hand = gun:GetFiringHand()
                    info = info .. " [type=" .. tostring(wtype) .. " hand=" .. tostring(hand) .. "]"
                end)
            end
        end
    end
    -- Check pawn hand state
    local pawn = FindFirstOf("VR4GamePlayerPawn")
    if pawn and pawn:IsValid() then
        pcall(function()
            local locked = pawn:AreHandsGrabLocked()
            if locked ~= nil then info = info .. " | grabLocked=" .. tostring(locked) end
        end)
    end
    Log(info)
end)

if SharedAPI and SharedAPI.DebugMenu then
    SharedAPI.DebugMenu.RegisterToggle("DualWield", "Dual Wield (Same Gun)",
        function() return state.enabled end,
        function(v) state.enabled = v; ModConfig.Save("DualWield", state) end)
end

Log(TAG .. ": v4.0 loaded — UE4SS RegisterHook EquipProp + native IsPresentOnBody/CanGrab/IsGrabAllowed/IsPropGrabbable")
