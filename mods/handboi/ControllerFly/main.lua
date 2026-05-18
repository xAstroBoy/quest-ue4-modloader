-- ControllerFly for HandBoi
-- Hold RIGHT GRIP to fly in the direction your right controller points.
-- Uses grip input so trigger/punch gameplay stays untouched.

local ModName = "ControllerFly"

local state = {
    enabled = false,
    flyHeld = false,
    prevHandPos = nil,
    wasApplyingFly = false,
    leftGripDown = false,
    rightGripDown = false,
    sequenceWindowSeconds = 1.2,
    sequenceStartedAt = 0,
    sequenceArmed = false,
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

local function make_vec(x, y, z)
    return { X = x or 0.0, Y = y or 0.0, Z = z or 0.0 }
end

local function vec_mul(v, s)
    return make_vec((v.X or 0) * s, (v.Y or 0) * s, (v.Z or 0) * s)
end

local function vec_add(a, b)
    return make_vec((a.X or 0) + (b.X or 0), (a.Y or 0) + (b.Y or 0), (a.Z or 0) + (b.Z or 0))
end

local function vec_sub(a, b)
    return make_vec((a.X or 0) - (b.X or 0), (a.Y or 0) - (b.Y or 0), (a.Z or 0) - (b.Z or 0))
end

local function vec_len(v)
    local x, y, z = v.X or 0, v.Y or 0, v.Z or 0
    return math.sqrt(x * x + y * y + z * z)
end

local function vec_clamp(v, maxLen)
    local l = vec_len(v)
    if l <= maxLen or l <= 0.0001 then
        return v
    end
    local s = maxLen / l
    return vec_mul(v, s)
end

local function now_seconds()
    local t = 0
    pcall(function() t = os.clock() end)
    return t or 0
end

local function notify_toggle(enabled)
    local mode = enabled and "ARMED" or "DISARMED"
    local body = "ControllerFly " .. mode .. "\nFlight only while RIGHT GRIP held\nToggle: LEFT GRIP then RIGHT GRIP"

    pcall(function()
        Notify("ControllerFly", body)
    end)

    print("[" .. ModName .. "] " .. body:gsub("\n", " | "))
end

local function try_toggle_sequence()
    local t = now_seconds()

    if not state.sequenceArmed then return end
    if not state.rightGripDown then return end

    if state.sequenceStartedAt > 0 and (t - state.sequenceStartedAt) <= state.sequenceWindowSeconds then
        state.enabled = not state.enabled
        state.flyHeld = false
        state.prevHandPos = nil
        state.sequenceStartedAt = 0
        state.sequenceArmed = false
        notify_toggle(state.enabled)
    end
end

local function set_player_velocity(playerObj, v)
    -- Keep critical ops in separate pcalls
    pcall(function() playerObj:Set("bFlyMode", true) end)
    pcall(function() playerObj:Call("SetMovementMode_Falling\\Flying", true) end)
    -- Non-additive velocity is much more controllable than stacking onto current velocity.
    pcall(function() playerObj:Call("BPI_Set Player Custom Velocity", v, 0.0, false, false) end)
    state.wasApplyingFly = true
end

local function stop_player_fly(playerObj)
    pcall(function() playerObj:Set("bFlyMode", false) end)
    pcall(function() playerObj:Call("SetMovementMode_Falling\\Flying", false) end)
    pcall(function() playerObj:Call("BPI_Set Player Custom Velocity", make_vec(0, 0, 0), 0.0, false, false) end)
    state.wasApplyingFly = false
end

local function get_motion_controller(rightHand)
    local mc = nil
    pcall(function() mc = rightHand:Get("MotionController") end)
    if mc and mc.IsValid and mc:IsValid() then
        return mc
    end
    return nil
end

local function get_hand_forward(rightHand)
    local mc = get_motion_controller(rightHand)
    if not mc then return nil end

    local forward = nil
    pcall(function() forward = mc:Call("GetForwardVector") end)
    return forward
end

-- Toggle sequence part 1: LEFT GRIP down/up
RegisterPostHook("BP_PlayerCharacter_C:InpActEvt_Grip Left_K2Node_InputActionEvent_7", function(self, func, parms)
    state.leftGripDown = true
    state.sequenceStartedAt = now_seconds()
    state.sequenceArmed = true
    try_toggle_sequence()
end)

RegisterPostHook("BP_PlayerCharacter_C:InpActEvt_Grip Left_K2Node_InputActionEvent_6", function(self, func, parms)
    state.leftGripDown = false
end)

-- RIGHT GRIP down/up: also used for hold-to-fly when enabled
RegisterPostHook("BP_PlayerCharacter_C:InpActEvt_Grip Right_K2Node_InputActionEvent_5", function(self, func, parms)
    state.rightGripDown = true
    try_toggle_sequence()

    if state.enabled then
        state.flyHeld = true
        state.prevHandPos = nil
    end
end)

RegisterPostHook("BP_PlayerCharacter_C:InpActEvt_Grip Right_K2Node_InputActionEvent_4", function(self, func, parms)
    state.rightGripDown = false
    state.flyHeld = false
    state.prevHandPos = nil

    local playerObj = unwrap_obj(self)
    if not playerObj then return end
    if state.wasApplyingFly then
        stop_player_fly(playerObj)
    end
end)

-- Drive movement from right controller forward direction while grip is held
RegisterPostHook("BP_PlayerCharacter_C:ReceiveTick", function(self, func, parms)
    local playerObj = unwrap_obj(self)
    if not playerObj then return end

    if not state.enabled or not state.flyHeld then
        -- IMPORTANT: do not zero velocity every tick while disabled, or normal walking gets suppressed.
        if state.wasApplyingFly then
            stop_player_fly(playerObj)
        end
        return
    end

    local rightHand = nil
    pcall(function() rightHand = playerObj:Get("RightHand") end)
    if not rightHand or not rightHand.IsValid or not rightHand:IsValid() then return end

    local mc = get_motion_controller(rightHand)
    if not mc then
        if state.wasApplyingFly then
            stop_player_fly(playerObj)
        end
        return
    end

    local forward = get_hand_forward(rightHand)
    if not forward then return end

    -- Stable directional fly: point controller where you want to go.
    -- (Controller pitch gives natural up/down; avoids noisy hand-delta jitter.)
    local flySpeed = 620.0
    local velocity = vec_mul(forward, flySpeed)
    velocity = vec_clamp(velocity, 700.0)

    set_player_velocity(playerObj, velocity)
end)

print("[" .. ModName .. "] Loaded: LEFT GRIP then RIGHT GRIP toggles fly mode")
notify_toggle(state.enabled)
