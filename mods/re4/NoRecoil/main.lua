-- mods/NoRecoil/main.lua v2.0
-- ═══════════════════════════════════════════════════════════════════════
-- No Recoil / No Spread — eliminates weapon recoil and bullet spread.
--
-- v2.0: Restored native hooks (modloader now has DobbyHook crash guard).
--   If any hook fails to install (stripped symbol, bad address), the
--   modloader catches the crash via sigsetjmp and continues loading.
--
-- v1.1: REMOVED native hooks due to SIGSEGV (pre-crash-guard era)
-- v1.0: Original with native hooks
-- ═══════════════════════════════════════════════════════════════════════
local TAG = "NoRecoil"

local state = { enabled = true }

local saved = ModConfig.Load("NoRecoil")
if saved then
    if saved.enabled ~= nil then state.enabled = saved.enabled end
end

-- ═══════════════════════════════════════════════════════════════════════
-- NATIVE HOOKS — Dobby hooks on C++ recoil/spread functions
-- These are stripped game-specific symbols. If symbol resolution fails,
-- the modloader logs a warning and skips. If DobbyHook() crashes on a
-- bad address, the modloader's sigsetjmp crash guard catches it and
-- returns 0 (hook not installed). Either way, mod keeps loading.
-- ═══════════════════════════════════════════════════════════════════════

local hookCount = 0

-- UpdateRecoil(self, dt) — block the recoil update entirely
-- sig: "pf" = self(ptr/X0), dt(float/D0)
local ok = RegisterNativeHook("UpdateRecoil", function(self, dt)
    if state.enabled then return "BLOCK" end
end, nil, "pf")
if ok then
    hookCount = hookCount + 1
    Log(TAG .. ": Hooked UpdateRecoil (BLOCK when enabled)")
else
    Log(TAG .. ": UpdateRecoil hook skipped (symbol not found or install failed)")
end

-- IsRecoiling(self) — return false (0)
ok = RegisterNativeHook("IsRecoiling", function(self)
    if state.enabled then return 0 end  -- false
end, nil, "p")
if ok then
    hookCount = hookCount + 1
    Log(TAG .. ": Hooked IsRecoiling (force false when enabled)")
else
    Log(TAG .. ": IsRecoiling hook skipped (symbol not found or install failed)")
end

-- GetShotSpreadMM(self) — return 0.0 spread via post-hook
-- sig: "fp" = float return, self(ptr/X0)
ok = RegisterNativeHook("GetShotSpreadMM", nil, function(ret_d0, self)
    if state.enabled then return 0.0 end
end, "fp")
if ok then
    hookCount = hookCount + 1
    Log(TAG .. ": Hooked GetShotSpreadMM (force 0.0 spread when enabled)")
else
    Log(TAG .. ": GetShotSpreadMM hook skipped (symbol not found or install failed)")
end

-- Filter00SetAddSpread(self, spread) — block spread addition
-- sig: "pf" = self(ptr/X0), spread(float/D0)
ok = RegisterNativeHook("Filter00SetAddSpread", function(self, spread)
    if state.enabled then return "BLOCK" end
end, nil, "pf")
if ok then
    hookCount = hookCount + 1
    Log(TAG .. ": Hooked Filter00SetAddSpread (BLOCK when enabled)")
else
    Log(TAG .. ": Filter00SetAddSpread hook skipped (symbol not found or install failed)")
end

Log(TAG .. ": " .. hookCount .. "/4 native hooks installed")

-- ═══════════════════════════════════════════════════════════════════════
-- COMMAND
-- ═══════════════════════════════════════════════════════════════════════

RegisterCommand("norecoil", function()
    state.enabled = not state.enabled
    ModConfig.Save("NoRecoil", state)
    Log(TAG .. ": " .. (state.enabled and "ON" or "OFF"))
    Notify(TAG, state.enabled and "ON" or "OFF")
end)

-- ═══════════════════════════════════════════════════════════════════════
-- DEBUG MENU
-- ═══════════════════════════════════════════════════════════════════════

if SharedAPI and SharedAPI.DebugMenu then
    SharedAPI.DebugMenu.RegisterToggle("NoRecoil", "No Recoil",
        function() return state.enabled end,
        function(v) state.enabled = v; ModConfig.Save("NoRecoil", state) end)
end

Log(TAG .. ": v2.0 loaded — native hooks restored (modloader crash guard active)")
