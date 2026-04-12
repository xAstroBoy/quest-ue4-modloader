-- mods/3DCutscenesFix/main.lua v12.2
-- ═══════════════════════════════════════════════════════════════════════
-- UE4SS-style 3D Cutscenes — teleports VR player to cinematic camera.
--
-- v12.0 — Full UE4SS API:
--   RegisterHook on VR4GamePlayerPawn:OnCameraModeChanged
--   FindFirstOf("VR4GamePlayerPawn") for pawn caching
--   NotifyOnNewObject("VR4GamePlayerPawn") for auto-detect
--   Native Dobby hooks on CutsceneUpdateCamera + SceEvent Begin/End
-- ═══════════════════════════════════════════════════════════════════════
local TAG = "3DCutscenesFix"

local state = {
    enabled    = true,
    inCutscene = false,
}

local saved = ModConfig.Load("3DCutscenesFix")
if saved then
    if saved.enabled ~= nil then state.enabled = saved.enabled end
end

-- ═══════════════════════════════════════════════════════════════════════
-- UE4SS PAWN TRACKING — FindFirstOf + NotifyOnNewObject
-- ═══════════════════════════════════════════════════════════════════════

local cachedPawn = nil
local gamePawnReady = false  -- gate: don't touch camera until real game pawn exists

local function refreshPawn()
    local pawn = FindFirstOf("VR4GamePlayerPawn")
    if pawn and pawn:IsValid() then
        cachedPawn = pawn:GetAddress()
        gamePawnReady = true
        return true
    end
    return false
end

-- Auto-detect pawn spawn — DELAY gamePawnReady so camera manager fully initializes.
-- The crash (SIGSEGV in UpdateViewTargetInternal) happens when SetAnchorInternal
-- is called before the pawn's camera subsystem is ready.
NotifyOnNewObject("VR4GamePlayerPawn", function(obj)
    if obj and obj:IsValid() then
        cachedPawn = obj:GetAddress()
        Log(TAG .. ": VR4GamePlayerPawn spawned — cached @ " .. ToHex(cachedPawn) .. " (delaying ready 3s)")
        -- Wait 3 seconds for camera manager to fully initialize
        ExecuteWithDelay(3000, function()
            if obj:IsValid() then
                cachedPawn = obj:GetAddress()
                gamePawnReady = true
                Log(TAG .. ": Game pawn READY (camera init grace period elapsed)")
            end
        end)
    end
end)
Log(TAG .. ": NotifyOnNewObject — VR4GamePlayerPawn (3s camera init delay)")

-- ── INITIAL PAWN SCAN — catch pawn that already exists at mod load time ──
-- The pawn may have spawned BEFORE our hooks were registered. Scan now and
-- also retry a few times with delays to catch late spawns.
local function tryInitialPawnScan()
    if gamePawnReady then return end
    if refreshPawn() then
        Log(TAG .. ": Pawn found via initial FindFirstOf scan — READY")
    end
end

-- Try immediately at load time
tryInitialPawnScan()

-- Retry at 1s, 3s, 5s, 10s in case pawn spawns after our load but before
-- NotifyOnNewObject hooks have a chance to fire
for _, delay in ipairs({1000, 3000, 5000, 10000}) do
    ExecuteWithDelay(delay, function()
        if not gamePawnReady then
            if refreshPawn() then
                Log(TAG .. ": Pawn found via delayed scan (" .. delay .. "ms) — READY")
            end
        end
    end)
end

-- ═══════════════════════════════════════════════════════════════════════
-- UE4SS HOOK — OnCameraModeChanged (pawn discovery via PE)
-- ═══════════════════════════════════════════════════════════════════════

RegisterHook("/Script/Game.VR4GamePlayerPawn:OnCameraModeChanged", function(Context, Parms)
    local self = Context:get()
    if self and self:IsValid() then
        local addr = self:GetAddress()
        if addr and addr ~= cachedPawn then
            cachedPawn = addr
            Log(TAG .. ": Cached pawn from OnCameraModeChanged @ " .. ToHex(addr))
        end
        -- Read camera mode via UE4SS reflection
        pcall(function()
            local mode = self.CameraMode
            if mode then Log(TAG .. ": CameraMode → " .. tostring(mode)) end
        end)
    end
end)
Log(TAG .. ": RegisterHook — VR4GamePlayerPawn:OnCameraModeChanged")

-- ═══════════════════════════════════════════════════════════════════════
-- NATIVE HOOKS — Dobby hooks on stripped C++ cutscene functions
-- ═══════════════════════════════════════════════════════════════════════

local sym_setAnchor = Resolve(
    "_ZN14AVR4PlayerPawn17SetAnchorInternalERK10FTransform", 0x068410D0)

local sym_updateCamera = Resolve(
    "_ZN22AVR4CutscenePlayerPawn12UpdateCameraERK10FTransformfb", 0x06443E64)

if sym_updateCamera and sym_setAnchor then
    pcall(function()
    RegisterNativeHookAt(sym_updateCamera, "CutsceneUpdateCamera", nil,
        function(retval, self_ptr, transform_ptr, fovY, screenOn)
            if not state.enabled then return retval end
            if not transform_ptr then return retval end
            -- CRITICAL: Don't engage until the real VR game pawn exists.
            -- At boot the game runs intro cutscenes before the world is ready.
            -- Teleporting the camera then = black void.
            if not gamePawnReady or not cachedPawn then return retval end
            -- Validate the cached pawn is still alive before touching its camera
            local pawn = FindFirstOf("VR4GamePlayerPawn")
            if not pawn or not pawn:IsValid() then return retval end
            cachedPawn = pawn:GetAddress() -- refresh in case it moved
            state.inCutscene = true
            -- CRITICAL: Do NOT use self_ptr as target — that's the CutscenePlayerPawn,
            -- not the real VR game pawn. SetAnchorInternal must be called on the
            -- actual VR4GamePlayerPawn (cachedPawn) to move the VR camera.
            CallNative(sym_setAnchor, "v", cachedPawn, transform_ptr)
            return retval
        end, "ppfi")
    Log(TAG .. ": Native hook — CutsceneUpdateCamera → SetAnchorInternal teleport (gated on game pawn)")
    end)
else
    if not sym_updateCamera then LogError(TAG .. ": UpdateCamera NOT FOUND") end
    if not sym_setAnchor then LogError(TAG .. ": SetAnchorInternal NOT FOUND") end
end

-- SceEvent hooks for cutscene begin/end detection
local sym_begin = Resolve("OnSceEventBegin", 0x0641A2EC)
local sym_end   = Resolve("OnSceEventEnd",   0x0641A340)

if sym_begin then
    pcall(function()
    RegisterNativeHookAt(sym_begin, "OnSceEventBegin", nil,
        function(retval, self_ptr)
            if not state.enabled then return retval end
            state.inCutscene = true
            Log(TAG .. ": Cutscene BEGIN")
            return retval
        end)
    end)
end

if sym_end then
    pcall(function()
    RegisterNativeHookAt(sym_end, "OnSceEventEnd", nil,
        function(retval, self_ptr)
            state.inCutscene = false
            Log(TAG .. ": Cutscene END")
            return retval
        end)
    end)
end

-- ═══════════════════════════════════════════════════════════════════════
-- COMMANDS — UE4SS reflection status
-- ═══════════════════════════════════════════════════════════════════════

RegisterCommand("cutscene", function()
    state.enabled = not state.enabled
    ModConfig.Save("3DCutscenesFix", state)
    Log(TAG .. ": " .. (state.enabled and "ON" or "OFF"))
    Notify(TAG, state.enabled and "3D Cutscenes ON" or "3D Cutscenes OFF")
end)

RegisterCommand("cutscene_status", function()
    refreshPawn()
    local info = TAG .. ": enabled=" .. tostring(state.enabled)
        .. " inCutscene=" .. tostring(state.inCutscene)
        .. " pawn=" .. tostring(cachedPawn ~= nil)
    -- Read player state via UE4SS
    local pawn = FindFirstOf("VR4GamePlayerPawn")
    if pawn and pawn:IsValid() then
        pcall(function()
            local active = pawn:IsActivePlayerPawn()
            if active ~= nil then info = info .. " | active=" .. tostring(active) end
        end)
        pcall(function()
            local mode = pawn.FirstPersonMovementMode
            if mode then info = info .. " | moveMode=" .. tostring(mode) end
        end)
    end
    Log(info)
end)

if SharedAPI and SharedAPI.DebugMenu then
    SharedAPI.DebugMenu.RegisterToggle("3DCutscenesFix", "3D Cutscenes",
        function() return state.enabled end,
        function(v) state.enabled = v; ModConfig.Save("3DCutscenesFix", state) end)
end

Log(TAG .. ": v12.1 loaded — UE4SS RegisterHook OnCameraModeChanged"
    .. " + NotifyOnNewObject pawn tracking + native Dobby CutsceneUpdateCamera"
    .. " | setAnchor=" .. tostring(sym_setAnchor ~= nil)
    .. " updateCamera=" .. tostring(sym_updateCamera ~= nil))
