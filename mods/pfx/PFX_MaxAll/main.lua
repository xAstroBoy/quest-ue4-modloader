-- ============================================================================
-- PFX_MaxAll v25 — Max All Progress + Unlock All + GRABBABLE Trophy Fix
-- ============================================================================
-- v25 ROOT CAUSE FIX:
--   Binary analysis of exec thunk at 0x4E62E30 revealed the REAL impl of
--   IsAchievementUnlocked is sub_4F6272C. It reads element[0x78] from the
--   AchievementsManager's INTERNAL TMap (not PFXCollectibleEntry.bIsUnlocked).
--   This TMap was never touched by our unlock_all_entries() — so IsAchievement-
--   Unlocked kept returning false at NATIVE C++ level, bypassing all PE hooks.
--
--   FIX: Binary patch sub_4F6272C → MOV W0,#1; RET (always return true).
--   This mirrors exactly what the working Frida script does via onLeave hook.
--   Combined with the existing IsHologram patch (0x4F631A0 → return false),
--   the game can never see any trophy as a hologram from ANY code path.
--
--   Also removed overly-aggressive BLOCKs on HandleOnSlotEntryChanged,
--   RemoveIfTrophyIsHologram, RestoreIfTrophyIsHologram, FindAndSetUnusedTrophy
--   — these are now safe to run since both check functions return correct values.
--
-- v24: Added BLOCK on Remove/Restore/Find/HandleOnSlotEntryChanged.
-- v23: Hooks 3+4 converted to PRE-BLOCK for trophy slots.
-- v22.2: Sweep made data-only.
-- v22: Binary patch MOV W0,#0; RET on IsHologram at 0x4F631A0.
-- ============================================================================
local TAG = "PFX_MaxAll"
local VERBOSE = true
local function V(...) if VERBOSE then Log(TAG .. " [V] " .. string.format(...)) end end

Log(TAG .. ": Loading v26...")

-- ============================================================================
-- STATE
-- ============================================================================
local state = {
    perks = 0,
    perks_fail = 0,
    masteries = 0,
    table_prog = 0,
    achievements_save = 0,
    entries_unlocked = 0,
    entries_unlock_fail = 0,
    entries_cm_unlock = 0,
    brandnew_cleared = 0,
    trophy_seen = 0,
    leagues = 0,
    match_progress_save = 0,
    holo_hook = false,
    holo_native_hook = false,
    achi_hook = false,
    restore_hook = false,
    change_hook = false,
    trophy_initial_swaps = 0,
    trophy_hook_swaps = 0,
    trophy_respawned = 0,
    trophy_sweep_fixes = 0,
    trophy_actors_spawned = 0,
    saved = false,
    ran = false,
    slots_ready = false,
    -- Championship batch state (v19)
    champ_total = 0,       -- total matches needing progress
    champ_set = 0,         -- matches set so far
    champ_done = false,    -- true when all 220 maxed
    benefits_set = 0,      -- benefits maxed
    errors = {},
}
local function add_error(msg)
    state.errors[#state.errors + 1] = msg
    Log(TAG .. " ERROR: " .. msg)
end

-- ============================================================================
-- HELPERS
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

-- ============================================================================
-- CONSTANTS — dynamic table ID discovery
-- ============================================================================
-- Fallback list (used only if live discovery fails)
local TABLE_IDS_FALLBACK = {
    91, 90, 92, 153, 160, 158, 181, 167, 177, 170, 156, 197,
    130, 131, 198, 163, 162, 109, 111, 120, 118, 119, 117, 122,
    121, 196, 194, 195, 157, 113, 123, 100, 101, 105, 103, 108
}

-- Discover all table IDs from the live YUPTableInfoHolder.
-- Each entry is a PFXVRTableInfo with PitID (int32) = the numeric table ID.
-- Returns a deduplicated sorted array.
local function get_all_table_ids()
    local ids = {}
    local seen = {}
    pcall(function()
        local holder = FindFirstOf("YUPTableInfoHolder")
        if not holder then return end
        local tables = holder:Get("m_Tables")
        if not tables then return end
        for i = 1, #tables do
            pcall(function()
                local entry = tables[i]
                if not entry then return end
                local id = nil
                -- Primary: PitID field
                pcall(function() id = entry:Get("PitID") end)
                -- Fallback: GetPITId() function
                if not id then pcall(function() id = entry:Call("GetPITId") end) end
                if type(id) == "number" and id > 0 and not seen[id] then
                    seen[id] = true
                    ids[#ids + 1] = id
                end
            end)
        end
    end)
    if #ids == 0 then
        Log(TAG .. ": WARN: YUPTableInfoHolder discovery failed — using fallback list")
        return TABLE_IDS_FALLBACK
    end
    table.sort(ids)
    Log(TAG .. ": Discovered " .. #ids .. " tables from YUPTableInfoHolder")
    return ids
end

-- TABLE_IDS is populated lazily on first use
local _TABLE_IDS_CACHE = nil
local function TABLE_IDS()
    if not _TABLE_IDS_CACHE then
        _TABLE_IDS_CACHE = get_all_table_ids()
    end
    return _TABLE_IDS_CACHE
end

local ENTRY_CLASSES = {
    "PFXCollectibleEntry_Arm",
    "PFXCollectibleEntry_BallSkin",
    "PFXCollectibleEntry_BallTrail",
    "PFXCollectibleEntry_Cabinet",
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

-- ============================================================================
-- GAME READINESS CHECK
-- ============================================================================
local function is_game_ready()
    local pm = find_live("BP_PerkManager_C", "PFXPerkManager")
    local sm = find_live("BP_SaveManager_C", "PFXSaveManager")
    local cm = find_live("BP_CollectiblesManager_C", "PFXCollectiblesManager")
    if not pm or not sm or not cm then return false, "managers missing" end

    local entries = nil
    pcall(function() entries = FindAllOf("PFXCollectibleEntry_BallSkin") end)
    if not entries or #entries == 0 then return false, "no entry UObjects yet" end

    local profile = nil
    pcall(function() profile = sm:Get("m_profile") end)
    if not profile then return false, "profile nil" end

    local masteryCount = 0
    pcall(function()
        local mmap = profile.MasteryProgress.MasteryProgress
        mmap:ForEach(function(k, v) masteryCount = masteryCount + 1 end)
    end)
    if masteryCount == 0 then return false, "profile empty (0 masteries)" end

    return true, "ready (" .. #entries .. " entries, " .. masteryCount .. " masteries)"
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
-- TROPHY HOLO → PHYSICAL MAP
-- ============================================================================
local holoToPhysical = {}  -- holo entry name (string) → physical entry (UObject)
local trophyMapReady = false

local function build_trophy_map()
    local entries = FindAllOf("PFXCollectibleEntry_Trophie")
    if not entries then return false end

    local byNum = { phys = {}, holo = {} }
    for _, e in ipairs(entries) do
        if not is_live(e) then goto cont end
        local tag = ""
        pcall(function()
            local eid = e:Get("CollectibleEntryID")
            if eid then tag = tostring(eid.TagName) end
        end)
        local hn = tag:match("HoloTrophy(%d+)")
        local pn = tag:match("^Collectibles%.Trophies%.Trophy(%d+)$")
        if hn then byNum.holo[tonumber(hn)] = e end
        if pn then byNum.phys[tonumber(pn)] = e end
        ::cont::
    end

    holoToPhysical = {}
    local count = 0
    for num, holoEntry in pairs(byNum.holo) do
        local physEntry = byNum.phys[num]
        if physEntry then
            local holoName = nil
            pcall(function() holoName = holoEntry:GetName() end)
            if holoName then
                holoToPhysical[holoName] = physEntry
                count = count + 1
            end
        end
    end

    trophyMapReady = count > 0
    Log(TAG .. ": Trophy map: " .. count .. " holo→physical pairs")
    return trophyMapReady
end

-- ============================================================================
-- SWAP SINGLE SLOT: data layer + deferred AC_CollectibleSlot_C respawn
-- Used by hook callbacks when game tries to set a holo entry at runtime.
-- ============================================================================
local swapping = false  -- re-entrancy guard for hooks

local function swap_slot_to_physical(component, actor, physEntry)
    if not component or not physEntry then return false end
    -- Step 1: Swap entry on the PFXCollectibleSlotComponent (data layer)
    -- NOTE: Do NOT call OnSlotEntryChanged — it fires HandleOnSlotEntryChanged
    -- which triggers a destroy+respawn cascade that kills trophy actors.
    local ok1 = pcall(function() component:Set("m_slotEntry", physEntry) end)
    -- Step 2: Deferred AC_CollectibleSlot_C destroy + respawn (visual layer)
    -- Deferred via LoopAsync(200ms) to avoid re-entrancy and let the game
    -- finish its current operation before we destroy/spawn actors.
    if actor and is_live(actor) then
        LoopAsync(200, function()
            pcall(function()
                if not is_live(actor) then return end
                local csr = actor:Get("CollectibleSlotRoot")
                if not csr then return end
                -- Set entry on AC component
                pcall(function() csr:Set("m_slotEntry", physEntry) end)
                -- Destroy holo actor + respawn physical
                pcall(function() csr:Call("DestroyPreviousCollectible") end)
                pcall(function() csr:Call("SetupWithCollectibleEntry", physEntry) end)
            end)
            return true  -- run once
        end)
    end
    return ok1
end

-- ============================================================================
-- TROPHY FIX v21 — Destroy holo actors + respawn with physical entries
-- The correct approach:
--   1. Swap holo→physical entries in RegisteredSlots (data consistency)
--   2. For each trophy wall slot's AC_CollectibleSlot_C component:
--      - Call DestroyPreviousCollectible → removes holo actor
--      - Call SetupWithCollectibleEntry(physEntry) → async-spawns physical actor
--   3. Wait for async spawns (~3-8s for all 30)
--   4. Call ShowSlot on each BP_TrophyCollectibleSlot_C to ensure visibility
-- ============================================================================
local function fix_all_trophies()
    V("fix_all_trophies")
    -- Step 1: Build trophy map if not already built
    if not trophyMapReady then
        build_trophy_map()
    end

    -- Step 2: Swap holo→physical entries in RegisteredSlots (data layer)
    local slotCount, regSlots = count_registered_slots()
    local dataSwapped = 0
    if trophyMapReady and regSlots then
        regSlots:ForEach(function(slotKey, slot)
            pcall(function()
                if not is_live(slot) then return end
                local entry = nil
                pcall(function() entry = slot:Get("m_slotEntry") end)
                if not entry or not is_live(entry) then return end
                local entryName = nil
                pcall(function() entryName = entry:GetName() end)
                if not entryName then return end
                local physEntry = holoToPhysical[entryName]
                if not physEntry then return end
                pcall(function() slot:Set("m_slotEntry", physEntry) end)
                -- NOTE: Do NOT call OnSlotEntryChanged — it triggers destroy+respawn cascade
                dataSwapped = dataSwapped + 1
            end)
        end)
    end
    state.trophy_initial_swaps = dataSwapped

    -- Step 3: Find trophy wall and slots
    local tw = nil
    pcall(function() tw = FindFirstOf("BP_Hub_TrophyWall_C_C") end)
    if not tw or not is_live(tw) then
        Log(TAG .. ": Trophy fix: no TrophyWall found")
        return dataSwapped
    end

    local slots = nil
    pcall(function() slots = tw:Get("TrophySlots") end)
    if not slots then
        Log(TAG .. ": Trophy fix: no TrophySlots")
        return dataSwapped
    end

    -- Step 4: Destroy holo actors + respawn with physical entries
    -- For each slot: get AC_CollectibleSlot_C → read m_slotEntry →
    -- look up physical entry in holoToPhysical map → swap entry on AC component →
    -- DestroyPreviousCollectible → SetupWithCollectibleEntry with physical entry
    local respawned, skipped, fail = 0, 0, 0
    for i = 1, #slots do
        local ok = pcall(function()
            local s = slots[i]
            local csr = s:Get("CollectibleSlotRoot")
            if not csr then fail = fail + 1; return end

            -- Get the entry currently assigned
            local entry = csr:Get("m_slotEntry")
            if not entry then fail = fail + 1; return end

            -- Check if spawned actor is already physical (skip if so)
            local sa = nil
            pcall(function() sa = csr:Get("SpawnedActor") end)
            if sa then
                local saClass = ""
                pcall(function() saClass = sa:GetClass():GetName() end)
                if saClass ~= "" and not saClass:match("Holo") then
                    skipped = skipped + 1
                    return  -- Already physical, no need to respawn
                end
            end

            -- Look up the physical entry for this holo entry
            local entryName = entry:GetName()
            local physEntry = holoToPhysical[entryName]
            if physEntry then
                -- Swap entry on AC component to physical
                entry = physEntry
                pcall(function() csr:Set("m_slotEntry", physEntry) end)
            end
            -- If no mapping found, entry is already physical — use as-is

            -- Destroy the holo actor
            pcall(function() csr:Call("DestroyPreviousCollectible") end)

            -- Setup with physical entry → triggers async spawn chain
            pcall(function() csr:Call("SetupWithCollectibleEntry", entry) end)
            respawned = respawned + 1
        end)
        if not ok then fail = fail + 1 end
    end

    state.trophy_respawned = respawned

    -- Set trophy wall hologram hidden flag
    pcall(function() tw:Set("IsHologramTrophiesHidden", true) end)

    Log(TAG .. ": Trophy fix: dataSwapped=" .. dataSwapped
        .. " respawned=" .. respawned
        .. " skipped=" .. skipped
        .. " fail=" .. fail
        .. " total=" .. #slots)

    return dataSwapped + respawned
end

-- ============================================================================
-- TROPHY ACTOR VERIFY — count spawned physical trophy actors
-- ============================================================================
local function count_trophy_actors()
    local actors = nil
    pcall(function() actors = FindAllOf("BP_Collectible_Trophy_Base_C") end)
    return actors and #actors or 0
end

-- ============================================================================
-- BACKGROUND TROPHY SWEEP — Lightweight data-only entry swap
-- Runs every 2s after initial trophy fix. Scans all 30 wall slots and silently
-- re-swaps any entries the game reverted to holo. DATA ONLY — never touches
-- actors (no DestroyPreviousCollectible, no SetupWithCollectibleEntry).
-- The binary patch handles IsHologram, so even if the actor class is "Holo",
-- it won't get hologram treatment. The physical actors from initial spawn stay.
-- ============================================================================
local trophySweepStarted = false

local function start_trophy_sweep()
    if trophySweepStarted then return end
    trophySweepStarted = true
    Log(TAG .. ": Starting background trophy sweep (data-only, every 2s)")

    LoopAsync(2000, function()
        if not trophyMapReady then return false end  -- keep waiting

        local fixed = 0
        pcall(function()
            local tw = FindFirstOf("BP_Hub_TrophyWall_C_C")
            if not tw then return end  -- not in hub, skip
            local slots = tw:Get("TrophySlots")
            if not slots then return end

            for i = 1, #slots do
                pcall(function()
                    local slot = slots[i]
                    if not slot or not is_live(slot) then return end

                    -- Check AC_CollectibleSlot_C entry
                    local csr = slot:Get("CollectibleSlotRoot")
                    if not csr then return end
                    local entry = csr:Get("m_slotEntry")
                    if not entry or not is_live(entry) then return end
                    local entryName = entry:GetName()
                    local physEntry = holoToPhysical[entryName]
                    if not physEntry then return end  -- not a holo entry, skip

                    -- DATA ONLY: swap entry references on this slot's CSR
                    pcall(function() csr:Set("m_slotEntry", physEntry) end)
                    fixed = fixed + 1
                end)
            end
        end)

        if fixed > 0 then
            state.trophy_sweep_fixes = state.trophy_sweep_fixes + fixed
        end
        return false  -- keep polling forever
    end)
end

-- ============================================================================
-- HOOKS — Registered IMMEDIATELY at load time
-- ============================================================================

-- HOOK 1: IsHologramByCollectibleEntry → always false (ProcessEvent level)
-- Prevents game from applying hologram visual effects to any trophy.
pcall(function()
    RegisterPreHook(
        "/Script/PFXVRQuest.PFXAchievementsManager:IsHologramByCollectibleEntry",
        function(self, funcPtr, parms)
            WriteU8(Offset(parms, 8), 0)  -- return value = false
            return "BLOCK"
        end
    )
    state.holo_hook = true
    Log(TAG .. ": Hook: IsHologramByCollectibleEntry → false (ProcessEvent)")
end)

-- PATCH 1b: BINARY PATCH IsHologram real impl → always return false
-- Overwrite the first two instructions with MOV W0,#0; RET so ALL callers
-- (native C++, Blueprint, exec thunk) see IsHologram=false.
pcall(function()
    local base = GetLibBase()
    local addr = Resolve("PFXAchievementsManager::IsHologram", 0x04F631A0)
    Log(TAG .. ": DIAG IsHologram: base=" .. tostring(base) .. " resolved=" .. tostring(addr))
    if not addr then
        Log(TAG .. ": WARN: IsHologram Resolve returned nil — trying offset fallback")
        if base then addr = Offset(base, 0x04F631A0) end
    end
    if not addr or addr == 0 then
        Log(TAG .. ": ERROR: IsHologram — cannot resolve address")
        return
    end
    if base and addr < base then
        addr = Offset(base, addr) -- relative offset → absolute
    end
    Log(TAG .. ": PATCH IsHologram: final addr=" .. tostring(addr))
    WriteU32(addr, 0x52800000)
    WriteU32(Offset(addr, 4), 0xD65F03C0)
    state.holo_native_hook = true
    Log(TAG .. ": PATCH: IsHologram binary patched → return false")
end)

-- PATCH 2: BINARY PATCH IsAchievementUnlocked → always return true
pcall(function()
    local base = GetLibBase()
    local addr = Resolve("PFXAchievementsManager::IsAchievementUnlocked_Real", 0x04F6272C)
    Log(TAG .. ": DIAG IsAchievementUnlocked: base=" .. tostring(base) .. " resolved=" .. tostring(addr))
    if not addr then
        Log(TAG .. ": WARN: IsAchievementUnlocked Resolve returned nil — trying offset fallback")
        if base then addr = Offset(base, 0x04F6272C) end
    end
    if not addr or addr == 0 then
        Log(TAG .. ": ERROR: IsAchievementUnlocked — cannot resolve address")
        return
    end
    if base and addr < base then
        addr = Offset(base, addr)
    end
    Log(TAG .. ": PATCH IsAchievementUnlocked: final addr=" .. tostring(addr))
    WriteU32(addr, 0x52800020)
    WriteU32(Offset(addr, 4), 0xD65F03C0)
    Log(TAG .. ": PATCH: IsAchievementUnlocked binary patched → return true")
end)

-- HOOK 2: IsAchievementUnlocked → always true
-- FPFXAchievementId struct size is unknown (could be 4, 8 or 12 bytes depending
-- on how the game defines it). Write true to all candidate return-value offsets
-- so the hook works regardless of struct layout.
pcall(function()
    RegisterPreHook(
        "/Script/PFXVRQuest.PFXAchievementsManager:IsAchievementUnlocked",
        function(self, funcPtr, parms)
            pcall(function() WriteU8(Offset(parms,  4), 1) end)  -- if 4-byte struct
            pcall(function() WriteU8(Offset(parms,  8), 1) end)  -- if 8-byte struct
            pcall(function() WriteU8(Offset(parms, 12), 1) end)  -- if 12-byte struct
            return "BLOCK"
        end
    )
    state.achi_hook = true
    Log(TAG .. ": Hook: IsAchievementUnlocked → true (multi-offset)")
end)

-- HOOK 3: RestoreSlotEntryFromProfile → PRE-hook BLOCK for trophy slots
-- The game's native implementation reads the holo entry from save profile and
-- calls ChangeSlotEntry internally, which DESTROYS the physical actor and
-- respawns with the holo entry. Our POST-hook was too late to prevent this.
-- FIX: PRE-hook BLOCK prevents the native code from ever running on trophy
-- slots. Our initial fix already set up physical entries + actors.
pcall(function()
    RegisterPreHook(
        "/Script/PFXVRQuest.PFXCollectibleSlotComponent:RestoreSlotEntryFromProfile",
        function(self, funcPtr, parms)
            -- DIAG: log first 5 calls + every 50th to detect retry storms
            state._restore_diag = (state._restore_diag or 0) + 1
            if state._restore_diag <= 5 or state._restore_diag % 50 == 0 then
                Log(TAG .. ": DIAG: RestoreSlotEntryFromProfile hook #" .. state._restore_diag)
            end
            local shouldBlock = false
            pcall(function()
                if not is_live(self) then return end
                local outer = self:GetOuter()
                if not outer or not is_live(outer) then return end
                local cls = outer:GetClass():GetName()
                if cls == "BP_TrophyCollectibleSlot_C" then
                    shouldBlock = true
                end
            end)
            if shouldBlock then
                state.trophy_hook_swaps = state.trophy_hook_swaps + 1
                V("RestoreSlotEntryFromProfile BLOCK #" .. state.trophy_hook_swaps)
                return "BLOCK"
            end
        end
    )
    state.restore_hook = true
    Log(TAG .. ": Hook: RestoreSlotEntryFromProfile → BLOCK for trophy slots")
end)

-- HOOK 4: ChangeSlotEntry → PRE-hook BLOCK for trophy slots
-- Same reasoning as Hook 3. The native ChangeSlotEntry destroys the old actor
-- and spawns a new one based on the entry. If the game passes a holo entry,
-- it would spawn a holo actor. BLOCK prevents this entirely for trophy slots.
pcall(function()
    RegisterPreHook(
        "/Script/PFXVRQuest.PFXCollectibleSlotComponent:ChangeSlotEntry",
        function(self, funcPtr, parms)
            local shouldBlock = false
            pcall(function()
                if not is_live(self) then return end
                local outer = self:GetOuter()
                if not outer or not is_live(outer) then return end
                local cls = outer:GetClass():GetName()
                if cls == "BP_TrophyCollectibleSlot_C" then
                    shouldBlock = true
                end
            end)
            if shouldBlock then
                state.trophy_hook_swaps = state.trophy_hook_swaps + 1
                return "BLOCK"
            end
        end
    )
    state.change_hook = true
    Log(TAG .. ": Hook: ChangeSlotEntry → BLOCK for trophy slots")
end)

-- HOOKS 5, 5b, 6, 7 REMOVED in v25:
-- With both binary patches active (IsHologram→false, IsAchievementUnlocked→true),
-- RemoveIfTrophyIsHologram, RestoreIfTrophyIsHologram, FindAndSetUnusedTrophy,
-- and HandleOnSlotEntryChanged are SAFE to run — they will all see IsHologram=false
-- and never hide or replace physical trophy actors.
-- Blocking them was preventing normal slot state updates on grab/place.

-- ============================================================================
-- PHASE 1: UNLOCK ALL COLLECTIBLE ENTRIES
-- ============================================================================
local function unlock_all_entries()
    local cm = find_live("BP_CollectiblesManager_C", "PFXCollectiblesManager")
    local ok_count, fail_count, cm_count = 0, 0, 0

    for _, cls in ipairs(ENTRY_CLASSES) do
        pcall(function()
            local entries = FindAllOf(cls)
            if not entries then return end
            for _, entry in ipairs(entries) do
                if is_live(entry) then
                    -- Guard: UnlockData must be non-nil — it's dereferenced immediately
                    -- inside the native SetUnlocked/SetBrandNewUnlock code. If it’s nil
                    -- the entry hasn’t been fully initialised yet and the call crashes.
                    local ud = nil
                    pcall(function() ud = entry:Get("UnlockData") end)
                    if not ud then
                        fail_count = fail_count + 1
                    else
                        local ok1 = pcall(function() entry:Call("SetUnlocked", true) end)
                        if ok1 then
                            ok_count = ok_count + 1
                        else
                            fail_count = fail_count + 1
                        end
                    end
                    -- NOTE: cm:Call("UnlockEntry") removed — Reason param is
                    -- EPFXCollectibleUnlockReasonType enum (1 byte), Call() writes int32
                    -- causing param out-of-bounds. SetUnlocked(true) is sufficient.
                    if cm then
                        cm_count = cm_count + 1
                    end
                end
            end
        end)
    end

    state.entries_unlocked = ok_count
    state.entries_unlock_fail = fail_count
    state.entries_cm_unlock = cm_count
    Log(TAG .. ": Entries: " .. ok_count .. " unlocked, " .. fail_count .. " fail, " .. cm_count .. " CM")
end

-- ============================================================================
-- PHASE 2: MAX PERKS
-- ============================================================================
local function max_perks()
    local pm = find_live("BP_PerkManager_C", "PFXPerkManager")
    if not pm then add_error("PerkManager NOT FOUND"); return end

    -- Try native cheat first (covers ALL tables + ALL perk types correctly)
    local cheat_ok = false
    pcall(function()
        local cm = find_live("PFXCheatManager")
        if cm then
            cm:Call("PFXDebug_TablePerkMaxAll")
            cheat_ok = true
            Log(TAG .. ": Perks: PFXDebug_TablePerkMaxAll() called via CheatManager")
        end
    end)

    -- Fallback: manual loop — perkType 0-8 (type 0 was previously missed!)
    local ok_count, fail_count = 0, 0
    for _, tableID in ipairs(TABLE_IDS()) do
        for perkType = 0, 8 do          -- was 1,8 — type 0 is valid and was SKIPPED
            local ok = pcall(function() pm:Call("SetPerkLevelOnTable", tableID, perkType, 10, true) end)
            if ok then ok_count = ok_count + 1 else fail_count = fail_count + 1 end
        end
    end
    state.perks = ok_count
    state.perks_fail = fail_count
    Log(TAG .. ": Perks: " .. ok_count .. " ok, " .. fail_count .. " fail (cheat=" .. tostring(cheat_ok) .. ")")
end

-- ============================================================================
-- PHASE 3: SAVE DATA (masteries, table progress, achievements, championship)
-- ============================================================================
local function max_save_data()
    local sm = find_live("BP_SaveManager_C", "PFXSaveManager")
    if not sm then add_error("SaveManager NOT FOUND"); return end
    local profile = nil
    pcall(function() profile = sm:Get("m_profile") end)
    if not profile then add_error("m_profile nil"); return end

    -- Masteries → level 5
    pcall(function()
        local mmap = profile.MasteryProgress.MasteryProgress
        local count = 0
        mmap:ForEach(function(key, val)
            pcall(function()
                val.Level = 5
                val.HitCountProgressToNext = 0
                count = count + 1
            end)
        end)
        state.masteries = count
        Log(TAG .. ": Masteries: " .. count .. " → level 5")
    end)

    -- Table Progress → max XP + level
    pcall(function()
        local tmap = profile.TableProgress.TableProgressMap
        local count = 0
        tmap:ForEach(function(key, val)
            pcall(function()
                val.ReceivedTableProgressLevel = 7
                val.ReceivedTableXp = 99999
                val.SeenTableXp = 99999
                count = count + 1
            end)
        end)
        state.table_prog = count
        Log(TAG .. ": Table Progress: " .. count .. " maxed")
    end)

    -- Achievement Progress → unlocked + max progress
    pcall(function()
        local amap = profile.achievements.AchievementProgress
        local count = 0
        amap:ForEach(function(key, val)
            pcall(function()
                val.m_IsUnlocked = true
                val.m_AchievementProgress = 9999
                count = count + 1
            end)
        end)
        state.achievements_save = count
        Log(TAG .. ": Achievements (save): " .. count .. " unlocked")
    end)

    -- Championship save data — max existing league/match progress in save profile
    -- NOTE: This updates EXISTING entries via ForEach.
    -- Phase 6b re-confirms all entries and sets LastSeenLeagueMapLevel.
    pcall(function()
        local cp = profile.ChampionshipProgress
        pcall(function() cp:CopyFrom({LastSeenTotalTrophyGained = 9999}) end)

        local lmap = cp.LeagueProgress
        local leagueCount = 0
        local matchCount = 0
        lmap:ForEach(function(lKey, lVal)
            pcall(function()
                lVal.LastSeenLeagueMapLevel = 99
                leagueCount = leagueCount + 1
                pcall(function()
                    local mmap = lVal.MatchProgress
                    mmap:ForEach(function(mKey, mVal)
                        pcall(function()
                            mVal.progress = 3
                            mVal.Score = 999999999
                            matchCount = matchCount + 1
                        end)
                    end)
                end)
            end)
        end)

        state.leagues = leagueCount
        state.match_progress_save = matchCount
        Log(TAG .. ": Championship save: " .. leagueCount .. " leagues, "
            .. matchCount .. " matches maxed (existing entries only)")
    end)
end

-- ============================================================================
-- PHASE 3b: MAX CHAMPIONSHIP BENEFITS
-- AddBenefit doesn't crash (confirmed working). 6 benefits × up to 99 each.
-- ============================================================================
local function max_benefits()
    local chm = find_live("BP_ChampionshipManager_C", "PFXChampionshipManager")
    if not chm then Log(TAG .. ": CHM not found for benefits"); return end
    local cd = nil
    pcall(function() cd = chm:Get("ChampionshipData") end)
    if not cd then Log(TAG .. ": No ChampionshipData for benefits"); return end
    local bl = nil
    pcall(function() bl = cd:Get("BenefitList") end)
    if not bl then Log(TAG .. ": No BenefitList"); return end

    local total_added = 0
    for i, benefit in ipairs(bl) do
        if not is_live(benefit) then goto next_benefit end

        -- Get current count
        local current = 0
        pcall(function() current = chm:Call("GetBenefitAmount", benefit) end)
        if type(current) ~= "number" then current = 0 end

        -- Add difference to reach 99
        local toAdd = 99 - math.floor(current)
        if toAdd <= 0 then goto next_benefit end

        for j = 1, toAdd do
            pcall(function() chm:Call("AddBenefit", benefit, false) end)
        end
        total_added = total_added + toAdd
        ::next_benefit::
    end

    -- Force-select all benefits so they show as active in UI
    local selected = 0
    pcall(function() chm:Call("ClearSelectedBenefits") end)
    for i, benefit in ipairs(bl) do
        if is_live(benefit) then
            pcall(function()
                chm:Call("ForceSelectBenefit", benefit)
                selected = selected + 1
            end)
        end
    end
    pcall(function() chm:Call("FinalizeBenefitSelection") end)

    state.benefits_set = total_added
    Log(TAG .. ": Benefits: " .. total_added .. " added, " .. selected .. " selected across " .. #bl .. " types")
end

-- ============================================================================
-- PHASE 4: CLEAR BRAND-NEW FLAGS
-- ============================================================================
local function clear_brandnew()
    local bn_count = 0
    for _, cls in ipairs(ENTRY_CLASSES) do
        pcall(function()
            local entries = FindAllOf(cls)
            if entries then
                for _, entry in ipairs(entries) do
                    if is_live(entry) then
                        -- SetBrandNewUnlock deref's BOTH UnlockData (+0x108) AND
                        -- CategoryData (+0x118). A null/garbage CategoryData causes
                        -- fault at 0x10 / 0x44 / 0x145 and leaves the object in
                        -- partial state that makes the UE5 GC recurse infinitely.
                        local ud = nil
                        local cd = nil
                        pcall(function() ud = entry:Get("UnlockData") end)
                        pcall(function() cd = entry:Get("CategoryData") end)
                        if ud and cd then
                            pcall(function()
                                entry:Call("SetBrandNewUnlock", false)
                                bn_count = bn_count + 1
                            end)
                        end
                    end
                end
            end
        end)
    end
    state.brandnew_cleared = bn_count
    Log(TAG .. ": BrandNew cleared: " .. bn_count)
end

-- ============================================================================
-- PHASE 5: MARK TROPHIES SEEN
-- ============================================================================
local function mark_trophies_seen()
    local am = find_live("BP_AchievementsManager_C", "PFXAchievementsManager")
    if not am then Log(TAG .. ": AchievementsManager not found"); return false end
    -- AchievementID is FPFXAchievementId enum (1-byte), not int32.
    -- Passing int32 causes param out-of-bounds. Use Set on each trophy directly.
    local count, fail = 0, 0
    local trophies = nil
    pcall(function() trophies = FindAllOf("PFXTrophyActor") end)
    if trophies then
        for _, t in ipairs(trophies) do
            pcall(function()
                if t:IsValid() then
                    t:Set("bHasBeenSeen", true)
                    count = count + 1
                end
            end)
        end
    end
    -- Fallback: try via achievements data array if trophies not found
    if count == 0 then
        pcall(function()
            local data = am:Get("AchievementsData")
            if data then
                for i = 1, #data do
                    pcall(function()
                        local entry = data[i]
                        if entry then
                            entry:Set("bHasBeenSeen", true)
                            count = count + 1
                        end
                    end)
                end
            end
        end)
    end
    state.trophy_seen = count
    Log(TAG .. ": TrophySeen: " .. count)
    return count > 0
end

-- ============================================================================
-- PHASE 6: CHAMPIONSHIP SYNC
-- ============================================================================
local function sync_championship()
    local chm = find_live("BP_ChampionshipManager_C", "PFXChampionshipManager")
    if not chm then Log(TAG .. ": ChampionshipManager not found"); return false end
    pcall(function() chm:Call("SetLastSeenTotalTrophyGainedToCurrent") end)
    Log(TAG .. ": Championship synced")
    return true
end

-- ============================================================================
-- PHASE 6b: CHAMPIONSHIP BATCH — Direct TMap writes only (no ProcessEvent)
-- SetMatchProgress crashes (fault_addr=0x38) because GameplayTag struct
-- cannot be serialized for ProcessEvent on this build.  Phase 5 already
-- writes progress=3 + Score=999999999 on existing save entries via ForEach.
-- This phase ensures every match from the runtime ChampionshipData also
-- exists in the save profile TMap, creating missing entries if needed.
-- ============================================================================
local function run_championship_batch()
    -- 1. Get save profile maps
    local sm = find_live("BP_SaveManager_C", "PFXSaveManager")
    if not sm then Log(TAG .. ": SaveManager not found for champ batch"); return end
    local profile = nil
    pcall(function() profile = sm:Get("m_profile") end)
    if not profile then Log(TAG .. ": No m_profile for champ batch"); return end
    local cp = nil
    pcall(function() cp = profile.ChampionshipProgress end)
    if not cp then Log(TAG .. ": No ChampionshipProgress in profile"); return end
    local leagueMap = nil
    pcall(function() leagueMap = cp.LeagueProgress end)
    if not leagueMap then Log(TAG .. ": No LeagueProgress TMap"); return end

    -- 2. Get runtime championship data
    local chm = find_live("BP_ChampionshipManager_C", "PFXChampionshipManager")
    if not chm then Log(TAG .. ": CHM not found for batch"); return end
    local cd = nil
    pcall(function() cd = chm:Get("ChampionshipData") end)
    if not cd then Log(TAG .. ": No ChampionshipData for batch"); return end
    local leagues = nil
    pcall(function() leagues = cd:Get("LeagueList") end)
    if not leagues then Log(TAG .. ": No LeagueList for batch"); return end

    -- 3. Walk every league → round → match.
    --    For each match, call SetMatchProgress via ProcessEvent with Clone'd GameplayTag.
    local total_set = 0
    local total_fail = 0
    local total_leagues = 0
    for li = 1, #leagues do
        local lg = leagues[li]
        if not lg then goto cl end
        pcall(function()
            total_leagues = total_leagues + 1

            -- Ensure all league entries have LastSeenLeagueMapLevel = 99
            pcall(function()
                leagueMap:ForEach(function(lKey, lVal)
                    pcall(function() lVal.LastSeenLeagueMapLevel = 99 end)
                end)
            end)

            local rounds = lg:Get("RoundList")
            if not rounds then return end
            rounds:ForEach(function(k, v)
                pcall(function()
                    local ml = v:Get("MatchList")
                    if not ml then return end
                    for mi = 1, #ml do
                        pcall(function()
                            local m = ml[mi]
                            if not m then return end
                            local tag = m:Get("ID")
                            if not tag then return end
                            -- Clone the GameplayTag to get an owning copy for ProcessEvent
                            local cloned = tag:Clone()
                            local ok, err = pcall(function()
                                chm:Call("SetMatchProgress", cloned, 3, 999999999, true, false)
                            end)
                            if ok then
                                total_set = total_set + 1
                            else
                                total_fail = total_fail + 1
                            end
                        end)
                    end
                end)
            end)
        end)
        ::cl::
    end

    state.champ_set = total_set
    state.champ_done = true
    Log(TAG .. ": Championship batch: " .. total_set .. " matches set via SetMatchProgress, "
        .. total_fail .. " failed, " .. total_leagues .. " leagues")
end

-- ============================================================================
-- PHASE 7: SAVE PROFILE
-- ============================================================================
local function save_profile()
    local pm = find_live("BP_PerkManager_C", "PFXPerkManager")
    if pm then
        pcall(function() pm:Call("SetPerkLevelOnTable", 91, 1, 10, false) end)
        state.saved = true
        Log(TAG .. ": Profile saved via perk system")
        return true
    end
    add_error("Could not save")
    return false
end

-- ============================================================================
-- MAIN: Run all phases
-- ============================================================================
local function run_maxall()
    V("run_maxall")
    if state.ran then return end
    state.ran = true
    Log(TAG .. ": ========== STARTING MaxAll v24 ==========")

    pcall(unlock_all_entries)
    pcall(max_perks)
    pcall(max_save_data)
    -- max_benefits() intentionally NOT called here (Phase A).
    -- BP_ChampionshipManager_C sub-objects (BenefitList entries' data fields) are
    -- still loading at T≈24s. AddBenefit/GetBenefitAmount/ForceSelectBenefit all
    -- crash with fault_addr=0xffffff80000000 (dangling sub-object ptr).
    -- max_benefits() and sync_championship() are called in Phase B (T≈35-60s,
    -- after slots≥100 and TrophyWall loaded) when all assets are initialized.
    --
    -- clear_brandnew() intentionally NOT called here either (Phase A).
    -- PrimaryDataAsset sub-objects (CategoryData, UnlockData) are still loading.
    -- clear_brandnew() is called in Phase B.
    pcall(mark_trophies_seen)
    pcall(build_trophy_map)
    pcall(save_profile)

    Log(TAG .. ": ========== MaxAll v24 initial phases COMPLETE ==========")
    Log(TAG .. string.format(
        ": E=%d/%d CM=%d P=%d M=%d T=%d A=%d BN=%d TS=%d L=%d MPS=%d BEN=%d S=%s",
        state.entries_unlocked, state.entries_unlocked + state.entries_unlock_fail,
        state.entries_cm_unlock,
        state.perks, state.masteries, state.table_prog,
        state.achievements_save, state.brandnew_cleared,
        state.trophy_seen, state.leagues, state.match_progress_save,
        state.benefits_set,
        tostring(state.saved)
    ))
end

-- ============================================================================
-- INIT: LoopAsync poll — multi-phase approach:
--   Phase A: Run MaxAll when game ready (perks, entries, save data)
--   Phase B: Wait for RegisteredSlots >= 100 → trophy fix + actor spawn
--   Phase C: Verify trophy actors spawned (with retry)
--   Phase D: Championship batch (10 matches per cycle)
-- ============================================================================
local MAX_POLLS = 90
local initDone = false
local trophyDataDone = false
local trophyVerifyRetries = 0
local trophyVerifyDone = false
local allDone = false
local pollCount = 0

LoopAsync(5000, function()
    Log(TAG .. ": DIAG: LoopAsync tick #" .. pollCount .. " — initDone=" .. tostring(initDone) .. " allDone=" .. tostring(allDone))
    pollCount = pollCount + 1

    if allDone then return true end

    -- Poll limit only applies to initial wait phases (A + B + C), not championship batch
    if pollCount > MAX_POLLS and not trophyVerifyDone then
        Log(TAG .. ": GAVE UP after " .. MAX_POLLS .. " polls")
        return true
    end

    -- Phase A: Run MaxAll once when game is ready
    if not initDone then
        local ready, reason = is_game_ready()
        Log(TAG .. ": Poll #" .. pollCount .. " — " .. reason)
        if ready then
            initDone = true
            pcall(run_maxall)
        end
        return false
    end

    -- Phase B: Trophy fix (needs slots >= 100 AND trophy wall present)
    if not trophyDataDone then
        local slotCount = count_registered_slots()
        if slotCount < 100 then
            Log(TAG .. ": Poll #" .. pollCount .. " — waiting for slots (" .. slotCount .. "/100)")
            return false
        end

        -- Also wait for trophy wall to be spawned
        local tw = nil
        pcall(function() tw = FindFirstOf("BP_Hub_TrophyWall_C_C") end)
        if not tw or not is_live(tw) then
            Log(TAG .. ": Poll #" .. pollCount .. " — waiting for TrophyWall (slots=" .. slotCount .. ")")
            return false
        end

        -- Check trophy wall has slots populated
        local trophySlots = nil
        pcall(function() trophySlots = tw:Get("TrophySlots") end)
        if not trophySlots or #trophySlots == 0 then
            Log(TAG .. ": Poll #" .. pollCount .. " — waiting for TrophySlots (tw exists)")
            return false
        end

        Log(TAG .. ": Poll #" .. pollCount .. " — slots ready! (" .. slotCount .. ")")
        state.slots_ready = true

        -- Trophy entry swap + destroy holo actors + respawn physical (v21)
        pcall(fix_all_trophies)

        if state.trophy_seen == 0 then pcall(mark_trophies_seen) end
        pcall(max_benefits)
        pcall(sync_championship)
        pcall(clear_brandnew)
        pcall(save_profile)

        trophyDataDone = true
        Log(TAG .. ": Trophy fix done (swapped=" .. state.trophy_initial_swaps
            .. " respawned=" .. state.trophy_respawned .. ")")
        return false
    end

    -- Phase C: Verify trophy actors + ShowSlot (2 retries × 5s = 10s for async)
    if not trophyVerifyDone then
        trophyVerifyRetries = trophyVerifyRetries + 1
        local actorCount = count_trophy_actors()
        state.trophy_actors_spawned = actorCount
        Log(TAG .. ": Trophy verify #" .. trophyVerifyRetries
            .. ": actors=" .. actorCount .. "/30")

        -- Call ShowSlot on all trophy slots to ensure visibility
        pcall(function()
            local tw = FindFirstOf("BP_Hub_TrophyWall_C_C")
            if tw then
                local slots = tw:Get("TrophySlots")
                if slots then
                    local showOk = 0
                    for i = 1, #slots do
                        local ok = pcall(function() slots[i]:Call("ShowSlot") end)
                        if ok then showOk = showOk + 1 end
                    end
                    Log(TAG .. ": ShowSlot called on " .. showOk .. "/" .. #slots)
                end
            end
        end)

        -- Count physical vs holo actors on slots
        local physCount, holoCount = 0, 0
        pcall(function()
            local tw = FindFirstOf("BP_Hub_TrophyWall_C_C")
            if tw then
                local slots = tw:Get("TrophySlots")
                if slots then
                    for i = 1, #slots do
                        pcall(function()
                            local csr = slots[i]:Get("CollectibleSlotRoot")
                            if csr then
                                local sa = csr:Get("SpawnedActor")
                                if sa then
                                    local cls = sa:GetClass():GetName()
                                    if cls:match("Holo") then
                                        holoCount = holoCount + 1
                                    else
                                        physCount = physCount + 1
                                    end
                                end
                            end
                        end)
                    end
                end
            end
        end)
        Log(TAG .. ": Slot check: physical=" .. physCount .. " holo=" .. holoCount)

        if physCount >= 25 or trophyVerifyRetries >= 3 then
            trophyVerifyDone = true
            -- Retry clear_brandnew here — Phase C fires ~10s after Phase B.
            -- Any PrimaryDataAsset entries still loading during Phase B will now
            -- be fully initialized, giving zero-crash coverage.
            if state.brandnew_cleared < 900 then
                pcall(clear_brandnew)
                pcall(save_profile)
            end
            -- Start background trophy sweep for ongoing grab/place protection
            pcall(start_trophy_sweep)
            if holoCount > 0 and physCount < 25 then
                -- Some slots still have holos, retry respawn on those
                Log(TAG .. ": Retrying respawn on remaining holo slots...")
                pcall(function()
                    local tw = FindFirstOf("BP_Hub_TrophyWall_C_C")
                    if tw then
                        local slots = tw:Get("TrophySlots")
                        if slots then
                            for i = 1, #slots do
                                pcall(function()
                                    local csr = slots[i]:Get("CollectibleSlotRoot")
                                    if not csr then return end
                                    local sa = csr:Get("SpawnedActor")
                                    if not sa then return end
                                    local cls = sa:GetClass():GetName()
                                    if not cls:match("Holo") then return end
                                    local entry = csr:Get("m_slotEntry")
                                    if not entry then return end
                                    pcall(function() csr:Call("DestroyPreviousCollectible") end)
                                    pcall(function() csr:Call("SetupWithCollectibleEntry", entry) end)
                                end)
                            end
                        end
                    end
                end)
            end
        end
        return false
    end

    -- Phase D: Championship batch (runs after trophy verify when game is fully loaded)
    if not state.champ_done then
        Log(TAG .. ": Phase D: running championship batch (direct TMap write + rehash)")
        pcall(run_championship_batch)
        pcall(save_profile)
    end

    allDone = true
    Log(TAG .. ": === All phases complete (v24) ===")
    return true
end)

-- ============================================================================
-- BRIDGE COMMANDS
-- ============================================================================
pcall(function()
    RegisterCommand("maxall_status", function()
        V("maxall_status command fired")
        local slotCount = count_registered_slots()
        local actorCount = count_trophy_actors()
        return string.format(
            "%s v24: holo=%s holoNat=%s achi=%s restore=%s change=%s E=%d/%d CM=%d P=%d/%d M=%d T=%d A=%d"
            .. " BN=%d TS=%d L=%d MPS=%d BEN=%d S=%s TInit=%d THook=%d TResp=%d TSweep=%d TActors=%d TMap=%s"
            .. " slots=%d polls=%d/%d done=%s/%s champ=%d/%s Err=%d",
            TAG,
            tostring(state.holo_hook),
            tostring(state.holo_native_hook),
            tostring(state.achi_hook),
            tostring(state.restore_hook), tostring(state.change_hook),
            state.entries_unlocked, state.entries_unlocked + state.entries_unlock_fail,
            state.entries_cm_unlock,
            state.perks, #TABLE_IDS() * 8,
            state.masteries, state.table_prog,
            state.achievements_save, state.brandnew_cleared,
            state.trophy_seen, state.leagues,
            state.match_progress_save, state.benefits_set,
            tostring(state.saved),
            state.trophy_initial_swaps, state.trophy_hook_swaps,
            state.trophy_respawned, state.trophy_sweep_fixes,
            actorCount, tostring(trophyMapReady),
            slotCount, pollCount, MAX_POLLS,
            tostring(initDone), tostring(trophyDataDone),
            state.champ_set, tostring(state.champ_done),
            #state.errors
        )
    end)
end)

pcall(function()
    RegisterCommand("maxall_run", function()
        V("maxall_run command fired")
        state.ran = false
        pcall(run_maxall)
        return "re-ran maxall v24"
    end)
end)

pcall(function()
    RegisterCommand("maxall_trophy_fix", function()
        if not trophyMapReady then pcall(build_trophy_map) end
        local count = fix_all_trophies()
        return TAG .. ": trophy fix=" .. count
            .. " (swaps=" .. state.trophy_initial_swaps
            .. " respawned=" .. state.trophy_respawned
            .. " hooks=" .. state.trophy_hook_swaps .. ")"
    end)
end)

pcall(function()
    RegisterCommand("maxall_trophy_verify", function()
        -- Check wall slot actors (the actual visual state)
        local tw = nil
        pcall(function() tw = FindFirstOf("BP_Hub_TrophyWall_C_C") end)
        if not tw then return "no trophy wall" end
        local slots = nil
        pcall(function() slots = tw:Get("TrophySlots") end)
        if not slots then return "no trophy slots" end

        local phys, holo, noActor, withGrab = 0, 0, 0, 0
        for i = 1, #slots do
            pcall(function()
                local csr = slots[i]:Get("CollectibleSlotRoot")
                if not csr then noActor = noActor + 1; return end
                local sa = csr:Get("SpawnedActor")
                if not sa then noActor = noActor + 1; return end
                local cls = sa:GetClass():GetName()
                if cls:match("Holo") then
                    holo = holo + 1
                else
                    phys = phys + 1
                    pcall(function()
                        local gc = sa:Get("GrabComponent")
                        if gc then withGrab = withGrab + 1 end
                    end)
                end
            end)
        end
        return string.format("physical=%d (grab=%d) holo=%d noActor=%d total=%d map=%s",
            phys, withGrab, holo, noActor, #slots, tostring(trophyMapReady))
    end)
end)

-- ============================================================================
-- GLOBAL EXPORTS — callable by other mods (e.g. PFX_ModMenu)
-- ============================================================================
PFX_Max = nil  -- will be set below
_G.PFX_Max = {
    fix_trophies = fix_all_trophies,
    run          = run_maxall,
    max_perks    = max_perks,
}
Log(TAG .. ": PFX_Max global exported")

Log(TAG .. ": v24 loaded — hooks: holo=" .. tostring(state.holo_hook)
    .. " holoPatch=" .. tostring(state.holo_native_hook)
    .. " achi=" .. tostring(state.achi_hook)
    .. " restore=" .. tostring(state.restore_hook)
    .. " change=" .. tostring(state.change_hook))
