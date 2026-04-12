-- ============================================================================
-- PFX_CollectionRandomizer v18 — Hub Slot Scramble + Per-Table Cosmetics
-- ============================================================================
-- v18 CHANGES from v17:
--   * FIX: FLIPPER ARMS — Pass entry directly as armSkinData param:
--     SetTableArmSkinOverride(tableID, entry, entry)
--     v17 used {OverrideMaterials=mats} which fails with type 23 (TArray)
--     Bridge verified: passing entry directly works!
--   * FIX: BALL SKINS — Pre-filter pool to only entries with valid ballskin
--     mesh (71/107 valid, 36 have nil soft ref). Skip nil entries.
--   * FIX: HUB SLOTS — After Set+OnSlotEntryChanged, call GetOuter() to get
--     owning actor and call SetupWithCollectibleEntry for visual update.
--     v17 only did data swap (Set+OnSlotEntryChanged), visuals didn't update.
--   * FIX: HOOK — RestoreSlotEntryFromProfile post-hook now also calls
--     SetupWithCollectibleEntry on outer actor for visual persistence.
--   * VERIFIED: SetTableBallMeshOverride works with raw UObject mesh
--   * VERIFIED: SetTableBallTrailOverride works with raw UObject trail
--   * VERIFIED: SetTableArmSkinOverride works passing entry as armSkinData
--   * VERIFIED: GetOuter() on slot component returns owning slot actor
--   * VERIFIED: SetupWithCollectibleEntry works on gadget/poster/statue actors
-- ============================================================================
local TAG = "PFX_Randomizer"

Log(TAG .. ": Loading v18...")

-- ============================================================================
-- CATEGORY CONSTANTS
-- ============================================================================
local CATEGORY_NAMES = {
    [1]  = "Arm",
    [2]  = "SkinTone",
    [3]  = "Poster",
    [4]  = "Statue",
    [5]  = "Gadget",
    [6]  = "Floor",
    [7]  = "Wall",
    [8]  = "Music",
    [9]  = "FlipperArm",
    [10] = "BallSkin",
    [11] = "BallTrail",
    [12] = "Trophy",
    [13] = "Cabinet",
    [14] = "HubInterior",
    [15] = "Door",
}

-- Categories to NEVER scramble in hub slots
local SKIP_CATEGORIES = {
    [0]  = true,  -- Invalid / unknown
    [12] = true,  -- Trophy (handled by MaxAll holo→physical swap)
    [13] = true,  -- Cabinet (table-specific dioramas)
    [14] = true,  -- HubInterior
    [15] = true,  -- Door
}

-- ============================================================================
-- RNG
-- ============================================================================
math.randomseed(os.time())
for i = 1, 10 do math.random() end

local function shuffle(t)
    for i = #t, 2, -1 do
        local j = math.random(i)
        t[i], t[j] = t[j], t[i]
    end
    return t
end

-- ============================================================================
-- UTILITY
-- ============================================================================
local function is_live(obj)
    if not obj then return false end
    local ok, valid = pcall(function() return obj:IsValid() end)
    if not ok or not valid then return false end
    local ok2, name = pcall(function() return obj:GetName() end)
    if not ok2 or not name then return false end
    return not name:match("^Default__") and not name:match("^REINST_") and not name:match("^SKEL_")
end

local function find_live(...)
    for _, cls in ipairs({...}) do
        local all = nil
        pcall(function() all = FindAllOf(cls) end)
        if all then
            for _, obj in ipairs(all) do
                if is_live(obj) then return obj end
            end
        end
    end
    return nil
end

local function get_category_id(entry)
    if not entry then return nil end
    local catID = nil
    pcall(function()
        local catData = entry:Get("CategoryData")
        if catData then catID = catData:Get("CategoryID") end
    end)
    if type(catID) == "number" and catID > 0 then return catID end
    return nil
end

-- ============================================================================
-- ENTRY POOL — ALL live non-CDO entries grouped by category
-- ============================================================================
local ENTRY_CLASSES = {
    "PFXCollectibleEntry_Arm",
    "PFXCollectibleEntry_BallSkin",
    "PFXCollectibleEntry_BallTrail",
    "PFXCollectibleEntry_Door",
    "PFXCollectibleEntry_Floor",
    "PFXCollectibleEntry_FlipperArm",
    "PFXCollectibleEntry_Gadget",
    "PFXCollectibleEntry_HubInterior",
    "PFXCollectibleEntry_Music",
    "PFXCollectibleEntry_Poster",
    "PFXCollectibleEntry_SkinTone",
    "PFXCollectibleEntry_Statue",
    "PFXCollectibleEntry_Trophie",
    "PFXCollectibleEntry_Wall",
}

local entryPool = {}   -- catID -> { entry UObject, ... }
local poolReady = false

-- Pre-filtered pools for table cosmetics (only entries with valid meshes/objects)
local ballPoolValid = {}   -- entries with non-nil ballskin mesh
local trailPoolValid = {}  -- entries with non-nil BallTrail
local flipperPoolAll = {}  -- all flipper entries (pass entry directly)

local stats = {
    hook_ok = 0, hook_skip = 0, hook_err = 0,
    sweep_ok = 0, sweep_skip = 0, sweep_err = 0,
    table_ball = 0, table_trail = 0, table_flipper = 0, table_fail = 0,
    pool_size = 0, sweeps = 0,
    empty_filled = 0,
}

local function build_entry_pool()
    entryPool = {}
    ballPoolValid = {}
    trailPoolValid = {}
    flipperPoolAll = {}
    local total = 0

    for _, cls in ipairs(ENTRY_CLASSES) do
        pcall(function()
            local entries = FindAllOf(cls)
            if entries then
                for _, entry in ipairs(entries) do
                    if is_live(entry) then
                        pcall(function()
                            local catID = get_category_id(entry)
                            if catID and not SKIP_CATEGORIES[catID] then
                                if not entryPool[catID] then entryPool[catID] = {} end
                                entryPool[catID][#entryPool[catID] + 1] = entry
                                total = total + 1
                            end

                            -- Build pre-filtered table cosmetics pools
                            if catID == 10 then  -- BallSkin
                                local mesh = nil
                                pcall(function() mesh = entry:Get("ballskin") end)
                                if mesh then
                                    ballPoolValid[#ballPoolValid + 1] = { entry = entry, mesh = mesh }
                                end
                            elseif catID == 11 then  -- BallTrail
                                local trail = nil
                                pcall(function() trail = entry:Get("BallTrail") end)
                                if trail then
                                    trailPoolValid[#trailPoolValid + 1] = { entry = entry, trail = trail }
                                end
                            elseif catID == 9 then  -- FlipperArm
                                flipperPoolAll[#flipperPoolAll + 1] = entry
                            end
                        end)
                    end
                end
            end
        end)
    end

    poolReady = total > 0
    stats.pool_size = total

    for catID, items in pairs(entryPool) do
        Log(TAG .. ":   " .. (CATEGORY_NAMES[catID] or ("Cat" .. catID))
            .. ": " .. #items .. " entries")
    end
    Log(TAG .. ": Pool: " .. total .. " entries | BallValid=" .. #ballPoolValid
        .. " TrailValid=" .. #trailPoolValid .. " Flipper=" .. #flipperPoolAll)
    return poolReady
end

-- ============================================================================
-- COUNT REGISTERED SLOTS
-- ============================================================================
local function count_registered_slots()
    local cm = find_live("BP_CollectiblesManager_C", "PFXCollectiblesManager")
    if not cm then return 0, nil end
    local regSlots = nil
    pcall(function() regSlots = cm:Get("RegisteredSlots") end)
    if not regSlots then return 0, nil end
    local count = 0
    pcall(function()
        regSlots:ForEach(function(k, v) count = count + 1 end)
    end)
    return count, regSlots
end

-- ============================================================================
-- SWAP SLOT — 3-step: component Set + OnSlotEntryChanged + actor Setup
-- GetOuter() on registered slot component returns owning actor
-- ============================================================================
local function swap_slot_entry(slot, newEntry)
    if not slot or not newEntry then return false end

    -- Step 1: Set data on component
    local ok = pcall(function() slot:Set("m_slotEntry", newEntry) end)

    -- Step 2: Fire change notification on component
    pcall(function() slot:Call("OnSlotEntryChanged", newEntry) end)

    -- Step 3: Visual update on owning ACTOR via GetOuter()
    pcall(function()
        local actor = slot:GetOuter()
        if actor then
            actor:Call("SetupWithCollectibleEntry", newEntry)
        end
    end)

    return ok
end

-- ============================================================================
-- SCRAMBLE HUB SLOTS — assign random entries with visual updates
-- ============================================================================
local function scramble_all_slots()
    if not poolReady then
        Log(TAG .. ": Scramble skipped — pool not ready")
        return 0
    end

    local slotCount, regSlots = count_registered_slots()
    if slotCount == 0 or not regSlots then
        Log(TAG .. ": Scramble skipped — RegisteredSlots empty (" .. slotCount .. ")")
        return 0
    end

    -- Group slots by category, track empty slots separately
    local slotsByCategory = {}
    local emptySlots = {}
    regSlots:ForEach(function(slotKey, slot)
        pcall(function()
            if not is_live(slot) then return end
            local entry = nil
            pcall(function() entry = slot:Get("m_slotEntry") end)
            if not entry or not is_live(entry) then
                emptySlots[#emptySlots + 1] = slot
                return
            end
            local catID = get_category_id(entry)
            if not catID or SKIP_CATEGORIES[catID] then return end
            if not slotsByCategory[catID] then slotsByCategory[catID] = {} end
            local entryName = ""
            pcall(function() entryName = entry:GetName() end)
            slotsByCategory[catID][#slotsByCategory[catID] + 1] = {
                slot = slot,
                currentName = entryName,
            }
        end)
    end)

    local totalOk = 0
    local totalSkip = 0
    local totalErr = 0

    -- For each category: shuffle pool, deal uniquely to slots
    for catID, slotList in pairs(slotsByCategory) do
        local pool = entryPool[catID]
        if not pool or #pool == 0 then
            totalSkip = totalSkip + #slotList
            goto next_cat
        end

        -- Shuffle a copy of the pool
        local shuffled = {}
        for i, e in ipairs(pool) do shuffled[i] = e end
        shuffle(shuffled)

        -- Track used entry names to avoid duplicates
        local used = {}
        local poolIdx = 1

        for _, slotInfo in ipairs(slotList) do
            local picked = nil
            local startIdx = poolIdx

            -- Try each entry in shuffled pool, skip same-as-current and already-used
            repeat
                local candidate = shuffled[poolIdx]
                poolIdx = poolIdx + 1
                if poolIdx > #shuffled then poolIdx = 1 end

                local cName = nil
                pcall(function() cName = candidate:GetName() end)

                if cName and cName ~= slotInfo.currentName and not used[cName] then
                    picked = candidate
                    used[cName] = true
                    break
                end
            until poolIdx == startIdx

            -- Fallback: allow dupes but still avoid same-as-current
            if not picked then
                for _, candidate in ipairs(shuffled) do
                    local cName = nil
                    pcall(function() cName = candidate:GetName() end)
                    if cName and cName ~= slotInfo.currentName then
                        picked = candidate
                        break
                    end
                end
            end

            if not picked then
                totalSkip = totalSkip + 1
                goto next_slot
            end

            -- 3-step swap: data + notify + visual
            if swap_slot_entry(slotInfo.slot, picked) then
                totalOk = totalOk + 1
            else
                totalErr = totalErr + 1
            end

            ::next_slot::
        end

        ::next_cat::
    end

    -- Fill empty slots with category-matched entries
    local SLOT_NAME_TO_CAT = {
        Gadget = 5, Statue = 4, Poster = 3, Music = 8,
        Floor = 6, Wall = 7, Trophy = 12, BallSkin = 10,
        BallTrail = 11, Flipper = 9, Arm = 1, Door = 15,
        Cabinet = 13, HubInterior = 14,
    }
    local emptyFilled = 0
    if #emptySlots > 0 then
        for _, emptySlot in ipairs(emptySlots) do
            pcall(function()
                local slotName = ""
                pcall(function() slotName = emptySlot:Get("m_slotName") end)
                if type(slotName) ~= "string" or slotName == "" then
                    pcall(function() slotName = emptySlot:GetName() end)
                end
                slotName = tostring(slotName)

                local targetCat = nil
                for keyword, catID in pairs(SLOT_NAME_TO_CAT) do
                    if slotName:match(keyword) then
                        targetCat = catID
                        break
                    end
                end

                -- Skip trophy category empty slots (MaxAll handles trophies)
                if targetCat == 12 then return end

                local pool = targetCat and entryPool[targetCat]
                if not pool or #pool == 0 then
                    pool = {}
                    for catID, entries in pairs(entryPool) do
                        if not SKIP_CATEGORIES[catID] then
                            for _, e in ipairs(entries) do pool[#pool + 1] = e end
                        end
                    end
                end
                if #pool == 0 then return end

                local entry = pool[math.random(#pool)]
                -- 3-step swap: data + notify + visual
                if swap_slot_entry(emptySlot, entry) then
                    emptyFilled = emptyFilled + 1
                end
            end)
        end
    end

    stats.sweep_ok = stats.sweep_ok + totalOk
    stats.sweep_skip = stats.sweep_skip + totalSkip
    stats.sweep_err = stats.sweep_err + totalErr
    stats.empty_filled = stats.empty_filled + emptyFilled
    stats.sweeps = stats.sweeps + 1
    Log(TAG .. ": Scramble #" .. stats.sweeps .. ": " .. totalOk .. " swapped, "
        .. totalSkip .. " skip, " .. totalErr .. " err, "
        .. emptyFilled .. "/" .. #emptySlots .. " empty filled")
    return totalOk + emptyFilled
end

-- ============================================================================
-- SCRAMBLE ONE SLOT (for RestoreSlotEntryFromProfile post-hook)
-- ============================================================================
local function scramble_one_slot(slot, source)
    if not poolReady then return false end
    if not is_live(slot) then return false end

    local currentEntry = nil
    pcall(function() currentEntry = slot:Get("m_slotEntry") end)
    if not currentEntry or not is_live(currentEntry) then
        stats[source .. "_skip"] = (stats[source .. "_skip"] or 0) + 1
        return false
    end

    local catID = get_category_id(currentEntry)
    if not catID or SKIP_CATEGORIES[catID] then
        stats[source .. "_skip"] = (stats[source .. "_skip"] or 0) + 1
        return false
    end

    local pool = entryPool[catID]
    if not pool or #pool == 0 then
        stats[source .. "_skip"] = (stats[source .. "_skip"] or 0) + 1
        return false
    end

    local currentName = nil
    pcall(function() currentName = currentEntry:GetName() end)

    local filtered = {}
    for _, entry in ipairs(pool) do
        local eName = nil
        pcall(function() eName = entry:GetName() end)
        if eName and eName ~= currentName then
            filtered[#filtered + 1] = entry
        end
    end
    if #filtered == 0 then
        stats[source .. "_skip"] = (stats[source .. "_skip"] or 0) + 1
        return false
    end

    local newEntry = filtered[math.random(#filtered)]

    -- 3-step swap: data + notify + visual
    if swap_slot_entry(slot, newEntry) then
        stats[source .. "_ok"] = (stats[source .. "_ok"] or 0) + 1
        return true
    else
        stats[source .. "_err"] = (stats[source .. "_err"] or 0) + 1
        return false
    end
end

-- ============================================================================
-- HOOK: RestoreSlotEntryFromProfile → scramble after each restore
-- ============================================================================
local hookRegistered = false

pcall(function()
    RegisterPostHook(
        "/Script/PFXVRQuest.PFXCollectibleSlotComponent:RestoreSlotEntryFromProfile",
        function(self, funcPtr, parms)
            pcall(function() scramble_one_slot(self, "hook") end)
        end
    )
    hookRegistered = true
    Log(TAG .. ": Hook: RestoreSlotEntryFromProfile → post-hook scramble + visual")
end)

-- ============================================================================
-- PER-TABLE COSMETICS — Ball + Trail + Flippers
-- ============================================================================
-- v18 Fixes:
--   Ball:    Use pre-filtered ballPoolValid (skip nil ballskin entries)
--   Trail:   Use pre-filtered trailPoolValid (skip nil BallTrail entries)
--   Flipper: Pass entry object directly as armSkinData param (bridge verified!)
--            v17 used {OverrideMaterials=mats} which failed with type 23 TArray
-- ============================================================================
local function scramble_table_cosmetics()
    local tcm = find_live("BP_TableCustomizationManager_C", "PFXTableCustomizationManager")
    if not tcm then
        Log(TAG .. ": Table cosmetics skipped — no TableCustomizationManager")
        return 0
    end

    if not poolReady then
        Log(TAG .. ": Table cosmetics skipped — pool not ready")
        return 0
    end

    if #ballPoolValid == 0 and #trailPoolValid == 0 and #flipperPoolAll == 0 then
        Log(TAG .. ": Table cosmetics skipped — no valid ball/trail/flipper entries")
        return 0
    end

    -- Get table IDs from BundleNameMap
    local cm = find_live("BP_CollectiblesManager_C", "PFXCollectiblesManager")
    if not cm then
        Log(TAG .. ": Table cosmetics skipped — no CollectiblesManager")
        return 0
    end

    local bnm = nil
    pcall(function() bnm = cm:Get("BundleNameMap") end)
    if not bnm then
        Log(TAG .. ": Table cosmetics skipped — no BundleNameMap")
        return 0
    end

    local bnmCount = 0
    pcall(function()
        bnm:ForEach(function(k, v) bnmCount = bnmCount + 1 end)
    end)
    if bnmCount == 0 then
        Log(TAG .. ": Table cosmetics skipped — BundleNameMap empty")
        return 0
    end

    local ballOk, trailOk, flipperOk, failed = 0, 0, 0, 0
    local tableCount = 0

    bnm:ForEach(function(k, bundle)
        local key = tostring(k)
        local num = key:match("^(%d+)_")
        if not num then return end
        local tableID = tonumber(num)
        tableCount = tableCount + 1

        -- Apply random BallSkin (pre-filtered: all have valid ballskin mesh)
        if #ballPoolValid > 0 then
            pcall(function()
                local pick = ballPoolValid[math.random(#ballPoolValid)]
                local ok = pcall(function()
                    tcm:Call("SetTableBallMeshOverride", tableID, pick.mesh, pick.entry)
                end)
                if ok then ballOk = ballOk + 1 else failed = failed + 1 end
            end)
        end

        -- Apply random BallTrail (pre-filtered: all have valid BallTrail object)
        if #trailPoolValid > 0 then
            pcall(function()
                local pick = trailPoolValid[math.random(#trailPoolValid)]
                local ok = pcall(function()
                    tcm:Call("SetTableBallTrailOverride", tableID, pick.trail, pick.entry)
                end)
                if ok then trailOk = trailOk + 1 else failed = failed + 1 end
            end)
        end

        -- Apply random FlipperArm — pass entry DIRECTLY as armSkinData param
        -- Bridge verified: SetTableArmSkinOverride(tableID, entry, entry) works!
        -- v17's {OverrideMaterials=mats} failed because TArray (type 23) can't serialize
        if #flipperPoolAll > 0 then
            pcall(function()
                local entry = flipperPoolAll[math.random(#flipperPoolAll)]
                local ok = pcall(function()
                    tcm:Call("SetTableArmSkinOverride", tableID, entry, entry)
                end)
                if ok then flipperOk = flipperOk + 1 else failed = failed + 1 end
            end)
        end
    end)

    stats.table_ball = ballOk
    stats.table_trail = trailOk
    stats.table_flipper = flipperOk
    stats.table_fail = failed
    Log(TAG .. ": Table cosmetics: " .. tableCount .. " tables"
        .. " | ball=" .. ballOk .. "/" .. #ballPoolValid
        .. " trail=" .. trailOk .. "/" .. #trailPoolValid
        .. " flipper=" .. flipperOk .. "/" .. #flipperPoolAll
        .. (failed > 0 and (" FAIL=" .. failed) or ""))
    return ballOk + trailOk + flipperOk
end

-- ============================================================================
-- INIT: Poll every 5s until RegisteredSlots >= 100
-- ============================================================================
local MAX_POLLS = 30
local initDone = false
local pollCount = 0

LoopAsync(5000, function()
    pollCount = pollCount + 1

    if initDone then return true end

    if pollCount > MAX_POLLS then
        Log(TAG .. ": GAVE UP after " .. MAX_POLLS .. " polls")
        return true
    end

    local slotCount = count_registered_slots()
    if slotCount < 100 then
        Log(TAG .. ": Poll #" .. pollCount .. " — slots " .. slotCount .. "/100")
        return false
    end

    -- Build pool (all entries should exist by now)
    pcall(build_entry_pool)
    if not poolReady then
        Log(TAG .. ": Poll #" .. pollCount .. " — slots=" .. slotCount
            .. " but pool empty!")
        return false
    end

    Log(TAG .. ": Poll #" .. pollCount .. " — READY! Pool=" .. stats.pool_size
        .. " entries, " .. slotCount .. " slots"
        .. " | BallValid=" .. #ballPoolValid
        .. " TrailValid=" .. #trailPoolValid
        .. " Flipper=" .. #flipperPoolAll)

    -- Run table cosmetics first (per-table overrides)
    pcall(scramble_table_cosmetics)

    -- Run hub slot scramble (with visual updates)
    pcall(scramble_all_slots)

    initDone = true
    Log(TAG .. ": === Init complete ===")
    return true
end)

-- ============================================================================
-- BRIDGE COMMANDS
-- ============================================================================
pcall(function()
    RegisterCommand("randomize", function()
        if not poolReady then pcall(build_entry_pool) end
        pcall(scramble_all_slots)
        pcall(scramble_table_cosmetics)
        return TAG .. " v18: sweep #" .. stats.sweeps
            .. " ok=" .. stats.sweep_ok
            .. " empty=" .. stats.empty_filled
            .. " ball=" .. stats.table_ball
            .. " trail=" .. stats.table_trail
            .. " flipper=" .. stats.table_flipper
    end)
end)

pcall(function()
    RegisterCommand("randomize_status", function()
        local slotCount = count_registered_slots()
        local msg = TAG .. " v18: pool=" .. stats.pool_size
            .. " slots=" .. slotCount
            .. " hook=" .. tostring(hookRegistered)
            .. " polls=" .. pollCount .. "/" .. MAX_POLLS
            .. " done=" .. tostring(initDone)
            .. "\nhook: ok=" .. stats.hook_ok
            .. " skip=" .. stats.hook_skip
            .. " err=" .. stats.hook_err
            .. "\nsweep: ok=" .. stats.sweep_ok
            .. " skip=" .. stats.sweep_skip
            .. " err=" .. stats.sweep_err
            .. " empty=" .. stats.empty_filled
            .. " (#" .. stats.sweeps .. ")"
            .. "\ntable: ball=" .. stats.table_ball
            .. " trail=" .. stats.table_trail
            .. " flipper=" .. stats.table_flipper
            .. " fail=" .. stats.table_fail
            .. "\npools: ballValid=" .. #ballPoolValid
            .. " trailValid=" .. #trailPoolValid
            .. " flipper=" .. #flipperPoolAll .. "\n"
        for catID, items in pairs(entryPool) do
            msg = msg .. "  " .. (CATEGORY_NAMES[catID] or ("Cat" .. catID))
                .. ": " .. #items .. "\n"
        end
        return msg
    end)
end)

pcall(function()
    RegisterCommand("randomize_rebuild", function()
        pcall(build_entry_pool)
        return TAG .. ": Pool rebuilt — " .. stats.pool_size .. " entries"
            .. " ballValid=" .. #ballPoolValid
            .. " trailValid=" .. #trailPoolValid
            .. " flipper=" .. #flipperPoolAll
    end)
end)

pcall(function()
    RegisterCommand("randomize_tables", function()
        local count = scramble_table_cosmetics()
        return TAG .. ": Re-randomized tables — ball=" .. stats.table_ball
            .. " trail=" .. stats.table_trail
            .. " flipper=" .. stats.table_flipper
            .. " fail=" .. stats.table_fail
    end)
end)

Log(TAG .. ": v18 loaded — hook=" .. tostring(hookRegistered)
    .. " | Fixes: flipper entry-direct, ball nil-filter, hub SetupWithCollectibleEntry")
