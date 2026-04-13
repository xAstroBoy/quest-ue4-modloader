-- mods/DualWield/main.lua v6.0
-- =====================================================================
-- Dual Wield -- Grab the same weapon in both hands.
--
-- v6.0 -- Complete rewrite: Uses EnableDualWielding game setting + PostHooks.
--   1. VR4GamePlayerSettings.EnableDualWielding = true (game's built-in setting!)
--   2. PostHook IsPresentOnBody -> true (allows grabbing duplicate from holster)
--   3. PostHook IsPropGrabbable -> true (ensures all holster props stay grabbable)
--   Removed: Broken native hooks (CanGrab/IsGrabAllowed/IsPropGrabbable resolve failures)
--   The game has a NATIVE EnableDualWielding bool in VR4GamePlayerSettings.
--   We just need to flip it on via the settings function library.
-- =====================================================================
local TAG = "DualWield"
local VERBOSE = false
local function V(...) if VERBOSE then Log(TAG .. " [V] " .. string.format(...)) end end

local state = {
    enabled = true,
}

local saved = ModConfig.Load("DualWield")
if saved then
    if saved.enabled ~= nil then state.enabled = saved.enabled end
end

-- =====================================================================
-- CORE: Apply EnableDualWielding via VR4GamePlayerSettingsFunctionLibrary
-- The game natively supports dual wielding -- it's just a settings toggle!
-- VR4GamePlayerSettings struct (0xB8 bytes), EnableDualWielding @ offset 0x0030
-- =====================================================================

local settingApplied = false

local function applyDualWieldSetting(enable)
    -- Method 1: Via function library CDO (static functions)
    local ok1 = pcall(function()
        local lib = GetCDO("VR4GamePlayerSettingsFunctionLibrary")
        if not lib then
            V("CDO VR4GamePlayerSettingsFunctionLibrary not found")
            return
        end
        local settings = lib:Call("GetGamePlayerSettings")
        if not settings then
            V("GetGamePlayerSettings returned nil")
            return
        end
        settings.EnableDualWielding = enable
        lib:Call("SetGamePlayerSettings", settings)
        settingApplied = true
        V("SetGamePlayerSettings: EnableDualWielding=%s (CDO path)", tostring(enable))
    end)

    -- Method 2: Via pawn's player settings (runtime property)
    local ok2 = pcall(function()
        local pawn = FindFirstOf("VR4Bio4PlayerPawn")
        if not pawn then pawn = FindFirstOf("VR4GamePlayerPawn") end
        if not pawn then
            V("No player pawn found")
            return
        end
        -- Try to get settings via the pawn's Get method
        local settingsObj = pawn:Get("PlayerSettings")
        if settingsObj then
            settingsObj.EnableDualWielding = enable
            pawn:Set("PlayerSettings", settingsObj)
            settingApplied = true
            V("Pawn PlayerSettings.EnableDualWielding=%s", tostring(enable))
        end
    end)

    -- Method 3: Via save game settings
    local ok3 = pcall(function()
        local sg = FindFirstOf("VR4Bio4SaveGame")
        if not sg then
            V("No save game found")
            return
        end
        local settings = sg:Get("Settings")
        if settings then
            settings.EnableDualWielding = enable
            sg:Set("Settings", settings)
            V("SaveGame Settings.EnableDualWielding=%s", tostring(enable))
        end
    end)

    return settingApplied
end

-- =====================================================================
-- Apply setting immediately + delayed retry + periodic enforcement
-- =====================================================================

-- Try immediately
if state.enabled then
    applyDualWieldSetting(true)
    if settingApplied then
        Log(TAG .. ": EnableDualWielding=true set immediately")
    else
        Log(TAG .. ": Initial apply deferred (game not ready yet)")
    end
end

-- Delayed retry after 3 seconds (game objects need time to initialize)
ExecuteWithDelay(3000, function()
    if state.enabled and not settingApplied then
        applyDualWieldSetting(true)
        if settingApplied then
            Log(TAG .. ": EnableDualWielding=true set (delayed 3s)")
        else
            Log(TAG .. ": [WARN] EnableDualWielding still not applied after 3s")
        end
    end
end)

-- Re-enforce every 10 seconds (game may reset settings on level load)
local enforceCount = 0
local function scheduleEnforcement()
    ExecuteWithDelay(10000, function()
        if state.enabled then
            pcall(function() applyDualWieldSetting(true) end)
            enforceCount = enforceCount + 1
            if enforceCount <= 3 then
                V("Enforcement #%d: EnableDualWielding=true", enforceCount)
            end
        end
        scheduleEnforcement()  -- reschedule
    end)
end
scheduleEnforcement()

-- =====================================================================
-- POST-HOOKS: Override grab/present checks via ProcessEvent
-- These are BACKUP mechanisms in case the settings approach doesn't
-- fully enable dual wielding (some code paths might check these directly)
-- =====================================================================

local hookCount = 0
local postHookFires = 0

-- IsPresentOnBody -> true (weapon is "present on body" = can grab another copy)
-- Parms layout: {bool ReturnValue @ 0x0}
pcall(function()
    RegisterPostHook("/Script/Game.VR4GamePlayerProp:IsPresentOnBody", function(self, func, parms)
        if not state.enabled then return end
        pcall(function() WriteU8(parms, 1) end)  -- true = present on body
        postHookFires = postHookFires + 1
        if postHookFires <= 3 then
            Log(TAG .. ": PostHook IsPresentOnBody -> true (fire #" .. postHookFires .. ")")
        end
    end)
    hookCount = hookCount + 1
    Log(TAG .. ": PostHook IsPresentOnBody -> true registered")
end)

-- IsPropGrabbable -> true (all holster props stay grabbable even with duplicate)
-- Parms layout: {bool ReturnValue @ 0x0}
pcall(function()
    RegisterPostHook("/Script/Game.VR4GamePlayerPropHolster:IsPropGrabbable", function(self, func, parms)
        if not state.enabled then return end
        pcall(function() WriteU8(parms, 1) end)  -- true = grabbable
    end)
    hookCount = hookCount + 1
    Log(TAG .. ": PostHook IsPropGrabbable -> true registered")
end)

Log(TAG .. ": " .. hookCount .. "/2 PostHooks registered")

-- =====================================================================
-- COMMANDS
-- =====================================================================

RegisterCommand("dualwield", function()
    state.enabled = not state.enabled
    if state.enabled then
        applyDualWieldSetting(true)
    else
        applyDualWieldSetting(false)
    end
    ModConfig.Save("DualWield", state)
    Log(TAG .. ": " .. (state.enabled and "ON" or "OFF"))
    Notify(TAG, state.enabled and "ON" or "OFF")
    return state.enabled and "ON" or "OFF"
end)

RegisterCommand("dualwield_status", function()
    local info = TAG .. ": enabled=" .. tostring(state.enabled) .. " applied=" .. tostring(settingApplied)
    -- Check current setting value
    pcall(function()
        local lib = GetCDO("VR4GamePlayerSettingsFunctionLibrary")
        if lib then
            local settings = lib:Call("GetGamePlayerSettings")
            if settings then
                info = info .. " | EnableDualWielding=" .. tostring(settings.EnableDualWielding)
            end
        end
    end)
    -- Check active guns
    pcall(function()
        local guns = FindAllOf("VR4GamePlayerGun")
        if guns then info = info .. " | guns=" .. #guns end
    end)
    Log(info)
    return info
end)

-- =====================================================================
-- DEBUG MENU
-- =====================================================================
if SharedAPI and SharedAPI.DebugMenu then
    SharedAPI.DebugMenu.RegisterToggle("DualWield", "Dual Wield (Same Gun)",
        function() return state.enabled end,
        function(v)
            state.enabled = v
            if v then
                applyDualWieldSetting(true)
            else
                applyDualWieldSetting(false)
            end
            ModConfig.Save("DualWield", state)
        end)
end

Log(TAG .. ": v6.0 loaded -- EnableDualWielding setting + " .. hookCount .. " PostHooks + DebugMenu")
