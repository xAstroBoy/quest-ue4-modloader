-- mods/Randomizer/main.lua v9.1
-- ═══════════════════════════════════════════════════════════════════════
-- UE4SS-enhanced Enemy Randomizer — re-randomizes on EVERY level load
--
-- v9.0 — Architecture rewrite:
--   EmSetEvent (0x062E9E8C) is the SOLE randomization hook for ALL spawns.
--   readEmList (0x065E4278) is a room-level ESL file loader, NOT a per-entry
--   accessor — used only for level-change detection (clears caches).
--   GetEmPtrFromList (0x062EA154) used for scrambleNow() ESL iteration.
-- v7.2 — Fix crash during load:
--   pcall-wrap Resolve + RegisterNativeHookAt so DobbyHook failures
--   don't crash the entire mod. C++ fix: unified signal handler priority
--   ensures hook-install crash guard fires before mod-loading guard.
-- v7.1 — Fix lightuserdata handling:
--   Native hooks pass pointer/integer args as lightuserdata (void*).
--   Use PtrToInt() for comparisons/formatting, keep raw ptr for ReadU8/WriteU8/Offset.
-- v7.0 — Crash-safe + verbose debug, pcall wrapper, settle period.
-- v6.0 — Per-level-load randomization, emId mismatch detection.
-- v5.0 — Once-per-slot randomization, debug menu submenu
-- v3.0 — RegisterNativeHookAt on readEmList, per-enemy toggle commands
-- ═══════════════════════════════════════════════════════════════════════
local TAG = "Randomizer"
local VERBOSE = false

local function V(msg)
    if VERBOSE then Log(TAG .. ": [V] " .. msg) end
end

-- ── Pointer helpers ─────────────────────────────────────────────────
-- Native hooks and CallNative with 'p' return pass lightuserdata (void*).
-- PtrToInt (C++) converts to Lua integer for arithmetic/formatting.
-- These helpers provide safe conversion with fallbacks.
local function ptrval(p)
    if p == nil then return 0 end
    if type(p) == "number" then return p end
    if PtrToInt then return PtrToInt(p) end
    -- Fallback: parse tostring output ("userdata: 0xABCD")
    local s = tostring(p)
    local hex = s:match(": 0x(%x+)")
    if hex then return tonumber(hex, 16) end
    return 0
end

local function ptrfmt(p)
    return string.format("0x%X", ptrval(p))
end

-- ── HP presets ──────────────────────────────────────────────────────
local HP = {
    EASY   = { 232, 3 },   NORMAL = { 184, 11 },  HARD = { 76, 29 },
}
local HP_BOSS = {
    EASY   = { 184, 11 },  NORMAL = { 136, 19 },  HARD = { 76, 29 },
}
local HP_GARRADOR = {
    EASY   = { 160, 15 },  NORMAL = { 136, 19 },  HARD = { 124, 21 },
}
local function makeHP(lo, hi) return lo + (hi * 256) end

-- ═══════════════════════════════════════════════════════════════════════
-- ENEMY POOL — Same data as EnemySpawner, with group field
-- Format: { name, bytes[9], hpType, group, removeInvincible }
-- ═══════════════════════════════════════════════════════════════════════
local POOL = {
    -- Villagers
    {"Villager 1500", {21,0,0,0,24,64,0,232,3}, nil, "Villagers"},
    {"Villager 1501", {21,0,0,0,8,20,1,232,3}, nil, "Villagers"},
    {"Villager 1502", {21,0,0,0,2,32,1,232,3}, nil, "Villagers"},
    {"Villager 1503", {21,3,0,0,0,48,32,232,3}, nil, "Villagers"},
    {"Villager 1504", {21,3,0,0,1,52,65,232,3}, nil, "Villagers"},
    {"Villager 1505", {21,3,0,0,68,56,1,232,3}, nil, "Villagers"},
    {"Villager 1506", {21,4,0,0,96,48,0,232,3}, nil, "Villagers"},
    {"Villager 1507", {21,4,0,0,104,32,1,232,3}, nil, "Villagers"},
    {"Villager 1508", {21,4,0,0,105,64,0,232,3}, nil, "Villagers"},
    -- Salvadors
    {"Dr. Salvador 150", {21,0,0,0,96,16,16,232,3}, nil, "Salvadors"},
    {"Dr. Salvador 151", {21,0,0,0,1,0,16,232,3}, nil, "Salvadors"},
    {"Dr. Salvador 152", {21,0,48,0,2,48,16,232,3}, nil, "Salvadors"},
    {"Bella Sister 00", {21,11,0,0,96,0,16,232,3}, nil, "Salvadors"},
    {"Bella Sister 01", {21,11,0,0,97,16,16,232,3}, nil, "Salvadors"},
    {"Bella Sister 02", {21,11,48,0,99,32,16,232,3}, nil, "Salvadors"},
    -- Zealots 1C
    {"Zealot 1C00", {28,7,0,0,0,20,0,232,3}, nil, "Zealots"},
    {"Zealot 1C01", {28,7,0,0,16,18,1,232,3}, nil, "Zealots"},
    {"Zealot 1C02", {28,7,0,0,2,2,1,232,3}, nil, "Zealots"},
    {"Zealot 1C03", {28,7,0,0,8,128,0,232,3}, nil, "Zealots"},
    {"Zealot 1C04", {28,7,0,0,8,4,129,232,3}, nil, "Zealots"},
    {"Zealot 1C08", {28,9,0,0,0,20,0,232,3}, nil, "Zealots"},
    {"Zealot 1C09", {28,9,0,0,16,2,1,232,3}, nil, "Zealots"},
    -- Zealots 1A
    {"Zealot 1A00", {26,7,0,2,0,16,0,232,3}, nil, "Zealots"},
    {"Zealot 1A01", {26,7,0,2,16,2,1,232,3}, nil, "Zealots"},
    {"Zealot 1A02", {26,7,0,2,2,3,1,232,3}, nil, "Zealots"},
    {"Zealot 1A08", {26,8,0,2,0,16,0,232,3}, nil, "Zealots"},
    -- Zealots misc
    {"Zealot 110", {17,8,0,96,2,72,0,232,3}, nil, "Zealots"},
    {"Zealot 140", {20,8,0,50,1,80,0,232,3}, nil, "Zealots"},
    {"Zealot 1B1", {27,7,37,32,32,7,0,232,3}, nil, "Zealots"},
    -- Garradors
    {"Garrador", {28,10,0,0,0,0,0,160,15}, "garrador", "Garradors"},
    {"Armored Garrador", {28,13,0,0,0,0,0,124,21}, "garrador", "Garradors"},
    {"Armored Garrador Plaga", {28,10,48,0,0,0,0,160,15}, "garrador", "Garradors"},
    {"Garrador 1B", {27,10,0,0,0,0,0,232,3}, "garrador", "Garradors"},
    -- Mace Ganados
    {"Mace Soldier 00", {31,24,0,0,24,0,1,232,3}, nil, "Mace Ganados"},
    {"Mace Soldier 01", {31,24,0,0,72,4,1,232,3}, nil, "Mace Ganados"},
    {"Mace Soldier 02", {31,24,0,0,89,16,1,232,3}, nil, "Mace Ganados"},
    {"Mace Soldier 03", {31,24,0,0,10,0,1,232,3}, nil, "Mace Ganados"},
    -- Soldiers
    {"Soldier 1D00", {29,14,0,8,72,1,0,232,3}, nil, "Soldiers"},
    {"Soldier 1D01", {29,14,0,0,2,2,129,232,3}, nil, "Soldiers"},
    {"Soldier 1E00", {30,14,0,8,1,3,160,232,3}, nil, "Soldiers"},
    {"Soldier 1E01", {30,14,0,8,1,0,192,232,3}, nil, "Soldiers"},
    {"Soldier 1E05", {30,15,0,2,16,32,137,232,3}, nil, "Soldiers"},
    {"Soldier 1E11", {30,23,0,2,48,17,8,232,3}, nil, "Soldiers"},
    {"Soldier 1F00", {31,14,0,10,0,80,33,232,3}, nil, "Soldiers"},
    -- JJs
    {"JJ", {29,2,0,0,0,0,0,136,19}, "boss", "JJs"},
    {"JJ Plaga", {29,2,48,0,0,0,0,136,19}, "boss", "JJs"},
    -- Super Salvadors
    {"Super Salvador", {32,22,0,0,0,0,0,172,13}, "boss", "Super Salvadors"},
    {"Super Salvador Plaga", {32,22,0,8,8,176,16,172,13}, "boss", "Super Salvadors"},
    -- Dogs
    {"Colmillos (Dog)", {34,0,0,0,0,0,0,232,3}, nil, "Dogs"},
    -- Armaduras
    {"Armadura 00", {60,0,0,0,0,0,0,232,3}, nil, "Armaduras"},
    {"Armadura 01", {60,1,0,0,0,0,0,232,3}, nil, "Armaduras"},
    {"Armadura 02", {60,2,0,0,0,0,0,232,3}, nil, "Armaduras"},
    -- Drones
    {"Drone", {58,0,0,0,0,0,0,232,3}, nil, "Drones"},
    {"Ground Robot", {58,2,0,0,0,0,0,232,3}, nil, "Drones"},
    -- Animals
    {"Chicken", {40,0,0,0,0,0,0,232,3}, nil, "Animals"},
    {"Crow", {35,0,0,0,0,0,0,232,3}, nil, "Animals"},
    {"Snake", {36,0,1,0,0,0,0,232,3}, nil, "Animals"},
    -- Traps
    {"Walking Parasite", {37,0,0,0,0,0,0,232,3}, nil, "Traps"},
    {"Bear Trap", {42,0,0,0,0,0,0,232,3}, nil, "Traps"},
    -- Novistadores
    {"Novistador", {45,0,0,0,0,0,0,232,3}, nil, "Novistadores"},
    -- Regenerators
    {"Regenerator", {54,0,0,0,0,0,0,232,3}, nil, "Regenerators"},
    {"Iron Maiden", {54,2,0,0,0,0,0,232,3}, nil, "Regenerators"},
    -- Gigantes
    {"Gigante", {43,0,0,0,0,0,0,232,3}, "boss", "Gigantes"},
    -- Verdugos
    {"Verdugo", {55,0,0,0,0,0,0,232,3}, "boss", "Verdugos"},
    {"Verdugo After", {56,0,0,0,0,0,0,232,3}, "boss", "Verdugos"},
    -- Bosses
    {"Mendez Phase 1", {53,0,0,0,0,0,0,232,3}, "boss", "Bosses"},
    {"Mendez Phase 2", {53,1,0,0,0,0,0,232,3}, "boss", "Bosses"},
    {"Knife Krauser", {57,1,0,0,0,0,0,232,3}, "boss", "Bosses"},
    {"Mutant Krauser", {57,2,0,0,0,0,0,232,3}, "boss", "Bosses"},
    {"U3 (It)", {50,1,0,0,0,0,0,232,3}, "boss", "Bosses", true},
}

-- Build processed pool
local ENEMIES = {}
local GROUPS = {}
local groupSet = {}
for _, raw in ipairs(POOL) do
    local e = {
        name   = raw[1],
        bytes  = raw[2],
        hpType = raw[3] or "normal",
        group  = raw[4] or "Other",
        removeInvincible = raw[5] or false,
    }
    ENEMIES[#ENEMIES + 1] = e
    if not groupSet[e.group] then
        groupSet[e.group] = true
        GROUPS[#GROUPS + 1] = e.group
    end
end

-- ═══════════════════════════════════════════════════════════════════════
-- STATE
-- ═══════════════════════════════════════════════════════════════════════
local state = {
    enabled = true,     -- v7.2: default ON (was false, causing no-scramble confusion)
    hpMode  = "MATCH",  -- MATCH, EASY, NORMAL, HARD, RANDOM
    enabledEnemies = {},
    swapCount = 0,
}

-- Default: all enabled
for _, e in ipairs(ENEMIES) do state.enabledEnemies[e.name] = true end

-- ═══════════════════════════════════════════════════════════════════════
-- PER-LEVEL TRACKING (must be before saveConfig for Lua scoping)
-- ═══════════════════════════════════════════════════════════════════════
local currentGen    = 0      -- Increments on each detected level change
local levelSwaps    = 0      -- Swaps since last level change
local lastDetectTime = 0     -- Debounce level-change log messages
local cachedPool    = nil    -- Pre-built enabled pool (rebuilt on config change)
local poolDirty     = true   -- Flag to rebuild cachedPool

local function invalidatePool()
    cachedPool = nil
    poolDirty = true
end

-- ═══════════════════════════════════════════════════════════════════════
-- CONFIG PERSISTENCE
-- ═══════════════════════════════════════════════════════════════════════
local function saveConfig()
    local names = {}
    for name, on in pairs(state.enabledEnemies) do
        if on then names[#names + 1] = name end
    end
    ModConfig.Save("Randomizer", {
        configVersion = 2,
        enabled = state.enabled,
        hpMode = state.hpMode,
        enabledNames = names,
    })
    invalidatePool()  -- Rebuild cached pool on next pickRandom
end

local function loadConfig()
    local cfg = ModConfig.Load("Randomizer")
    if not cfg then
        Log(TAG .. ": No saved config — using defaults (enabled=true)")
        return
    end
    -- v7.2: Config migration — old configs had enabled=false by default
    if cfg.configVersion and cfg.configVersion >= 2 then
        if cfg.enabled ~= nil then state.enabled = cfg.enabled end
    else
        -- Old config without version: ignore saved 'enabled' (was incorrectly false)
        state.enabled = true
        Log(TAG .. ": CONFIG MIGRATION — old config had enabled=false, forcing ON")
        -- Persist the migration immediately so it doesn't re-run on next launch
        pcall(saveConfig)
    end
    if cfg.hpMode then state.hpMode = cfg.hpMode end
    if cfg.enabledNames then
        for name, _ in pairs(state.enabledEnemies) do state.enabledEnemies[name] = false end
        for _, name in ipairs(cfg.enabledNames) do
            if state.enabledEnemies[name] ~= nil then state.enabledEnemies[name] = true end
        end
    end
end
loadConfig()

-- ═══════════════════════════════════════════════════════════════════════
-- HP CALCULATION
-- ═══════════════════════════════════════════════════════════════════════
local function getRandomHP(enemy)
    local mode = state.hpMode
    if mode == "MATCH" then return { enemy.bytes[8], enemy.bytes[9] } end
    if mode == "RANDOM" then
        local modes = { "EASY", "NORMAL", "HARD" }
        mode = modes[math.random(#modes)]
    end
    local t = enemy.hpType
    if t == "boss" then return HP_BOSS[mode] or HP_BOSS.NORMAL end
    if t == "garrador" then return HP_GARRADOR[mode] or HP_GARRADOR.NORMAL end
    return HP[mode] or HP.NORMAL
end

local function getEnabledPool()
    if cachedPool and not poolDirty then return cachedPool end
    local pool = {}
    for _, e in ipairs(ENEMIES) do
        if state.enabledEnemies[e.name] then pool[#pool + 1] = e end
    end
    cachedPool = pool
    poolDirty = false
    return pool
end

local function pickRandom()
    local pool = getEnabledPool()
    if #pool == 0 then return nil end
    return pool[math.random(#pool)]
end

-- ═══════════════════════════════════════════════════════════════════════
-- NATIVE HOOKS — Randomize enemy spawns via EmSetEvent
-- ═══════════════════════════════════════════════════════════════════════
-- Architecture:
--   readEmList (0x065E4278) is a ROOM-LEVEL FILE LOADER that loads the
--   entire ESL binary from disk on room transitions. We hook it ONLY
--   for level-change detection (it fires once per room change).
--
--   EmSetEvent (universal spawn) is the SOLE randomization hook.
--   It catches ALL enemy spawns and modifies the EM_LIST buffer
--   before the engine processes it.
--
--   GetEmPtrFromList (0x062EA154) returns cEm* for already-alive enemies.
--   GetEmIdFromList  (0x062EA1F4) reads emId from ESL slot by index.
--   getEmListNum always returns 19 (compile-time constant, NOT enemy count).
local sym_readEmList     = nil    -- Room ESL file loader (level-change detection)
local sym_EmListSetAlive = nil

-- pcall Resolve calls — these compute (lib_base + offset), should be safe
-- but protect anyway in case the library base is not yet available
pcall(function() sym_readEmList     = Resolve("readEmList",    0x065E4278) end)
pcall(function() sym_EmListSetAlive = Resolve("EmListSetAlive",0x062EA224) end)

V("Resolve: readEmList="    .. ptrfmt(sym_readEmList)
    .. " EmListSetAlive=" .. ptrfmt(sym_EmListSetAlive))

local slotMap = {}          -- [emListPtr_value] = {name, replEmId} for double-randomization prevention

-- No settle period needed — EmSetEvent is sole randomization hook (no double-randomization risk)
local SETTLE_CALLS = 0      -- No skip needed (was 5 in old eslPointers architecture)
local settleCounter = 0     -- Counts down after level change detected (0 = ready)
local hookErrors = 0        -- Consecutive hook/write errors (reset on success)
local MAX_HOOK_ERRORS = 20  -- Auto-disable writes after this many consecutive errors

-- Hook readEmList — fires ONCE per room change (loads ESL file from disk)
-- We use this ONLY for level-change detection: clear caches so EmSetEvent
-- re-randomizes all enemies freshly in the new room.
local hookInstalled = false
if sym_readEmList then
    local hookOk, hookErr = pcall(function()
        RegisterNativeHookAt(sym_readEmList, "readEmList",
        function(heapCtx)
            -- Pre-hook: readEmList arg is a heap context, NOT an enemy index
            -- Just note that a room load is happening
            pcall(function()
                currentGen = currentGen + 1
                slotMap = {}
                levelSwaps = 0
                settleCounter = SETTLE_CALLS
                hookErrors = 0
                Log(TAG .. ": === ROOM LOAD #" .. currentGen
                    .. " === readEmList fired — clearing caches, settle=" .. SETTLE_CALLS)
            end)
        end,
        function(ret)
            -- Post-hook: return value unchanged (it's from FMemory::Free / stack cleanup)
            return ret
        end)
    end)
    if hookOk then
        hookInstalled = true
        Log(TAG .. ": Hooked readEmList — level-change detection (room file loader)")
    else
        Log(TAG .. ": [ERROR] Failed to hook readEmList: " .. tostring(hookErr))
        Log(TAG .. ": Level-change detection unavailable — EmSetEvent will still randomize")
    end
else
    Log(TAG .. ": [WARN] readEmList symbol not resolved — level-change detection unavailable")
end

-- ═══════════════════════════════════════════════════════════════════════
-- NATIVE HOOK: EmSetEvent — THE SOLE RANDOMIZATION HOOK
-- ═══════════════════════════════════════════════════════════════════════
-- EmSetEvent is the universal enemy spawn function called for EVERY enemy
-- spawn: ESL combat enemies, event-spawned NPCs, crows, chickens, etc.
-- This is the ONLY hook that modifies enemy data. readEmList is used
-- solely for level-change detection (it fires once per room load).
local sym_EmSetEvent = nil
-- Use mangled C++ name for reliable dlsym resolution (plain "EmSetEvent" may fall back to wrong offset)
pcall(function() sym_EmSetEvent = Resolve("_Z10EmSetEventP7EM_LIST", 0x062E9E8C) end)
if not sym_EmSetEvent then
    pcall(function() sym_EmSetEvent = Resolve("EmSetEvent", 0x062E9E8C) end)
end
Log(TAG .. ": Resolve: EmSetEvent=" .. ptrfmt(sym_EmSetEvent))

local emSetEventHooked = false
if sym_EmSetEvent then
    local hookOk, hookErr = pcall(function()
        RegisterNativeHookAt(sym_EmSetEvent, "EmSetEvent",
            function(emListPtr)
                -- Pre-hook: modify EM_LIST bytes BEFORE game reads them
                -- EM_LIST struct layout (32 bytes):
                --   +0x00  u8   status   (0=ready, game sets to 7 after spawn — DO NOT TOUCH)
                --   +0x01  u8   emId     Enemy type ID
                --   +0x02  u8   subType  -> cEm+0x119
                --   +0x03  u8   param1   -> cEm+0x47d
                --   +0x04  u8   param2   -> cEm+0x4c4
                --   +0x05-07    (unused by EmSetEvent)
                --   +0x08  s16  hp       HP (LE int16)
                --   +0x0c+ position/rotation (preserve!)
                pcall(function()
                    if not state.enabled then return end
                    if emListPtr == nil or ptrval(emListPtr) == 0 then return end
                    if hookErrors >= MAX_HOOK_ERRORS then return end

                    -- Skip EnemySpawner's deliberate manual spawns
                    if SharedAPI and SharedAPI._skipRandomizer then
                        V("EmSetEvent: SKIP (EnemySpawner manual spawn)")
                        return
                    end

                    -- Settling period after level change
                    if settleCounter > 0 then
                        settleCounter = settleCounter - 1
                        V("EmSetEvent: SETTLE skip (" .. settleCounter .. " remaining)")
                        return
                    end

                    -- Diagnostic: log first 3 hook fires to confirm hook is working
                    if state.swapCount < 3 then
                        Log(TAG .. ": EmSetEvent PRE-HOOK FIRED #" .. (state.swapCount + 1)
                            .. " gen=" .. currentGen .. " enabled=" .. tostring(state.enabled))
                    end

                    -- Read original emId from correct offset (+0x01)
                    local origEmId = ReadU8(Offset(emListPtr, 1))
                    local origBytes = ""
                    pcall(function()
                        local parts = {}
                        for j = 0, 9 do
                            parts[j+1] = string.format("%02X", ReadU8(Offset(emListPtr, j)))
                        end
                        origBytes = table.concat(parts, " ")
                    end)
                    V("EmSetEvent: emId=" .. origEmId .. " bytes=[" .. origBytes .. "]")

                    -- Pick random replacement
                    local pick = pickRandom()
                    if not pick then V("EmSetEvent: empty pool"); return end
                    local hp = getRandomHP(pick)

                    -- Write replacement bytes at CORRECT EM_LIST offsets
                    -- bytes[1]=emId, [2]=subType, [3]=param1, [4]=param2, [5-7]=unused
                    -- Status (+0x00) and position (+0x0c+) are PRESERVED
                    local writeOk = pcall(function()
                        WriteU8(Offset(emListPtr, 0x01), pick.bytes[1])   -- emId
                        WriteU8(Offset(emListPtr, 0x02), pick.bytes[2])   -- subType
                        WriteU8(Offset(emListPtr, 0x03), pick.bytes[3])   -- param1
                        WriteU8(Offset(emListPtr, 0x04), pick.bytes[4])   -- param2
                        -- HP at +0x08 as LE int16
                        WriteU8(Offset(emListPtr, 0x08), hp[1])           -- HP lo
                        WriteU8(Offset(emListPtr, 0x09), hp[2])           -- HP hi
                        if pick.removeInvincible then
                            local flags = ReadU8(Offset(emListPtr, 0x03))
                            WriteU8(Offset(emListPtr, 0x03), flags & ~0x40)
                        end
                    end)

                    if writeOk then
                        state.swapCount = state.swapCount + 1
                        levelSwaps = levelSwaps + 1
                        hookErrors = 0
                        Log(TAG .. ": EmSetEvent [gen" .. currentGen .. "] emId=" .. origEmId
                            .. " → " .. pick.name
                            .. " HP=" .. makeHP(hp[1], hp[2]))
                    else
                        hookErrors = hookErrors + 1
                        V("EmSetEvent: WRITE-FAIL for emId=" .. origEmId)
                    end
                end)
            end,
            function(ret) return ret end)
    end)
    if hookOk then
        emSetEventHooked = true
        Log(TAG .. ": Hooked EmSetEvent — randomizes ALL spawns (crows/NPCs/events)")
    else
        Log(TAG .. ": [ERROR] Failed to hook EmSetEvent: " .. tostring(hookErr))
    end
else
    Log(TAG .. ": [WARN] EmSetEvent symbol not resolved — NPC/event spawns not randomized")
end

-- ═══════════════════════════════════════════════════════════════════════
-- SCRAMBLE — Immediate randomization of all current alive enemies
-- ═══════════════════════════════════════════════════════════════════════
-- Uses GetEmPtrFromList (0x062EA154) to iterate alive cEm* objects.
-- The ESL has up to 255 entries. getEmListNum always returns 19 (hardcoded
-- stage-type count), NOT the actual enemy count. We iterate all 255 slots.
local sym_GetEmPtrFromList = nil
local sym_GetEmIdFromList  = nil
pcall(function() sym_GetEmPtrFromList = Resolve("GetEmPtrFromList", 0x062EA154) end)
pcall(function() sym_GetEmIdFromList  = Resolve("GetEmIdFromList",  0x062EA1F4) end)
V("Resolve: GetEmPtrFromList=" .. ptrfmt(sym_GetEmPtrFromList)
    .. " GetEmIdFromList=" .. ptrfmt(sym_GetEmIdFromList))

local function scrambleNow()
    if not sym_GetEmPtrFromList then
        LogWarn(TAG .. ": Missing GetEmPtrFromList — cannot scramble")
        return 0
    end

    -- Clear caches and reset
    slotMap = {}
    levelSwaps = 0
    settleCounter = 0
    hookErrors = 0

    local swapped = 0
    -- Iterate ESL slots 0..254 (0xFF = invalid sentinel)
    for i = 0, 254 do
        local ptr = nil
        pcall(function() ptr = CallNative(sym_GetEmPtrFromList, "pi", i) end)
        if ptr and ptrval(ptr) ~= 0 then
            local pick = pickRandom()
            if pick then
                local hp = getRandomHP(pick)
                V("SCRAMBLE: slot " .. i .. " → " .. pick.name)
                local writeOk = pcall(function()
                    -- Write to the cEm's EM_LIST data (at the emId/subType/param offsets)
                    -- cEm+0x118 = emId, cEm+0x119 = subType, etc.
                    -- Actually: for a live cEm, changing the emId doesn't change the 3D model.
                    -- Scramble only works on ESL entries before they spawn.
                    -- For live enemies, we'd need to kill+respawn.
                end)
                swapped = swapped + 1
            end
        end
    end

    -- Note: scrambling live enemies has limited effect since the 3D model
    -- is already loaded. The real randomization happens in EmSetEvent pre-hook
    -- during the next room load.
    Log(TAG .. ": Scramble scan complete — " .. swapped .. " alive enemies found")
    Log(TAG .. ": Note: scramble triggers re-randomization on NEXT room load")
    -- Force a cache clear so EmSetEvent re-randomizes everything
    slotMap = {}
    settleCounter = 0
    Notify(TAG, "Re-randomizing on next room load!")
    return swapped
end

-- ═══════════════════════════════════════════════════════════════════════
-- COMMANDS
-- ═══════════════════════════════════════════════════════════════════════

RegisterCommand("randomizer", function()
    state.enabled = not state.enabled
    slotMap = {}
    levelSwaps = 0
    settleCounter = 0
    hookErrors = 0
    saveConfig()
    V("TOGGLE: enabled=" .. tostring(state.enabled) .. " slotMap cleared")
    Log(TAG .. ": " .. (state.enabled and "ON" or "OFF"))
    Notify(TAG, state.enabled and "ON" or "OFF")
end)

RegisterCommand("rnd_hpmode", function(args)
    local m = (args or "MATCH"):upper()
    local valid = { MATCH=true, EASY=true, NORMAL=true, HARD=true, RANDOM=true }
    if valid[m] then
        state.hpMode = m
        saveConfig()
        Log(TAG .. ": HP mode → " .. m)
        Notify(TAG, "HP: " .. m)
    else
        Log(TAG .. ": Valid modes: MATCH, EASY, NORMAL, HARD, RANDOM")
    end
end)

RegisterCommand("rnd_enable_group", function(args)
    local count = 0
    for _, e in ipairs(ENEMIES) do
        if e.group == args then state.enabledEnemies[e.name] = true; count = count + 1 end
    end
    saveConfig()
    Log(TAG .. ": Enabled " .. count .. " in " .. tostring(args))
end)

RegisterCommand("rnd_disable_group", function(args)
    local count = 0
    for _, e in ipairs(ENEMIES) do
        if e.group == args then state.enabledEnemies[e.name] = false; count = count + 1 end
    end
    saveConfig()
    Log(TAG .. ": Disabled " .. count .. " in " .. tostring(args))
end)

RegisterCommand("rnd_enable_all", function()
    for name, _ in pairs(state.enabledEnemies) do state.enabledEnemies[name] = true end
    saveConfig()
    Log(TAG .. ": All " .. #ENEMIES .. " enemies enabled")
end)

RegisterCommand("rnd_disable_all", function()
    for name, _ in pairs(state.enabledEnemies) do state.enabledEnemies[name] = false end
    saveConfig()
    Log(TAG .. ": All enemies disabled")
end)

RegisterCommand("rnd_scramble", function() scrambleNow() end)

RegisterCommand("rnd_groups", function()
    for _, g in ipairs(GROUPS) do
        local total, on = 0, 0
        for _, e in ipairs(ENEMIES) do
            if e.group == g then
                total = total + 1
                if state.enabledEnemies[e.name] then on = on + 1 end
            end
        end
        Log("  " .. g .. ": " .. on .. "/" .. total)
    end
end)

RegisterCommand("rnd_verbose", function()
    VERBOSE = not VERBOSE
    Log(TAG .. ": Verbose logging " .. (VERBOSE and "ON" or "OFF"))
    Notify(TAG, "Verbose: " .. (VERBOSE and "ON" or "OFF"))
end)

RegisterCommand("rnd_status", function()
    local pool = getEnabledPool()
    Log(TAG .. ": enabled=" .. tostring(state.enabled)
        .. " hpMode=" .. state.hpMode
        .. " pool=" .. #pool .. "/" .. #ENEMIES
        .. " totalSwaps=" .. state.swapCount
        .. " levelGen=" .. currentGen
        .. " levelSwaps=" .. levelSwaps
        .. " settle=" .. settleCounter
        .. " errors=" .. hookErrors
        .. " groups=" .. #GROUPS)
end)

-- ═══════════════════════════════════════════════════════════════════════
-- SHARED API
-- ═══════════════════════════════════════════════════════════════════════

if SharedAPI then
    SharedAPI.Randomizer = {
        isEnabled  = function() return state.enabled end,
        setEnabled = function(v) state.enabled = v; slotMap = {}; levelSwaps = 0; settleCounter = 0; hookErrors = 0; saveConfig() end,
        getHPMode  = function() return state.hpMode end,
        setHPMode  = function(m) state.hpMode = m; saveConfig() end,
        scramble   = scrambleNow,
        getPoolSize = function() return #getEnabledPool() end,
        getSwapCount = function() return state.swapCount end,
        GROUPS = GROUPS,
    }
end

if SharedAPI and SharedAPI.DebugMenu then
    local api = SharedAPI.DebugMenu

    api.RegisterSubMenu("Randomizer", "Randomizer", function()
        api.NavigateTo({ populate = function()
            -- Main toggle
            local st = state.enabled and "ON" or "OFF"
            api.AddItem("[" .. st .. "] Enemy Randomizer", function()
                state.enabled = not state.enabled
                slotMap = {}
                levelSwaps = 0
                settleCounter = 0
                hookErrors = 0
                saveConfig()
                api.Refresh()
            end)

            -- HP Mode cycle
            api.AddItem("HP Mode: " .. state.hpMode, function()
                local modes = {"MATCH", "EASY", "NORMAL", "HARD", "RANDOM"}
                local idx = 1
                for i, m in ipairs(modes) do
                    if m == state.hpMode then idx = i; break end
                end
                state.hpMode = modes[(idx % #modes) + 1]
                saveConfig()
                api.Refresh()
            end)

            -- Stats
            local pool = getEnabledPool()
            api.AddItem("Pool: " .. #pool .. "/" .. #ENEMIES .. " enemies", nil)
            api.AddItem("Total Swaps: " .. state.swapCount, nil)
            api.AddItem("Level #" .. currentGen .. " | Swaps: " .. levelSwaps, nil)

            api.AddItem("--- ACTIONS ---", nil)
            api.AddItem(">> Scramble Now! <<", function() scrambleNow() end)
            api.AddItem(">> Force Re-randomize <<", function()
                slotMap = {}
                levelSwaps = 0
                settleCounter = 0
                hookErrors = 0
                Log(TAG .. ": Forced re-randomization — all slots will re-roll")
                Notify(TAG, "Re-randomizing on next enemy read!")
                api.Refresh()
            end)
            api.AddItem("Enable All Enemies", function()
                for name, _ in pairs(state.enabledEnemies) do
                    state.enabledEnemies[name] = true
                end
                saveConfig()
                api.Refresh()
            end)
            api.AddItem("Disable All Enemies", function()
                for name, _ in pairs(state.enabledEnemies) do
                    state.enabledEnemies[name] = false
                end
                saveConfig()
                api.Refresh()
            end)

            -- Per-group toggles
            api.AddItem("--- GROUPS ---", nil)
            for _, g in ipairs(GROUPS) do
                local groupName = g
                local total, on = 0, 0
                for _, e in ipairs(ENEMIES) do
                    if e.group == groupName then
                        total = total + 1
                        if state.enabledEnemies[e.name] then on = on + 1 end
                    end
                end
                local allOn = (on == total)
                api.AddItem("[" .. (allOn and "ALL" or on .. "/" .. total) .. "] " .. groupName, function()
                    local newState = not allOn
                    for _, e in ipairs(ENEMIES) do
                        if e.group == groupName then
                            state.enabledEnemies[e.name] = newState
                        end
                    end
                    saveConfig()
                    api.Refresh()
                end)
            end
        end })
    end)
end

Log(TAG .. ": v9.1 loaded — " .. #ENEMIES .. " enemies, "
    .. #GROUPS .. " groups, "
    .. (state.enabled and "ON" or "OFF")
    .. " hpMode=" .. state.hpMode
    .. " readEmList=" .. (hookInstalled and "YES" or "NO")
    .. " EmSetEvent=" .. (emSetEventHooked and "YES" or "NO")
    .. " | ESL + universal spawn hooks (pcall + settle=" .. SETTLE_CALLS .. ")")
