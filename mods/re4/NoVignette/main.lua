-- mods/NoVignette/main.lua v4.0
-- ═══════════════════════════════════════════════════════════════════════
-- UE4SS-style Vignette Disabler — multi-layer approach:
--   1. RegisterPreHook to BLOCK SetVignetteActive
--   2. RegisterPostHook to override IsVignetteActive/IsAnyVignetteActive
--   3. NotifyOnNewObject to zero vignette properties on spawn
--   4. Native hooks for stripped C++ vignette functions (best-effort)
--
-- v4.0: Restored native hooks (modloader now has DobbyHook crash guard).
--   If any hook fails to install (stripped symbol → symbol not found,
--   or bad address → DobbyHook SIGSEGV caught by modloader), the mod
--   continues loading with UE4SS reflection hooks providing full coverage.
--
-- v3.1: Removed native hooks (SIGSEGV crashes before crash guard)
-- v3.0: Full UE4SS API with native hooks
-- ═══════════════════════════════════════════════════════════════════════
local TAG = "NoVignette"
local VERBOSE = true
local function V(...) if VERBOSE then Log(TAG .. " [V] " .. string.format(...)) end end

local state = {
    vignetteOff = true,
}

local saved = ModConfig.Load("NoVignette")
if saved then
    if saved.vignetteOff ~= nil then state.vignetteOff = saved.vignetteOff end
end

-- ═══════════════════════════════════════════════════════════════════════
-- UE4SS PRE-HOOK — BLOCK SetVignetteActive entirely
-- ═══════════════════════════════════════════════════════════════════════

RegisterPreHook("/Script/Game.VR4Vignette:SetVignetteActive", function(self, func, parms)
    if not state.vignetteOff then return end
    V("PreHook BLOCK SetVignetteActive")
    return "BLOCK"
end)
Log(TAG .. ": RegisterPreHook — VR4Vignette:SetVignetteActive → BLOCK")

-- ═══════════════════════════════════════════════════════════════════════
-- UE4SS POST-HOOKS — Override vignette state queries
-- ═══════════════════════════════════════════════════════════════════════

RegisterPostHook("/Script/Game.VR4Vignette:IsVignetteActive", function(self, func, parms)
    if not state.vignetteOff then return end
    V("PostHook IsVignetteActive -> false")
    local p = CastParms(parms, "VR4Vignette:IsVignetteActive")
    if p then p:SetReturnValue(false) end
end)

RegisterPostHook("/Script/Game.VR4Vignette:IsAnyVignetteActive", function(self, func, parms)
    if not state.vignetteOff then return end
    V("PostHook IsAnyVignetteActive -> false")
    local p = CastParms(parms, "VR4Vignette:IsAnyVignetteActive")
    if p then p:SetReturnValue(false) end
end)

Log(TAG .. ": RegisterPostHook — IsVignetteActive, IsAnyVignetteActive → false")

-- ═══════════════════════════════════════════════════════════════════════
-- UE4SS VIGNETTE SPAWN TRACKING — Nullify on creation
-- ═══════════════════════════════════════════════════════════════════════

local vignetteCount = 0

NotifyOnNewObject("VR4Vignette", function(obj)
    V("NotifyOnNewObject VR4Vignette fired, obj=%s", tostring(obj))
    if not state.vignetteOff then return end
    if not obj or not obj:IsValid() then return end
    vignetteCount = vignetteCount + 1

    -- Zero out vignette properties via UE4SS reflection
    pcall(function() obj.VignetteScale = 0.0 end)
    pcall(function() obj.PawnTimeScale = 1.0 end)
    pcall(function() obj.ColorSaturation = 1.0 end)

    Log(TAG .. ": VR4Vignette spawned #" .. vignetteCount .. " — properties zeroed")
end)
Log(TAG .. ": NotifyOnNewObject — VR4Vignette (zero on spawn)")

-- ═══════════════════════════════════════════════════════════════════════
-- NATIVE HOOKS — Best-effort C++ hooks on stripped vignette functions
-- These are game-specific symbols that may not be in the export table.
-- If symbol resolution fails → modloader logs warning, no crash.
-- If DobbyHook() crashes on bad address → modloader sigsetjmp guard
-- catches it, logs error, hook not installed, mod keeps loading.
-- The UE4SS reflection hooks above provide full vignette disabling
-- regardless of whether these native hooks succeed.
-- ═══════════════════════════════════════════════════════════════════════

local nativeHookCount = 0

-- SetVignetteActive C++ impl — block entirely
local ok = RegisterNativeHook("SetVignetteActive", function(self, active)
    if state.vignetteOff then
        V("Native BLOCK SetVignetteActive")
        return "BLOCK"
    end
end, nil, "pp")
if ok then
    nativeHookCount = nativeHookCount + 1
    Log(TAG .. ": Native hook: SetVignetteActive (BLOCK)")
else
    Log(TAG .. ": Native hook: SetVignetteActive skipped (symbol not found)")
end

-- IsVignetteActive C++ impl — force return false
ok = RegisterNativeHook("IsVignetteActive", function(self)
    if state.vignetteOff then
        V("Native IsVignetteActive -> 0")
        return 0
    end
end, nil, "p")
if ok then
    nativeHookCount = nativeHookCount + 1
    Log(TAG .. ": Native hook: IsVignetteActive (force false)")
else
    Log(TAG .. ": Native hook: IsVignetteActive skipped (symbol not found)")
end

-- IsAnyVignetteActive C++ impl — force return false
ok = RegisterNativeHook("IsAnyVignetteActive", function(self)
    if state.vignetteOff then
        V("Native IsAnyVignetteActive -> 0")
        return 0
    end
end, nil, "p")
if ok then
    nativeHookCount = nativeHookCount + 1
    Log(TAG .. ": Native hook: IsAnyVignetteActive (force false)")
else
    Log(TAG .. ": Native hook: IsAnyVignetteActive skipped (symbol not found)")
end

-- ActivateVignette — block activation
ok = RegisterNativeHook("ActivateVignette", function(self)
    if state.vignetteOff then
        V("Native BLOCK ActivateVignette")
        return "BLOCK"
    end
end, nil, "p")
if ok then
    nativeHookCount = nativeHookCount + 1
    Log(TAG .. ": Native hook: ActivateVignette (BLOCK)")
else
    Log(TAG .. ": Native hook: ActivateVignette skipped (symbol not found)")
end

-- UpdateVignette — block vignette tick updates
ok = RegisterNativeHook("UpdateVignette", function(self, dt)
    if state.vignetteOff then
        V("Native BLOCK UpdateVignette")
        return "BLOCK"
    end
end, nil, "pf")
if ok then
    nativeHookCount = nativeHookCount + 1
    Log(TAG .. ": Native hook: UpdateVignette (BLOCK)")
else
    Log(TAG .. ": Native hook: UpdateVignette skipped (symbol not found)")
end

-- ApplyVignetteEffect — block vignette rendering
ok = RegisterNativeHook("ApplyVignetteEffect", function(self)
    if state.vignetteOff then
        V("Native BLOCK ApplyVignetteEffect")
        return "BLOCK"
    end
end, nil, "p")
if ok then
    nativeHookCount = nativeHookCount + 1
    Log(TAG .. ": Native hook: ApplyVignetteEffect (BLOCK)")
else
    Log(TAG .. ": Native hook: ApplyVignetteEffect skipped (symbol not found)")
end

Log(TAG .. ": " .. nativeHookCount .. "/6 native hooks installed (UE4SS hooks always active)")

-- ═══════════════════════════════════════════════════════════════════════
-- COMMANDS
-- ═══════════════════════════════════════════════════════════════════════

RegisterCommand("vignette", function()
    state.vignetteOff = not state.vignetteOff
    V("toggle: vignetteOff=%s", tostring(state.vignetteOff))
    ModConfig.Save("NoVignette", state)
    Log(TAG .. ": Vignette " .. (state.vignetteOff and "DISABLED" or "ENABLED"))
    Notify(TAG, "Vignette " .. (state.vignetteOff and "OFF" or "ON"))
end)

RegisterCommand("vignette_status", function()
    local info = TAG .. ": vignetteOff=" .. tostring(state.vignetteOff)
        .. " vignetteSpawns=" .. vignetteCount
    -- Inspect live vignette instances via UE4SS
    local vigs = FindAllOf("VR4Vignette")
    if vigs then
        info = info .. " | Active instances: " .. #vigs
        for i, v in ipairs(vigs) do
            if v and v:IsValid() then
                pcall(function()
                    local scale = v.VignetteScale
                    local sat = v.ColorSaturation
                    if scale then info = info .. " [scale=" .. tostring(scale) .. " sat=" .. tostring(sat) .. "]" end
                end)
            end
        end
    end
    Log(info)
end)

if SharedAPI and SharedAPI.DebugMenu then
    SharedAPI.DebugMenu.RegisterToggle("VignetteOff", "Vignette Off",
        function() return state.vignetteOff end,
        function(v) state.vignetteOff = v; ModConfig.Save("NoVignette", state) end)
end

Log(TAG .. ": v4.0 loaded — UE4SS hooks + native hooks restored (modloader crash guard active)")
