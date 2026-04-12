-- mods/RadioSkip/main.lua v6.3
-- ═══════════════════════════════════════════════════════════════════════
-- Radio Auto-Skip — when a VR4Communicator spawns, immediately call
-- Open() then Abort() so it closes itself without player interaction.
-- No blocking — lets the game flow naturally, just auto-dismisses.
-- ═══════════════════════════════════════════════════════════════════════
local TAG = "RadioSkip"

local state = {
    enabled = true,
}

local saved = ModConfig.Load("RadioSkip")
if saved then
    if saved.enabled ~= nil then state.enabled = saved.enabled end
end

-- ═══════════════════════════════════════════════════════════════════════
-- SKIP FLAGS — Enable all communicator skip flags (global failsafe)
-- ═══════════════════════════════════════════════════════════════════════

local base = GetLibBase()
local sym_SkipFlags = nil
if base then
    pcall(function()
        sym_SkipFlags = Offset(base, 0x0A5AA430)
        for i = 0, 7 do WriteU8(Offset(sym_SkipFlags, i), 1) end
        Log(TAG .. ": sVR4SkipCommunicators flags set (8 bytes)")
    end)
    if not sym_SkipFlags then
        Log(TAG .. ": Skip flags write failed (pcall caught) — fallback to UE4SS hooks only")
    end
end

-- ═══════════════════════════════════════════════════════════════════════
-- UE4SS SPAWN TRACKING — Auto Open() → Abort() on every new instance
-- ═══════════════════════════════════════════════════════════════════════

local commCount = 0

NotifyOnNewObject("VR4Communicator", function(obj)
    if not state.enabled then return end
    if not obj or not obj:IsValid() then return end
    commCount = commCount + 1
    Log(TAG .. ": VR4Communicator spawned #" .. commCount .. " — auto Open→Abort")

    -- Let it initialize, then Open → Abort to close it naturally
    ExecuteWithDelay(50, function()
        if not obj:IsValid() then return end
        pcall(function() obj:Open() end)
        pcall(function() obj:Abort() end)
        pcall(function() obj:Cleanup() end)
        Log(TAG .. ": Communicator #" .. commCount .. " auto-dismissed")
    end)
end)
Log(TAG .. ": NotifyOnNewObject — VR4Communicator (auto Open→Abort)")

-- ═══════════════════════════════════════════════════════════════════════
-- UE4SS HOOK — Fallback: if Open() is called on an existing instance
-- that NotifyOnNewObject missed, auto-abort it here too
-- ═══════════════════════════════════════════════════════════════════════

RegisterHook("/Script/Game.VR4Communicator:Open", function(Context, Parms)
    if not state.enabled then return end
    local self = Context:get()
    if not self or not self:IsValid() then return end
    Log(TAG .. ": VR4Communicator:Open fired — auto Abort+Cleanup")
    pcall(function() self:Abort() end)
    pcall(function() self:Cleanup() end)
end)
Log(TAG .. ": RegisterHook — VR4Communicator:Open (auto Abort fallback)")

-- ═══════════════════════════════════════════════════════════════════════
-- NATIVE HOOKS — Backup: auto-abort after native Open completes
-- ═══════════════════════════════════════════════════════════════════════

local sym_Abort   = Resolve("VR4Communicator_Abort",   0x0627EB6C)
local sym_Cleanup = Resolve("VR4Communicator_Cleanup", 0x0627DEE4)

pcall(function()
    RegisterNativeHook("VR4Communicator_Open", nil,
        function(retval, self_ptr)
            if not state.enabled then return retval end
            if not self_ptr or self_ptr == 0 then return retval end
            -- After Open finishes, immediately Abort + Cleanup
            if sym_Abort   then CallNative(sym_Abort,   "vp", self_ptr) end
            if sym_Cleanup then CallNative(sym_Cleanup, "vp", self_ptr) end
            Log(TAG .. ": Native post-Open → Abort+Cleanup")
            return retval
        end)
    Log(TAG .. ": Native hook — VR4Communicator_Open post → Abort+Cleanup")
end)

-- ═══════════════════════════════════════════════════════════════════════
-- COMMANDS
-- ═══════════════════════════════════════════════════════════════════════

RegisterCommand("radioskip", function()
    state.enabled = not state.enabled
    ModConfig.Save("RadioSkip", state)
    Log(TAG .. ": " .. (state.enabled and "ON" or "OFF"))
    Notify(TAG, state.enabled and "Radio skip ON" or "Radio skip OFF")
end)

RegisterCommand("radioskip_status", function()
    local info = TAG .. ": enabled=" .. tostring(state.enabled)
        .. " commDismissed=" .. commCount
    local comms = FindAllOf("VR4Communicator")
    if comms then
        info = info .. " | Alive: " .. #comms
    else
        info = info .. " | Alive: 0"
    end
    Log(info)
end)

if SharedAPI and SharedAPI.DebugMenu then
    SharedAPI.DebugMenu.RegisterToggle("RadioSkip", "Radio Auto-Skip",
        function() return state.enabled end,
        function(v) state.enabled = v; ModConfig.Save("RadioSkip", state) end)
end

Log(TAG .. ": v6.2 loaded — auto Open→Abort on spawn + Open hook fallback + native post-hook + skip flags")
Notify(TAG, "Radio auto-skip active")
