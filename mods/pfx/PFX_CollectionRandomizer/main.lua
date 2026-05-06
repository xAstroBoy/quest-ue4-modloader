-- ============================================================================
-- PFX_CollectionRandomizer v34 — Hub Slot Scramble + Per-Table Cosmetics
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
local VERBOSE = true
local function V(...) if VERBOSE then Log(TAG .. " [V] " .. string.format(...)) end end
Log(TAG .. ": Loading v40...")

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

-- Entries whose localized name contains any of these substrings are excluded from pools
local ENTRY_NAME_BLACKLIST = {
    "arcade2tv",
    "arcade 2 tv",
    "arcade2 tv",
    "silver ball",
    "xarcade",
    "x arcade",
}

local function is_blacklisted_entry(entry)
    if not entry then return false end
    local locName = ""
    pcall(function() locName = tostring(entry:Call("GetLocalizedName")) end)
    if locName == "" then
        pcall(function() locName = entry:GetName() end)
    end
    if type(locName) ~= "string" or locName == "" then return false end
    local lower = locName:lower()
    for _, bl in ipairs(ENTRY_NAME_BLACKLIST) do
        if lower:find(bl, 1, true) then return true end
    end
    return false
end

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
        remaining = {},  -- items not yet picked this round
        usedKeys = {},   -- keys picked this round (for dupe detection)
    }
    -- Fill remaining with shuffled copy
    for i, item in ipairs(unique) do state.remaining[i] = item end
    if #state.remaining > 1 then shuffle(state.remaining) end
    return state
end

local function refill_cycle(state)
    state.remaining = {}
    state.usedKeys = {}
    for i, item in ipairs(state.unique or {}) do state.remaining[i] = item end
    if #state.remaining > 1 then shuffle(state.remaining) end
end

local function take_from_cycle(state, avoidKey)
    if not state or not state.unique or #state.unique == 0 then return nil end

    -- If remaining is empty, refill (start new cycle — grab ALL items again)
    if #state.remaining == 0 then
        refill_cycle(state)
    end

    -- Find first item in remaining whose key != avoidKey
    -- usedKeys is NOT checked here — once we refill, every item is fair game
    -- The only constraint is: don't give the slot its own current entry back
    for i = 1, #state.remaining do
        local candidate = state.remaining[i]
        local key = state.keyFn(candidate)
        if key and key ~= "" and key ~= (avoidKey or "") then
            -- Remove from remaining (swap with last for O(1) remove)
            state.remaining[i] = state.remaining[#state.remaining]
            state.remaining[#state.remaining] = nil
            state.usedKeys[key] = true
            return candidate
        end
    end

    -- All remaining match avoidKey — refill and try once more (new shuffle order)
    refill_cycle(state)
    for i = 1, #state.remaining do
        local candidate = state.remaining[i]
        local key = state.keyFn(candidate)
        if key and key ~= "" then
            state.remaining[i] = state.remaining[#state.remaining]
            state.remaining[#state.remaining] = nil
            state.usedKeys[key] = true
            return candidate
        end
    end

    -- Absolute fallback: return first unique entry
    return state.unique[1]
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

-- Reverse map: class-name suffix -> category ID
local CLASS_TO_CATID = {
    Arm         = 1,
    SkinTone    = 2,
    Poster      = 3,
    Statue      = 4,
    Gadget      = 5,
    Floor       = 6,
    Wall        = 7,
    Music       = 8,
    FlipperArm  = 9,
    BallSkin    = 10,
    BallTrail   = 11,
    Trophie     = 12,
    Cabinet     = 13,
    HubInterior = 14,
    Door        = 15,
}

local function get_category_id(entry)
    if not entry then return nil end
    -- Primary: derive from class name (PFXCollectibleEntry_Gadget -> 5)
    local className = nil
    pcall(function() className = entry:GetClassName() end)
    if className then
        local suffix = className:match("PFXCollectibleEntry_(%w+)")
        if suffix and CLASS_TO_CATID[suffix] then
            return CLASS_TO_CATID[suffix]
        end
    end
    -- Fallback: entry object name (PFXCollectibleEntry_Gadget_7 -> Gadget)
    local objName = nil
    pcall(function() objName = entry:GetName() end)
    if objName then
        local suffix = objName:match("PFXCollectibleEntry_(%a+)")
        if suffix and CLASS_TO_CATID[suffix] then
            return CLASS_TO_CATID[suffix]
        end
    end
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
-- Per-table pools (each table has ~3 entries per cosmetic type)
local ballByHint = {}    -- hint -> { {entry=, mesh=}, ... }
local trailByHint = {}   -- hint -> { {entry=, trail=}, ... }
local flipperByHint = {} -- hint -> { entry, ... }
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
    ballByHint = {}
    trailByHint = {}
    flipperByHint = {}
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
                    if is_live(entry) and not is_blacklisted_entry(entry) then
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
                                    if hint then
                                        local hk = hint:lower()
                                        if not ballByHint[hk] then ballByHint[hk] = {} end
                                        ballByHint[hk][#ballByHint[hk] + 1] = item
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
                                    if hint then
                                        local hk = hint:lower()
                                        if not trailByHint[hk] then trailByHint[hk] = {} end
                                        trailByHint[hk][#trailByHint[hk] + 1] = item
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
                                    if hint then
                                        local hk = hint:lower()
                                        if not flipperByHint[hk] then flipperByHint[hk] = {} end
                                        flipperByHint[hk][#flipperByHint[hk] + 1] = entry
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
    -- Log per-hint pool sizes for ball/trail/flipper
    local hintBallList, hintTrailList, hintFlipperList = {}, {}, {}
    for h, p in pairs(ballByHint) do hintBallList[#hintBallList+1] = h.."="..#p end
    for h, p in pairs(trailByHint) do hintTrailList[#hintTrailList+1] = h.."="..#p end
    for h, p in pairs(flipperByHint) do hintFlipperList[#hintFlipperList+1] = h.."="..#p end
    table.sort(hintBallList); table.sort(hintTrailList); table.sort(hintFlipperList)
    Log(TAG .. ": BallByHint(" .. #hintBallList .. "): " .. table.concat(hintBallList, ", "))
    Log(TAG .. ": TrailByHint(" .. #hintTrailList .. "): " .. table.concat(hintTrailList, ", "))
    Log(TAG .. ": FlipperByHint(" .. #hintFlipperList .. "): " .. table.concat(hintFlipperList, ", "))
    Log(TAG .. ": TableHints: " .. (function()
        local out = {}
        for h, ip in pairs(tableHintToIP) do out[#out+1] = h .. "->" .. ip end
        table.sort(out)
        return table.concat(out, ", ")
    end)())
    -- Log per-category pool sizes
    local catSizes = {}
    for catID, items in pairs(entryPool) do
        catSizes[#catSizes+1] = (CATEGORY_NAMES[catID] or ("Cat"..catID)) .. "=" .. #items
    end
    table.sort(catSizes)
    Log(TAG .. ": PoolByCat: " .. table.concat(catSizes, ", "))
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
                -- Use entry_id for filled slots, object name for empty ones
                local key = entry_id(slot)
                if key == "" then
                    key = "__empty__" .. name_of(slot)
                end
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
-- SWAP SLOT — Bridge-verified flow (v37):
--   1. RemovePreviousCollectible(true) — destroys old actor instantly
--   2. Set("m_slotEntry", newEntry) — update entry reference
--   3. SetupWithCollectibleEntry(newEntry) — async loads actor class, spawns, attaches
-- All three calls are on AC_CollectibleSlot_C (subclass of PFXCollectibleSlotComponent).
-- The spawn is ASYNC — actor appears after soft class finishes loading (~1-3s).
-- ============================================================================
local function is_floor_wall_slot(slot)
    local cn = nil
    pcall(function() cn = slot:GetClass():GetName() end)
    return cn == "AC_CollectibleSlot_FloorAndWall_C"
end

local function is_hub_slot(slot)
    local cn = nil
    pcall(function() cn = slot:GetClass():GetName() end)
    return cn == "AC_CollectibleSlot_Hub_C"
end

-- Cached reference to the single BP_Customization_Machine_C actor (floor/wall texture handler)
local cachedMachine = nil
local function get_machine()
    if cachedMachine and is_live(cachedMachine) then return cachedMachine end
    pcall(function() cachedMachine = FindFirstOf("BP_Customization_Machine_C") end)
    return cachedMachine
end

-- Cached reference to the room actor (BP_VR_Room_C) that holds floor/wall materials
local cachedRoom = nil
local function get_room()
    if cachedRoom and is_live(cachedRoom) then return cachedRoom end
    local machine = get_machine()
    if machine then
        pcall(function() cachedRoom = machine:Get("RoomRef") end)
    end
    return cachedRoom
end

local function swap_slot_entry(slot, newEntry)
    if not slot or not newEntry then return false end
    if not is_live(slot) or not is_live(newEntry) then return false end

    local previousEntry = nil
    pcall(function() previousEntry = slot:Get("m_slotEntry") end)

    if is_floor_wall_slot(slot) then
        -- FloorAndWall slots are texture-based (no spawned actor).
        -- Set the entry, then call ApplyFloorStyle/ApplyWallStyle on the
        -- BP_VR_Room_C actor (accessed via machine.RoomRef) to apply the texture.
        local wrote = pcall(function() slot:Set("m_slotEntry", newEntry) end)
        if not wrote then return false end
        -- Apply to ALL rooms (there are multiple BP_VR_Room_C instances)
        local cat = nil
        pcall(function() cat = slot:Get("SlotCategory") end)
        local rooms = nil
        pcall(function() rooms = FindAllOf("BP_VR_Room_C") end)
        if rooms then
            for _, room in ipairs(rooms) do
                if cat == 6 then
                    pcall(function() room:Call("ApplyFloorStyle", newEntry) end)
                elseif cat == 7 then
                    pcall(function() room:Call("ApplyWallStyle", newEntry) end)
                end
            end
        end
        -- Also update the customization machine's preview display
        local machine = get_machine()
        if machine then
            if cat == 6 then
                pcall(function() machine:Call("SetPreview_Floor", newEntry) end)
            elseif cat == 7 then
                pcall(function() machine:Call("SetPreview_Wall", newEntry) end)
            end
        end
        return true
    end

    if is_hub_slot(slot) then
        -- Hub interior uses level streaming. Set entry then ApplyNewInterior.
        local wrote = pcall(function() slot:Set("m_slotEntry", newEntry) end)
        if not wrote then return false end
        pcall(function() slot:Call("ApplyNewInterior") end)
        return true
    end

    -- Actor-based slots: full 3-step flow
    -- Step 1: Set entry first (SetupWithCollectibleEntry handles old actor internally)
    local wrote = pcall(function() slot:Set("m_slotEntry", newEntry) end)
    if not wrote then
        -- Restore on failure
        if previousEntry and is_live(previousEntry) then
            pcall(function() slot:Set("m_slotEntry", previousEntry) end)
        end
        return false
    end

    -- Step 2: Full setup — handles removing old actor, async loads new actor class, spawns
    pcall(function() slot:Call("SetupWithCollectibleEntry", newEntry) end)

    return true
end

local function infer_slot_category(slot)
    if not slot then return nil end

    -- FloorAndWall slots have an explicit SlotCategory property (6=Floor, 7=Wall)
    if is_floor_wall_slot(slot) then
        local slotCat = nil
        pcall(function() slotCat = slot:Get("SlotCategory") end)
        if type(slotCat) == "number" and slotCat > 0 then return slotCat end
    end

    -- Hub slot is always category 14
    if is_hub_slot(slot) then return 14 end

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
-- SCRAMBLE HUB SLOTS — batched with delays to avoid engine stale-ref crashes
-- v28: Execute swaps in small batches (BATCH_SIZE per tick) with delay between
-- batches, giving the engine time to process component updates.
-- ============================================================================
local SCRAMBLE_BATCH_SIZE = 8   -- swaps per batch
local SCRAMBLE_BATCH_DELAY = 200 -- ms between batches

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

    -- Build work list: {slot, picked_entry} pairs
    local workList = {}

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

    local totalSkip = 0
    local cyclesByCategory = {}

    -- Build work items for each category
    for catID, slotList in pairs(slotsByCategory) do
        local pool = entryPool[catID]
        if not pool or #pool == 0 then
            totalSkip = totalSkip + #slotList
            goto next_cat
        end

        if STRICT_UNIQUE_CATEGORIES[catID] then
            -- v30: Global cycle for strict-unique categories — no repeats across
            -- ALL stations until the entire pool is exhausted, then reshuffle.
            if not cyclesByCategory[catID] then
                cyclesByCategory[catID] = make_cycle_state(pool, entry_key)
            end
            local cycle = cyclesByCategory[catID]

            -- Shuffle slot order so different shelves get variety each run
            local shuffledSlots = {}
            for i, si in ipairs(slotList) do shuffledSlots[i] = si end
            shuffle(shuffledSlots)

            for _, slotInfo in ipairs(shuffledSlots) do
                local picked = take_from_cycle(cycle, slotInfo.currentKey)
                if picked then
                    workList[#workList + 1] = { slot = slotInfo.slot, entry = picked }
                else
                    -- Fallback: force pick first from pool (never leave empty)
                    workList[#workList + 1] = { slot = slotInfo.slot, entry = pool[math.random(#pool)] }
                end
            end
        else
            if not cyclesByCategory[catID] then
                cyclesByCategory[catID] = make_cycle_state(pool, entry_key)
            end
            local cycle = cyclesByCategory[catID]

            for _, slotInfo in ipairs(slotList) do
                local picked = take_from_cycle(cycle, slotInfo.currentKey)
                if picked then
                    workList[#workList + 1] = { slot = slotInfo.slot, entry = picked }
                else
                    totalSkip = totalSkip + 1
                end
            end
        end

        ::next_cat::
    end

    -- Fill empty slots — v30: also probe sibling slots for category hints
    if #emptySlots > 0 then
        local emptyByCategory = {}
        local uncategorized = {}
        for _, emptySlot in ipairs(emptySlots) do
            pcall(function()
                local targetCat = infer_slot_category(emptySlot)
                if targetCat and not SKIP_CATEGORIES[targetCat] then
                    if not emptyByCategory[targetCat] then emptyByCategory[targetCat] = {} end
                    emptyByCategory[targetCat][#emptyByCategory[targetCat] + 1] = emptySlot
                else
                    -- Try to infer from sibling slots on the same station
                    local stationKey = nil
                    pcall(function()
                        local ownerStation = emptySlot:Get("m_ownerStationComponent")
                        if ownerStation and is_live(ownerStation) then
                            stationKey = entry_id(ownerStation)
                        end
                    end)
                    if not stationKey then
                        pcall(function()
                            local outer = emptySlot:GetOuter()
                            if outer and is_live(outer) then stationKey = entry_id(outer) end
                        end)
                    end

                    -- Find what category other slots on the same station have
                    local siblingCat = nil
                    if stationKey then
                        for catID, slotList in pairs(slotsByCategory) do
                            for _, slotInfo in ipairs(slotList) do
                                if slotInfo.stationKey == stationKey then
                                    siblingCat = catID
                                    break
                                end
                            end
                            if siblingCat then break end
                        end
                    end

                    if siblingCat and not SKIP_CATEGORIES[siblingCat] then
                        if not emptyByCategory[siblingCat] then emptyByCategory[siblingCat] = {} end
                        emptyByCategory[siblingCat][#emptyByCategory[siblingCat] + 1] = emptySlot
                    else
                        uncategorized[#uncategorized + 1] = emptySlot
                    end
                end
            end)
        end

        -- For uncategorized empties, try Gadget first (most common empty), then Statue
        for _, emptySlot in ipairs(uncategorized) do
            local bestCat = 5  -- default to Gadget
            if entryPool[5] and #entryPool[5] > 0 then
                bestCat = 5
            elseif entryPool[4] and #entryPool[4] > 0 then
                bestCat = 4
            elseif entryPool[3] and #entryPool[3] > 0 then
                bestCat = 3  -- Poster
            end
            if not emptyByCategory[bestCat] then emptyByCategory[bestCat] = {} end
            emptyByCategory[bestCat][#emptyByCategory[bestCat] + 1] = emptySlot
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
                    if not picked then
                        -- Force pick: never leave a slot empty
                        picked = pool[math.random(#pool)]
                    end
                    if picked then
                        workList[#workList + 1] = { slot = emptySlot, entry = picked, isEmpty = true }
                    end
                end
            end
        end

        Log(TAG .. ": Empty slots: " .. #emptySlots .. " total, "
            .. #uncategorized .. " uncategorized (assigned to fallback cat)")
    end

    -- Execute work list in staggered batches
    local totalWork = #workList
    local numBatches = math.ceil(totalWork / SCRAMBLE_BATCH_SIZE)
    stats.sweeps = stats.sweeps + 1
    local sweepNum = stats.sweeps

    Log(TAG .. ": Scramble #" .. sweepNum .. ": " .. totalWork .. " swaps queued in "
        .. numBatches .. " batches (" .. SCRAMBLE_BATCH_SIZE .. "/batch, "
        .. SCRAMBLE_BATCH_DELAY .. "ms apart), " .. totalSkip .. " skip, "
        .. #emptySlots .. " empty")

    for batch = 1, numBatches do
        local startIdx = (batch - 1) * SCRAMBLE_BATCH_SIZE + 1
        local endIdx = math.min(batch * SCRAMBLE_BATCH_SIZE, totalWork)
        local delay = (batch - 1) * SCRAMBLE_BATCH_DELAY

        ExecuteWithDelay(delay, function()
            local batchOk, batchErr, batchEmpty = 0, 0, 0
            for i = startIdx, endIdx do
                local work = workList[i]
                if work and is_live(work.slot) and is_live(work.entry) then
                    if swap_slot_entry(work.slot, work.entry) then
                        batchOk = batchOk + 1
                        if work.isEmpty then batchEmpty = batchEmpty + 1 end
                    else
                        batchErr = batchErr + 1
                    end
                else
                    batchErr = batchErr + 1
                end
            end
            stats.sweep_ok = stats.sweep_ok + batchOk
            stats.sweep_err = stats.sweep_err + batchErr
            stats.empty_filled = stats.empty_filled + batchEmpty

            if batch == numBatches then
                Log(TAG .. ": Scramble #" .. sweepNum .. " complete: ok="
                    .. stats.sweep_ok .. " err=" .. stats.sweep_err
                    .. " empty=" .. stats.empty_filled)
            end
        end)
    end

    return totalWork
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
local hookPoolBuilt = false   -- lazy pool build on first hook call
local hookCallCount = 0       -- how many times the hook fired

-- v36: Remove initDone guard — swap entries AS the game restores them.
-- The game calls RestoreSlotEntryFromProfile during hub load. If actors are
-- spawned AFTER all restores complete, our post-hook entry swaps will cause
-- the game to spawn the correct actors. If actors are spawned during each
-- restore, we at least ensure m_slotEntry is correct for future reference.
pcall(function()
    RegisterHook("/Script/PFXCollectibles.PFXCollectibleSlotComponent:RestoreSlotEntryFromProfile",
        function(self)  -- pre-hook: do nothing, let the game restore
        end,
        function(self)  -- post-hook: immediately scramble entry
            hookCallCount = hookCallCount + 1
            pcall(function()
                -- Lazy-build the entry pool on first call
                if not hookPoolBuilt then
                    pcall(build_entry_pool)
                    hookPoolBuilt = true
                    if poolReady then
                        Log(TAG .. ": Hook: pool built lazily on first RestoreSlotEntryFromProfile (size="
                            .. stats.pool_size .. ")")
                    else
                        Log(TAG .. ": Hook: pool build FAILED on first RestoreSlotEntryFromProfile")
                    end
                end
                if not poolReady then return end
                scramble_one_slot(self:get(), "hook")
            end)
        end
    )
    hookRegistered = true
    Log(TAG .. ": Hook: RestoreSlotEntryFromProfile post-hook ENABLED (v36 — no initDone guard)")
end)
if not hookRegistered then
    Log(TAG .. ": Hook: RestoreSlotEntryFromProfile FAILED to register")
end

-- ============================================================================
-- PER-TABLE COSMETICS — Ball + Trail + Flippers (IP-MATCHED)
-- ============================================================================
-- v28 Fix: ALL ball/trail/flipper entries are IP-locked (table-specific).
-- Each table only gets cosmetics from its own IP group.
-- Table→IP mapping is built from entry tags + bundle name matching.
-- ============================================================================

-- Resolve table ID to IP by matching bundle name against tableHintToIP map
local function resolve_table_hint(bundleKey)
    if not bundleKey or bundleKey == "" then return nil end
    local bk = bundleKey:lower()
    for hint, _ in pairs(tableHintToIP) do
        if bk:find(hint, 1, true) then
            return hint
        end
    end
    return nil
end

local function resolve_table_ip(bundleKey)
    local hint = resolve_table_hint(bundleKey)
    return hint and tableHintToIP[hint] or nil
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

    -- Build work items for ball/trail, queue flippers separately
    local cosmeticWork = {}  -- {func, tableID, arg1, arg2, kind}

    for _, te in ipairs(tableEntries) do
        local tableID = te.tableID
        local hint = resolve_table_hint(te.bundleKey)
        tableCount = tableCount + 1

        if not hint then
            noIP = noIP + 1
            goto next_table
        end

        -- Queue BallSkin override — pick randomly from this table's own entries
        local bpool = ballByHint[hint]
        if bpool and #bpool > 0 then
            local pick = bpool[math.random(#bpool)]
            if pick then
                cosmeticWork[#cosmeticWork + 1] = {
                    kind = "ball", tableID = tableID,
                    fn = "SetTableBallMeshOverride", args = { tableID, pick.mesh, pick.entry }
                }
            end
        end

        -- Queue BallTrail override — pick randomly from this table's own entries
        local tpool = trailByHint[hint]
        if tpool and #tpool > 0 then
            local pick = tpool[math.random(#tpool)]
            if pick then
                cosmeticWork[#cosmeticWork + 1] = {
                    kind = "trail", tableID = tableID,
                    fn = "SetTableBallTrailOverride", args = { tableID, pick.trail, pick.entry }
                }
            end
        end

        -- Queue flipper arm override — pick randomly from this table's own entries
        local fpool = flipperByHint[hint]
        if fpool and #fpool > 0 then
            local pick = fpool[math.random(#fpool)]
            if pick then
                local fmats = nil
                pcall(function() fmats = pick:Get("OverrideMaterials") end)
                table.insert(pendingFlipperOverrides, {
                    tableID = tableID,
                    pick = pick,
                })
                flipperOk = flipperOk + 1
            end
        end

        ::next_table::
    end

    -- Count queued work items synchronously
    local ballQueued, trailQueued = 0, 0
    for _, w in ipairs(cosmeticWork) do
        if w.kind == "ball" then ballQueued = ballQueued + 1
        elseif w.kind == "trail" then trailQueued = trailQueued + 1 end
    end

    -- Execute ball/trail overrides in staggered batches (4 per batch, 150ms apart)
    local COSMETIC_BATCH = 4
    local COSMETIC_DELAY = 150
    local totalCos = #cosmeticWork
    local numCosBatches = math.ceil(totalCos / COSMETIC_BATCH)

    for batch = 1, numCosBatches do
        local si = (batch - 1) * COSMETIC_BATCH + 1
        local ei = math.min(batch * COSMETIC_BATCH, totalCos)
        local delay = (batch - 1) * COSMETIC_DELAY

        ExecuteWithDelay(delay, function()
            local liveTcm = find_live("BP_TableCustomizationManager_C", "PFXTableCustomizationManager")
            if not liveTcm then return end
            for i = si, ei do
                local w = cosmeticWork[i]
                if w then
                    local ok, err = pcall(function()
                        liveTcm:Call(w.fn, w.args[1], w.args[2], w.args[3])
                    end)
                    if w.kind == "ball" then ballOk = ballOk + 1
                    elseif w.kind == "trail" then trailOk = trailOk + 1 end
                    if not ok then
                        Log(TAG .. ": Cosmetic FAIL " .. w.kind .. " tbl=" .. w.tableID .. " fn=" .. w.fn .. " err=" .. tostring(err))
                    end
                end
            end
        end)
    end

    stats.table_ball = ballQueued
    stats.table_trail = trailQueued
    stats.table_flipper = flipperOk
    stats.table_fail = failed
    Log(TAG .. ": Table cosmetics: " .. tableCount .. " tables (noIP=" .. noIP .. ")"
        .. " | ball=" .. ballQueued .. " trail=" .. trailQueued
        .. " flipper=" .. flipperOk .. "/" .. (flipperOk + flipperFail)
        .. (failed > 0 and (" FAIL=" .. failed) or ""))
    return ballQueued + trailQueued
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
-- DESTROY XARCADE PROMO — repeating to catch late-streamed sublevel actors
-- BP_XArcadePromo_C lives in LVL_Arcade_80s_Rooms_OPTI_DLC1 (streamed)
-- Also hides door logo material and disables lights/shadows
-- ============================================================================
local XARCADE_CLASSES = {"BP_XArcadePromo_C"}
local xarcadeTotalDestroyed = 0

local function destroy_xarcade_actors()
    local destroyed = 0
    for _, cls in ipairs(XARCADE_CLASSES) do
        pcall(function()
            local actors = FindAllOf(cls)
            if actors then
                for _, a in ipairs(actors) do
                    if is_live(a) then
                        -- Disable shadow casting + lights on all components
                        pcall(function()
                            local allComps = a:Call("K2_GetComponentsByClass", FindClass("ActorComponent"))
                            if allComps then
                                for _, comp in ipairs(allComps) do
                                    pcall(function() comp:Set("bVisible", false) end)
                                    pcall(function() comp:Set("CastShadow", false) end)
                                    pcall(function() comp:Set("bCastDynamicShadow", false) end)
                                    pcall(function() comp:Set("bCastStaticShadow", false) end)
                                    pcall(function() comp:Set("bAffectsWorld", false) end)
                                    pcall(function() comp:Set("Intensity", 0) end)
                                end
                            end
                        end)
                        local apath = "?"
                        pcall(function() apath = a:GetPathName() end)
                        Log(TAG .. ": [XArcade promo] DESTROYING: " .. apath)
                        pcall(function() a:Call("SetActorHiddenInGame", true) end)
                        pcall(function() a:Call("SetActorEnableCollision", false) end)
                        pcall(function() a:Set("bActorEnableCollision", false) end)
                        -- Move it far underground to hide any remnant shadows
                        pcall(function() a:Call("K2_SetActorLocation", {X=0, Y=0, Z=-50000}, false, false) end)
                        pcall(function() a:Call("K2_DestroyActor") end)
                        destroyed = destroyed + 1
                    end
                end
            end
        end)
    end
    if destroyed > 0 then
        xarcadeTotalDestroyed = xarcadeTotalDestroyed + destroyed
        Log(TAG .. ": Destroyed " .. destroyed .. " XArcade promo actor(s) (total=" .. xarcadeTotalDestroyed .. ")")
    end

    -- Find and destroy the XArcade door arch sign StaticMeshActor.
    -- StaticMeshActors are few (~hundreds) so checking their root SMC materials is safe.
    -- We only call GetMaterial on the root StaticMeshComponent, not arbitrary components.
    pcall(function()
        local smas = FindAllOf("StaticMeshActor")
        if not smas then return end
        for _, sma in ipairs(smas) do
            pcall(function()
                if not is_live(sma) then return end
                local smc = nil
                pcall(function() smc = sma:Get("StaticMeshComponent") end)
                if not smc then return end
                -- Only check actors that have a loaded mesh
                local mesh = nil
                pcall(function() mesh = smc:Get("StaticMesh") end)
                if not mesh then return end
                local numMats = 0
                pcall(function() numMats = smc:Call("GetNumMaterials") end)
                if numMats == 0 then return end
                for mi = 0, numMats - 1 do
                    pcall(function()
                        local mat = nil
                        pcall(function() mat = smc:Call("GetMaterial", mi) end)
                        if mat then
                            local mn = ""
                            pcall(function() mn = mat:GetName() end)
                            if mn:lower():find("xarcade") then
                                local apath = "?"
                                local mpath = "?"
                                pcall(function() apath = sma:GetPathName() end)
                                pcall(function() mpath = mat:GetPathName() end)
                                Log(TAG .. ": [XArcade sign] ACTOR=" .. apath)
                                Log(TAG .. ": [XArcade sign] MAT[" .. mi .. "]=" .. mpath)
                                pcall(function() sma:Call("SetActorHiddenInGame", true) end)
                                pcall(function() sma:Call("SetActorEnableCollision", false) end)
                                pcall(function() sma:Call("K2_DestroyActor") end)
                                Log(TAG .. ": [XArcade sign] DESTROYED")
                            end
                        end
                    end)
                end
            end)
        end
    end)

    return destroyed
end

-- Repeating timer: keep checking for late-streamed XArcade actors for 90s after init
local xarcadeCheckCount = 0
local MAX_XARCADE_CHECKS = 15  -- 15 checks * 6s = 90s

-- ============================================================================
-- INIT: Poll every 3s until slots stabilize (v34: wait for FULL hub load)
-- Hub has ~206 slots. We require 150+ AND 3 consecutive stable polls so all
-- RestoreSlotEntryFromProfile calls finish before we scramble.
-- ============================================================================
local MAX_POLLS = 90
local MIN_SLOTS = 150
local READY_STABLE_POLLS = 3
local initDone = false
local pollCount = 0
local maintRuns = 0
local MAX_MAINT_RUNS = 30
local lastMaintSlotCount = 0
local stableReadyCount = 0
local lastReadySlotCount = 0

LoopAsync(3000, function()
    pollCount = pollCount + 1

    if initDone then return true end

    if pollCount > MAX_POLLS then
        Log(TAG .. ": GAVE UP after " .. MAX_POLLS .. " polls")
        return true
    end

    local slotCount = count_registered_slots()
    if slotCount < MIN_SLOTS then
        Log(TAG .. ": Poll #" .. pollCount .. " — slots " .. slotCount .. "/" .. MIN_SLOTS)
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

    -- Destroy XArcade promo actors — runs once now, then repeats on a timer
    -- to catch late-loaded sublevel actors (LVL_Arcade_80s_Rooms_OPTI_DLC1)
    pcall(destroy_xarcade_actors)

    initDone = true
    lastMaintSlotCount = slotCount
    Log(TAG .. ": === Init complete ===")

    -- Start repeating XArcade destroy timer (catches late-streamed sublevel)
    LoopAsync(6000, function()
        xarcadeCheckCount = xarcadeCheckCount + 1
        if xarcadeCheckCount > MAX_XARCADE_CHECKS then return true end
        pcall(destroy_xarcade_actors)
        return false
    end)

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
        V("randomize command fired")
        if not poolReady then pcall(build_entry_pool) end
        pcall(scramble_all_slots)
        pcall(scramble_table_cosmetics)
        pcall(apply_pending_flipper_overrides)
        return TAG .. " v34: sweep #" .. stats.sweeps
            .. " ok=" .. stats.sweep_ok
            .. " empty=" .. stats.empty_filled
            .. " ball=" .. stats.table_ball
            .. " trail=" .. stats.table_trail
            .. " flipper=" .. stats.table_flipper
    end)
end)

pcall(function()
    RegisterCommand("randomize_status", function()
        V("randomize_status command fired")
        local slotCount = count_registered_slots()
        local msg = TAG .. " v34: pool=" .. stats.pool_size
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

Log(TAG .. ": v42 loaded — hook=" .. tostring(hookRegistered)
    .. " | Fixes: entry blacklist, XArcade repeating destroy, all-room floor/wall+preview, Silver Ball+XArcade blacklist")

-- ============================================================================
-- GLOBAL EXPORTS — callable by other mods (e.g. PFX_ModMenu)
-- ============================================================================
local function scramble_by_catid(catID)
    if not poolReady then pcall(build_entry_pool) end
    if not poolReady then return 0 end
    local _, liveSlots = count_registered_slots()
    if not liveSlots or #liveSlots == 0 then return 0 end
    local done = 0
    for _, slot in ipairs(liveSlots) do
        pcall(function()
            if not is_live(slot) then return end
            local entry = nil
            pcall(function() entry = slot:Get("m_slotEntry") end)
            local cid = (entry and is_live(entry)) and get_category_id(entry) or infer_slot_category(slot)
            if cid == catID then
                if scramble_one_slot(slot, "manual") then done = done + 1 end
            end
        end)
    end
    return done
end

PFX_Rand = nil  -- will be set below
_G.PFX_Rand = {
    scramble_all    = scramble_all_slots,
    scramble_tables = scramble_table_cosmetics,
    scramble_cat    = scramble_by_catid,
    -- category IDs
    CAT_POSTER   = 3,
    CAT_STATUE   = 4,
    CAT_GADGET   = 5,
    CAT_FLOOR    = 6,
    CAT_WALL     = 7,
    CAT_HUB      = 14,
}
Log(TAG .. ": PFX_Rand global exported")
