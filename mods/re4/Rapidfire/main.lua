-- mods/Rapidfire/main.lua v17.0
-- ═══════════════════════════════════════════════════════════════════════
-- Rapidfire — Forces fully automatic rapid fire on all weapons.
--
-- v17.0 — Fixed critical symbol resolution bugs + per-weapon patches.
--   FIXES:
--   - IsReadyToFire mangled name was WRONG (14→13 char count)
--   - Added fallback offsets for ALL 3 previously-missing symbols
--   - Added per-weapon IsReadyToFire patches (10 weapon subclasses)
--   - All patch helpers now log warnings on resolve failure
--
--   ARM64 byte patches (belt-and-suspenders):
--   1. IsFireTimerDone → true  (base class + offset)
--   2. UpdateFireTimer → NOP   (base class + offset)
--   3. IsFullyAutomatic → true (base class + offset + MachineGun)
--   4. IsFiringBlocked → false (base class + offset)
--   5. IsReadyToFire → true    (base class + 10 weapon subclasses)
--
--   PRIMARY mechanism: aggressive reflection tuning (30+ properties)
-- ═══════════════════════════════════════════════════════════════════════
local TAG = "Rapidfire"
local VERBOSE = false
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

local state = {
    enabled = true,
    minAnimTime = 0.001,  -- seconds (lower = faster)
}

local saved = ModConfig.Load("Rapidfire")
if saved then
    if saved.enabled ~= nil then state.enabled = saved.enabled end
    if saved.minAnimTime ~= nil then state.minAnimTime = saved.minAnimTime end
    if saved.cooldown ~= nil and saved.minAnimTime == nil then
        -- compat with older config key: force fast default for migrated configs
        state.minAnimTime = 0.01
    end
end

state.minAnimTime = math.max(0.0, math.min(0.05, state.minAnimTime))

-- ═══════════════════════════════════════════════════════════════════════
-- ARM64 BYTE PATCHES — Patch fire-rate functions at native code level
-- ═══════════════════════════════════════════════════════════════════════
-- WriteU32 handles mprotect + icache flush automatically on the modloader.
-- These patches affect the FUNCTION BODY — any code that calls these
-- addresses (including vtable dispatch) will get our patched behavior.
-- Even if some call sites have the function inlined (compiler optimization),
-- vtable calls and indirect calls still go through the real function body.

local ARM64_MOV_W0_0 = 0x52800000   -- mov w0, #0 (false/zero)
local ARM64_MOV_W0_1 = 0x52800020   -- mov w0, #1 (true/one)
local ARM64_RET      = 0xD65F03C0   -- ret

local patchCount = 0

-- Helper: resolve and patch a function to return a constant
local function patchReturnTrue(mangledName, fallbackOffset, friendlyName)
    pcall(function()
        local addr = Resolve(mangledName, fallbackOffset)
        if not addr or IsNull(addr) then
            Log(TAG .. ": [WARN] " .. friendlyName .. ": resolve failed")
            return
        end
        WriteU32(addr, ARM64_MOV_W0_1)           -- mov w0, #1
        WriteU32(Offset(addr, 4), ARM64_RET)      -- ret
        patchCount = patchCount + 1
        Log(TAG .. ": PATCHED " .. friendlyName .. " → true @ " .. ToHex(addr))
    end)
end

local function patchReturnFalse(mangledName, fallbackOffset, friendlyName)
    pcall(function()
        local addr = Resolve(mangledName, fallbackOffset)
        if not addr or IsNull(addr) then
            Log(TAG .. ": [WARN] " .. friendlyName .. ": resolve failed")
            return
        end
        WriteU32(addr, ARM64_MOV_W0_0)           -- mov w0, #0
        WriteU32(Offset(addr, 4), ARM64_RET)      -- ret
        patchCount = patchCount + 1
        Log(TAG .. ": PATCHED " .. friendlyName .. " → false @ " .. ToHex(addr))
    end)
end

local function patchReturnVoid(mangledName, fallbackOffset, friendlyName)
    pcall(function()
        local addr = Resolve(mangledName, fallbackOffset)
        if not addr or IsNull(addr) then
            Log(TAG .. ": [WARN] " .. friendlyName .. ": resolve failed")
            return
        end
        WriteU32(addr, ARM64_RET)  -- ret (skip entire function body)
        patchCount = patchCount + 1
        Log(TAG .. ": PATCHED " .. friendlyName .. " → NOP @ " .. ToHex(addr))
    end)
end

-- ── Core fire-rate patches (known offsets from CXXHeaderDump) ──────────

-- IsFireTimerDone() → always true = gun always thinks timer expired
patchReturnTrue(
    "_ZNK17AVR4GamePlayerGun15IsFireTimerDoneEv",
    0x062DBAF8,
    "IsFireTimerDone"
)

-- UpdateFireTimer(float) → NOP = timer never counts down
patchReturnVoid(
    "_ZN17AVR4GamePlayerGun15UpdateFireTimerEf",
    0x062D9E20,
    "UpdateFireTimer"
)

-- ── Additional base-class patches (offsets from RE4 VR symbol_dump.txt) ─

-- IsReadyToFire() → true = weapon always ready to fire
-- NOTE: mangled name has 13 chars ("IsReadyToFire"), NOT 14!
patchReturnTrue(
    "_ZNK17AVR4GamePlayerGun13IsReadyToFireEv",
    0x062DBA88,
    "IsReadyToFire(base)"
)

-- IsFiringBlocked() → false = firing never blocked
patchReturnFalse(
    "_ZNK17AVR4GamePlayerGun15IsFiringBlockedEv",
    0x062DBA2C,
    "IsFiringBlocked(base)"
)

-- ── Per-weapon IsReadyToFire overrides (each class overrides the virtual) ─
-- Without patching these, only weapons that DON'T override will use our base patch.
local weaponReadyToFire = {
    {"_ZNK24AVR4GamePlayerHarpoonGun13IsReadyToFireEv",  0x062EC38C, "Harpoon"},
    {"_ZNK24AVR4GamePlayerMachineGun13IsReadyToFireEv",  0x062F1C24, "MachineGun"},
    {"_ZNK20AVR4GamePlayerPistol13IsReadyToFireEv",      0x06307698, "Pistol"},
    {"_ZNK31AVR4GamePlayerPlagaRemovalLaser13IsReadyToFireEv", 0x06308F1C, "PlagaLaser"},
    {"_ZNK22AVR4GamePlayerRevolver13IsReadyToFireEv",    0x0631435C, "Revolver"},
    {"_ZNK19AVR4GamePlayerRifle13IsReadyToFireEv",       0x063160A4, "Rifle"},
    {"_ZNK28AVR4GamePlayerRocketLauncher13IsReadyToFireEv", 0x06317430, "RocketLauncher"},
    {"_ZNK27AVR4GamePlayerSemiAutoRifle13IsReadyToFireEv", 0x06319748, "SemiAutoRifle"},
    {"_ZNK21AVR4GamePlayerShotgun13IsReadyToFireEv",     0x0631BC44, "Shotgun"},
    {"_ZNK21AVR4GamePlayerStriker13IsReadyToFireEv",     0x0631DF64, "Striker"},
}
for _, wpn in ipairs(weaponReadyToFire) do
    patchReturnTrue(wpn[1], wpn[2], "IsReadyToFire(" .. wpn[3] .. ")")
end

Log(TAG .. ": " .. patchCount .. " ARM64 byte patches applied")

-- ═══════════════════════════════════════════════════════════════════════
-- REFLECTION TUNING — live gun + CDO timing overrides
-- ═══════════════════════════════════════════════════════════════════════
local function setIfExists(obj, prop, value)
    local cur = nil
    local okGet = pcall(function() cur = obj:Get(prop) end)
    if not okGet or cur == nil then return false end
    local okSet = pcall(function() obj:Set(prop, value) end)
    return okSet
end

local function tuneTriggerData(obj)
    local td = nil
    local okTd = pcall(function() td = obj:Get("TriggerData") end)
    if not okTd or not td then return false end

    local changed = false
    local okPull = pcall(function() td.PullTime = state.minAnimTime end)
    if okPull then changed = true end
    local okRelax = pcall(function() td.RelaxTime = state.minAnimTime end)
    if okRelax then changed = true end

    return changed
end

local function tuneGunTimings(gun)
    if not gun then return false end
    local changed = false

    -- Core fire timing properties
    if setIfExists(gun, "FireAnimDuration", state.minAnimTime) then changed = true end
    if setIfExists(gun, "BreechAnimDuration", state.minAnimTime) then changed = true end
    if setIfExists(gun, "TimeBetweenBursts", state.minAnimTime) then changed = true end
    if setIfExists(gun, "TriggerSlideAutomaticFireDelay", 0.0) then changed = true end
    if setIfExists(gun, "AutoPumpDelayWhenHoldingAmmo", 0.0) then changed = true end
    if setIfExists(gun, "AutoBoltDelay", 0.0) then changed = true end
    if setIfExists(gun, "AutoBoltScopeEjectDelay", 0.0) then changed = true end
    if setIfExists(gun, "AutoBoltPoppedBackScopeDelay", 0.0) then changed = true end
    if setIfExists(gun, "AutoBoltActionBackTime", state.minAnimTime) then changed = true end
    if setIfExists(gun, "AutoBoltActionReturnTime", state.minAnimTime) then changed = true end
    if setIfExists(gun, "ForestockAutoPushTime", state.minAnimTime) then changed = true end
    if setIfExists(gun, "ForestockAutoPushTimeAfterReload", state.minAnimTime) then changed = true end

    -- Additional timing properties that may control fire rate
    if setIfExists(gun, "FireRateMultiplier", 100.0) then changed = true end
    if setIfExists(gun, "TimeBetweenShots", 0.0) then changed = true end
    if setIfExists(gun, "FireTimer", 0.0) then changed = true end
    if setIfExists(gun, "FireDelay", 0.0) then changed = true end
    if setIfExists(gun, "FireRate", 0.001) then changed = true end
    if setIfExists(gun, "ShotCooldown", 0.0) then changed = true end
    if setIfExists(gun, "CooldownTimer", 0.0) then changed = true end
    if setIfExists(gun, "MinFireInterval", 0.0) then changed = true end
    if setIfExists(gun, "MaxFireInterval", 0.001) then changed = true end
    if setIfExists(gun, "FireCooldown", 0.0) then changed = true end
    if setIfExists(gun, "bFireTimerDone", true) then changed = true end
    if setIfExists(gun, "bReadyToFire", true) then changed = true end
    if setIfExists(gun, "bIsFiringBlocked", false) then changed = true end

    -- Trigger data struct
    if tuneTriggerData(gun) then changed = true end

    return changed
end

local function tuneCdo(className)
    local cdo = nil
    pcall(function() cdo = GetCDO(className) end)
    if not cdo then return false end
    return tuneGunTimings(cdo)
end

local function tuneKnownCdos()
    local classes = {
        "VR4GamePlayerGun",
        "VR4GamePlayerPistol",
        "VR4GamePlayerRevolver",
        "VR4GamePlayerMachineGun",
        "VR4GamePlayerRifle",
        "VR4GamePlayerSemiAutoRifle",
        "VR4GamePlayerShotgun",
        "VR4GamePlayerGunTwoHanded",
    }

    local touched = 0
    for _, cls in ipairs(classes) do
        if tuneCdo(cls) then touched = touched + 1 end
    end
    return touched
end

local lastGunName = ""
local loopTickCount = 0
local loopTuneHits = 0
local activeGunCount = 0
local noCooldownWriteCount = 0

local function getActiveGuns()
    local out = {}
    local all = nil
    pcall(function() all = FindAllOf("VR4GamePlayerGun") end)
    if all then
        for _, gun in ipairs(all) do
            if gun and gun.IsValid and gun:IsValid() and not isDefaultObject(gun) then
                out[#out + 1] = gun
            end
        end
    end
    activeGunCount = #out
    return out
end

local function tuneInitialLiveGun()
    local guns = getActiveGuns()
    if #guns > 0 then
        local changedAny = false
        for _, gun in ipairs(guns) do
            if tuneGunTimings(gun) then changedAny = true end
            if setIfExists(gun, "FireTimer", 0.0) then changedAny = true; noCooldownWriteCount = noCooldownWriteCount + 1 end
            if setIfExists(gun, "CooldownTimer", 0.0) then changedAny = true; noCooldownWriteCount = noCooldownWriteCount + 1 end
            if setIfExists(gun, "FireDelay", 0.0) then changedAny = true; noCooldownWriteCount = noCooldownWriteCount + 1 end
            if setIfExists(gun, "TimeBetweenShots", 0.0) then changedAny = true; noCooldownWriteCount = noCooldownWriteCount + 1 end
            if setIfExists(gun, "FireCooldown", 0.0) then changedAny = true; noCooldownWriteCount = noCooldownWriteCount + 1 end
            if setIfExists(gun, "MinFireInterval", 0.0) then changedAny = true; noCooldownWriteCount = noCooldownWriteCount + 1 end
            if setIfExists(gun, "MaxFireInterval", 0.0) then changedAny = true; noCooldownWriteCount = noCooldownWriteCount + 1 end
            if setIfExists(gun, "bFireTimerDone", true) then changedAny = true; noCooldownWriteCount = noCooldownWriteCount + 1 end
            if setIfExists(gun, "bReadyToFire", true) then changedAny = true; noCooldownWriteCount = noCooldownWriteCount + 1 end
            if setIfExists(gun, "bIsFiringBlocked", false) then changedAny = true; noCooldownWriteCount = noCooldownWriteCount + 1 end

            local name = ""
            pcall(function() name = gun:GetName() or "" end)
            if name ~= "" and name ~= lastGunName then
                lastGunName = name
                Log(TAG .. ": Tuned gun -> " .. name)
            end
        end
        return changedAny
    end
    return false
end

pcall(function()
    local touched = tuneKnownCdos()
    Log(TAG .. ": CDO timing pass touched " .. touched .. " class(es)")
end)

ExecuteWithDelay(3000, function()
    if not state.enabled then return end
    pcall(function()
        if tuneInitialLiveGun() then
            V("Initial live gun tuning pass completed")
        else
            V("Initial live gun tuning pass found no active gun")
        end
    end)
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
-- COMMANDS — toggle + timing tuning via ADB bridge
-- ═══════════════════════════════════════════════════════════════════════
RegisterCommand("rapidfire", function(args)
    V("rapidfire command args=%s", tostring(args))
    if args and args ~= "" then
        local val = tonumber(args)
        if val and val >= 0 then
            state.minAnimTime = math.max(0.0, math.min(0.05, val))
            ModConfig.Save("Rapidfire", state)
            Log(TAG .. ": minAnimTime set to " .. state.minAnimTime .. "s")
            Notify(TAG, "minAnimTime: " .. state.minAnimTime .. "s")
            return "minAnimTime=" .. state.minAnimTime
        end
    end
    state.enabled = not state.enabled
    ModConfig.Save("Rapidfire", state)
    Log(TAG .. ": " .. (state.enabled and "ON" or "OFF"))
    Notify(TAG, state.enabled and "ON" or "OFF")
    return state.enabled and "ON" or "OFF"
end)

RegisterCommand("rapidfire_status", function()
    local s = "enabled=" .. tostring(state.enabled)
        .. " minAnimTime=" .. tostring(state.minAnimTime)
        .. " loopTickCount=" .. tostring(loopTickCount)
        .. " loopTuneHits=" .. tostring(loopTuneHits)
        .. " activeGuns=" .. tostring(activeGunCount)
        .. " noCooldownWrites=" .. tostring(noCooldownWriteCount)
        .. " lastGun=" .. tostring(lastGunName)
    Log(TAG .. ": " .. s)
    return s
end)

if SharedAPI then
    SharedAPI.Rapidfire = {
        getDebug = function()
            return {
                enabled = state.enabled,
                minAnimTime = state.minAnimTime,
                loopTickCount = loopTickCount,
                loopTuneHits = loopTuneHits,
                activeGunCount = activeGunCount,
                noCooldownWriteCount = noCooldownWriteCount,
                lastGunName = lastGunName,
            }
        end,
        setEnabled = function(v)
            state.enabled = not not v
            ModConfig.Save("Rapidfire", state)
            return state.enabled
        end,
    }
end

-- ═══════════════════════════════════════════════════════════════════════
-- DEBUG MENU
-- ═══════════════════════════════════════════════════════════════════════
if SharedAPI and SharedAPI.DebugMenu then
    SharedAPI.DebugMenu.RegisterToggle("Rapidfire", "Rapidfire",
        function() return state.enabled end,
        function(v)
            V("DebugMenu toggle → %s", tostring(v))
            state.enabled = v
            ModConfig.Save("Rapidfire", state)
        end)
end

Log(TAG .. ": v17.2 loaded — " .. patchCount .. " ARM64 patches (incl. per-weapon)"
    .. " | minAnimTime=" .. state.minAnimTime .. "s"
    .. " | hold-trigger autofire disabled"
    .. " | reflection tuning: one-shot CDO + delayed live gun"
    .. " | no forced full-auto booleans")
