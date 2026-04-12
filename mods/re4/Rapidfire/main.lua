-- mods/Rapidfire/main.lua v12.0
-- ═══════════════════════════════════════════════════════════════════════
-- Rapidfire — Forces fully automatic rapid fire on all weapons.
--
-- v12.0 — Simplified. No trigger state machine, no BLOCK, no crash risk.
--   1. PostHook IsFiringBlocked → false (gun can always fire)
--   2. PostHook IsFullyAutomatic → true (holding trigger keeps firing)
--   3. PostHook IsReadyToFire → true (fire timer always "done")
--   4. Native hook UpdateFireTimer → accelerate cooldown timer
--   5. Native hook IsFireTimerDone → always returns 1 (ready)
--   6. Native hook GetEnemyReactionCooldown → 0 (no stagger cooldown)
--
-- Fire rate controlled by state.cooldown — lower = faster.
-- ═══════════════════════════════════════════════════════════════════════
local TAG = "Rapidfire"

local state = {
    enabled = true,
    cooldown = 0.15,  -- seconds between shots (lower = faster)
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
    Log(TAG .. ": Startup delay complete — mod active (cooldown=" .. state.cooldown .. "s)")
end)

-- ═══════════════════════════════════════════════════════════════════════
-- UE4SS POST-HOOKS — Override return values via ProcessEvent (safe)
--
-- These fire AFTER the original function runs. We overwrite the return
-- value in the parms buffer so the caller sees our value.
-- No BLOCK needed. Original always runs safely.
-- ═══════════════════════════════════════════════════════════════════════

-- IsFiringBlocked → always false (gun is never blocked from firing)
RegisterPostHook("/Script/Game.VR4GamePlayerGun:IsFiringBlocked", function(self, func, parms)
    if not state.enabled or not modReady then return end
    WriteU8(parms, 0)  -- false = not blocked
end)

-- IsFullyAutomatic → always true (holding trigger keeps firing)
RegisterPostHook("/Script/Game.VR4GamePlayerGun:IsFullyAutomatic", function(self, func, parms)
    if not state.enabled or not modReady then return end
    WriteU8(parms, 1)  -- true = full auto
end)

-- IsReadyToFire → always true (fire timer always "done" from ProcessEvent perspective)
RegisterPostHook("/Script/Game.VR4GamePlayerGun:IsReadyToFire", function(self, func, parms)
    if not state.enabled or not modReady then return end
    WriteU8(parms, 1)  -- true = ready to fire
end)

-- WasTriggerJustPressed → true when trigger held (for semi-auto guns)
RegisterPostHook("/Script/Game.VR4GamePlayerGun:WasTriggerJustPressed", function(self, func, parms)
    if not state.enabled or not modReady then return end
    WriteU8(parms, 1)  -- true = trigger just pressed
end)

Log(TAG .. ": 4 UE4SS PostHooks registered (IsFiringBlocked, IsFullyAutomatic, IsReadyToFire, WasTriggerJustPressed)")

-- ═══════════════════════════════════════════════════════════════════════
-- NATIVE HOOKS — Fire timer acceleration + enemy reaction bypass
-- ═══════════════════════════════════════════════════════════════════════

local lastFireTime = 0  -- os.clock() of last rapid fire shot

-- UpdateFireTimer(self, float deltaTime) — accelerate the cooldown timer
pcall(function()
    RegisterNativeHook("UpdateFireTimer",
        function(self_ptr, dt)
            if not state.enabled or not modReady then return self_ptr, dt end
            local now = os.clock()
            if (now - lastFireTime) >= state.cooldown then
                lastFireTime = now
                return self_ptr, 999.0  -- complete timer instantly → fires now
            end
            return self_ptr, 0.0  -- hold timer at zero until next cooldown window
        end, nil, "pf")
    Log(TAG .. ": UpdateFireTimer hooked (cooldown=" .. state.cooldown .. "s)")
end)

-- IsFireTimerDone() → always returns 1 (timer complete, ready to fire)
pcall(function()
    RegisterNativeHook("IsFireTimerDone", nil,
        function(retval)
            if not state.enabled or not modReady then return retval end
            return 1  -- timer done
        end)
    Log(TAG .. ": IsFireTimerDone hooked")
end)

-- GetEnemyReactionCooldown() → 0 (no stagger/reaction delay between hits)
pcall(function()
    RegisterNativeHook("GetEnemyReactionCooldown", nil,
        function(retval)
            if not state.enabled or not modReady then return retval end
            return 0.0  -- instant reaction
        end, ">f")
    Log(TAG .. ": GetEnemyReactionCooldown hooked")
end)

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
--
--   echo '{"cmd":"rapidfire"}' | nc 127.0.0.1 19420           → toggle
--   echo '{"cmd":"rapidfire","args":"0.05"}' | nc ...          → set 0.05s
-- ═══════════════════════════════════════════════════════════════════════
RegisterCommand("rapidfire", function(args)
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
            state.enabled = v
            lastFireTime = 0
            ModConfig.Save("Rapidfire", state)
        end)
end

Log(TAG .. ": v12.0 loaded — cooldown=" .. state.cooldown .. "s | No state machine, pure PostHook + timer acceleration")
