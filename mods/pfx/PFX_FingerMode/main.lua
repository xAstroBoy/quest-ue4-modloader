-- PFX_FingerMode v3 — Sweep-Based Grab (ball collides while held!)
--
-- ARCHITECTURE:
-- YUP writes ball transforms at ~90Hz with NO UE physics body.
-- CollisionEnabled=0, IsSimulating=false. PhysicsHandle won't work.
-- PauseGame() freezes YUP so it stops overwriting position.
--
-- NEW: K2_SetActorLocation with bSweep=true performs UE collision
-- traces even without physics simulation. The ball will stop at walls,
-- flippers, bumpers — it physically cannot pass through geometry.
--
-- On release: ResumeGame() restores YUP physics. We also apply
-- a velocity-like effect by calling OnNudgeGesture (table tilt) based
-- on the throw direction, so the ball keeps moving after release.
--------------------------------------------------------------------------------
local TAG = "PFX_FingerMode"
Log(TAG .. ": Loading v3 (sweep-based collision)...")

--------------------------------------------------------------------------------
-- State
--------------------------------------------------------------------------------
local enabled       = false
local grabbed       = false
local grab_hand     = "right"
local was_paused    = false   -- did WE pause the game?
local pause_mode    = nil     -- "yup" or "game"
local grab_dist     = 30      -- auto-grab radius UU
local grab_dist_sq  = 900
local throw_nudge   = 2.0     -- nudge multiplier on release
local ball_z_offset = -3.0    -- offset below hand (ball center vs palm)
local sweep_mode    = true    -- true=sweep (collision), false=teleport (through walls)
local vel_history   = {}      -- last N hand positions for velocity
local vel_window    = 5       -- frames to average for throw velocity

-- Cached objects
local cache = {}

-- YUP native physics write state
local s_yup_pos_offset = nil   -- byte offset of X float in ball_struct (nil = not found)
local s_yup_units = 0.01       -- UE cm → YUP m (divide by 100)
local s_yup_ball_struct = 0    -- last captured ball_struct address

--------------------------------------------------------------------------------
-- Helpers
--------------------------------------------------------------------------------
local function is_live(obj)
    if not obj then return false end
    local ok, v = pcall(function() return obj:IsValid() end)
    return ok and v
end

local function vec3(x, y, z)
    return { X = x or 0, Y = y or 0, Z = z or 0 }
end

local function invalidate()
    cache = {}
    mc_right_cache = nil
    mc_left_cache = nil
end

local function get_pawn()
    if is_live(cache.pawn) then return cache.pawn end
    pcall(function() cache.pawn = FindFirstOf("BP_VR_Pawn_C") end)
    return cache.pawn
end

local function get_play_table()
    if is_live(cache.pt) then return cache.pt end
    pcall(function() cache.pt = FindFirstOf("GFO_PlayTable_C") end)
    return cache.pt
end

local function get_input_handler()
    if is_live(cache.ih) then return cache.ih end
    pcall(function() cache.ih = FindFirstOf("PFXPlayTableInputHandler") end)
    if not is_live(cache.ih) then
        local pt = get_play_table()
        if pt then pcall(function() cache.ih = pt:Get("MainInputHandler") end) end
    end
    return cache.ih
end

local function find_active_ball()
    if is_live(cache.ball) then return cache.ball end
    cache.ball = nil
    local balls = nil
    pcall(function() balls = FindAllOf("ball_C") end)
    if not balls then return nil end
    for _, b in ipairs(balls) do
        if is_live(b) then
            local ok, nm = pcall(function() return b:GetName() end)
            if ok and nm and not nm:match("Default__") then
                cache.ball = b
                return b
            end
        end
    end
    return nil
end

-- Cached motion controller refs (avoid re-fetching every frame)
local mc_right_cache = nil
local mc_left_cache = nil

local function get_mc(hand)
    if hand == "right" then
        if is_live(mc_right_cache) then return mc_right_cache end
        local p = get_pawn()
        if not p then return nil end
        pcall(function() mc_right_cache = p:Get("MC_Right") end)
        return mc_right_cache
    else
        if is_live(mc_left_cache) then return mc_left_cache end
        local p = get_pawn()
        if not p then return nil end
        pcall(function() mc_left_cache = p:Get("MC_Left") end)
        return mc_left_cache
    end
end

-- Reusable position tables (avoid allocating every frame)
local _hand_pos = { X = 0, Y = 0, Z = 0 }
local _ball_pos = { X = 0, Y = 0, Z = 0 }

local function get_hand_pos(hand)
    local mc = get_mc(hand)
    if not is_live(mc) then return nil end
    local pos = nil
    pcall(function() pos = mc:Call("K2_GetComponentLocation") end)
    if pos then
        _hand_pos.X = pos.X; _hand_pos.Y = pos.Y; _hand_pos.Z = pos.Z
        return _hand_pos
    end
    return nil
end

local function get_ball_pos()
    local b = find_active_ball()
    if not b then return nil end
    local pos = nil
    pcall(function() pos = b:Call("K2_GetActorLocation") end)
    if pos then
        _ball_pos.X = pos.X; _ball_pos.Y = pos.Y; _ball_pos.Z = pos.Z
        return _ball_pos
    end
    return nil
end

local function dist_sq(a, b)
    local dx = a.X - b.X; local dy = a.Y - b.Y; local dz = a.Z - b.Z
    return dx*dx + dy*dy + dz*dz
end

local function has_yup_direct_pause()
    local yup = rawget(_G, "YUP")
    return yup and type(yup) == "table"
        and type(yup.GetTableWorld) == "function"
        and type(yup.DirectSetPause) == "function"
end

local function get_yup_pause_state()
    local yup = rawget(_G, "YUP")
    if not has_yup_direct_pause() then return nil end
    if type(yup.DirectGetPause) ~= "function" then return nil end

    local ok, paused = pcall(function()
        local tw = yup.GetTableWorld()
        if not tw or tw == 0 then return nil end
        return yup.DirectGetPause()
    end)
    if not ok then return nil end
    if paused == nil or paused < 0 then return nil end
    return paused ~= 0
end

local function set_yup_pause(paused)
    local yup = rawget(_G, "YUP")
    if not has_yup_direct_pause() then return false end

    local ok, result = pcall(function()
        local tw = yup.GetTableWorld()
        if not tw or tw == 0 then return false end
        return yup.DirectSetPause(paused)
    end)
    return ok and result == true
end

--------------------------------------------------------------------------------
-- YUP native ball_struct capture + position offset scan
--------------------------------------------------------------------------------
local function yup_capture_ball_struct()
    local yup = rawget(_G, "YUP")
    if not yup or type(yup.CaptureBallStructFromTableWorld) ~= "function" then return 0 end
    local ok, bs = pcall(function() return yup.CaptureBallStructFromTableWorld(0) end)
    if ok and bs and bs ~= 0 then
        s_yup_ball_struct = bs
        Log(TAG .. ": ball_struct = " .. string.format("0x%X", bs))
    end
    return (ok and bs) or 0
end

-- Attempt to find the XYZ position offset inside ball_struct.
-- ball = the UE ball actor (position read in UU, converted to YUP units).
-- Must be called AFTER yup_capture_ball_struct() and WHILE ball is stationary.
local function yup_find_pos_offset(ball)
    local yup = rawget(_G, "YUP")
    if not yup or s_yup_ball_struct == 0 then return end
    if type(yup.ScanBallStructForXYZ) ~= "function" then return end

    local ball_loc = nil
    pcall(function() ball_loc = ball:Call("K2_GetActorLocation") end)
    if not ball_loc then
        Log(TAG .. ": yup_find_pos_offset — could not get ball location")
        return
    end

    -- Try meters (UE cm ÷ 100)
    local off = -1
    pcall(function()
        off = yup.ScanBallStructForXYZ(
            ball_loc.X * 0.01,
            ball_loc.Y * 0.01,
            ball_loc.Z * 0.01,
            0.5   -- 50cm tolerance in meters
        )
    end)
    if off >= 0 then
        s_yup_pos_offset = off
        s_yup_units = 0.01
        Log(TAG .. ": YUP pos offset=+" .. string.format("0x%X", off) .. " (meters, ÷100)")
        return
    end

    -- Try centimeters (same units as UE)
    pcall(function()
        off = yup.ScanBallStructForXYZ(
            ball_loc.X, ball_loc.Y, ball_loc.Z, 10.0
        )
    end)
    if off >= 0 then
        s_yup_pos_offset = off
        s_yup_units = 1.0
        Log(TAG .. ": YUP pos offset=+" .. string.format("0x%X", off) .. " (cm, ×1)")
        return
    end

    -- Dump the struct for manual analysis
    Log(TAG .. ": WARN: pos offset not found — dumping ball_struct floats")
    if type(yup.DumpBallStruct) == "function" then
        pcall(function()
            local t = yup.DumpBallStruct()
            local out = "ball_struct dump (ball_loc=" ..
                string.format("%.1f,%.1f,%.1f", ball_loc.X, ball_loc.Y, ball_loc.Z) .. "):\n"
            for off2 = 0, 0x80, 4 do
                local v = t[off2]
                if v and v == v and math.abs(v) < 9999 then  -- exclude NaN/inf/garbage
                    out = out .. string.format("  +0x%02X = %.4f\n", off2, v)
                end
            end
            Log(TAG .. ": " .. out)
        end)
    end
end

--------------------------------------------------------------------------------
-- Grab
--------------------------------------------------------------------------------
local function do_grab(hand)
    local ball = find_active_ball()
    if not ball then
        Log(TAG .. ": grab failed — no ball")
        return false
    end

    -- Pause YUP to stop it overwriting our position.
    -- Prefer direct YUP pause so the player stays active while the ball is frozen.
    was_paused = false
    pause_mode = nil

    local direct_paused = get_yup_pause_state()
    if direct_paused == false then
        if set_yup_pause(true) then
            was_paused = true
            pause_mode = "yup"
            Log(TAG .. ": DirectSetPause(true) — YUP frozen")
        end
    elseif direct_paused == true then
        Log(TAG .. ": YUP already paused")
    end

    if not was_paused and direct_paused ~= true then
        local pt = get_play_table()
        if pt then
            local already = false
            pcall(function() already = pt:Call("IsPaused") end)
            if not already then
                pcall(function() pt:Call("PauseGame") end)
                was_paused = true
                pause_mode = "game"
                Log(TAG .. ": PauseGame — fallback freeze")
            end
        end
    end

    grabbed   = true
    grab_hand = hand
    vel_history = {}

    -- Capture YUP ball_struct and find position offset (once per session)
    if s_yup_ball_struct == 0 then
        yup_capture_ball_struct()
    end
    if s_yup_ball_struct ~= 0 and s_yup_pos_offset == nil then
        yup_find_pos_offset(ball)
    end

    -- Snap immediately on grab so the ball jumps to the hand without waiting
    -- for the next async tick.
    local hand_pos = get_hand_pos(grab_hand)
    if hand_pos then
        local target = _hand_pos
        target.Z = hand_pos.Z + ball_z_offset
        local rot = {Pitch=0, Roll=0, Yaw=0}
        pcall(function() local r = ball:Call("K2_GetActorRotation"); if r then rot=r end end)
        pcall(function()
            ball:Call("K2_SetActorLocationAndRotation", target, rot, false, false)
        end)
    end

    Log(TAG .. ": GRABBED with " .. hand)
    return true
end

local function do_release()
    if not grabbed then return end
    grabbed = false

    -- Calculate throw direction from velocity history
    local throw_x, throw_y = 0, 0
    if #vel_history >= 2 then
        local last = vel_history[#vel_history]
        local prev = vel_history[math.max(1, #vel_history - vel_window)]
        if last and prev then
            throw_x = (last.X - prev.X)
            throw_y = (last.Y - prev.Y)
        end
    end

    -- Resume YUP physics
    if was_paused then
        if pause_mode == "yup" then
            local restored = set_yup_pause(false)
            if restored then
                Log(TAG .. ": DirectSetPause(false) — YUP restored")
            else
                Log(TAG .. ": WARN: failed to restore YUP via DirectSetPause(false)")
            end
        elseif pause_mode == "game" then
            local pt = get_play_table()
            if pt then
                pcall(function() pt:Call("ResumeGame") end)
                Log(TAG .. ": ResumeGame — fallback restore")
            end
        end
        was_paused = false
        pause_mode = nil
    end

    -- Apply throw as table nudge (YUP processes nudge as real physics force)
    local mag = math.sqrt(throw_x * throw_x + throw_y * throw_y)
    if mag > 0.5 then
        local ih = get_input_handler()
        if ih then
            local nx = math.max(-1, math.min(1, throw_x * throw_nudge / mag))
            local ny = math.max(-1, math.min(1, throw_y * throw_nudge / mag))
            pcall(function() ih:Call("OnNudgeGesture", nx, ny) end)
            Log(TAG .. ": Throw nudge (" .. string.format("%.2f,%.2f", nx, ny) .. ")")
        end
    end

    vel_history = {}
    Log(TAG .. ": RELEASED")
end

--------------------------------------------------------------------------------
-- Per-frame: move ball to hand WITH sweep collision
--------------------------------------------------------------------------------
local function update_ball()
    if not grabbed then return end

    local ball = find_active_ball()
    if not ball then do_release(); return end

    local hand_pos = get_hand_pos(grab_hand)
    if not hand_pos then return end

    -- Apply Z offset (ball sits slightly below palm) — reuse table
    local target = _hand_pos
    target.Z = hand_pos.Z + ball_z_offset

    -- Record for velocity tracking (copy values, don't store ref to reusable table)
    vel_history[#vel_history + 1] = { X = target.X, Y = target.Y, Z = target.Z }
    if #vel_history > vel_window * 2 then
        table.remove(vel_history, 1)
    end

    -- Move ball to hand — visual update via UE actor
    local rot = {Pitch=0, Roll=0, Yaw=0}
    if sweep_mode then
        pcall(function()
            ball:Call("K2_SetActorLocationAndRotation", target, rot, true, false)
        end)
    else
        pcall(function()
            ball:Call("K2_SetActorLocationAndRotation", target, rot, false, false)
        end)
    end

    -- Write directly to YUP physics body so the position is real after unpause
    if s_yup_ball_struct ~= 0 and s_yup_pos_offset ~= nil then
        local yup = rawget(_G, "YUP")
        if yup and type(yup.WriteFloatAt) == "function" then
            local ux = target.X * s_yup_units
            local uy = target.Y * s_yup_units
            local uz = target.Z * s_yup_units
            pcall(function()
                yup.WriteFloatAt(s_yup_ball_struct, s_yup_pos_offset,     ux)
                yup.WriteFloatAt(s_yup_ball_struct, s_yup_pos_offset + 4, uy)
                yup.WriteFloatAt(s_yup_ball_struct, s_yup_pos_offset + 8, uz)
                -- Zero out velocity so ball doesn't fly off on unpause
                yup.WriteFloatAt(s_yup_ball_struct, 0x38, 0.0)
                yup.WriteFloatAt(s_yup_ball_struct, 0x3C, 0.0)
                yup.WriteFloatAt(s_yup_ball_struct, 0x40, 0.0)
            end)
        end
    end
end

--------------------------------------------------------------------------------
-- Auto-grab check
--------------------------------------------------------------------------------
local function check_auto_grab()
    if grabbed or not enabled then return end
    local bp = get_ball_pos()
    if not bp then return end

    local rp = get_hand_pos("right")
    if rp and dist_sq(rp, bp) < grab_dist_sq then
        do_grab("right"); return
    end
    local lp = get_hand_pos("left")
    if lp and dist_sq(lp, bp) < grab_dist_sq then
        do_grab("left"); return
    end
end

--------------------------------------------------------------------------------
-- Main loop
--------------------------------------------------------------------------------
local loop_active = false

local function start_loop()
    if loop_active then return end
    loop_active = true

    LoopAsync(33, function()  -- ~30Hz (smooth enough, saves CPU)
        if not enabled then
            if grabbed then do_release() end
            loop_active = false
            return true  -- stop loop
        end

        if grabbed then
            update_ball()
        else
            check_auto_grab()
        end
        return false  -- continue
    end)
    Log(TAG .. ": Loop started (90Hz)")
end

--------------------------------------------------------------------------------
-- Grip hooks
--------------------------------------------------------------------------------
local function setup_hooks()
    -- Right grip
    pcall(function()
        RegisterHook("BP_Pawn_Base_C:OnGripRight_Pressed", function()
            if enabled and not grabbed then do_grab("right")
            elseif grabbed and grab_hand == "right" then do_release() end
        end)
        Log(TAG .. ": Hook: OnGripRight_Pressed")
    end)
    pcall(function()
        RegisterHook("BP_Pawn_Base_C:OnGripRight_Released", function()
            if grabbed and grab_hand == "right" then do_release() end
        end)
    end)
    pcall(function()
        RegisterHook("BP_Pawn_Base_C:OnGripLeft_Pressed", function()
            if enabled and not grabbed then do_grab("left")
            elseif grabbed and grab_hand == "left" then do_release() end
        end)
        Log(TAG .. ": Hook: OnGripLeft_Pressed")
    end)
    pcall(function()
        RegisterHook("BP_Pawn_Base_C:OnGripLeft_Released", function()
            if grabbed and grab_hand == "left" then do_release() end
        end)
    end)

    -- BP_VR_Pawn_C variants
    pcall(function()
        RegisterHook("BP_VR_Pawn_C:OnGripRight_Pressed", function()
            if enabled and not grabbed then do_grab("right")
            elseif grabbed and grab_hand == "right" then do_release() end
        end)
    end)
    pcall(function()
        RegisterHook("BP_VR_Pawn_C:OnGripRight_Released", function()
            if grabbed and grab_hand == "right" then do_release() end
        end)
    end)
    pcall(function()
        RegisterHook("BP_VR_Pawn_C:OnGripLeft_Pressed", function()
            if enabled and not grabbed then do_grab("left")
            elseif grabbed and grab_hand == "left" then do_release() end
        end)
    end)
    pcall(function()
        RegisterHook("BP_VR_Pawn_C:OnGripLeft_Released", function()
            if grabbed and grab_hand == "left" then do_release() end
        end)
    end)

    -- GrabComponent input handler
    pcall(function()
        RegisterHook("IH_Grab_C:GrabPressed", function()
            if enabled and not grabbed then do_grab("right") end
        end)
        Log(TAG .. ": Hook: IH_Grab_C:GrabPressed")
    end)
    pcall(function()
        RegisterHook("IH_Grab_C:GrabReleased", function()
            if grabbed then do_release() end
        end)
    end)
end

--------------------------------------------------------------------------------
-- Integration with PFX_Cheats (replaces its finger_mode)
--------------------------------------------------------------------------------
local function integrate_cheats()
    if _G.PFX_Cheats then
        _G.PFX_Cheats.toggle_finger_mode = function()
            enabled = not enabled
            if enabled then
                start_loop()
            else
                if grabbed then do_release() end
            end
            _G.PFX_Cheats.cheats.finger_mode = enabled
            Log(TAG .. ": finger_mode=" .. tostring(enabled) .. " (sweep v3)")
            return enabled
        end
        Log(TAG .. ": Integrated with PFX_Cheats.toggle_finger_mode")
    end
end

pcall(function()
    ExecuteWithDelay(2000, function()
        integrate_cheats()
    end)
end)

--------------------------------------------------------------------------------
-- Bridge commands
--------------------------------------------------------------------------------
pcall(function() RegisterCommand("finger_on", function()
    enabled = true
    start_loop()
    return TAG .. ": ENABLED (sweep=" .. tostring(sweep_mode) .. ")"
end) end)

pcall(function() RegisterCommand("finger_off", function()
    if grabbed then do_release() end
    enabled = false
    return TAG .. ": DISABLED"
end) end)

pcall(function() RegisterCommand("finger_grab", function(hand)
    hand = hand or "right"
    enabled = true
    start_loop()
    return do_grab(hand) and "GRABBED" or "FAILED"
end) end)

pcall(function() RegisterCommand("finger_release", function()
    do_release()
    return "RELEASED"
end) end)

pcall(function() RegisterCommand("finger_sweep", function(mode)
    if mode == "0" or mode == "false" or mode == "off" then
        sweep_mode = false
    elseif mode == "1" or mode == "true" or mode == "on" then
        sweep_mode = true
    else
        sweep_mode = not sweep_mode
    end
    return "sweep_mode=" .. tostring(sweep_mode)
end) end)

pcall(function() RegisterCommand("finger_status", function()
    local parts = {
        "enabled=" .. tostring(enabled),
        "grabbed=" .. tostring(grabbed),
        "hand=" .. grab_hand,
        "sweep=" .. tostring(sweep_mode),
        "grab_dist=" .. grab_dist,
        "throw_nudge=" .. throw_nudge,
        "z_offset=" .. ball_z_offset,
    }
    local bp = get_ball_pos()
    if bp then parts[#parts+1] = string.format("ball=(%.0f,%.0f,%.0f)", bp.X, bp.Y, bp.Z) end
    local hp = get_hand_pos("right")
    if hp then parts[#parts+1] = string.format("hand_r=(%.0f,%.0f,%.0f)", hp.X, hp.Y, hp.Z) end
    return table.concat(parts, " ")
end) end)

pcall(function() RegisterCommand("finger_dist", function(d)
    d = tonumber(d)
    if d and d > 0 then grab_dist = d; grab_dist_sq = d*d end
    return "grab_dist=" .. grab_dist
end) end)

pcall(function() RegisterCommand("finger_nudge", function(n)
    n = tonumber(n)
    if n then throw_nudge = n end
    return "throw_nudge=" .. throw_nudge
end) end)

pcall(function() RegisterCommand("finger_zoff", function(z)
    z = tonumber(z)
    if z then ball_z_offset = z end
    return "z_offset=" .. ball_z_offset
end) end)

pcall(function() RegisterCommand("finger_teleport", function(x, y, z)
    x = tonumber(x) or 0; y = tonumber(y) or 0; z = tonumber(z) or 0
    local ball = find_active_ball()
    if not ball then return "no ball" end
    pcall(function() ball:Call("K2_SetActorLocationAndRotation", vec3(x,y,z), {Pitch=0,Roll=0,Yaw=0}, false, false) end)
    return string.format("teleported %.0f,%.0f,%.0f", x, y, z)
end) end)

pcall(function() RegisterCommand("finger_force_nudge", function(nx, ny)
    nx = tonumber(nx) or 0; ny = tonumber(ny) or 0
    local ih = get_input_handler()
    if not ih then return "no input handler" end
    pcall(function() ih:Call("OnNudgeGesture", nx, ny) end)
    return string.format("nudge(%.2f, %.2f)", nx, ny)
end) end)

--------------------------------------------------------------------------------
-- Init
--------------------------------------------------------------------------------
setup_hooks()

-- Global API for other mods (PFX_ModMenu, etc.)
_G.PFX_FingerMode = {
    enable  = function() enabled = true; start_loop() end,
    disable = function() if grabbed then do_release() end; enabled = false end,
    toggle  = function()
        enabled = not enabled
        if enabled then start_loop()
        elseif grabbed then do_release() end
        return enabled
    end,
    is_enabled  = function() return enabled end,
    is_grabbed  = function() return grabbed end,
    grab        = do_grab,
    release     = do_release,
    set_sweep   = function(b) sweep_mode = not not b end,
    get_ball_pos = get_ball_pos,
    get_hand_pos = get_hand_pos,
}

Log(TAG .. ": v3 loaded — sweep collision, grip hooks, PFX_Cheats integration")
Log(TAG .. ": bridge: finger_on/off, finger_grab/release, finger_sweep, finger_status")
Log(TAG .. ": tuning: finger_dist, finger_nudge, finger_zoff, finger_force_nudge")
