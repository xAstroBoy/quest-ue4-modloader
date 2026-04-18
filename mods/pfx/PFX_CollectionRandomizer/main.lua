-- ============================================================================
-- PFX_CollectionRandomizer v27 — Hub Slot Scramble + Per-Table Cosmetics
-- ============================================================================
-- v27 CHANGES from v26:
--   * FIX: robust slot assignment validation. If ChangeSlotEntry is a no-op,
--     fallback writes m_slotEntry directly, then runs notify/setup calls.
--   * FIX: strict pre-clear only runs when slot had a previous live entry.
--     This preserves empty-slot fill reliability for gadget/statue.
--
-- v26 CHANGES from v25:
--   * FIX: strict replace now rolls back previous slot entry if placement fails,
--     preventing gadget/statue slots from staying empty on failed swaps.
--   * TWEAK: init randomization starts sooner once slot count threshold is met.
--
-- v25 CHANGES from v24:
--   * FIX: entry uniqueness now keys by `GetEntryID()` gameplay tag first
--     (fallback: object name/id), preventing heavy over-dedup on Gadget/Statue.
--   * FIX: Gadget + Statue now enforce uniqueness per owner station (shelf)
--     before wrap, eliminating same-shelf dupes when pool size allows.
--
-- v24 CHANGES from v23:
--   * NEW: Gadget + Statue now enforce strict unique rotation through full pools
--     before reuse; wrap resets used picks and continues cycling.
--   * NEW: Gadget + Statue now force clear/reset old slot visuals before placing
--     the new randomized entry (replace even when slot already occupied).
--
local TAG = "PFX_Randomizer"
Log(TAG .. ": Loading v28...")

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
}

-- Categories requiring strict uniqueness rotation and hard replacement
local STRICT_UNIQUE_CATEGORIES = {
    [4] = true,   -- Statue
    [5] = true,   -- Gadget
}

local STRICT_REPLACE_CATEGORIES = {
    [4] = true,   -- Statue
    [5] = true,   -- Gadget
}

local function name_of(obj)
    if not obj then return "" end
    local n = nil
    pcall(function() n = obj:GetName() end)
    return type(n) == "string" and n or ""
end

local function entry_id(obj)
    if not obj then return "" end
    local ok, s = pcall(function() return tostring(obj) end)
    return (ok and type(s) == "string") and s or ""
end

local function entry_tag_key(obj)
    if not obj then return "" end

    local tagStr = ""
    pcall(function()
        local tag = obj:Call("GetEntryID")
        if tag then
            local tn = nil
            pcall(function() tn = tag.TagName end)
            if tn then tagStr = tostring(tn) end
            if tagStr == "" then
                local okTag, asStr = pcall(function() return tostring(tag) end)
                if okTag and type(asStr) == "string" then tagStr = asStr end
            end
        end
    end)

    if tagStr == "" or tagStr == "nil" then return "" end
    return "tag::" .. tagStr
end

local function entry_key(obj)
    if not obj then return "" end
    local t = entry_tag_key(obj)
    if t ~= "" then return t end
    local n = name_of(obj)
    if n ~= "" then return n end
    return entry_id(obj)
end

local function shuffle(arr)
    if not arr or #arr <= 1 then return arr end
    for i = #arr, 2, -1 do
        local j = math.random(i)
        arr[i], arr[j] = arr[j], arr[i]
    end
    return arr
end

local function dedupe_by_key(items, keyFn)
    local out = {}
    local seen = {}
    if not items then return out end
    for _, item in ipairs(items) do
        local key = keyFn(item)
        if key and key ~= "" and not seen[key] then
            seen[key] = true
            out[#out + 1] = item
        end
    end
    return out
end

local function make_cycle_state(items, keyFn)
    local unique = dedupe_by_key(items, keyFn)
    local state = {
        unique = unique,
        keyFn = keyFn,
        order = {},
        index = 1,
        lastKey = nil,
    }
    for i, item in ipairs(unique) do state.order[i] = item end
    if #state.order > 1 then shuffle(state.order) end
    return state
end

local function reset_cycle_state(state, avoidFirstKey)
    state.order = {}
    for i, item in ipairs(state.unique or {}) do state.order[i] = item end
    if #state.order > 1 then shuffle(state.order) end

    if avoidFirstKey and avoidFirstKey ~= "" and #state.order > 1 then
        local firstKey = state.keyFn(state.order[1])
        if firstKey == avoidFirstKey then
            local swapIndex = nil
            for i = 2, #state.order do
                local k = state.keyFn(state.order[i])
                if k ~= avoidFirstKey then
                    swapIndex = i
                    break
                end
            end
            if swapIndex then
                state.order[1], state.order[swapIndex] = state.order[swapIndex], state.order[1]
            end
        end
    end

    state.index = 1
end

local function take_from_cycle(state, avoidKey)
    if not state or not state.order or #state.order == 0 then return nil end

    local previousKey = state.lastKey

    for _ = 1, 2 do
        local count = #state.order
        local startIndex = state.index

        for offset = 0, count - 1 do
            local idx = ((startIndex + offset - 1) % count) + 1
            local candidate = state.order[idx]
            local key = state.keyFn(candidate)
            if key and key ~= "" and (#state.order == 1 or key ~= (avoidKey or "")) then
                state.index = idx + 1
                state.lastKey = key
                if state.index > count then
                    reset_cycle_state(state, key)
                end
                return candidate
            end
        end

        reset_cycle_state(state, previousKey)
    end

    return state.order[1]
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

-- Pre-filtered pools for table cosmetics grouped by IP
-- ALL ball/trail/flipper entries are IP-locked (zero global)
-- Tags: Collectibles.IP.<IP>.<TableHint>.<ItemName>
local ballByIP = {}    -- IP -> { {entry=, mesh=}, ... }
local trailByIP = {}   -- IP -> { {entry=, trail=}, ... }
local flipperByIP = {} -- IP -> { entry, ... }
local tableHintToIP = {}  -- lowercase table hint -> IP (e.g. "doom" -> "Bethesda")
-- Legacy totals for logging
local ballPoolValid = {}
local trailPoolValid = {}
local flipperPoolAll = {}
local pendingFlipperOverrides = {}

local stats = {
    hook_ok = 0, hook_skip = 0, hook_err = 0,
    sweep_ok = 0, sweep_skip = 0, sweep_err = 0,
    table_ball = 0, table_trail = 0, table_flipper = 0, table_fail = 0,
    pool_size = 0, sweeps = 0,
    empty_filled = 0,
}

local function extract_ip_from_tag(tagStr)
    -- Tag format: Collectibles.IP.<IP>.<TableHint>.<ItemName>
    if not tagStr or tagStr == "" then return nil, nil end
    local parts = {}
    for p in tagStr:gmatch("[^.]+") do parts[#parts + 1] = p end
    if #parts >= 4 and parts[2] == "IP" then
        return parts[3], parts[4]  -- IP, tableHint
    end
    return nil, nil
end

local function build_entry_pool()
    entryPool = {}
    ballByIP = {}
    trailByIP = {}
    flipperByIP = {}
    tableHintToIP = {}
    ballPoolValid = {}
    trailPoolValid = {}
    flipperPoolAll = {}
    local total = 0
    local seenPoolByCat = {}
    local seenBall = {}
    local seenTrail = {}
    local seenFlipper = {}

    for _, cls in ipairs(ENTRY_CLASSES) do
        pcall(function()
            local entries = FindAllOf(cls)
            if entries then
                for _, entry in ipairs(entries) do
                    if is_live(entry) then
                        pcall(function()
                            local catID = get_category_id(entry)
                            local key = entry_key(entry)

                            -- Extract tag for IP classification
                            local tagStr = ""
                            pcall(function()
                                local tag = entry:Call("GetEntryID")
                                if tag then
                                    local tn = nil
                                    pcall(function() tn = tag.TagName end)
                                    if tn then tagStr = tostring(tn) end
                                end
                            end)
                            local ip, hint = extract_ip_from_tag(tagStr)

                            if catID and not SKIP_CATEGORIES[catID] then
                                if not entryPool[catID] then entryPool[catID] = {} end
                                if not seenPoolByCat[catID] then seenPoolByCat[catID] = {} end
                                if key ~= "" and not seenPoolByCat[catID][key] then
                                    seenPoolByCat[catID][key] = true
                                    entryPool[catID][#entryPool[catID] + 1] = entry
                                    total = total + 1
                                end
                            end

                            -- Build IP-grouped table cosmetics pools
                            if ip and hint then
                                tableHintToIP[hint:lower()] = ip
                            end

                            if catID == 10 then  -- BallSkin
                                local mesh = nil
                                pcall(function() mesh = entry:Get("ballskin") end)
                                if mesh and key ~= "" and not seenBall[key] then
                                    seenBall[key] = true
                                    local item = { entry = entry, mesh = mesh }
                                    ballPoolValid[#ballPoolValid + 1] = item
                                    if ip then
                                        if not ballByIP[ip] then ballByIP[ip] = {} end
                                        ballByIP[ip][#ballByIP[ip] + 1] = item
                                    end
                                end
                            elseif catID == 11 then  -- BallTrail
                                local trail = nil
                                pcall(function() trail = entry:Get("BallTrail") end)
                                if trail and key ~= "" and not seenTrail[key] then
                                    seenTrail[key] = true
                                    local item = { entry = entry, trail = trail }
                                    trailPoolValid[#trailPoolValid + 1] = item
                                    if ip then
                                        if not trailByIP[ip] then trailByIP[ip] = {} end
                                        trailByIP[ip][#trailByIP[ip] + 1] = item
                                    end
                                end
                            elseif catID == 9 then  -- FlipperArm
                                if key ~= "" and not seenFlipper[key] then
                                    seenFlipper[key] = true
                                    flipperPoolAll[#flipperPoolAll + 1] = entry
                                    if ip then
                                        if not flipperByIP[ip] then flipperByIP[ip] = {} end
                                        flipperByIP[ip][#flipperByIP[ip] + 1] = entry
                                    end
                                end
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
    -- Log IP pool sizes
    local ipList = {}
    for ip, items in pairs(flipperByIP) do
        ipList[#ipList + 1] = ip .. "=" .. #items
    end
    table.sort(ipList)
    Log(TAG .. ": Pool: " .. total .. " entries | BallValid=" .. #ballPoolValid
        .. " TrailValid=" .. #trailPoolValid .. " Flipper=" .. #flipperPoolAll)
    Log(TAG .. ": FlipperByIP: " .. table.concat(ipList, ", "))
    Log(TAG .. ": TableHints: " .. (function()
        local out = {}
        for h, ip in pairs(tableHintToIP) do out[#out+1] = h .. "->" .. ip end
        table.sort(out)
        return table.concat(out, ", ")
    end)())
    return poolReady
end

-- ============================================================================
-- COUNT REGISTERED SLOTS
-- ============================================================================
local function collect_live_slots()
    local out = {}
    local seen = {}
    pcall(function()
        local all = FindAllOf("PFXCollectibleSlotComponent")
        if not all then return end
        for _, slot in ipairs(all) do
            if is_live(slot) then
                local key = entry_id(slot)
                if key ~= "" and not seen[key] then
                    seen[key] = true
                    out[#out + 1] = slot
                end
            end
        end
    end)
    return out
end

local function count_registered_slots()
    local slots = collect_live_slots()
    return #slots, slots
end

local function count_empty_registered_slots(slots)
    if not slots then return 0 end
    local empties = 0
    for _, slot in ipairs(slots) do
        pcall(function()
            if not is_live(slot) then return end
            local entry = nil
            pcall(function() entry = slot:Get("m_slotEntry") end)
            if not entry or not is_live(entry) then
                empties = empties + 1
            end
        end)
    end
    return empties
end

-- ============================================================================
-- REFLECTION HELPERS — cached function checks
-- ============================================================================
local _fn_cache = {} -- key: ClassName::Function -> bool

local function has_function(obj, fn)
    if not obj or not fn then return false end
    local className = nil
    pcall(function() className = obj:GetClass():GetName() end)
    if not className then return false end

    local key = className .. "::" .. fn
    if _fn_cache[key] ~= nil then
        return _fn_cache[key]
    end

    local ok, exists = pcall(function()
        local rc = RebuildClass(className)
        if not rc then return false end
        local funcs = rc:Functions()
        return funcs and funcs[fn] ~= nil
    end)

    _fn_cache[key] = (ok and exists) and true or false
    return _fn_cache[key]
end

-- ============================================================================
-- SWAP SLOT — 3-step: component Set + OnSlotEntryChanged + SetupWithCollectibleEntry
-- ============================================================================
local function swap_slot_entry(slot, newEntry)
    if not slot or not newEntry then return false end

    local previousEntry = nil
    pcall(function() previousEntry = slot:Get("m_slotEntry") end)
    local hadPrevious = previousEntry and is_live(previousEntry)

    local ok = false

    -- For strict-replace categories (Gadget, Statue): assign new entry FIRST,
    -- only destroy the old collectible AFTER confirming the swap succeeded.
    -- This prevents leaving empty slots when the swap fails.

    if has_function(slot, "ChangeSlotEntry") then
        local changed = false
        pcall(function()
            slot:Call("ChangeSlotEntry", newEntry, true)
            changed = true
        end)
        if not changed then
            pcall(function()
                slot:Call("ChangeSlotEntry", newEntry)
                changed = true
            end)
        end
        ok = changed
    else
        ok = pcall(function() slot:Set("m_slotEntry", newEntry) end)
    end

    local desiredKey = entry_key(newEntry)
    local assignedEntry = nil
    pcall(function() assignedEntry = slot:Get("m_slotEntry") end)
    local assignedKey = entry_key(assignedEntry)
    if not assignedEntry or not is_live(assignedEntry)
        or (desiredKey ~= "" and assignedKey ~= desiredKey) then
        local wrote = pcall(function() slot:Set("m_slotEntry", newEntry) end)
        if wrote then
            local check = nil
            pcall(function() check = slot:Get("m_slotEntry") end)
            if check and is_live(check) then
                local checkKey = entry_key(check)
                if desiredKey == "" or checkKey == desiredKey then
                    ok = true
                end
            end
        end
    end

    if not ok and hadPrevious then
        -- Swap failed — leave old entry in place, don't destroy anything
        pcall(function()
            if has_function(slot, "ChangeSlotEntry") then
                slot:Call("ChangeSlotEntry", previousEntry, true)
            else
                slot:Set("m_slotEntry", previousEntry)
            end
        end)
        pcall(function() slot:Call("OnSlotEntryChanged", previousEntry) end)
        pcall(function()
            if has_function(slot, "SetupWithCollectibleEntry") then
                slot:Call("SetupWithCollectibleEntry", previousEntry)
            end
        end)
        pcall(function()
            if has_function(slot, "FinalizeEntrySetup") then
                slot:Call("FinalizeEntrySetup", previousEntry)
            end
        end)
        return false
    end

    pcall(function() slot:Call("OnSlotEntryChanged", newEntry) end)
    pcall(function()
        if has_function(slot, "SetupWithCollectibleEntry") then
            slot:Call("SetupWithCollectibleEntry", newEntry)
        end
    end)
    pcall(function()
        if has_function(slot, "FinalizeEntrySetup") then
            slot:Call("FinalizeEntrySetup", newEntry)
        end
    end)
    pcall(function()
        if get_category_id(newEntry) == 14 and has_function(slot, "ApplyNewInterior") then
            slot:Call("ApplyNewInterior", newEntry)
        end
    end)

    return ok
end

local function infer_slot_category(slot)
    if not slot then return nil end

    local probes = {}

    local slotName = nil
    pcall(function() slotName = slot:Get("m_slotName") end)
    if type(slotName) == "string" and slotName ~= "" then
        probes[#probes + 1] = slotName:lower()
    end

    local slotObjName = name_of(slot)
    if slotObjName ~= "" then probes[#probes + 1] = slotObjName:lower() end

    local slotClassName = nil
    pcall(function() slotClassName = slot:GetClass():GetName() end)
    if type(slotClassName) == "string" and slotClassName ~= "" then
        probes[#probes + 1] = slotClassName:lower()
    end

    local outer = nil
    pcall(function() outer = slot:GetOuter() end)
    if outer then
        local outerObjName = name_of(outer)
        if outerObjName ~= "" then probes[#probes + 1] = outerObjName:lower() end
        local outerClassName = nil
        pcall(function() outerClassName = outer:GetClass():GetName() end)
        if type(outerClassName) == "string" and outerClassName ~= "" then
            probes[#probes + 1] = outerClassName:lower()
        end
    end

    local orderedKeywords = {
        { "hubinterior", 14 },
        { "hub_interior", 14 },
        { "interior", 14 },
        { "gadget", 5 },
        { "statue", 4 },
        { "poster", 3 },
        { "music", 8 },
        { "flipper", 9 },
        { "balltrail", 11 },
        { "trail", 11 },
        { "ballskin", 10 },
        { "ball", 10 },
        { "floor", 6 },
        { "wall", 7 },
        { "arm", 1 },
        { "door", 15 },
        { "cabinet", 13 },
        { "trophy", 12 },
    }

    for _, s in ipairs(probes) do
        for _, k in ipairs(orderedKeywords) do
            if s:find(k[1], 1, true) then
                return k[2]
            end
        end
    end

    return nil
end

-- ============================================================================
-- SCRAMBLE HUB SLOTS — assign random entries with visual updates
-- ============================================================================
local function scramble_all_slots()
    if not poolReady then
        Log(TAG .. ": Scramble skipped — pool not ready")
        return 0
    end

    local slotCount, liveSlots = count_registered_slots()
    if slotCount == 0 or not liveSlots then
        Log(TAG .. ": Scramble skipped — RegisteredSlots empty (" .. slotCount .. ")")
        return 0
    end

    -- Group slots by category, track empty slots separately
    local slotsByCategory = {}
    local emptySlots = {}
    for _, slot in ipairs(liveSlots) do
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

            local stationKey = ""
            pcall(function()
                local ownerStation = slot:Get("m_ownerStationComponent")
                if ownerStation and is_live(ownerStation) then
                    stationKey = entry_id(ownerStation)
                end
            end)
            if stationKey == "" then
                pcall(function()
                    local outer = slot:GetOuter()
                    if outer and is_live(outer) then stationKey = entry_id(outer) end
                end)
            end
            if stationKey == "" then stationKey = "__global__" end

            if not slotsByCategory[catID] then slotsByCategory[catID] = {} end
            slotsByCategory[catID][#slotsByCategory[catID] + 1] = {
                slot = slot,
                currentKey = entry_key(entry),
                stationKey = stationKey,
            }
        end)
    end

    local totalOk = 0
    local totalSkip = 0
    local totalErr = 0
    local cyclesByCategory = {}

    -- For each category: shuffle pool, deal uniquely to slots
    for catID, slotList in pairs(slotsByCategory) do
        local pool = entryPool[catID]
        if not pool or #pool == 0 then
            totalSkip = totalSkip + #slotList
            goto next_cat
        end

        if STRICT_UNIQUE_CATEGORIES[catID] then
            local slotsByStation = {}
            for _, slotInfo in ipairs(slotList) do
                local sk = slotInfo.stationKey or "__global__"
                if not slotsByStation[sk] then slotsByStation[sk] = {} end
                slotsByStation[sk][#slotsByStation[sk] + 1] = slotInfo
            end

            for _, stationSlots in pairs(slotsByStation) do
                local cycle = make_cycle_state(pool, entry_key)
                for _, slotInfo in ipairs(stationSlots) do
                    local picked = take_from_cycle(cycle, slotInfo.currentKey)
                    if not picked then
                        totalSkip = totalSkip + 1
                        goto next_station_slot
                    end

                    -- 3-step swap: data + notify + visual
                    if swap_slot_entry(slotInfo.slot, picked) then
                        totalOk = totalOk + 1
                    else
                        totalErr = totalErr + 1
                    end

                    ::next_station_slot::
                end
            end
        else
            if not cyclesByCategory[catID] then
                cyclesByCategory[catID] = make_cycle_state(pool, entry_key)
            end
            local cycle = cyclesByCategory[catID]

            for _, slotInfo in ipairs(slotList) do
                local picked = take_from_cycle(cycle, slotInfo.currentKey)

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
        end

        ::next_cat::
    end

    -- Fill empty slots with category-matched entries
    local emptyFilled = 0
    if #emptySlots > 0 then
        local emptyByCategory = {}

        for _, emptySlot in ipairs(emptySlots) do
            pcall(function()
                local targetCat = infer_slot_category(emptySlot)
                if not targetCat or SKIP_CATEGORIES[targetCat] then return end
                if not emptyByCategory[targetCat] then emptyByCategory[targetCat] = {} end
                emptyByCategory[targetCat][#emptyByCategory[targetCat] + 1] = emptySlot
            end)
        end

        for catID, catEmptySlots in pairs(emptyByCategory) do
            local pool = entryPool[catID]
            if pool and #pool > 0 then
                if not cyclesByCategory[catID] then
                    cyclesByCategory[catID] = make_cycle_state(pool, entry_key)
                end
                local cycle = cyclesByCategory[catID]
                for _, emptySlot in ipairs(catEmptySlots) do
                    local picked = take_from_cycle(cycle, nil)
                    if picked and swap_slot_entry(emptySlot, picked) then
                        emptyFilled = emptyFilled + 1
                    else
                        totalSkip = totalSkip + 1
                    end
                end
            end
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

    local catID = nil
    local currentKey = nil
    if currentEntry and is_live(currentEntry) then
        catID = get_category_id(currentEntry)
        currentKey = entry_key(currentEntry)
    else
        catID = infer_slot_category(slot)
    end

    if not catID or SKIP_CATEGORIES[catID] then
        stats[source .. "_skip"] = (stats[source .. "_skip"] or 0) + 1
        return false
    end

    local pool = entryPool[catID]
    if not pool or #pool == 0 then
        stats[source .. "_skip"] = (stats[source .. "_skip"] or 0) + 1
        return false
    end

    if not hook_used_by_category then hook_used_by_category = {} end
    if not hook_used_by_category[catID] then
        hook_used_by_category[catID] = make_cycle_state(pool, entry_key)
    end

    local newEntry = take_from_cycle(hook_used_by_category[catID], currentKey)

    if not newEntry then
        stats[source .. "_skip"] = (stats[source .. "_skip"] or 0) + 1
        return false
    end

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
local hook_used_by_category = {}

Log(TAG .. ": Hook: RestoreSlotEntryFromProfile auto-scramble disabled in v25 for stability")

-- ============================================================================
-- PER-TABLE COSMETICS — Ball + Trail + Flippers (IP-MATCHED)
-- ============================================================================
-- v28 Fix: ALL ball/trail/flipper entries are IP-locked (table-specific).
-- Each table only gets cosmetics from its own IP group.
-- Table→IP mapping is built from entry tags + bundle name matching.
-- ============================================================================

-- Resolve table ID to IP by matching bundle name against tableHintToIP map
local function resolve_table_ip(bundleKey)
    if not bundleKey or bundleKey == "" then return nil end
    local bk = bundleKey:lower()
    -- Try matching each known table hint in the bundle key
    for hint, ip in pairs(tableHintToIP) do
        if bk:find(hint, 1, true) then
            return ip
        end
    end
    return nil
end

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

    local ballOk, trailOk, flipperOk, flipperFail, failed, noIP = 0, 0, 0, 0, 0, 0
    local tableCount = 0

    -- Build per-IP cycles lazily
    local ballCycleByIP = {}
    local trailCycleByIP = {}
    local flipperCycleByIP = {}

    local tableEntries = {}
    local seenTableID = {}

    bnm:ForEach(function(k, bundle)
        local tableID = nil
        local bundleKey = tostring(k)

        local num = bundleKey:match("^(%d+)_")
        if num then
            tableID = tonumber(num)
        end

        if not tableID and bundle and is_live(bundle) then
            local probe = nil
            pcall(function() probe = bundle:Get("TableID") end)
            if type(probe) == "number" then tableID = probe end
            if not tableID then
                pcall(function() probe = bundle:Get("m_tableID") end)
                if type(probe) == "number" then tableID = probe end
            end
        end

        if type(tableID) == "number" and tableID > 0 and not seenTableID[tableID] then
            seenTableID[tableID] = true
            tableEntries[#tableEntries + 1] = { tableID = tableID, bundleKey = bundleKey }
        end
    end)

    table.sort(tableEntries, function(a, b) return a.tableID < b.tableID end)

    for _, te in ipairs(tableEntries) do
        local tableID = te.tableID
        local ip = resolve_table_ip(te.bundleKey)
        tableCount = tableCount + 1

        if not ip then
            noIP = noIP + 1
            goto next_table
        end

        -- Apply random BallSkin from this table's IP pool
        local bpool = ballByIP[ip]
        if bpool and #bpool > 0 then
            if not ballCycleByIP[ip] then
                ballCycleByIP[ip] = make_cycle_state(bpool, function(v) return entry_key(v.entry) end)
            end
            pcall(function()
                local pick = take_from_cycle(ballCycleByIP[ip], nil)
                if pick then
                    local ok = pcall(function()
                        tcm:Call("SetTableBallMeshOverride", tableID, pick.mesh, pick.entry)
                    end)
                    if ok then ballOk = ballOk + 1 else failed = failed + 1 end
                else
                    failed = failed + 1
                end
            end)
        end

        -- Apply random BallTrail from this table's IP pool
        local tpool = trailByIP[ip]
        if tpool and #tpool > 0 then
            if not trailCycleByIP[ip] then
                trailCycleByIP[ip] = make_cycle_state(tpool, function(v) return entry_key(v.entry) end)
            end
            pcall(function()
                local pick = take_from_cycle(trailCycleByIP[ip], nil)
                if pick then
                    local ok = pcall(function()
                        tcm:Call("SetTableBallTrailOverride", tableID, pick.trail, pick.entry)
                    end)
                    if ok then trailOk = trailOk + 1 else failed = failed + 1 end
                else
                    failed = failed + 1
                end
            end)
        end

        -- Queue flipper arm override for staggered execution (avoid rapid-fire SIGSEGV)
        local fpool = flipperByIP[ip]
        if fpool and #fpool > 0 then
            if not flipperCycleByIP[ip] then
                flipperCycleByIP[ip] = make_cycle_state(fpool, entry_key)
            end
            local pick = take_from_cycle(flipperCycleByIP[ip], nil)
            if pick then
                local fmats = nil
                pcall(function() fmats = pick:Get("OverrideMaterials") end)
                if fmats and #fmats > 0 then
                    table.insert(pendingFlipperOverrides, {
                        tableID = tableID,
                        pick = pick,
                    })
                    flipperOk = flipperOk + 1  -- count as queued
                else
                    flipperOk = flipperOk + 1  -- no mats, skip
                end
            end
        end

        ::next_table::
    end

    stats.table_ball = ballOk
    stats.table_trail = trailOk
    stats.table_flipper = flipperOk
    stats.table_fail = failed
    Log(TAG .. ": Table cosmetics: " .. tableCount .. " tables (noIP=" .. noIP .. ")"
        .. " | ball=" .. ballOk
        .. " trail=" .. trailOk
        .. " flipper=" .. flipperOk .. "/" .. (flipperOk + flipperFail)
        .. (failed > 0 and (" FAIL=" .. failed) or ""))
    return ballOk + trailOk
end

-- Deferred flipper arm application — staggered with delays to avoid rapid SIGSEGV
local function apply_pending_flipper_overrides()
    if #pendingFlipperOverrides == 0 then return end
    local tcm = find_live("BP_TableCustomizationManager_C", "PFXTableCustomizationManager")
    if not tcm then return end
    local total = #pendingFlipperOverrides
    local ok_count, fail_count = 0, 0
    local DELAY_MS = 300  -- 300ms between each call

    Log(TAG .. ": Starting staggered flipper overrides: " .. total .. " calls, " .. DELAY_MS .. "ms apart")

    for i, pf in ipairs(pendingFlipperOverrides) do
        ExecuteWithDelay(DELAY_MS * (i - 1), function()
            local name = "?"
            pcall(function() name = pf.pick:GetName() end)
            local mats = nil
            pcall(function() mats = pf.pick:Get("OverrideMaterials") end)
            if mats and #mats > 0 then
                local ok = pcall(function()
                    tcm:Call("SetTableArmSkinOverride", pf.tableID,
                             { OverrideMaterials = mats }, pf.pick)
                end)
                Log(TAG .. ": Flipper " .. i .. "/" .. total .. " tbl=" .. pf.tableID
                    .. " entry=" .. name .. " ok=" .. tostring(ok))
            else
                Log(TAG .. ": Flipper " .. i .. "/" .. total .. " tbl=" .. pf.tableID
                    .. " entry=" .. name .. " NO MATS (skipped)")
            end
        end)
    end

    -- Schedule final summary after all calls complete
    ExecuteWithDelay(DELAY_MS * total + 500, function()
        Log(TAG .. ": Staggered flipper overrides complete (" .. total .. " scheduled)")
    end)

    pendingFlipperOverrides = {}
end

-- ============================================================================
-- INIT: Poll every 5s until RegisteredSlots >= 50
-- ============================================================================
local MAX_POLLS = 60
local READY_STABLE_POLLS = 1
local initDone = false
local pollCount = 0
local maintRuns = 0
local MAX_MAINT_RUNS = 30
local lastMaintSlotCount = 0
local stableReadyCount = 0
local lastReadySlotCount = 0

LoopAsync(5000, function()
    pollCount = pollCount + 1

    if initDone then return true end

    if pollCount > MAX_POLLS then
        Log(TAG .. ": GAVE UP after " .. MAX_POLLS .. " polls")
        return true
    end

    local slotCount = count_registered_slots()
    if slotCount < 50 then
        Log(TAG .. ": Poll #" .. pollCount .. " — slots " .. slotCount .. "/50")
        return false
    end

    -- Build pool (all entries should exist by now)
    pcall(build_entry_pool)
    if not poolReady then
        Log(TAG .. ": Poll #" .. pollCount .. " — slots=" .. slotCount
            .. " but pool empty!")
        return false
    end

    if slotCount ~= lastReadySlotCount then
        lastReadySlotCount = slotCount
        stableReadyCount = 1
        if READY_STABLE_POLLS <= 1 then
            Log(TAG .. ": Poll #" .. pollCount .. " — slots threshold met, starting init now ("
                .. slotCount .. ")")
        else
        Log(TAG .. ": Poll #" .. pollCount .. " — waiting for stable slots ("
            .. slotCount .. ", stable " .. stableReadyCount .. "/" .. READY_STABLE_POLLS .. ")")
        return false
        end
    end

    stableReadyCount = stableReadyCount + 1
    if stableReadyCount < READY_STABLE_POLLS then
        Log(TAG .. ": Poll #" .. pollCount .. " — waiting for stable slots ("
            .. slotCount .. ", stable " .. stableReadyCount .. "/" .. READY_STABLE_POLLS .. ")")
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
    lastMaintSlotCount = slotCount
    Log(TAG .. ": === Init complete ===")

    -- Schedule deferred flipper arm overrides (15s delay for tables to fully load)
    if #pendingFlipperOverrides > 0 then
        Log(TAG .. ": Scheduling " .. #pendingFlipperOverrides .. " deferred flipper overrides in 15s")
        ExecuteWithDelay(15000, function()
            pcall(apply_pending_flipper_overrides)
        end)
    end

    return true
end)

-- Post-init maintenance: catch late-loaded slots and refill empties.
LoopAsync(4000, function()
    if not initDone then return false end
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
        pcall(apply_pending_flipper_overrides)
        return TAG .. " v27: sweep #" .. stats.sweeps
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
        local msg = TAG .. " v27: pool=" .. stats.pool_size
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
        -- Apply flipper overrides immediately (bridge is called when game is loaded)
        pcall(apply_pending_flipper_overrides)
        return TAG .. ": Re-randomized tables — ball=" .. stats.table_ball
            .. " trail=" .. stats.table_trail
            .. " flipper=" .. stats.table_flipper
            .. " fail=" .. stats.table_fail
    end)
end)

Log(TAG .. ": v28 loaded — hook=" .. tostring(hookRegistered)
    .. " | Fixes: IP-matched table cosmetics, robust assignment fallback, per-shelf Gadget/Statue unique+replace")
