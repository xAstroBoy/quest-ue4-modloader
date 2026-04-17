-- mods/Randomizer/main.lua v12.6
-- ═══════════════════════════════════════════════════════════════════════
-- UE4SS-enhanced Enemy Randomizer — re-randomizes on EVERY level load
--
-- v12.4 — Strict EM_LIST cast + non-enemy skip + replacement dump:
--   Every rewrite now reads/casts EM_LIST fields first, applies writes via a
--   single structured write path, skips non-randomizable emIds (NPC/police
--   class rows), and journals every replace/skip event for full dumps.
--
-- v12.3 — Exact-emId room-list hardening:
--   Fresh v12.2 session evidence showed silent death still occurs even with
--   EmSetFromList2 direct mutation OFF. The remaining correlated path is the
--   room-load full rewrite itself. Direct list rewrites now prefer exact-emId
--   variants first, preserving room-load scrambling while avoiding broader
--   cross-emId slot substitutions that appear to explode streaming/memory.
--
-- v12.2 — Pre-placed/direct activation hardening:
--   Keep room-load rewriting ENABLED, but stop re-randomizing slots that were
--   already rewritten this generation and disable direct EmSetFromList2
--   mutation by default. Direct list targets are also narrowed to safer
--   per-family archetypes (e.g. no Garrador/JJ/Super Salvador flood in large
--   room lists) to reduce invalid activation paths and memory blow-ups.
--
-- v12.1 — Room-load rewrite preserved, cross-family list rewrites constrained:
--   Keep automatic readEmList/full-list rewriting ENABLED, but stop the list
--   pass from converting cheap room entries into arbitrary cross-stage heavy
--   archetypes. Direct list rewrites now stay inside compatible source-family
--   groups (village/castle/island/etc.) while EmSetEvent still handles native
--   scripted rows safely. This preserves the intended room-load scramble while
--   reducing the post-load memory spike/crash window.
--
-- v11.9 — Scripted native row handling:
--   HP-only native fallback no longer skips unknown signature rows.
--   Scripted/native rows now get safe HP rewrite (emId/params preserved).
--
-- v11.7 — Preserve-emid hardening:
--   For native preserve-emid paths, tail bytes are now copied ONLY from
--   templates with the exact same emId. If no exact template exists in the
--   current enabled pool, rewrite automatically degrades to HP-only.
--
-- v11.6 — SDK-backed native emId families + safe structure writes:
--   EEmID contains MANY native enemy IDs, not just 18/35.
--   VR4EnemyRemapRow / VR4EnemyListRow show native enemy rows are family-
--   specific structured data, so a coarse "ganado/enemy/boss" guess is wrong.
--   Fix: native emIds now use explicit compatibility families derived from SDK
--   and either (a) preserve the original emId while copying compatible tail
--   bytes, or (b) do HP-only rewrites for unsupported boss-only natives.
--
-- v11.5 — Initial native emId fix.
--
-- v11.4 — Scripted-cutscene safety gate:
--   Adds native cutscene begin/end hooks (AVR4CutsceneManager::OnSceEventBegin/End)
--   and suppresses EmSetEvent rewrites while cutscenes are active (plus short
--   post-cutscene cooldown), preventing scripted cutscene spawn rewrites.
--
-- v11.3 — Use EM_LIST structure directly, no signature gate:
--   EmSetEvent accepts ANY non-zero emId — the structure layout is known
--   (+0x01=emId, +0x02=subType, +0x03=param1, +0x04=param2, +0x05-07=extra,
--    +0x08-09=HP). Pool lookup kept for logging only. emId=18 and all other
--   game-internal entries are now randomized correctly.
--
-- v11.2 — Signature-only writes (too strict — blocked all emId=18 entries).
-- v11.1 — Unknown-source skip guard.
-- v10.0 — Dual-hook (EmSetEvent + EmListSetAlive) — broken.
-- v9.0 — Architecture rewrite (EmSetEvent only — was correct all along).
-- ═══════════════════════════════════════════════════════════════════════
local TAG = "Randomizer"
local VERBOSE = true
-- v12.5 safety default:
-- Room-load full-list rewrites are the crash-correlated path in live sessions.
-- Keep manual rewrite command available for testing, but disable automatic room
-- rewrites so only EmSetEvent performs spawn-time mutation.
local ENABLE_AUTO_DIRECT_LIST_REWRITES = false
local ALLOW_MANUAL_DIRECT_LIST_REWRITES = true
-- ⚠️ CRITICAL: EmSetFromList2 direct mutation MUST STAY FALSE
-- Reason: EmSetFromList2 fires SYNCHRONOUSLY during engine spawn.
-- If Lua rewriteListSlotByIndex() is called inside this hook, the interpreter
-- locks the engine for hundreds of frames (confirmed: crash log shows FPS=1/90).
-- The room-load rewrite (readEmList) ALREADY handles all pre-placed items at
-- level-change time BEFORE any spawns. This flag serves ONLY as fallback logic.
-- Verdict: NEVER set to true. It will cause instant game freeze/stall.
local ENABLE_EMSETFROMLIST2_DIRECT_REWRITES = false
local listContains
local buildNativeRewritePlan
local lastChoiceBySourceSig = {}

local function V(msg)
    if VERBOSE then Log(TAG .. ": [V] " .. msg) end
end

local INSTALL_HOOKS_WHEN_DISABLED = false

-- ── Pointer helpers ─────────────────────────────────────────────────
-- Native hooks and CallNative with 'p' return pass lightuserdata (void*).
-- PtrToInt (C++) converts to Lua integer for arithmetic/formatting.
-- These helpers provide safe conversion with fallbacks.
local function ptrval(p)
    if p == nil then return 0 end
    if type(p) == "number" then return p end
    ---@diagnostic disable-next-line: undefined-field
    local ptrToInt = _G and _G.PtrToInt or nil
    if ptrToInt then return ptrToInt(p) end
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
local ENEMIES_BY_SIGNATURE = {}
local ENEMIES_BY_EMID = {}

local function buildEnemySignature(bytes)
    if not bytes then return "" end
    local parts = {}
    for i = 1, 7 do
        parts[#parts + 1] = tostring(bytes[i] or 0)
    end
    return table.concat(parts, ":")
end

local function getEmListSignature(emListPtr)
    if not emListPtr or ptrval(emListPtr) == 0 then return "" end
    local parts = {}
    for i = 1, 7 do
        parts[i] = tostring(ReadU8(Offset(emListPtr, i)) or 0)
    end
    return table.concat(parts, ":")
end

local function findOriginalEnemyDef(emListPtr, origEmId, allowEmIdFallback)
    local sig = ""
    pcall(function()
        sig = getEmListSignature(emListPtr)
    end)
    if sig ~= "" then
        local bySig = ENEMIES_BY_SIGNATURE[sig]
        if bySig then return bySig, "signature" end
    end
    if allowEmIdFallback then
        local byId = ENEMIES_BY_EMID[origEmId]
        if byId then return byId, "emId" end
    end
    return nil, nil
end

for _, raw in ipairs(POOL) do
    local e = {
        name   = raw[1],
        bytes  = raw[2],
        hpType = raw[3] or "normal",
        group  = raw[4] or "Other",
        removeInvincible = raw[5] or false,
    }
    ENEMIES[#ENEMIES + 1] = e
    local sig = buildEnemySignature(e.bytes)
    if sig ~= "" and not ENEMIES_BY_SIGNATURE[sig] then
        ENEMIES_BY_SIGNATURE[sig] = e
    end
    local emId = e.bytes and e.bytes[1] or nil
    if emId ~= nil and ENEMIES_BY_EMID[emId] == nil then
        ENEMIES_BY_EMID[emId] = e
    end
    if not groupSet[e.group] then
        groupSet[e.group] = true
        GROUPS[#GROUPS + 1] = e.group
    end
end

local function describeOriginalEnemy(emListPtr, origEmId)
    local rawName = nil
    pcall(function()
        rawName = findOriginalEnemyDef(emListPtr, origEmId, true)
    end)
    if rawName then return rawName.name end
    return "Unknown emId " .. tostring(origEmId)
end

-- ═══════════════════════════════════════════════════════════════════════
-- STATE
-- ═══════════════════════════════════════════════════════════════════════
local state = {
    enabled = false,    -- v12.6 isolation default: OFF until explicitly enabled
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
        Log(TAG .. ": No saved config — using defaults (enabled=false)")
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

local function pickFromCandidatesAvoidName(candidates, avoidName)
    if #candidates == 0 then return nil end
    if not avoidName then
        return candidates[math.random(#candidates)]
    end
    local filtered = {}
    for _, e in ipairs(candidates) do
        if e.name ~= avoidName then
            filtered[#filtered + 1] = e
        end
    end
    if #filtered > 0 then
        return filtered[math.random(#filtered)]
    end
    return candidates[math.random(#candidates)]
end

local DIRECT_LIST_COMPAT_GROUPS = {
    ["Villagers"] = {"Villagers", "Salvadors"},
    ["Salvadors"] = {"Villagers", "Salvadors"},
    ["Zealots"] = {"Zealots"},
    ["Garradors"] = {"Zealots", "Garradors"},
    ["Soldiers"] = {"Soldiers", "Mace Ganados"},
    ["Mace Ganados"] = {"Soldiers", "Mace Ganados"},
    ["JJs"] = {"Soldiers", "Mace Ganados", "JJs"},
    ["Super Salvadors"] = {"Soldiers", "Mace Ganados", "Super Salvadors"},
    ["Dogs"] = {"Dogs"},
    ["Animals"] = {"Animals"},
    ["Traps"] = {"Traps"},
    ["Novistadores"] = {"Novistadores"},
    ["Regenerators"] = {"Regenerators"},
    ["Gigantes"] = {"Gigantes"},
    ["Verdugos"] = {"Verdugos"},
    ["Bosses"] = {"Bosses"},
    ["Drones"] = {"Drones"},
    ["Armaduras"] = {"Armaduras"},
}

local function buildCompatibleCandidates(origEntry, origEmId)
    local pool = getEnabledPool()
    if #pool == 0 then return pool end

    local compatGroups = nil
    if origEntry and origEntry.group then
        compatGroups = DIRECT_LIST_COMPAT_GROUPS[origEntry.group] or {origEntry.group}
    end

    local compatible = {}
    for _, e in ipairs(pool) do
        local eid = e.bytes and e.bytes[1] or nil
        local sameEmId = (eid ~= nil and eid == origEmId)
        local inCompatGroup = (compatGroups == nil) or listContains(compatGroups, e.group)
        local safeDirect = not (e.group == "Bosses"
            or e.group == "Gigantes"
            or e.group == "Verdugos"
            or e.group == "Regenerators"
            or e.group == "Drones")
        if inCompatGroup and safeDirect and not sameEmId then
            compatible[#compatible + 1] = e
        end
    end

    if #compatible > 0 then return compatible end

    local nonSameId = {}
    for _, e in ipairs(pool) do
        local eid = e.bytes and e.bytes[1] or nil
        if eid ~= nil and eid ~= origEmId then
            nonSameId[#nonSameId + 1] = e
        end
    end
    if #nonSameId > 0 then return nonSameId end

    return pool
end

local function buildExactEmIdCandidates(origEmId)
    local pool = getEnabledPool()
    if #pool == 0 then return pool end

    local exact = {}
    for _, e in ipairs(pool) do
        local eid = e.bytes and e.bytes[1] or nil
        if eid ~= nil and eid == origEmId then
            exact[#exact + 1] = e
        end
    end
    return exact
end

local function chooseDirectListTarget(origEntry, origEmId, sourceSig)
    local avoidName = nil
    if sourceSig and sourceSig ~= "" then
        avoidName = lastChoiceBySourceSig[sourceSig]
    end

    local nativePlan = buildNativeRewritePlan(origEmId, avoidName)
    if nativePlan then
        return nativePlan.template, nativePlan
    end

    local exact = buildExactEmIdCandidates(origEmId)
    if #exact > 0 then
        return pickFromCandidatesAvoidName(exact, avoidName), {
            mode = "preserve-emid",
            note = "direct-list exact-emid",
        }
    end

    local candidates = buildCompatibleCandidates(origEntry, origEmId)
    if #candidates == 0 then return nil, nil end
    return pickFromCandidatesAvoidName(candidates, avoidName), nil
end

local function pickReplacement(origEmId, avoidName)
    local pool = getEnabledPool()
    if #pool == 0 then return nil end

    local crowId = 35
    local eligible = {}
    for _, e in ipairs(pool) do
        local eid = e.bytes and e.bytes[1] or nil
        if eid ~= nil then
            if eid ~= origEmId and not (origEmId == crowId and eid == crowId) then
                eligible[#eligible + 1] = e
            end
        end
    end

    if #eligible > 0 then
        return pickFromCandidatesAvoidName(eligible, avoidName)
    end

    -- Hard fallback for crow sources: never keep crow when any non-crow exists globally.
    if origEmId == crowId then
        local globalNonCrow = {}
        for _, e in ipairs(ENEMIES) do
            local eid = e.bytes and e.bytes[1] or nil
            if eid and eid ~= crowId then
                globalNonCrow[#globalNonCrow + 1] = e
            end
        end
        if #globalNonCrow > 0 then
            return pickFromCandidatesAvoidName(globalNonCrow, avoidName)
        end
    end

    -- Fallback: if user enabled only one type, still return something.
    return pickFromCandidatesAvoidName(pool, avoidName)
end

-- ═══════════════════════════════════════════════════════════════════════
-- NATIVE HOOKS — Randomize enemy spawns via EmSetEvent
-- ═══════════════════════════════════════════════════════════════════════
-- Architecture:
--   readEmList (0x065E4278) is a ROOM-LEVEL FILE LOADER that loads the
--   entire ESL binary from disk on room transitions. We hook it ONLY
--   for level-change detection (it fires once per room change).
--
--   EmSetEvent (0x062E9E8C) is still the only place we directly rewrite
--   EM_LIST bytes.
--   EmSetFromList / EmSetFromList2 / EmListSetAlive are used as explicit
--   coverage markers for room-load / pre-placed / crow activation paths so
--   we stop pretending the load path is a single-hook problem.
--
--   GetEmPtrFromList (0x062EA154) returns cEm* for already-alive enemies.
--   GetEmIdFromList  (0x062EA1F4) reads emId from ESL slot by index.
--   getEmListNum always returns 19 (compile-time constant, NOT enemy count).
local sym_readEmList       = nil    -- Room ESL file loader (level-change detection)
local sym_GetEmPtrFromList = nil    -- Get cEm* for alive ESL entry (for scramble)
local sym_EmSetFromList    = nil    -- Whole-room / pre-placed load path marker
local sym_EmSetFromList2   = nil    -- Indexed pre-placed spawn path marker
local sym_EmListSetAlive   = nil    -- Reactivation marker for already-live ESL entries
local sym_OnSceEventBegin  = nil    -- Cutscene begin marker
local sym_OnSceEventEnd    = nil    -- Cutscene end marker

-- pcall Resolve calls — these compute (lib_base + offset), should be safe
pcall(function() sym_readEmList       = Resolve("readEmList",        0x065E4278) end)
pcall(function() sym_GetEmPtrFromList = Resolve("GetEmPtrFromList",  0x062EA154) end)
pcall(function() sym_EmSetFromList    = Resolve("_Z13EmSetFromListv", 0x05EE9610) end)
if not sym_EmSetFromList then
    pcall(function() sym_EmSetFromList = Resolve("EmSetFromList", 0x05EE9610) end)
end
pcall(function() sym_EmSetFromList2   = Resolve("_Z14EmSetFromList2jj", 0x05EE9A6C) end)
if not sym_EmSetFromList2 then
    pcall(function() sym_EmSetFromList2 = Resolve("EmSetFromList2", 0x05EE9A6C) end)
end
pcall(function() sym_EmListSetAlive   = Resolve("_Z14EmListSetAlivejb", 0x05EEA224) end)
if not sym_EmListSetAlive then
    pcall(function() sym_EmListSetAlive = Resolve("EmListSetAlive", 0x05EEA224) end)
end
pcall(function() sym_OnSceEventBegin = Resolve("OnSceEventBegin", 0x0644680C) end)
pcall(function() sym_OnSceEventEnd = Resolve("OnSceEventEnd", 0x064468D4) end)

V("Resolve: readEmList=" .. ptrfmt(sym_readEmList)
    .. " GetEmPtrFromList=" .. ptrfmt(sym_GetEmPtrFromList)
    .. " EmSetFromList=" .. ptrfmt(sym_EmSetFromList)
    .. " EmSetFromList2=" .. ptrfmt(sym_EmSetFromList2)
    .. " EmListSetAlive=" .. ptrfmt(sym_EmListSetAlive)
    .. " OnSceEventBegin=" .. ptrfmt(sym_OnSceEventBegin)
    .. " OnSceEventEnd=" .. ptrfmt(sym_OnSceEventEnd))

-- ═══════════════════════════════════════════════════════════════════════
-- NATIVE GAME EMID COMPATIBILITY (SDK-backed)
-- Source of truth:
--   Current Modloader SDK/Resident Evil 4/SDK/Enums/EEmID.lua
--   Current Modloader SDK/Resident Evil 4/SDK/Structs/VR4EnemyRemapRow.lua
--   sdks/re4 sdk/CXXHeaderDump/Script/VR4.hpp
--
-- VR4EnemyRemapRow / VR4EnemyListRow prove native enemy rows are structured by
-- family (EnemyId/Type/Em10Type/Set/Em10Set/flags/Health/Character/GuardRange).
-- So when a native emId is NOT one of our exact pool emIds, we must NOT blindly
-- rewrite it to an arbitrary other emId. That was the crash bug.
-- ═══════════════════════════════════════════════════════════════════════
listContains = function(list, value)
    if not list then return false end
    for _, v in ipairs(list) do
        if v == value then return true end
    end
    return false
end

local NATIVE_EMID_COMPAT = {}
local RANDOMIZABLE_EMIDS = {}

local function setNativeCompatRange(firstId, lastId, spec)
    for emId = firstId, lastId do
        NATIVE_EMID_COMPAT[emId] = spec
    end
end

local function setNativeCompatList(ids, spec)
    for _, emId in ipairs(ids) do
        NATIVE_EMID_COMPAT[emId] = spec
    end
end

setNativeCompatRange(16, 25, {
    mode = "preserve-emid",
    groups = {"Villagers", "Salvadors"},
    note = "Village ganado family",
})

setNativeCompatRange(26, 28, {
    mode = "preserve-emid",
    groups = {"Zealots", "Garradors"},
    note = "Castle ganado family",
})

setNativeCompatRange(29, 32, {
    mode = "preserve-emid",
    groups = {"Soldiers", "Mace Ganados", "JJs", "Super Salvadors"},
    note = "Island ganado family",
})

setNativeCompatRange(64, 79, {
    mode = "preserve-emid",
    groups = {"Soldiers", "Mace Ganados", "JJs", "Super Salvadors"},
    note = "Late-game island ganado family",
})

setNativeCompatList({33, 34}, {
    mode = "preserve-emid",
    groups = {"Dogs"},
    preferredEmIds = {34},
    note = "Dog family",
})

setNativeCompatList({35}, {
    mode = "preserve-emid",
    groups = {"Animals"},
    preferredEmIds = {35},
    note = "Crow family",
})

setNativeCompatList({36}, {
    mode = "preserve-emid",
    groups = {"Animals"},
    preferredEmIds = {36},
    note = "Snake family",
})

setNativeCompatList({37}, {
    mode = "preserve-emid",
    groups = {"Traps"},
    preferredEmIds = {37},
    note = "Parasite family",
})

setNativeCompatList({40}, {
    mode = "preserve-emid",
    groups = {"Animals"},
    preferredEmIds = {40},
    note = "Chicken family",
})

setNativeCompatList({42}, {
    mode = "preserve-emid",
    groups = {"Traps"},
    preferredEmIds = {42},
    note = "Trap family",
})

setNativeCompatList({43}, {
    mode = "preserve-emid",
    groups = {"Gigantes"},
    preferredEmIds = {43},
    note = "Gigante family",
})

setNativeCompatList({45}, {
    mode = "preserve-emid",
    groups = {"Novistadores"},
    preferredEmIds = {45},
    note = "InsectHuman / Novistador family",
})

setNativeCompatList({50}, {
    mode = "preserve-emid",
    groups = {"Bosses"},
    preferredEmIds = {50},
    note = "U3 family",
})

setNativeCompatList({52, 53}, {
    mode = "preserve-emid",
    groups = {"Bosses"},
    preferredEmIds = {53},
    note = "Mayor / Mendez family",
})

setNativeCompatList({54}, {
    mode = "preserve-emid",
    groups = {"Regenerators"},
    preferredEmIds = {54},
    note = "Regenerator family",
})

setNativeCompatList({55, 56}, {
    mode = "preserve-emid",
    groups = {"Verdugos"},
    preferredEmIds = {55, 56},
    note = "No2 / Verdugo family",
})

setNativeCompatList({57}, {
    mode = "preserve-emid",
    groups = {"Bosses"},
    preferredEmIds = {57},
    note = "No3 / Krauser family",
})

setNativeCompatList({58}, {
    mode = "preserve-emid",
    groups = {"Drones"},
    preferredEmIds = {58},
    note = "Seeker / drone family",
})

setNativeCompatList({60}, {
    mode = "preserve-emid",
    groups = {"Armaduras"},
    preferredEmIds = {60},
    note = "Armor family",
})

setNativeCompatList({48, 49, 63}, {
    mode = "hp-only",
    groups = {"Bosses"},
    note = "Saddler family (HP-only safe fallback)",
})

for _, e in ipairs(ENEMIES) do
    if e and e.bytes and e.bytes[1] ~= nil then
        RANDOMIZABLE_EMIDS[e.bytes[1]] = true
    end
end
for emId, _ in pairs(NATIVE_EMID_COMPAT) do
    RANDOMIZABLE_EMIDS[emId] = true
end

buildNativeRewritePlan = function(origEmId, avoidName)
    local spec = NATIVE_EMID_COMPAT[origEmId]
    if not spec then return nil end

    local pool = getEnabledPool()
    local exact = {}
    local preferred = {}
    local grouped = {}

    for _, e in ipairs(pool) do
        if e and e.bytes then
            local eid = e.bytes[1]
            if eid == origEmId then
                exact[#exact + 1] = e
            elseif spec.preferredEmIds and listContains(spec.preferredEmIds, eid) then
                preferred[#preferred + 1] = e
            elseif spec.groups and listContains(spec.groups, e.group) then
                grouped[#grouped + 1] = e
            end
        end
    end

    -- Safety rule (v11.7): for preserve-emid natives, only copy tail bytes
    -- from templates that have the EXACT same emId. Cross-emId tail copies
    -- caused crashes in cutscene/room-load scripted paths.
    if spec.mode == "preserve-emid" and #exact > 0 then
        local template = pickFromCandidatesAvoidName(exact, avoidName)
        return {
            mode = "preserve-emid",
            template = template,
            note = (spec.note or "native") .. " exact-emid",
        }
    end

    local template = nil
    if #preferred > 0 then
        template = pickFromCandidatesAvoidName(preferred, avoidName)
    elseif #grouped > 0 then
        template = pickFromCandidatesAvoidName(grouped, avoidName)
    end

    if not template then return nil end

    if spec.mode == "preserve-emid" then
        return {
            mode = "hp-only",
            template = template,
            note = (spec.note or "native") .. " hp-only fallback (no exact emId template)",
        }
    end

    return {
        mode = spec.mode,
        template = template,
        note = spec.note or "native",
    }
end

local function chooseRewriteTarget(origEmId)
    local nativePlan = buildNativeRewritePlan(origEmId)
    if nativePlan then
        return nativePlan.template, nativePlan
    end
    return pickReplacement(origEmId), nil
end

local function castEmListRow(emListPtr)
    if not emListPtr or ptrval(emListPtr) == 0 then return nil end
    return {
        status = ReadU8(Offset(emListPtr, 0x00)),
        emId = ReadU8(Offset(emListPtr, 0x01)),
        subType = ReadU8(Offset(emListPtr, 0x02)),
        param1 = ReadU8(Offset(emListPtr, 0x03)),
        param2 = ReadU8(Offset(emListPtr, 0x04)),
        extra1 = ReadU8(Offset(emListPtr, 0x05)),
        extra2 = ReadU8(Offset(emListPtr, 0x06)),
        extra3 = ReadU8(Offset(emListPtr, 0x07)),
        hpLo = ReadU8(Offset(emListPtr, 0x08)),
        hpHi = ReadU8(Offset(emListPtr, 0x09)),
    }
end

local function applyReplacementToEmListRow(emListPtr, pick, hp, nativePlan)
    if nativePlan == nil or nativePlan.mode == "preserve-emid" then
        local orig = castEmListRow(emListPtr)
        if not orig then return nil end
        local targetEmId = nativePlan and orig.emId or pick.bytes[1]
        WriteU8(Offset(emListPtr, 0x01), targetEmId)
        WriteU8(Offset(emListPtr, 0x02), pick.bytes[2])
        WriteU8(Offset(emListPtr, 0x03), pick.bytes[3])
        WriteU8(Offset(emListPtr, 0x04), pick.bytes[4])
        WriteU8(Offset(emListPtr, 0x05), pick.bytes[5] or 0)
        WriteU8(Offset(emListPtr, 0x06), pick.bytes[6] or 0)
        WriteU8(Offset(emListPtr, 0x07), pick.bytes[7] or 0)
    end

    WriteU8(Offset(emListPtr, 0x08), hp[1])
    WriteU8(Offset(emListPtr, 0x09), hp[2])

    if pick.removeInvincible then
        local p3 = ReadU8(Offset(emListPtr, 0x03))
        WriteU8(Offset(emListPtr, 0x03), p3 & ~0x40)
    end

    local final = castEmListRow(emListPtr)
    return final and final.emId or nil
end

local currentChoiceBySourceSig = {}

local function chooseRewriteTargetWithHistory(origEmId, sourceSig)
    local avoidName = nil
    if sourceSig and sourceSig ~= "" then
        avoidName = lastChoiceBySourceSig[sourceSig]
    end
    local nativePlan = buildNativeRewritePlan(origEmId, avoidName)
    if nativePlan then
        return nativePlan.template, nativePlan
    end
    return pickReplacement(origEmId, avoidName), nil
end

local function shouldSkipUnknownNativeRow(matchKind, nativePlan)
    if not nativePlan then return false end
    if nativePlan.mode ~= "hp-only" then return false end
    -- v11.9: Do NOT skip unknown native rows for hp-only fallback.
    -- We preserve emId/params and only touch HP, which safely affects
    -- scripted/native rows that do not match our pool signatures.
    return false
end

local slotMap = {}          -- [emListPtr_value] = {name, replEmId} for double-randomization prevention
local replacementJournal = {}
local replacementSeq = 0
local MAX_JOURNAL = 4096

local function pushReplacementEvent(ev)
    replacementSeq = replacementSeq + 1
    ev.seq = replacementSeq
    replacementJournal[#replacementJournal + 1] = ev
    if #replacementJournal > MAX_JOURNAL then
        table.remove(replacementJournal, 1)
    end
end

local function dumpReplacementJournal(filterGen)
    Log(TAG .. ": ==== Replacement Journal Dump START ==== entries=" .. tostring(#replacementJournal)
        .. " filterGen=" .. tostring(filterGen))
    local dumped = 0
    for _, ev in ipairs(replacementJournal) do
        if filterGen == nil or ev.gen == filterGen then
            dumped = dumped + 1
            Log(TAG .. ": DUMP #" .. tostring(ev.seq)
                .. " gen=" .. tostring(ev.gen)
                .. " src=" .. tostring(ev.source)
                .. " ptr=" .. tostring(ev.ptr)
                .. " action=" .. tostring(ev.action)
                .. " old=" .. tostring(ev.oldName)
                .. " oldId=" .. tostring(ev.oldEmId)
                .. " -> new=" .. tostring(ev.newName)
                .. " newId=" .. tostring(ev.newEmId)
                .. (ev.mode and (" mode=" .. ev.mode) or "")
                .. (ev.note and (" note=" .. ev.note) or "")
                .. (ev.reason and (" reason=" .. ev.reason) or "")
                .. (ev.bytes and (" bytes=[" .. ev.bytes .. "]") or ""))
        end
    end
    Log(TAG .. ": ==== Replacement Journal Dump END ==== dumped=" .. tostring(dumped))
end

local function isRandomizableEmId(emId)
    return emId ~= nil and RANDOMIZABLE_EMIDS[emId] == true
end

local base = nil
local addr_pG = nil
local pG_ptr = nil

local function initNativePointers()
    if base and addr_pG then return end
    pcall(function() base = GetLibBase() end)
    -- Prefer symbol (safer across builds), then fallback offset.
    pcall(function() addr_pG = Resolve("pG", 0x0A856E48) end)
    if (not addr_pG or ptrval(addr_pG) == 0) and base then
        pcall(function() addr_pG = Offset(base, 0x0A856E48) end)
    end
end

local function getListPtrFromIndex(idx)
    if idx == nil then return nil end
    if idx < 0 or idx > 0xFE then return nil end
    initNativePointers()
    if not addr_pG then return nil end
    if not pG_ptr or ptrval(pG_ptr) == 0 then
        pcall(function() pG_ptr = ReadPtr(addr_pG) end)
    end
    if not pG_ptr or ptrval(pG_ptr) == 0 then return nil end
    -- Guard obvious bad pointer case seen with stale fallback offsets.
    if ptrval(pG_ptr) <= 0x100000000 then return nil end
    return Offset(pG_ptr, (idx << 5) + 0x549c)
end

-- No settle period needed — EmSetEvent is sole randomization hook (no double-randomization risk)
local SETTLE_CALLS = 0      -- No skip needed (was 5 in old eslPointers architecture)
local settleCounter = 0     -- Counts down after level change detected (0 = ready)
local hookErrors = 0        -- Consecutive hook/write errors (reset on success)
local MAX_HOOK_ERRORS = 20  -- Auto-disable writes after this many consecutive errors
local roomLoadEvents = 0
local preplacedPathEvents = 0
local reactivationEvents = 0
local emSetEventCalls = 0
local crowSourceSeen = 0
local crowReplacementSelected = 0
local skippedUnknownSources = 0
local preplacedRewrites = 0
local cutsceneDepth = 0
local cutsceneActive = false
local cutsceneSuppressUntil = 0.0
local cutsceneBeginEvents = 0
local cutsceneEndEvents = 0
local cutsceneSkipCount = 0
local CUTSCENE_POST_SUPPRESS_SEC = 0.75
local rewriteListSlotByIndex
local fullLevelRewriteQueued = 0
local fullLevelRewriteCompleted = 0
local fullLevelRewriteChanged = 0
local fullRewriteInFlight = false
local fullRewriteIndex = 0
local fullRewriteGen = -1
local levelDetected = false
local function runImmediateFullLevelRewrite(reason, force)
    local allowDirect = force and ALLOW_MANUAL_DIRECT_LIST_REWRITES or ENABLE_AUTO_DIRECT_LIST_REWRITES
    if not allowDirect then
        V("Full-level rewrite disabled; skipping reason=" .. tostring(reason))
        return false
    end
    if not state.enabled then return false end
    if not levelDetected then return false end
    if fullRewriteInFlight then return false end

    fullRewriteInFlight = true
    fullLevelRewriteQueued = fullLevelRewriteQueued + 1
    fullRewriteGen = currentGen

    local changed = 0
    for idx = 0, 254 do
        local ok, did = pcall(function()
            return rewriteListSlotByIndex(idx, reason or "full-level")
        end)
        if ok and did then
            changed = changed + 1
        end
    end

    fullLevelRewriteCompleted = fullLevelRewriteCompleted + 1
    fullLevelRewriteChanged = changed
    fullRewriteInFlight = false
    fullRewriteIndex = 255
    Log(TAG .. ": Full-level rewrite complete gen=" .. currentGen
        .. " changed=" .. tostring(changed)
        .. " reason=" .. tostring(reason))
    return changed > 0
end

local function queueFullLevelRewrite(reason, force)
    return runImmediateFullLevelRewrite(reason, force)
end

local function nowSec()
    local ok, t = pcall(os.clock)
    if ok and type(t) == "number" then return t end
    return 0
end

local function isCutsceneSuppressed()
    if cutsceneActive then return true, "active" end
    if nowSec() < cutsceneSuppressUntil then return true, "cooldown" end
    return false, nil
end

rewriteListSlotByIndex = function(idx, sourceTag)
    if not state.enabled then return false end
    if not levelDetected then return false end
    local listPtr = getListPtrFromIndex(idx)
    if not listPtr or ptrval(listPtr) == 0 then return false end

    -- tombstone-05 mitigation: stale pG_ptr can produce an offset inside
    -- libUE4.so (.rodata/vtables). Writing there corrupts the vtable and causes
    -- SIGBUS (BUS_ADRALN). Reject any pointer that falls inside the ~200 MB
    -- engine library window.  Below 0x100000000 is also invalid on 64-bit AArch64.
    local _lpv = ptrval(listPtr)
    if _lpv < 0x100000000 then return false end
    if base and ptrval(base) > 0 then
        local _lbStart = ptrval(base)
        if _lpv >= _lbStart and _lpv < (_lbStart + 0x0D000000) then
            Log(TAG .. ": rewriteListSlotByIndex SKIP ptr inside engine range 0x"
                .. string.format("%X", _lpv))
            return false
        end
    end

    local row = castEmListRow(listPtr)
    if not row then return false end
    local origEmId = row.emId
    if not origEmId or origEmId == 0 then return false end

    if not isRandomizableEmId(origEmId) then
        pushReplacementEvent({
            gen = currentGen,
            source = sourceTag or "list",
            ptr = ptrfmt(listPtr),
            action = "skip",
            reason = "non-randomizable-emid",
            oldName = "Unknown/NPC",
            oldEmId = origEmId,
            newName = "(unchanged)",
            newEmId = origEmId,
        })
        return false
    end

    -- Trust the known EM_LIST structure — write to any non-zero emId slot.
    -- Pool lookup is for logging only; signature match is NOT required.
    local origEntry, matchKind = findOriginalEnemyDef(listPtr, origEmId, true)
    local origName = origEntry and origEntry.name or ("emId=" .. origEmId)

    local sourceSig = ""
    pcall(function()
        sourceSig = getEmListSignature(listPtr)
    end)

    local pick, nativePlan = chooseDirectListTarget(origEntry, origEmId, sourceSig)
    if not pick or not pick.bytes then return false end
    if shouldSkipUnknownNativeRow(matchKind, nativePlan) then
        V("List rewrite SKIP unknown native row (no exact template) idx=" .. tostring(idx)
            .. " emId=" .. tostring(origEmId))
        return false
    end
    local hp = getRandomHP(pick)

    local finalEmId = applyReplacementToEmListRow(listPtr, pick, hp, nativePlan)
    if not finalEmId then return false end

    if origEmId == 35 then crowSourceSeen = crowSourceSeen + 1 end
    if pick.bytes[1] == 35 then crowReplacementSelected = crowReplacementSelected + 1 end

    slotMap[ptrval(listPtr)] = {
        name = pick.name,
        replEmId = finalEmId,
        source = sourceTag or "list",
    }
    pushReplacementEvent({
        gen = currentGen,
        source = sourceTag or "list",
        ptr = ptrfmt(listPtr),
        action = "replace",
        oldName = origName,
        oldEmId = origEmId,
        newName = pick.name,
        newEmId = finalEmId,
        mode = nativePlan and nativePlan.mode or "full",
        note = nativePlan and nativePlan.note or nil,
    })
    if sourceSig ~= "" then
        currentChoiceBySourceSig[sourceSig] = pick.name
    end
    state.swapCount = state.swapCount + 1
    levelSwaps = levelSwaps + 1
    return true, origName, pick.name
end

local function resetRoomTracking(reason)
    levelDetected = true
    currentGen = currentGen + 1
    lastChoiceBySourceSig = currentChoiceBySourceSig
    currentChoiceBySourceSig = {}
    slotMap = {}
    levelSwaps = 0
    settleCounter = SETTLE_CALLS
    hookErrors = 0
    roomLoadEvents = 0
    preplacedPathEvents = 0
    reactivationEvents = 0
    emSetEventCalls = 0
    crowSourceSeen = 0
    crowReplacementSelected = 0
    skippedUnknownSources = 0
    preplacedRewrites = 0
    cutsceneSkipCount = 0
    fullRewriteInFlight = false
    fullRewriteIndex = 0
    fullRewriteGen = -1
    fullLevelRewriteChanged = 0
    Log(TAG .. ": === ROOM LOAD #" .. currentGen
        .. " === " .. tostring(reason) .. " — clearing caches, settle=" .. SETTLE_CALLS)
end

local function ensureLevelDetected(reason)
    if levelDetected then return end
    resetRoomTracking(reason or "spawn bootstrap")
end

-- Hook readEmList — fires ONCE per room change (loads ESL file from disk)
-- We use this ONLY for level-change detection: clear caches so EmSetEvent
-- re-randomizes all enemies freshly in the new room.
local hookInstalled = false
if sym_readEmList and (state.enabled or INSTALL_HOOKS_WHEN_DISABLED) then
    local hookOk, hookErr = pcall(function()
        RegisterNativeHookAt(sym_readEmList, "readEmList",
        function(heapCtx)
            -- Pre-hook: readEmList arg is a heap context, NOT an enemy index
            -- Just note that a room load is happening
            pcall(function()
                resetRoomTracking("readEmList fired")
            end)
        end,
        function(ret)
            -- Post-hook: return value unchanged (it's from FMemory::Free / stack cleanup)
            pcall(function()
                queueFullLevelRewrite("readEmList post")
            end)
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
    if not sym_readEmList then
        Log(TAG .. ": [WARN] readEmList symbol not resolved — level-change detection unavailable")
    else
        Log(TAG .. ": readEmList hook skipped (Randomizer disabled at startup)")
    end
end

local pathHooksInstalled = false

local function installPathMarkerHooks()
    if pathHooksInstalled then return end
    if not state.enabled and not INSTALL_HOOKS_WHEN_DISABLED then
        Log(TAG .. ": Path marker hooks skipped (Randomizer disabled at startup)")
        return
    end

    if sym_EmSetFromList then
        local ok = pcall(function()
            RegisterNativeHookAt(sym_EmSetFromList, "EmSetFromList",
                function()
                    pcall(function()
                        roomLoadEvents = roomLoadEvents + 1
                        V("EmSetFromList: room pre-placed load path fired (#" .. roomLoadEvents .. ")")
                    end)
                end,
                    function(ret)
                    end)
        end)
        if ok then
            Log(TAG .. ": Hooked EmSetFromList — room-array load marker")
            pathHooksInstalled = true
        end
    end

    if sym_EmSetFromList2 then
        local ok = pcall(function()
            RegisterNativeHookAt(sym_EmSetFromList2, "EmSetFromList2",
                function(idx, flags)
                    pcall(function()
                        preplacedPathEvents = preplacedPathEvents + 1
                        ensureLevelDetected("EmSetFromList2 bootstrap")
                        if ENABLE_EMSETFROMLIST2_DIRECT_REWRITES and ENABLE_AUTO_DIRECT_LIST_REWRITES and state.enabled and not SharedAPI._skipRandomizer then
                            local did, origName, newName = rewriteListSlotByIndex(tonumber(idx) or -1, "EmSetFromList2")
                            if did then
                                preplacedRewrites = preplacedRewrites + 1
                                if preplacedRewrites <= 20 then
                                    Log(TAG .. ": EmSetFromList2 [gen" .. currentGen .. "] "
                                        .. tostring(origName)
                                        .. " -> " .. tostring(newName)
                                        .. " idx=" .. tostring(idx))
                                end
                            end
                        end
                        if preplacedPathEvents <= 12 then
                            Log(TAG .. ": EmSetFromList2 FIRE #" .. preplacedPathEvents
                                .. " gen=" .. currentGen
                                .. " idx=" .. tostring(idx)
                                .. " flags=" .. tostring(flags))
                        end
                    end)
                end,
                    function(ret)
                    end)
        end)
        if ok then
            Log(TAG .. ": Hooked EmSetFromList2 — indexed pre-placed spawn marker")
            pathHooksInstalled = true
        end
    end

    if sym_EmListSetAlive then
        local ok = pcall(function()
            RegisterNativeHookAt(sym_EmListSetAlive, "EmListSetAlive",
                function(idx, alive)
                    pcall(function()
                        if alive and alive ~= 0 then
                            reactivationEvents = reactivationEvents + 1
                            if reactivationEvents <= 12 then
                                Log(TAG .. ": EmListSetAlive FIRE #" .. reactivationEvents
                                    .. " gen=" .. currentGen
                                    .. " idx=" .. tostring(idx)
                                    .. " alive=" .. tostring(alive))
                            end
                        end
                    end)
                end,
                    function(ret)
                    end)
        end)
        if ok then
            Log(TAG .. ": Hooked EmListSetAlive — reactivation marker")
            pathHooksInstalled = true
        end
    end

    if sym_OnSceEventBegin then
        local ok = pcall(function()
            RegisterNativeHookAt(sym_OnSceEventBegin, "OnSceEventBegin",
                function()
                    pcall(function()
                        cutsceneBeginEvents = cutsceneBeginEvents + 1
                        cutsceneDepth = cutsceneDepth + 1
                        cutsceneActive = true
                        local untilTs = nowSec() + CUTSCENE_POST_SUPPRESS_SEC
                        if untilTs > cutsceneSuppressUntil then
                            cutsceneSuppressUntil = untilTs
                        end
                        if cutsceneBeginEvents <= 20 then
                            Log(TAG .. ": Cutscene BEGIN #" .. cutsceneBeginEvents
                                .. " depth=" .. cutsceneDepth)
                        end
                    end)
                end,
                function(ret)
                end)
        end)
        if ok then
            Log(TAG .. ": Hooked OnSceEventBegin — cutscene guard start")
            pathHooksInstalled = true
        end
    end

    if sym_OnSceEventEnd then
        local ok = pcall(function()
            RegisterNativeHookAt(sym_OnSceEventEnd, "OnSceEventEnd",
                function()
                    pcall(function()
                        cutsceneEndEvents = cutsceneEndEvents + 1
                        cutsceneDepth = cutsceneDepth - 1
                        if cutsceneDepth < 0 then cutsceneDepth = 0 end
                        cutsceneActive = (cutsceneDepth > 0)
                        local untilTs = nowSec() + CUTSCENE_POST_SUPPRESS_SEC
                        if untilTs > cutsceneSuppressUntil then
                            cutsceneSuppressUntil = untilTs
                        end
                        if cutsceneEndEvents <= 20 then
                            Log(TAG .. ": Cutscene END #" .. cutsceneEndEvents
                                .. " depth=" .. cutsceneDepth
                                .. " cooldown=" .. CUTSCENE_POST_SUPPRESS_SEC .. "s")
                        end
                    end)
                end,
                function(ret)
                end)
        end)
        if ok then
            Log(TAG .. ": Hooked OnSceEventEnd — cutscene guard end")
            pathHooksInstalled = true
        end
    end
end

installPathMarkerHooks()

-- ═══════════════════════════════════════════════════════════════════════
-- NATIVE HOOK: EmSetEvent — THE SOLE RANDOMIZATION HOOK
-- ═══════════════════════════════════════════════════════════════════════
-- EmSetEvent is still the actual mutation point where we rewrite EM_LIST.
-- The room-load / pre-placed path markers above exist because relying on a
-- single-hook assumption caused room arrays and crows to slip through.
local sym_EmSetEvent = nil
-- Use mangled C++ name for reliable dlsym resolution (plain "EmSetEvent" may fall back to wrong offset)
pcall(function() sym_EmSetEvent = Resolve("_Z10EmSetEventP7EM_LIST", 0x062E9E8C) end)
if not sym_EmSetEvent then
    pcall(function() sym_EmSetEvent = Resolve("EmSetEvent", 0x062E9E8C) end)
end
Log(TAG .. ": Resolve: EmSetEvent=" .. ptrfmt(sym_EmSetEvent))

local emSetEventHooked = false
local function installEmSetEventHook()
    if emSetEventHooked then return true end
    if not sym_EmSetEvent then
        Log(TAG .. ": [WARN] EmSetEvent symbol not resolved — event spawns not randomized")
        return false
    end
    if not state.enabled and not INSTALL_HOOKS_WHEN_DISABLED then
        Log(TAG .. ": EmSetEvent hook skipped (Randomizer disabled at startup)")
        return false
    end

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
                    ensureLevelDetected("EmSetEvent bootstrap")
                    if emListPtr == nil or ptrval(emListPtr) == 0 then return end
                    -- tombstone-05 mitigation: reject pointer in engine library range
                    do
                        local _epv = ptrval(emListPtr)
                        if _epv < 0x100000000 then return end
                        if base and ptrval(base) > 0 then
                            local _lbStart = ptrval(base)
                            if _epv >= _lbStart and _epv < (_lbStart + 0x0D000000) then
                                Log(TAG .. ": EmSetEvent SKIP ptr inside engine range 0x"
                                    .. string.format("%X", _epv))
                                return
                            end
                        end
                    end
                    if hookErrors >= MAX_HOOK_ERRORS then return end

                    -- Skip EnemySpawner's deliberate manual spawns
                    if SharedAPI and SharedAPI._skipRandomizer then
                        V("EmSetEvent: SKIP (EnemySpawner manual spawn)")
                        return
                    end

                    local blockedByCutscene, cutsceneReason = isCutsceneSuppressed()
                    if blockedByCutscene then
                        cutsceneSkipCount = cutsceneSkipCount + 1
                        if cutsceneSkipCount <= 30 then
                            Log(TAG .. ": EmSetEvent SKIP cutscene(" .. tostring(cutsceneReason)
                                .. ") depth=" .. cutsceneDepth)
                        end
                        return
                    end

                    -- Settling period after level change
                    if settleCounter > 0 then
                        settleCounter = settleCounter - 1
                        V("EmSetEvent: SETTLE skip (" .. settleCounter .. " remaining)")
                        return
                    end

                    -- Read original emId from correct offset (+0x01)
                    local emKey = ptrval(emListPtr)
                    local prepatched = slotMap[emKey]
                    if prepatched then
                        V("EmSetEvent: SKIP already randomized via " .. tostring(prepatched.source)
                            .. " (emId=" .. tostring(prepatched.replEmId) .. ")")
                        return
                    end

                    local row = castEmListRow(emListPtr)
                    if not row then return end
                    local origEmId = row.emId
                    emSetEventCalls = emSetEventCalls + 1
                    local origBytes = ""
                    pcall(function()
                        local parts = {}
                        for j = 0, 9 do
                            parts[j+1] = string.format("%02X", ReadU8(Offset(emListPtr, j)))
                        end
                        origBytes = table.concat(parts, " ")
                    end)

                    -- Skip empty ESL slots (emId=0)
                    if origEmId == 0 then return end

                    if not isRandomizableEmId(origEmId) then
                        pushReplacementEvent({
                            gen = currentGen,
                            source = "EmSetEvent",
                            ptr = ptrfmt(emListPtr),
                            action = "skip",
                            reason = "non-randomizable-emid",
                            oldName = "Unknown/NPC",
                            oldEmId = origEmId,
                            newName = "(unchanged)",
                            newEmId = origEmId,
                            bytes = origBytes,
                        })
                        V("EmSetEvent: SKIP non-randomizable emId=" .. tostring(origEmId)
                            .. " bytes=[" .. origBytes .. "]")
                        return
                    end

                    -- Trust the known EM_LIST structure — write to any non-zero emId.
                    -- (+0x01=emId, +0x02=subType, +0x03=param1, +0x04=param2,
                    --  +0x05-07=extra, +0x08-09=HP LE int16)
                    -- Pool lookup is for logging only; no signature gate.
                    local origEntry, matchKind = findOriginalEnemyDef(emListPtr, origEmId, true)
                    local origName = origEntry and origEntry.name or ("emId=" .. origEmId)

                    -- Diagnostic: log first 10 hook fires per level to confirm hook is working
                    if levelSwaps < 10 then
                        Log(TAG .. ": EmSetEvent FIRE #" .. (levelSwaps + 1)
                            .. " gen=" .. currentGen
                            .. " old=" .. origName
                            .. " (emId=" .. origEmId .. ")"
                            .. " match=" .. tostring(matchKind)
                            .. " bytes=[" .. origBytes .. "]")
                    end

                    local sourceSig = ""
                    pcall(function()
                        sourceSig = getEmListSignature(emListPtr)
                    end)

                    -- Pick replacement using explicit SDK-backed native families.
                    -- Avoid repeating the same template for the same source signature
                    -- on consecutive room loads when alternatives exist.
                    local pick, nativePlan = chooseRewriteTargetWithHistory(origEmId, sourceSig)
                    if not pick then V("EmSetEvent: empty pool"); return end
                    if shouldSkipUnknownNativeRow(matchKind, nativePlan) then
                        skippedUnknownSources = skippedUnknownSources + 1
                        if skippedUnknownSources <= 30 then
                            Log(TAG .. ": EmSetEvent SKIP unknown native row"
                                .. " emId=" .. tostring(origEmId)
                                .. " bytes=[" .. origBytes .. "]")
                        end
                        return
                    end
                    local hp = getRandomHP(pick)
                    if origEmId == 35 then crowSourceSeen = crowSourceSeen + 1 end
                    if pick.bytes and pick.bytes[1] == 35 then crowReplacementSelected = crowReplacementSelected + 1 end

                    -- Write replacement through casted EM_LIST row helper.
                    local finalEmId = nil
                    local writeOk = pcall(function()
                        finalEmId = applyReplacementToEmListRow(emListPtr, pick, hp, nativePlan)
                    end)

                    if writeOk and finalEmId ~= nil then
                        state.swapCount = state.swapCount + 1
                        levelSwaps = levelSwaps + 1
                        hookErrors = 0
                        if sourceSig ~= "" then
                            currentChoiceBySourceSig[sourceSig] = pick.name
                        end
                        pushReplacementEvent({
                            gen = currentGen,
                            source = "EmSetEvent",
                            ptr = ptrfmt(emListPtr),
                            action = "replace",
                            oldName = origName,
                            oldEmId = origEmId,
                            newName = pick.name,
                            newEmId = finalEmId,
                            mode = nativePlan and nativePlan.mode or "full",
                            note = nativePlan and nativePlan.note or nil,
                            bytes = origBytes,
                        })
                        Log(TAG .. ": EmSetEvent [gen" .. currentGen .. "] "
                            .. origName .. " (emId=" .. origEmId .. ")"
                            .. " -> " .. pick.name
                            .. " (outEmId=" .. tostring(finalEmId) .. ")"
                            .. (nativePlan and (" mode=" .. nativePlan.mode .. " note=" .. nativePlan.note) or "")
                            .. " HP=" .. makeHP(hp[1], hp[2]))
                    else
                        hookErrors = hookErrors + 1
                        V("EmSetEvent: WRITE-FAIL for " .. origName .. " (emId=" .. origEmId .. ")")
                    end
                end)
            end,
            function(ret)
            end)
    end)
    if hookOk then
        emSetEventHooked = true
        Log(TAG .. ": Hooked EmSetEvent — rewrites EM_LIST at spawn time")
        return true
    else
        Log(TAG .. ": [ERROR] Failed to hook EmSetEvent: " .. tostring(hookErr))
        return false
    end
end
installEmSetEventHook()

local function ensureHooksInstalled()
    if not pathHooksInstalled then
        installPathMarkerHooks()
    end
    if not emSetEventHooked then
        installEmSetEventHook()
    end
end

-- NOTE: EmListSetAlive hook REMOVED in v11.0.
-- EmListSetAlive(unsigned int index, bool) is a REACTIVATION function
-- for already-spawned enemies. It takes an INDEX, not an EM_LIST*.
-- All actual spawns (including ESL-loaded ones) go through EmSetEvent
-- via the EmSetFromList2 → EmSetEvent call chain.

-- ═══════════════════════════════════════════════════════════════════════
-- SCRAMBLE — Immediate randomization of all current alive enemies
-- ═══════════════════════════════════════════════════════════════════════
-- Uses GetEmPtrFromList (0x062EA154) to iterate alive cEm* objects.
-- The ESL has up to 255 entries. getEmListNum always returns 19 (hardcoded
-- stage-type count), NOT the actual enemy count. We iterate all 255 slots.

local function scrambleNow()
    ensureLevelDetected("scramble command")

    slotMap = {}
    levelSwaps = 0
    settleCounter = 0
    hookErrors = 0

    local ok = queueFullLevelRewrite("scramble command", true)
    local changed = fullLevelRewriteChanged or 0
    Log(TAG .. ": Scramble immediate rewrite -> " .. tostring(ok) .. " changed=" .. tostring(changed))
    Notify(TAG, ok and ("Scramble done: " .. tostring(changed)) or "Scramble skipped")
    return changed
end

-- ═══════════════════════════════════════════════════════════════════════
-- COMMANDS
-- ═══════════════════════════════════════════════════════════════════════

RegisterCommand("randomizer", function()
    state.enabled = not state.enabled
    if state.enabled then
        ensureHooksInstalled()
    end
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
        .. " skippedUnknown=" .. skippedUnknownSources
        .. " cutsceneActive=" .. tostring(cutsceneActive)
        .. " cutsceneDepth=" .. cutsceneDepth
        .. " cutsceneSkips=" .. cutsceneSkipCount
        .. " autoDirect=" .. tostring(ENABLE_AUTO_DIRECT_LIST_REWRITES)
        .. " levelDetected=" .. tostring(levelDetected)
        .. " fullChanged=" .. fullLevelRewriteChanged
        .. " fullQ=" .. fullLevelRewriteQueued
        .. " fullDone=" .. fullLevelRewriteCompleted
        .. " journal=" .. #replacementJournal
        .. " groups=" .. #GROUPS)
end)

    RegisterCommand("rnd_rewrite_level", function()
            local ok = queueFullLevelRewrite("command", true)
            Log(TAG .. ": rnd_rewrite_level -> " .. tostring(ok))
            Notify(TAG, ok and "rewrite done" or "rewrite skipped")
    end)

    RegisterCommand("rnd_counters", function()
        Log(TAG .. ": counters"
        .. " gen=" .. currentGen
        .. " emSetEventCalls=" .. emSetEventCalls
        .. " preplacedRewrites=" .. preplacedRewrites
        .. " crowSourceSeen=" .. crowSourceSeen
        .. " crowReplacementSelected=" .. crowReplacementSelected
        .. " skippedUnknownSources=" .. skippedUnknownSources
        .. " cutsceneBeginEvents=" .. cutsceneBeginEvents
        .. " cutsceneEndEvents=" .. cutsceneEndEvents
        .. " cutsceneDepth=" .. cutsceneDepth
        .. " cutsceneActive=" .. tostring(cutsceneActive)
        .. " cutsceneSkips=" .. cutsceneSkipCount
        .. " fullLevelRewriteQueued=" .. fullLevelRewriteQueued
        .. " fullLevelRewriteCompleted=" .. fullLevelRewriteCompleted
        .. " fullLevelRewriteChanged=" .. fullLevelRewriteChanged
        .. " levelDetected=" .. tostring(levelDetected)
        .. " roomLoadEvents=" .. roomLoadEvents
        .. " preplacedPathEvents=" .. preplacedPathEvents
        .. " reactivationEvents=" .. reactivationEvents
        .. " levelSwaps=" .. levelSwaps
        .. " journal=" .. #replacementJournal)
    end)

    RegisterCommand("rnd_dump_replacements", function(args)
        local mode = tostring(args or "gen")
        if mode == "all" then
            dumpReplacementJournal(nil)
        else
            dumpReplacementJournal(currentGen)
        end
    end)

    RegisterCommand("rnd_clear_replacements", function()
        replacementJournal = {}
        Log(TAG .. ": Replacement journal cleared")
    end)

-- ═══════════════════════════════════════════════════════════════════════
-- SHARED API
-- ═══════════════════════════════════════════════════════════════════════

if SharedAPI then
    SharedAPI.Randomizer = {
        isEnabled  = function() return state.enabled end,
        setEnabled = function(v)
            state.enabled = v
            if state.enabled then
                ensureHooksInstalled()
            end
            slotMap = {}
            levelSwaps = 0
            settleCounter = 0
            hookErrors = 0
            saveConfig()
        end,
        getHPMode  = function() return state.hpMode end,
        setHPMode  = function(m) state.hpMode = m; saveConfig() end,
        scramble   = scrambleNow,
        rewriteLevel = function() return queueFullLevelRewrite("sharedapi", true) end,
        getPoolSize = function() return #getEnabledPool() end,
        getSwapCount = function() return state.swapCount end,
        getDebugCounters = function()
            return {
                gen = currentGen,
                emSetEventCalls = emSetEventCalls,
                preplacedRewrites = preplacedRewrites,
                crowSourceSeen = crowSourceSeen,
                crowReplacementSelected = crowReplacementSelected,
                skippedUnknownSources = skippedUnknownSources,
                cutsceneBeginEvents = cutsceneBeginEvents,
                cutsceneEndEvents = cutsceneEndEvents,
                cutsceneDepth = cutsceneDepth,
                cutsceneActive = cutsceneActive,
                cutsceneSkips = cutsceneSkipCount,
                fullLevelRewriteQueued = fullLevelRewriteQueued,
                fullLevelRewriteCompleted = fullLevelRewriteCompleted,
                fullLevelRewriteChanged = fullLevelRewriteChanged,
                levelDetected = levelDetected,
                roomLoadEvents = roomLoadEvents,
                preplacedPathEvents = preplacedPathEvents,
                reactivationEvents = reactivationEvents,
                levelSwaps = levelSwaps,
                totalSwaps = state.swapCount,
                journalCount = #replacementJournal,
            }
        end,
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
                if state.enabled then
                    ensureHooksInstalled()
                end
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

Log(TAG .. ": v12.6 loaded — " .. #ENEMIES .. " enemies, "
    .. #GROUPS .. " groups, "
    .. (state.enabled and "ON" or "OFF")
    .. " hpMode=" .. state.hpMode
    .. " autoDirect=" .. (ENABLE_AUTO_DIRECT_LIST_REWRITES and "ON" or "OFF")
    .. " EmSetFromList2Direct=" .. (ENABLE_EMSETFROMLIST2_DIRECT_REWRITES and "ON" or "OFF")
    .. " readEmList=" .. (hookInstalled and "YES" or "NO")
    .. " EmSetEvent=" .. (emSetEventHooked and "YES" or "NO")
    .. " | room-load rewrite " .. (ENABLE_AUTO_DIRECT_LIST_REWRITES and "ON" or "OFF")
    .. " + exact-emid + EM_LIST cast + NPC skip + replacement journal")
