-- ============================================================================
-- PFX_Cheats v1 — In-game Cheat Menu + Infinite Ball Save
-- ============================================================================
-- Features:
--   1. Infinite Ball Save — hooks OnGameStateChanged on PFXGameflowObject_PlayTable
--      and calls SkipPrepareGameEnd() to cancel drain. Logs all state transitions
--      so we can tune the exact state number.
--   2. Bridge commands — toggle cheats at runtime via TCP bridge console
--   3. Wrist-menu stub — hooks BP_WristMenu_C:Open to inject cheat entries
-- ============================================================================
local TAG = "PFX_Cheats"
local VERBOSE = true
local function V(...) if VERBOSE then Log(TAG .. " [V] " .. string.format(...)) end end
Log(TAG .. ": Loading v1...")

-- ============================================================================
-- STATE
-- ============================================================================
local cheats = {
    infinite_ball_save  = true,   -- toggle via bridge: cheats_ballsave 0|1
    log_game_states     = true,   -- log every OnGameStateChanged transition
}

local stats = {
    ball_saves          = 0,
    state_changes       = 0,
    game_end_blocked    = 0,
}

-- Known game state values (discovered via live logging — expand as needed)
-- State 3 = "PrepareBallEnd" / drain in most YUP pinball builds
-- We block on any state > 1 that is not "Paused" (usually 2)
local DRAIN_STATES = { [3]=true, [5]=true, [6]=true, [7]=true }
local PAUSE_STATE  = 2

-- ============================================================================
-- HELPERS
-- ============================================================================
local function is_live(obj)
    if not obj then return false end
    local ok, valid = pcall(function() return obj:IsValid() end)
    if not ok or not valid then return false end
    local ok2, name = pcall(function() return obj:GetName() end)
    if not ok2 or not name then return false end
    return not name:match("^Default__") and not name:match("^REINST_")
end

local function find_live(...)
    for _, cls in ipairs({...}) do
        local all = nil
        pcall(function() all = FindAllOf(cls) end)
        if all then
            for _, obj in ipairs(all) do
                if is_live(obj) then return obj end
            end
        end
    end
    return nil
end

local function get_play_table()
    return find_live("PFXGameflowObject_PlayTable")
end

local function cheats_set_ballsave(enable)
    V("cheats_set_ballsave enable=%s", tostring(enable))
    cheats.infinite_ball_save = not not enable
    Log(TAG .. ": infinite_ball_save = " .. tostring(cheats.infinite_ball_save))
    return cheats.infinite_ball_save
end

local function cheats_toggle_ballsave()
    V("cheats_toggle_ballsave")
    return cheats_set_ballsave(not cheats.infinite_ball_save)
end

local function cheats_set_logstates(enable)
    V("cheats_set_logstates enable=%s", tostring(enable))
    cheats.log_game_states = not not enable
    return cheats.log_game_states
end

local function cheats_toggle_logstates()
    V("cheats_toggle_logstates")
    return cheats_set_logstates(not cheats.log_game_states)
end

local function cheats_saveball()
    V("cheats_saveball")
    local pt = get_play_table()
    if not pt then return false, "not in game" end
    local ok = pcall(function() pt:Call("SkipPrepareGameEnd") end)
    if ok then
        stats.ball_saves = stats.ball_saves + 1
        return true, "ball saved (total=" .. stats.ball_saves .. ")"
    end
    return false, "SkipPrepareGameEnd failed"
end

local function cheats_restartball()
    V("cheats_restartball")
    local pt = get_play_table()
    if not pt then return false, "not in game" end
    local ok = pcall(function() pt:Call("RestartGame") end)
    return ok, "RestartGame ok=" .. tostring(ok)
end

local function cheats_pause_resume()
    V("cheats_pause_resume")
    local pt = get_play_table()
    if not pt then return false, "not in game" end
    local paused = false
    pcall(function() paused = pt:Call("IsPaused") end)
    if paused then
        pcall(function() pt:Call("ResumeGame") end)
        return true, "Game RESUMED"
    else
        pcall(function() pt:Call("PauseGame") end)
        return true, "Game PAUSED"
    end
end

local function cheats_status_line()
    local drain_list = {}
    for k in pairs(DRAIN_STATES) do drain_list[#drain_list+1] = k end
    table.sort(drain_list)
    return string.format(
        "%s v1: ballsave=%s logstates=%s | saves=%d end_blocked=%d states_seen=%d | drain_states={%s}",
        TAG,
        tostring(cheats.infinite_ball_save),
        tostring(cheats.log_game_states),
        stats.ball_saves,
        stats.game_end_blocked,
        stats.state_changes,
        table.concat(drain_list, ",")
    )
end

-- ============================================================================
-- INFINITE BALL SAVE
-- Hook: PFXGameflowObject_PlayTable:OnGameStateChanged (BlueprintEvent)
-- Pre-hook fires BEFORE the BP graph runs — we can inspect the new state
-- and call SkipPrepareGameEnd() to cancel drain sequences.
-- ============================================================================

-- Pre-hook: intercept state change BEFORE the BP graph handles it
pcall(function()
    RegisterHook("PFXGameflowObject_PlayTable:OnGameStateChanged", function(ctx, state_raw)
        V("OnGameStateChanged hook fired")
        local newState = 0
        pcall(function() newState = ReadU8(state_raw) end)
        stats.state_changes = stats.state_changes + 1

        if cheats.log_game_states then
            Log(TAG .. ": [GameState] -> " .. tostring(newState))
        end

        if cheats.infinite_ball_save and DRAIN_STATES[newState] then
            -- Try to skip prepare-game-end (cancels the drain sequence)
            local pt = nil
            pcall(function() pt = ctx:GetOuter() end)
            if not pt then pcall(function() pt = ctx end) end
            if pt and is_live(pt) then
                local ok = pcall(function() pt:Call("SkipPrepareGameEnd") end)
                if ok then
                    stats.ball_saves = stats.ball_saves + 1
                    Log(TAG .. ": [BallSave] Blocked drain at state=" .. newState
                        .. " (saves=" .. stats.ball_saves .. ")")
                end
            end
        end
    end)
    Log(TAG .. ": OnGameStateChanged hook registered")
end)

-- Post-hook on OnGameEnd: if ball save enabled and game ends, restart
-- (catches cases where drain slipped through state hook)
pcall(function()
    RegisterHook("PFXGameflowObject_PlayTable:OnGameEnd", function(ctx)
        V("OnGameEnd hook fired, ballsave=%s", tostring(cheats.infinite_ball_save))
        if not cheats.infinite_ball_save then return end
        local pt = nil
        pcall(function() pt = ctx end)
        if pt and is_live(pt) then
            local ok = pcall(function() pt:Call("SkipPrepareGameEnd") end)
            if ok then
                stats.game_end_blocked = stats.game_end_blocked + 1
                Log(TAG .. ": [BallSave] OnGameEnd blocked (total=" .. stats.game_end_blocked .. ")")
            end
        end
    end)
    Log(TAG .. ": OnGameEnd hook registered")
end)

-- ============================================================================
-- WRIST MENU INJECTION (stub — hooks BP_WristMenu_C when it opens in-game)
-- This logs available entries and attempts to inject a Cheats tab.
-- ============================================================================
pcall(function()
    RegisterHook("BP_WristMenu_C:Open", function(ctx)
        V("BP_WristMenu_C:Open hook fired")
        Log(TAG .. ": [WristMenu] Opened — cheats available via bridge commands")
        -- Future: inject cheat entries into the wrist menu entries array
        -- For now just log so we can probe the menu structure
        pcall(function()
            local wm = ctx
            if not is_live(wm) then return end
            local wn = "?"
            pcall(function() wn = wm:GetName() end)
            Log(TAG .. ": [WristMenu] instance=" .. wn)
        end)
    end)
    Log(TAG .. ": WristMenu:Open hook registered")
end)

-- ============================================================================
-- BRIDGE COMMANDS
-- ============================================================================

-- Toggle infinite ball save
pcall(function()
    RegisterCommand("cheats_ballsave", function(args)
        local enable = (args == nil or args == "" or args == "1" or args == "true")
        cheats_set_ballsave(enable)
        return TAG .. ": infinite_ball_save = " .. tostring(cheats.infinite_ball_save)
    end)
end)

-- Toggle state logging
pcall(function()
    RegisterCommand("cheats_logstates", function(args)
        local enable = (args == nil or args == "" or args == "1" or args == "true")
        cheats_set_logstates(enable)
        return TAG .. ": log_game_states = " .. tostring(cheats.log_game_states)
    end)
end)

-- Add/remove drain state numbers
pcall(function()
    RegisterCommand("cheats_drainstate", function(args)
        local n = tonumber(args)
        if not n then return "usage: cheats_drainstate <number>" end
        DRAIN_STATES[math.floor(n)] = true
        Log(TAG .. ": Added drain state " .. n)
        local states = {}
        for k in pairs(DRAIN_STATES) do states[#states+1] = k end
        table.sort(states)
        return TAG .. ": drain states = {" .. table.concat(states, ",") .. "}"
    end)
end)

-- Manual ball save trigger (call while in game if auto-hook misses)
pcall(function()
    RegisterCommand("cheats_saveball", function()
        local ok, msg = cheats_saveball()
        if ok then return TAG .. ": " .. msg end
        return TAG .. ": " .. msg
    end)
end)

-- Force restart current ball (if drain already happened)
pcall(function()
    RegisterCommand("cheats_restartball", function()
        local _, msg = cheats_restartball()
        return TAG .. ": " .. msg
    end)
end)

-- Pause/resume game
pcall(function()
    RegisterCommand("cheats_pause", function()
        local _, msg = cheats_pause_resume()
        return TAG .. ": " .. msg
    end)
end)

-- Status dump
pcall(function()
    RegisterCommand("cheats_status", function()
        return cheats_status_line()
    end)
end)

-- Global API for other mods (e.g. PFX_ModMenu)
PFX_Cheats = nil
_G.PFX_Cheats = {
    cheats = cheats,
    stats = stats,
    set_ball_save = cheats_set_ballsave,
    toggle_ball_save = cheats_toggle_ballsave,
    set_log_states = cheats_set_logstates,
    toggle_log_states = cheats_toggle_logstates,
    save_ball = cheats_saveball,
    restart_ball = cheats_restartball,
    pause_resume = cheats_pause_resume,
    status = cheats_status_line,
}
Log(TAG .. ": global API exported (_G.PFX_Cheats)")

-- Max all perks right now via CheatManager (works in-game)
pcall(function()
    RegisterCommand("cheats_maxperks", function()
        local cm = find_live("PFXCheatManager")
        if not cm then return "PFXCheatManager not found" end
        local ok = pcall(function() cm:Call("PFXDebug_TablePerkMaxAll") end)
        return TAG .. ": PFXDebug_TablePerkMaxAll ok=" .. tostring(ok)
    end)
end)

-- Max all masteries via CheatManager
pcall(function()
    RegisterCommand("cheats_maxmasteries", function()
        local cm = find_live("PFXCheatManager")
        if not cm then return "PFXCheatManager not found" end
        local ok = pcall(function() cm:Call("PFXDebug_TableMasteryMaxAll") end)
        return TAG .. ": PFXDebug_TableMasteryMaxAll ok=" .. tostring(ok)
    end)
end)

-- Unlock all collectibles via CheatManager
pcall(function()
    RegisterCommand("cheats_unlockall", function()
        local cm = find_live("PFXCheatManager")
        if not cm then return "PFXCheatManager not found" end
        local ok = pcall(function() cm:Call("PFXDebug_Collectibles_UnlockAll", false) end)
        return TAG .. ": PFXDebug_Collectibles_UnlockAll ok=" .. tostring(ok)
    end)
end)

-- Max all championship via CheatManager
pcall(function()
    RegisterCommand("cheats_maxchamp", function()
        local cm = find_live("PFXCheatManager")
        if not cm then return "PFXCheatManager not found" end
        local ok = pcall(function() cm:Call("PFXDebug_Championship_MaxAll") end)
        return TAG .. ": PFXDebug_Championship_MaxAll ok=" .. tostring(ok)
    end)
end)

Log(TAG .. ": v1 loaded — ball_save=" .. tostring(cheats.infinite_ball_save)
    .. " log_states=" .. tostring(cheats.log_game_states))
Log(TAG .. ": Bridge cmds: cheats_status, cheats_ballsave, cheats_saveball, cheats_restartball")
Log(TAG .. ": Bridge cmds: cheats_pause, cheats_logstates, cheats_drainstate <n>")
Log(TAG .. ": Bridge cmds: cheats_maxperks, cheats_maxmasteries, cheats_unlockall, cheats_maxchamp")
