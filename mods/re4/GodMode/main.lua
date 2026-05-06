-- mods/GodMode/main.lua v8.0
-- ═══════════════════════════════════════════════════════════════════════
-- God Mode — Damage prevention ONLY. Does NOT block death sequences.
--
-- v8.0 — Fix softlock: Removed kill-screen/game-over/stun blocks.
--   ONLY prevents damage. Death menu, game-over state, and stun
--   animations are allowed to play through normally. This prevents
--   softlocks when the player triggers a scripted death or boss kill.
--
-- Layers:
--   Layer 1: bCanBeDamaged = false on pawn (engine-level damage block)
--   Layer 2: PreHook BLOCK on Bio4Utils:HurtPlayer / HurtPlayerWithConstDamage
--   Layer 3: PreHook BLOCK on Bio4Utils:HurtAshley (Ashley protection)
--   Layer 4: Block KillZ volume actor kills (fall-death protection)
--
-- REMOVED (caused softlocks):
--   - IsGameOver post-hook forcing false
--   - VR4DeathMenu spawn blocking/destroying
--   - OnStunnedChanged blocking (prevented grab/stun animations)
-- ═══════════════════════════════════════════════════════════════════════
local TAG = "GodMode"
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

local state = { enabled = true }

local saved = ModConfig.Load("GodMode")
if saved then
    if saved.enabled ~= nil then state.enabled = saved.enabled end
end

-- ═══════════════════════════════════════════════════════════════════════
-- LAYER 1: bCanBeDamaged = false (UE4 engine-level damage gate)
-- This is the most efficient protection — the engine skips ALL damage
-- processing before any game code runs.
-- ═══════════════════════════════════════════════════════════════════════

local cachedPawn = nil
local pawnProtected = false

local function protectPawn(pawn)
    if not pawn or not pawn:IsValid() then return end
    V("protectPawn called, pawn=%s", tostring(pawn))
    cachedPawn = pawn
    pcall(function()
        pawn.bCanBeDamaged = false
        pawnProtected = true
        Log(TAG .. ": bCanBeDamaged = false (engine-level invincibility)")
    end)
end

local function refreshProtection()
    if not state.enabled then return end
    V("refreshProtection called")
    local pawn = findFirstNonDefault("VR4Bio4PlayerPawn")
    if pawn and pawn:IsValid() then
        protectPawn(pawn)
    end
end

-- Auto-protect on pawn spawn
NotifyOnNewObject("VR4Bio4PlayerPawn", function(obj)
    V("NotifyOnNewObject VR4Bio4PlayerPawn fired, obj=%s", tostring(obj))
    if obj and obj:IsValid() then
        cachedPawn = obj
        if state.enabled then
            protectPawn(obj)
        end
        Log(TAG .. ": VR4Bio4PlayerPawn cached (protected=" .. tostring(pawnProtected) .. ")")
    end
end)

-- ═══════════════════════════════════════════════════════════════════════
-- LAYER 2+3: PreHook BLOCK on all Bio4Utils damage functions
-- ═══════════════════════════════════════════════════════════════════════

RegisterPreHook("/Script/Game.Bio4Utils:HurtPlayerWithConstDamage", function(self, func, parms)
    if not state.enabled then return end
    V("BLOCK HurtPlayerWithConstDamage")
    return "BLOCK"
end)

RegisterPreHook("/Script/Game.Bio4Utils:HurtPlayer", function(self, func, parms)
    if not state.enabled then return end
    V("BLOCK HurtPlayer")
    return "BLOCK"
end)

RegisterPreHook("/Script/Game.Bio4Utils:HurtAshley", function(self, func, parms)
    if not state.enabled then return end
    V("BLOCK HurtAshley")
    return "BLOCK"
end)

Log(TAG .. ": PreHook BLOCK — HurtPlayer + HurtPlayerWithConstDamage + HurtAshley")

-- NOTE: Layers 4-6 from v7.0 REMOVED — they caused softlocks:
--   Layer 4 (IsGameOver → false): blocked kill screen, game couldn't restart
--   Layer 5 (VR4DeathMenu destroy): blocked death UI, softlocked on boss kills
--   Layer 6 (OnStunnedChanged block): prevented grab/stun animations, broke gameplay
-- Death sequences and game-over MUST be allowed to complete normally.
-- Damage prevention (Layers 1-3 above) is sufficient for god mode.

-- ═══════════════════════════════════════════════════════════════════════
-- LAYER 4: Block KillZ volume kills + actor destruction of pawn
-- ═══════════════════════════════════════════════════════════════════════

-- Wait for player pawn before destroying KillZ volumes
local function waitForPlayerThenDestroyKillZ(obj)
    local pawn = findFirstNonDefault("VR4GamePlayerPawn")
    if not pawn or not pawn:IsValid() then
        -- Player not ready — retry in 200ms
        ExecuteWithDelay(200, function()
            if obj and obj:IsValid() then
                waitForPlayerThenDestroyKillZ(obj)
            end
        end)
        return
    end
    
    -- Player exists — safe to destroy after one frame
    ExecuteWithDelay(100, function()
        V("KillZVolume deferred destroy executing")
        if obj and obj:IsValid() then
            pcall(function() obj:K2_DestroyActor() end)
            Log(TAG .. ": KillZVolume destroyed (deferred)")
        end
    end)
end

NotifyOnNewObject("KillZVolume", function(obj)
    if not state.enabled then return end
    if not obj or not obj:IsValid() then return end
    V("NotifyOnNewObject KillZVolume fired, obj=%s", tostring(obj))
    Log(TAG .. ": KillZVolume spawned — scheduling deferred destroy")
    
    -- Disable collision immediately to prevent kills
    pcall(function() obj:SetActorEnableCollision(false) end)
    waitForPlayerThenDestroyKillZ(obj)
end)
Log(TAG .. ": NotifyOnNewObject — KillZVolume (auto-destroy, deferred)")

-- ═══════════════════════════════════════════════════════════════════════
-- TOGGLE — When toggled, update bCanBeDamaged on the live pawn
-- ═══════════════════════════════════════════════════════════════════════

local function onToggle(newState)
    V("onToggle: enabled %s -> %s", tostring(state.enabled), tostring(newState))
    state.enabled = newState
    ModConfig.Save("GodMode", state)
    if cachedPawn and cachedPawn:IsValid() then
        pcall(function()
            cachedPawn.bCanBeDamaged = not newState
            pawnProtected = newState
        end)
    end
    if newState then refreshProtection() end
end

-- ═══════════════════════════════════════════════════════════════════════
-- COMMANDS
-- ═══════════════════════════════════════════════════════════════════════

RegisterCommand("godmode", function()
    onToggle(not state.enabled)
    Log(TAG .. ": " .. (state.enabled and "ON" or "OFF"))
    Notify(TAG, state.enabled and "ON" or "OFF")
end)

RegisterCommand("godmode_status", function()
    refreshProtection()
    local info = TAG .. ": enabled=" .. tostring(state.enabled)
        .. " pawnProtected=" .. tostring(pawnProtected)
    if cachedPawn and cachedPawn:IsValid() then
        pcall(function()
            local hp = cachedPawn:GetPlayerCurrentHealth()
            local maxHp = cachedPawn:GetPlayerMaxHealth()
            if hp and maxHp then info = info .. " | Leon=" .. hp .. "/" .. maxHp end
            info = info .. " | bCanBeDamaged=" .. tostring(cachedPawn.bCanBeDamaged)
        end)
    end
    Log(info)
end)

-- ═══════════════════════════════════════════════════════════════════════
-- DEBUG MENU
-- ═══════════════════════════════════════════════════════════════════════

if SharedAPI and SharedAPI.DebugMenu then
    SharedAPI.DebugMenu.RegisterToggle("GodMode", "God Mode",
        function() return state.enabled end,
        function(v) onToggle(v) end)
end

Log(TAG .. ": v8.0 loaded — damage prevention only (bCanBeDamaged + PreHook + KillZ)")
Log(TAG .. ": Death menu, game-over, and stun animations are NOT blocked (no softlock)")
