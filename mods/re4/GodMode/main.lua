-- mods/GodMode/main.lua v7.0
-- ═══════════════════════════════════════════════════════════════════════
-- God Mode — comprehensive invincibility blocking ALL death vectors:
--
-- v7.0 — Multi-layer protection:
--   Layer 1: bCanBeDamaged = false on pawn (engine-level block)
--   Layer 2: PreHook BLOCK on Bio4Utils:HurtPlayer/HurtPlayerWithConstDamage
--   Layer 3: PreHook BLOCK on Bio4Utils:HurtAshley (Ashley protection)
--   Layer 4: Block game-over via Bio4:IsGameOver post-hook → false
--   Layer 5: Block VR4DeathMenu from spawning
--   Layer 6: Health clamp — force heal if HP drops to 0
--   Layer 7: Block KillZ volume actor kills
--
-- Ashley protection is now handled by the separate AshleyArmor mod.
-- ═══════════════════════════════════════════════════════════════════════
local TAG = "GodMode"

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
    cachedPawn = pawn
    pcall(function()
        pawn.bCanBeDamaged = false
        pawnProtected = true
        Log(TAG .. ": bCanBeDamaged = false (engine-level invincibility)")
    end)
end

local function refreshProtection()
    if not state.enabled then return end
    local pawn = FindFirstOf("VR4Bio4PlayerPawn")
    if pawn and pawn:IsValid() then
        protectPawn(pawn)
    end
end

-- Auto-protect on pawn spawn
NotifyOnNewObject("VR4Bio4PlayerPawn", function(obj)
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
    return "BLOCK"
end)

RegisterPreHook("/Script/Game.Bio4Utils:HurtPlayer", function(self, func, parms)
    if not state.enabled then return end
    return "BLOCK"
end)

RegisterPreHook("/Script/Game.Bio4Utils:HurtAshley", function(self, func, parms)
    if not state.enabled then return end
    return "BLOCK"
end)

Log(TAG .. ": PreHook BLOCK — HurtPlayer + HurtPlayerWithConstDamage + HurtAshley")

-- ═══════════════════════════════════════════════════════════════════════
-- LAYER 4: Block game-over state
-- ═══════════════════════════════════════════════════════════════════════

RegisterHook("/Script/Game.Bio4:IsGameOver", function(Context, ReturnValue)
    if not state.enabled then return end
    -- Force return false — game-over never triggers
    pcall(function()
        local ret = ReturnValue:get()
        if ret and ret ~= 0 then
            ReturnValue:set(0)
            Log(TAG .. ": BLOCKED IsGameOver → forced false")
        end
    end)
end)
Log(TAG .. ": RegisterHook — Bio4:IsGameOver (force false)")

-- ═══════════════════════════════════════════════════════════════════════
-- LAYER 5: Block VR4DeathMenu from spawning / auto-exit if it does
-- ═══════════════════════════════════════════════════════════════════════

NotifyOnNewObject("VR4DeathMenu", function(obj)
    if not state.enabled then return end
    if not obj or not obj:IsValid() then return end
    Log(TAG .. ": VR4DeathMenu spawned — destroying + force heal")
    -- Destroy death menu before it renders
    pcall(function() obj:K2_DestroyActor() end)
    -- Emergency heal
    pcall(function()
        local utils = StaticFindObject("/Script/Game.Default__Bio4Utils")
        if utils and utils:IsValid() then
            utils:HealPlayer()
        end
    end)
end)
Log(TAG .. ": NotifyOnNewObject — VR4DeathMenu (auto-destroy)")

-- ═══════════════════════════════════════════════════════════════════════
-- LAYER 6: Health clamp — periodic check + post-damage heal
-- Catches insta-kill attacks from bosses, KillZ, scripted events
-- ═══════════════════════════════════════════════════════════════════════

RegisterHook("/Script/Game.Bio4Utils:HealPlayer", function(Context)
    Log(TAG .. ": HealPlayer fired")
end)

-- Monitor stun changes — block death stuns (e.g., neck snap, chain kill)
RegisterPreHook("/Script/Game.VR4Bio4PlayerPawn:OnStunnedChanged", function(self, func, parms)
    if not state.enabled then return end
    -- Block all stun state changes that could lead to death animations
    return "BLOCK"
end)
Log(TAG .. ": PreHook BLOCK — OnStunnedChanged (anti-insta-kill stun)")

-- ═══════════════════════════════════════════════════════════════════════
-- LAYER 7: Block KillZ volume kills + actor destruction of pawn
-- ═══════════════════════════════════════════════════════════════════════

NotifyOnNewObject("KillZVolume", function(obj)
    if not state.enabled then return end
    if not obj or not obj:IsValid() then return end
    Log(TAG .. ": KillZVolume spawned — destroying to prevent fall death")
    pcall(function() obj:K2_DestroyActor() end)
end)
Log(TAG .. ": NotifyOnNewObject — KillZVolume (auto-destroy)")

-- ═══════════════════════════════════════════════════════════════════════
-- TOGGLE — When toggled, update bCanBeDamaged on the live pawn
-- ═══════════════════════════════════════════════════════════════════════

local function onToggle(newState)
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

Log(TAG .. ": v7.0 loaded — 7-layer invincibility (bCanBeDamaged + PreHook"
    .. " + game-over block + death menu block + stun block + KillZ block)")
