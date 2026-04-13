-- mods/Rapidfire/main.lua v13.0
-- ═══════════════════════════════════════════════════════════════════════
-- Rapidfire — Forces fully automatic rapid fire on all weapons.
--
-- v13.0 — Complete rewrite with mangled symbol resolution + direct parms writes.
--   PostHooks: WriteU8(parms) for bool returns (CastParms removed)
--   Native hooks: Use mangled C++ names with fallback offsets for reliability
--   Removed: GetEnemyReactionCooldown (wrong class — was on VR4GamePlayerKnife)
--   Added: Diagnostic logging to verify hooks fire
--
-- Architecture:
--   1. PostHook IsFiringBlocked → false (override via ProcessEvent path)
--   2. PostHook IsFullyAutomatic → true (ProcessEvent path)
--   3. PostHook IsReadyToFire → true (ProcessEvent path)
--   4. Native hook UpdateFireTimer → dt=999.0 (force timer completion)
--   5. Native hook IsFireTimerDone → always returns 1
--
-- Note: PostHooks fire when game calls via ProcessEvent (Blueprint paths).
-- Native hooks fire when game calls via C++ vtable (primary code path).
-- Both are installed for maximum coverage.
-- ═══════════════════════════════════════════════════════════════════════
local TAG = "Rapidfire"
local VERBOSE = false
local function V(...) if VERBOSE then Log(TAG .. " [V] " .. string.format(...)) end end

local state = {
    enabled = true,
    cooldown = 0.10,  -- seconds between shots (lower = faster)
}

local saved = ModConfig.Load("Rapidfire")
if saved then
    if saved.enabled ~= nil then state.enabled = saved.enabled end
    if saved.cooldown ~= nil then state.cooldown = saved.cooldown end
end

-- ═══════════════════════════════════════════════════════════════════════
-- 0.5s STARTUP DELAY — let game fully initialize before overriding
-- ═══════════════════════════════════════════════════════════════════════
local modReady = false
ExecuteWithDelay(500, function()
    modReady = true
    Log(TAG .. ": Startup delay complete — mod active")
end)

-- ═══════════════════════════════════════════════════════════════════════
-- UE4SS POST-HOOKS — Override return values via ProcessEvent (safe)
--
-- These fire AFTER the original function runs. We overwrite the return
-- value directly in the parms buffer (bool @ offset 0x0 for parameterless
-- bool-returning functions).
-- ═══════════════════════════════════════════════════════════════════════

local hookCount = 0
local postHookFires = 0  -- diagnostic counter

-- IsFiringBlocked → always false (gun is never blocked from firing)
-- Parms layout: {bool ReturnValue @ 0x0}
pcall(function()
    RegisterPostHook("/Script/Game.VR4GamePlayerGun:IsFiringBlocked", function(self, func, parms)
        if not state.enabled or not modReady then return end
        pcall(function() WriteU8(parms, 0) end)  -- false = not blocked
        postHookFires = postHookFires + 1
        if postHookFires <= 3 then
            Log(TAG .. ": PostHook IsFiringBlocked → false (fire #" .. postHookFires .. ")")
        end
    end)
    hookCount = hookCount + 1
end)

-- IsFullyAutomatic → always true (holding trigger keeps firing)
pcall(function()
    RegisterPostHook("/Script/Game.VR4GamePlayerGun:IsFullyAutomatic", function(self, func, parms)
        if not state.enabled or not modReady then return end
        pcall(function() WriteU8(parms, 1) end)  -- true = full auto
    end)
    hookCount = hookCount + 1
end)

-- IsReadyToFire → always true (fire timer always "done")
pcall(function()
    RegisterPostHook("/Script/Game.VR4GamePlayerGun:IsReadyToFire", function(self, func, parms)
        if not state.enabled or not modReady then return end
        pcall(function() WriteU8(parms, 1) end)  -- true = ready
    end)
    hookCount = hookCount + 1
end)

Log(TAG .. ": " .. hookCount .. "/3 PostHooks registered (IsFiringBlocked, IsFullyAutomatic, IsReadyToFire)")

-- ═══════════════════════════════════════════════════════════════════════
-- NATIVE HOOKS — Fire timer acceleration via Dobby (C++ code path)
--
-- These are the PRIMARY mechanism for rapid fire since the game's
-- shooting logic calls these functions via C++ vtable, not ProcessEvent.
-- ═══════════════════════════════════════════════════════════════════════

local lastFireTime = 0  -- os.clock() of last rapid fire shot
local nativeHookFires = 0  -- diagnostic counter

-- UpdateFireTimer(float deltaTime)
-- Mangled: _ZN17AVR4GamePlayerGun15UpdateFireTimerEf @ 0x062D9E20
-- Pre-hook: override dt to control fire rate
local sym_UpdateFireTimer = nil
pcall(function() sym_UpdateFireTimer = Resolve("_ZN17AVR4GamePlayerGun15UpdateFireTimerEf", 0x062D9E20) end)
if not sym_UpdateFireTimer then
    pcall(function() sym_UpdateFireTimer = Resolve("UpdateFireTimer", 0x062D9E20) end)
end

if sym_UpdateFireTimer then
    pcall(function()
        RegisterNativeHookAt(sym_UpdateFireTimer, "UpdateFireTimer_gun",
            function(self_ptr, dt)
                if not state.enabled or not modReady then return self_ptr, dt end
                nativeHookFires = nativeHookFires + 1
                if nativeHookFires <= 3 then
                    Log(TAG .. ": Native UpdateFireTimer fire #" .. nativeHookFires)
                end
                local now = os.clock()
                if (now - lastFireTime) >= state.cooldown then
                    lastFireTime = now
                    return self_ptr, 999.0  -- complete timer instantly → fires now
                end
                return self_ptr, 0.0  -- hold timer at zero until next cooldown window
            end, nil, "pf")
        Log(TAG .. ": UpdateFireTimer hooked (mangled) — cooldown=" .. state.cooldown .. "s")
    end)
else
    Log(TAG .. ": [WARN] UpdateFireTimer not resolved")
end

-- IsFireTimerDone() → always returns 1 (timer complete, ready to fire)
-- Mangled: _ZNK17AVR4GamePlayerGun15IsFireTimerDoneEv @ 0x062DBAF8
-- Post-hook: override return value to 1
local sym_IsFireTimerDone = nil
pcall(function() sym_IsFireTimerDone = Resolve("_ZNK17AVR4GamePlayerGun15IsFireTimerDoneEv", 0x062DBAF8) end)
if not sym_IsFireTimerDone then
    pcall(function() sym_IsFireTimerDone = Resolve("IsFireTimerDone", 0x062DBAF8) end)
end

if sym_IsFireTimerDone then
    pcall(function()
        RegisterNativeHookAt(sym_IsFireTimerDone, "IsFireTimerDone_gun",
            nil,
            function(retval)
                if not state.enabled or not modReady then return retval end
                -- Rate-limit to match UpdateFireTimer's cooldown
                local now = os.clock()
                if (now - lastFireTime) < state.cooldown then
                    return retval  -- respect cooldown — return original value
                end
                return 1  -- timer done → ready to fire
            end)
        Log(TAG .. ": IsFireTimerDone hooked (mangled)")
    end)
else
    Log(TAG .. ": [WARN] IsFireTimerDone not resolved")
end

-- GetEnemyReactionCooldown — REMOVED
-- This function is on VR4GamePlayerKnife (wrong class), not VR4GamePlayerGun.
-- It does not affect gun fire rate. Hooking it would only affect knife stagger.

-- ═══════════════════════════════════════════════════════════════════════
-- CDO — force laser sight while firing (cosmetic)
-- ═══════════════════════════════════════════════════════════════════════
pcall(function()
    local cdo = GetCDO("VR4GamePlayerGun")
    if cdo then
        pcall(function() cdo.ShowLaserSightWhileFiring = true end)
        Log(TAG .. ": CDO ShowLaserSightWhileFiring=true")
    end
end)

-- ═══════════════════════════════════════════════════════════════════════
-- COMMANDS — toggle + cooldown tuning via ADB bridge
-- ═══════════════════════════════════════════════════════════════════════
RegisterCommand("rapidfire", function(args)
    V("rapidfire command args=%s", tostring(args))
    if args and args ~= "" then
        local val = tonumber(args)
        if val and val > 0 then
            state.cooldown = val
            ModConfig.Save("Rapidfire", state)
            Log(TAG .. ": cooldown set to " .. state.cooldown .. "s")
            Notify(TAG, "Cooldown: " .. state.cooldown .. "s")
            return "cooldown=" .. state.cooldown
        end
    end
    state.enabled = not state.enabled
    lastFireTime = 0
    ModConfig.Save("Rapidfire", state)
    Log(TAG .. ": " .. (state.enabled and "ON" or "OFF"))
    Notify(TAG, state.enabled and "ON" or "OFF")
    return state.enabled and "ON" or "OFF"
end)

-- ═══════════════════════════════════════════════════════════════════════
-- DEBUG MENU
-- ═══════════════════════════════════════════════════════════════════════
if SharedAPI and SharedAPI.DebugMenu then
    SharedAPI.DebugMenu.RegisterToggle("Rapidfire", "Rapidfire",
        function() return state.enabled end,
        function(v)
            V("DebugMenu toggle → %s", tostring(v))
            state.enabled = v
            lastFireTime = 0
            ModConfig.Save("Rapidfire", state)
        end)
end

Log(TAG .. ": v13.0 loaded — cooldown=" .. state.cooldown .. "s | " .. hookCount .. " PostHooks + mangled native hooks")
