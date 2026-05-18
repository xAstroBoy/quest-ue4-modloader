-- Unlimited Wing Flaps Cheat Mod for HandBoi

local ModName = "UnlimitedWingFlaps"
local state = {
    flapCounterBefore = 0,
}

local function unwrap_obj(hookSelf)
    if not hookSelf then return nil end

    local okGet, obj = pcall(function()
        return hookSelf:get()
    end)
    if okGet and obj and obj.IsValid and obj:IsValid() then
        return obj
    end

    if hookSelf.IsValid and hookSelf:IsValid() then
        return hookSelf
    end

    return nil
end

local function enforce_unlimited_flaps(playerObj)
    if not playerObj or not playerObj.IsValid or not playerObj:IsValid() then return end

    local flap = nil
    pcall(function()
        flap = playerObj:Get("BPAC_Character_Flap")
    end)
    if not flap or not flap.IsValid or not flap:IsValid() then return end

    pcall(function()
        flap:Set("FlapsCounterMax", 999)
    end)

    pcall(function()
        local nowCounter = flap:Get("FlapsCounter")
        if nowCounter and state.flapCounterBefore and nowCounter < state.flapCounterBefore then
            flap:Set("FlapsCounter", state.flapCounterBefore)
        end
    end)
end

-- Unlimited flaps: use trace-confirmed hot path
RegisterPreHook("BP_PlayerCharacter_C:ReceiveTick", function(self, func, parms)
    local playerObj = unwrap_obj(self)
    if not playerObj then return end

    local flap = nil
    pcall(function()
        flap = playerObj:Get("BPAC_Character_Flap")
    end)
    if not flap or not flap.IsValid or not flap:IsValid() then return end

    pcall(function()
        state.flapCounterBefore = flap:Get("FlapsCounter") or 0
    end)
end)

RegisterPostHook("BP_PlayerCharacter_C:ReceiveTick", function(self, func, parms)
    local playerObj = unwrap_obj(self)
    if not playerObj then return end
    enforce_unlimited_flaps(playerObj)
end)

print("[" .. ModName .. "] Loaded: unlimited flaps")
