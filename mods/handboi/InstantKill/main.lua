-- Instant Kill Cheat Mod for HandBoi

local ModName = "InstantKill"

local state = {
    punchArmedUntil = 0,
    punchWindowSeconds = 0.35,
}

local function unwrap_obj(hookSelf)
    if not hookSelf then return nil end

    local okGet, obj = pcall(function()
        local getter = nil
        pcall(function() getter = hookSelf.get end)
        if type(getter) == "function" then
            return getter(hookSelf)
        end
        return nil
    end)
    if okGet and obj and obj.IsValid and obj:IsValid() then
        return obj
    end

    if hookSelf.IsValid and hookSelf:IsValid() then
        return hookSelf
    end

    return nil
end

local function is_enemy(obj)
    if not obj or not obj.IsValid or not obj:IsValid() then return false end

    local className = ""
    pcall(function()
        className = obj:GetClassName() or ""
    end)

    if className == "BP_PlayerCharacter_C" or className == "BP_PlayerHand_C" then
        return false
    end

    -- Based on PE trace enemy classes
    if string.find(className, "Enemy", 1, true) then return true end
    if string.find(className, "Broomba", 1, true) then return true end
    if string.find(className, "SecurityBot", 1, true) then return true end
    if string.find(className, "Boomlin", 1, true) then return true end
    if string.find(className, "Drone", 1, true) then return true end
    if string.find(className, "Turret", 1, true) then return true end
    if string.find(className, "LaserProjectile", 1, true) then return true end

    return false
end

local function kill_enemy_now(obj)
    if not is_enemy(obj) then return end

    -- Keep operations isolated (project rule: separate pcall blocks)
    pcall(function() obj:Set("Health", 0.0) end)
    pcall(function() obj:Set("Dead", true) end)
    pcall(function() obj:Call("Death", true) end)
    pcall(function() obj:Call("Death", false) end)
    pcall(function() obj:Call("BPI_Disintegrate") end)
    pcall(function() obj:Call("BPI_SetActive", false) end)
    pcall(function() obj:Call("BPI_EnableTarget", false) end)
end

local function now_seconds()
    local t = 0
    pcall(function() t = os.clock() end)
    return t or 0
end

local function arm_punch_window()
    state.punchArmedUntil = now_seconds() + state.punchWindowSeconds
end

local function punch_window_active()
    return now_seconds() <= (state.punchArmedUntil or 0)
end

-- Generic UE damage callbacks from your PE trace
RegisterPostHook("Actor:ReceivePointDamage", function(self, func, parms)
    local obj = unwrap_obj(self)
    if not obj then return end
    if not punch_window_active() then return end
    kill_enemy_now(obj)
end)

RegisterPostHook("Actor:ReceiveAnyDamage", function(self, func, parms)
    local obj = unwrap_obj(self)
    if not obj then return end
    if not punch_window_active() then return end
    kill_enemy_now(obj)
end)

-- Shared enemy interface path from SDK
RegisterPostHook("BPI_Enemy:BPI_Damage", function(self, func, parms)
    local obj = unwrap_obj(self)
    if not obj then return end
    if not punch_window_active() then return end
    kill_enemy_now(obj)
end)

-- Punch-trigger path from PE trace (reliable one-punch behavior)
RegisterPostHook("BP_PlayerCharacter_C:InpActEvt_Trigger Right_K2Node_InputActionEvent_0", function(self, func, parms)
    arm_punch_window()
end)

RegisterPostHook("BP_PlayerCharacter_C:InpActEvt_Trigger Right_K2Node_InputActionEvent_1", function(self, func, parms)
    arm_punch_window()
end)

RegisterPostHook("BP_PlayerCharacter_C:InpActEvt_Trigger Left_K2Node_InputActionEvent_2", function(self, func, parms)
    arm_punch_window()
end)

RegisterPostHook("BP_PlayerCharacter_C:InpActEvt_Trigger Left_K2Node_InputActionEvent_3", function(self, func, parms)
    arm_punch_window()
end)

print("[" .. ModName .. "] Loaded: enemies die on punch input")
