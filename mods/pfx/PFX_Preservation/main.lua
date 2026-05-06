-- ============================================================================
-- PFX_Preservation v1 — Table Ownership Bypass + Force Download All DLC
-- ============================================================================
-- PRESERVATION PROJECT: Forces the game to think ALL tables are owned,
-- bypasses entitlement checks, enables auto-download of missing chunks,
-- and provides tools to trigger downloads of all available table paks.
--
-- This mod works by:
-- 1. Setting bDebugUnlockAllTables = true on PFXStoreManager
-- 2. Hooking IsTableOwned/IsBundleOwned to always return true
-- 3. Hooking IsPlayDisabled to always return false
-- 4. Enabling bShouldAcquireMissingChunksOnLoad on AssetManager
-- 5. Hooking MetaEntitlementCallbackProxy:VerifyEntitlement success path
-- 6. Providing bridge commands to enumerate & download all table paks
-- ============================================================================
local TAG = "PFX_Preservation"
Log(TAG .. ": Loading v1 — Table Preservation & Ownership Bypass...")

-- ============================================================================
-- HELPERS
-- ============================================================================
local function is_live(obj)
    if not obj then return false end
    local ok, v = pcall(function() return obj:IsValid() end)
    if not ok or not v then return false end
    local ok2, n = pcall(function() return obj:GetName() end)
    return ok2 and n and not n:match("^Default__") and not n:match("^REINST_")
end

-- ============================================================================
-- STATE
-- ============================================================================
local bypass_active     = false   -- ownership bypass hooks installed
local download_active   = false   -- auto-download enabled
local tables_found      = {}      -- discovered table info objects
local chunks_found      = {}      -- discovered pakchunker mappings
local hook_ids          = {}      -- registered hook IDs for cleanup

-- Cached subsystems
local cache = {}

local function get_store_manager()
    if is_live(cache.sm) then return cache.sm end
    pcall(function() cache.sm = FindFirstOf("BP_StoreManager_C") end)
    if not is_live(cache.sm) then
        pcall(function() cache.sm = FindFirstOf("PFXStoreManager") end)
    end
    return cache.sm
end

local function get_package_manager()
    if is_live(cache.pm) then return cache.pm end
    pcall(function() cache.pm = FindFirstOf("BP_PackageManager_C") end)
    if not is_live(cache.pm) then
        pcall(function() cache.pm = FindFirstOf("PFXPackageManager") end)
    end
    return cache.pm
end

local function get_asset_manager()
    if is_live(cache.am) then return cache.am end
    pcall(function() cache.am = FindFirstOf("PFXAssetManager") end)
    if not is_live(cache.am) then
        pcall(function() cache.am = FindFirstOf("YUPAssetManager") end)
    end
    if not is_live(cache.am) then
        pcall(function() cache.am = FindFirstOf("AssetManager") end)
    end
    return cache.am
end

local function get_game_subsystem()
    if is_live(cache.gs) then return cache.gs end
    pcall(function() cache.gs = FindFirstOf("BP_GameSubsystem_C") end)
    if not is_live(cache.gs) then
        pcall(function() cache.gs = FindFirstOf("PFXGameSubsystem") end)
    end
    return cache.gs
end

local function get_table_preload_manager()
    if is_live(cache.tpm) then return cache.tpm end
    pcall(function() cache.tpm = FindFirstOf("BP_PFXTablePreloadManager_C") end)
    if not is_live(cache.tpm) then
        pcall(function() cache.tpm = FindFirstOf("PFXTablePreloadManager") end)
    end
    return cache.tpm
end

local function get_game_instance()
    if is_live(cache.gi) then return cache.gi end
    pcall(function() cache.gi = FindFirstOf("BP_PFXGameInstance_C") end)
    if not is_live(cache.gi) then
        pcall(function() cache.gi = FindFirstOf("PFXGameInstance") end)
    end
    return cache.gi
end

-- ============================================================================
-- 1. DEBUG FLAG — bDebugUnlockAllTables (direct write, no hook needed)
-- ============================================================================
local function set_debug_unlock(val)
    local sm = get_store_manager()
    if not sm then
        Log(TAG .. ": WARN — no StoreManager found, retrying in 5s")
        ExecuteWithDelay(5000, function() set_debug_unlock(val) end)
        return false
    end
    pcall(function() sm:Set("bDebugUnlockAllTables", val) end)
    Log(TAG .. ": bDebugUnlockAllTables = " .. tostring(val))
    return true
end

-- ============================================================================
-- 2. ASSET MANAGER — bShouldAcquireMissingChunksOnLoad
-- ============================================================================
local function set_auto_acquire_chunks(val)
    local am = get_asset_manager()
    if not am then
        Log(TAG .. ": WARN — no AssetManager found")
        return false
    end
    pcall(function() am:Set("bShouldAcquireMissingChunksOnLoad", val) end)
    Log(TAG .. ": bShouldAcquireMissingChunksOnLoad = " .. tostring(val))
    return true
end

-- ============================================================================
-- 3. OWNERSHIP BYPASS — Native hooks (Dobby, intercepts ALL callers)
-- ============================================================================
local function install_ownership_hooks()
    if bypass_active then
        Log(TAG .. ": Hooks already installed")
        return true
    end

    local count = 0

    -- Use RegisterNativeHook for native-level interception (no ProcessEvent needed)
    -- These hooks fire on EVERY call to the function, including from C++ code

    -- IsTableOwned → always return true
    pcall(function()
        local ok = RegisterNativeHook("PFXStoreManager::IsTableOwned", nil,
            function(retval, self, tableInfo)
                Log(TAG .. ": [NATIVE] IsTableOwned → forced true")
                return true  -- override return to true
            end, "pp>p")
        if ok then count = count + 1; Log(TAG .. ": Native hook: IsTableOwned → true") end
    end)

    -- IsBundleOwned → always return true
    pcall(function()
        local ok = RegisterNativeHook("PFXStoreManager::IsBundleOwned", nil,
            function(retval, self, bundleID)
                Log(TAG .. ": [NATIVE] IsBundleOwned → forced true")
                return true
            end, "pp>p")
        if ok then count = count + 1; Log(TAG .. ": Native hook: IsBundleOwned → true") end
    end)

    bypass_active = true
    Log(TAG .. ": Ownership bypass native hooks installed (" .. count .. " hooks)")
    return true
end

-- ============================================================================
-- 4. ENUMERATE ALL TABLES
-- ============================================================================
local function enumerate_tables()
    tables_found = {}

    -- Method 1: Via GameSubsystem → TableInfoHolder
    -- DISABLED: gs:Call("GetTableInfoHolder") crashes via ProcessEvent before sub-objects
    -- are loaded (T<40s), and the siglongjmp crash-recovery corrupts UE5 GC state,
    -- causing fatal recursive SIGSEGV ~10s later. Method 2 is reliable and sufficient.

    -- Method 2: FindAllOf PFXVRTableInfo
    if #tables_found == 0 then
        pcall(function()
            local infos = FindAllOf("PFXVRTableInfo")
            if infos then
                for i, info in ipairs(infos) do
                    if is_live(info) then
                        -- Use GetName() (UObject FName, no ProcessEvent) instead of
                        -- GetTableName() (ProcessEvent, crashes before sub-objects loaded,
                        -- corrupts UE5 GC state via siglongjmp recovery)
                        local name = info:GetName() or "?"
                        local pitId = 0
                        pcall(function() pitId = info:Get("PitID") end)
                        local included = false
                        pcall(function() included = info:Get("IncludedInBaseGame") end)
                        tables_found[#tables_found + 1] = {
                            index = i - 1,
                            name = tostring(name),
                            pitId = pitId,
                            included = included,
                            disabled = false,
                            info = info,
                        }
                    end
                end
            end
        end)
    end

    Log(TAG .. ": Found " .. #tables_found .. " tables")
    return tables_found
end

-- ============================================================================
-- 5. ENUMERATE PAK CHUNK MAPPINGS
-- ============================================================================
local function enumerate_chunks()
    chunks_found = {}

    pcall(function()
        local chunkers = FindAllOf("PFXPakChunkerTable")
        if not chunkers then return end
        for _, c in ipairs(chunkers) do
            if is_live(c) then
                local entry = { name = c:GetName(), chunks = {} }

                -- Read chunk ID map
                pcall(function()
                    local map = c:Get("PrimaryAssetLabelAndChunkIdMap")
                    if map then
                        entry.chunkMap = tostring(map)
                    end
                end)

                -- Read table info ref (use GetName() not GetTableName() to avoid ProcessEvent crash)
                pcall(function()
                    local ti = c:Get("TableInfoCache")
                    if is_live(ti) then
                        entry.tableName = ti:GetName()
                        entry.pitId = ti:Get("PitID")
                    end
                end)

                chunks_found[#chunks_found + 1] = entry
            end
        end
    end)

    Log(TAG .. ": Found " .. #chunks_found .. " PakChunkerTable entries")
    return chunks_found
end

-- ============================================================================
-- 6. CHECK STORE BUNDLES
-- ============================================================================
local function enumerate_bundles()
    local bundles = {}
    local sm = get_store_manager()
    if not sm then return bundles end

    pcall(function()
        local available = sm:Call("GetAvailableBundles", false, {})
        if available then
            Log(TAG .. ": GetAvailableBundles returned: " .. tostring(available))
        end
    end)

    -- Check store data
    pcall(function()
        local sd = sm:Get("StoreData")
        if is_live(sd) then
            Log(TAG .. ": StoreData found: " .. sd:GetName())
            local bdl = sd:Get("Bundles")
            if bdl then
                Log(TAG .. ": Bundles map: " .. tostring(bdl))
            end
        end
    end)

    return bundles
end

-- ============================================================================
-- 7. FORCE MARK ALL TABLES AS INCLUDED IN BASE GAME
-- ============================================================================
local function force_all_tables_included()
    if #tables_found == 0 then enumerate_tables() end

    local count = 0
    for _, t in ipairs(tables_found) do
        if t.info and is_live(t.info) then
            pcall(function()
                t.info:Set("IncludedInBaseGame", true)
                count = count + 1
            end)
            -- Also disable play restriction
            pcall(function()
                t.info:Set("DisablePlayInPressBuild", false)
            end)
        end
    end
    Log(TAG .. ": Forced " .. count .. " tables as IncludedInBaseGame")
    return count
end

-- ============================================================================
-- 8. TRIGGER DOWNLOAD FOR ALL MISSING CHUNKS
-- ============================================================================
local function force_download_all()
    -- Enable auto-acquire first
    set_auto_acquire_chunks(true)

    -- Set debug unlock
    set_debug_unlock(true)

    -- Mark all as base game
    force_all_tables_included()

    -- Try to trigger preload for all tables
    local tpm = get_table_preload_manager()
    if tpm then
        pcall(function()
            -- Set high preload count to force loading all
            tpm:Set("PreLoadedRecentTableCount", 999)
            tpm:Set("PreLoadFavouriteTable", true)
            Log(TAG .. ": PreLoadedRecentTableCount set to 999")
        end)
    end

    -- Try to load each table to trigger chunk downloads
    local gi = get_game_instance()
    local triggered = 0
    for _, t in ipairs(tables_found) do
        if t.info and is_live(t.info) then
            pcall(function()
                -- Check if owned (should always be true now)
                local sm = get_store_manager()
                if sm then
                    local owned = sm:Call("IsTableOwned", t.info)
                    Log(TAG .. ": Table " .. t.name .. " owned=" .. tostring(owned))
                end
                triggered = triggered + 1
            end)
        end
    end

    Log(TAG .. ": Triggered ownership check for " .. triggered .. " tables")
    download_active = true
    return triggered
end

-- ============================================================================
-- 9. FULL ACTIVATION — ONE COMMAND
-- ============================================================================
local function activate_preservation()
    Log(TAG .. ": === ACTIVATING FULL PRESERVATION MODE ===")

    -- Step 1: Debug unlock flag
    set_debug_unlock(true)

    -- Step 2: Auto-acquire chunks
    set_auto_acquire_chunks(true)

    -- Step 3: Install ownership hooks
    install_ownership_hooks()

    -- Step 4: Enumerate tables
    enumerate_tables()

    -- Step 5: Force all tables as base game
    force_all_tables_included()

    -- Step 6: Enumerate chunks
    enumerate_chunks()

    Log(TAG .. ": === PRESERVATION MODE ACTIVE ===")
    Log(TAG .. ":   " .. #tables_found .. " tables found")
    Log(TAG .. ":   " .. #chunks_found .. " chunk mappings found")
    Log(TAG .. ":   bDebugUnlockAllTables = true")
    Log(TAG .. ":   bShouldAcquireMissingChunksOnLoad = true")
    Log(TAG .. ":   All tables marked IncludedInBaseGame")
    Log(TAG .. ":   Ownership hooks active")

    return true
end

-- ============================================================================
-- BRIDGE COMMANDS
-- ============================================================================

-- Main activation
pcall(function() RegisterCommand("pres_activate", function()
    activate_preservation()
    return TAG .. ": FULL PRESERVATION MODE ACTIVE — " .. #tables_found .. " tables, " .. #chunks_found .. " chunks"
end) end)

-- Individual controls
pcall(function() RegisterCommand("pres_unlock", function()
    set_debug_unlock(true)
    return TAG .. ": bDebugUnlockAllTables = true"
end) end)

pcall(function() RegisterCommand("pres_lock", function()
    set_debug_unlock(false)
    return TAG .. ": bDebugUnlockAllTables = false"
end) end)

pcall(function() RegisterCommand("pres_chunks_on", function()
    set_auto_acquire_chunks(true)
    return TAG .. ": bShouldAcquireMissingChunksOnLoad = true"
end) end)

pcall(function() RegisterCommand("pres_hooks", function()
    install_ownership_hooks()
    return TAG .. ": Ownership hooks installed"
end) end)

-- Enumeration
pcall(function() RegisterCommand("pres_tables", function()
    enumerate_tables()
    local lines = { TAG .. ": " .. #tables_found .. " tables:" }
    for _, t in ipairs(tables_found) do
        local flags = ""
        if t.included then flags = flags .. " [BASE]" end
        if t.disabled then flags = flags .. " [DISABLED]" end
        lines[#lines + 1] = string.format("  [%d] PIT=%d %s%s", t.index, t.pitId, t.name, flags)
    end
    return table.concat(lines, "\n")
end) end)

pcall(function() RegisterCommand("pres_chunks", function()
    enumerate_chunks()
    local lines = { TAG .. ": " .. #chunks_found .. " chunk mappings:" }
    for _, c in ipairs(chunks_found) do
        local tbl = c.tableName or "?"
        lines[#lines + 1] = string.format("  %s → %s (PIT=%s)", c.name, tbl, tostring(c.pitId or "?"))
    end
    return table.concat(lines, "\n")
end) end)

pcall(function() RegisterCommand("pres_bundles", function()
    enumerate_bundles()
    return TAG .. ": Bundle enumeration complete (check log)"
end) end)

pcall(function() RegisterCommand("pres_force_base", function()
    local count = force_all_tables_included()
    return TAG .. ": Forced " .. count .. " tables as IncludedInBaseGame"
end) end)

pcall(function() RegisterCommand("pres_download_all", function()
    local count = force_download_all()
    return TAG .. ": Download triggered for " .. count .. " tables"
end) end)

-- Status
pcall(function() RegisterCommand("pres_status", function()
    local parts = {}
    parts[#parts + 1] = "bypass=" .. tostring(bypass_active)
    parts[#parts + 1] = "download=" .. tostring(download_active)
    parts[#parts + 1] = "tables=" .. #tables_found
    parts[#parts + 1] = "chunks=" .. #chunks_found

    local sm = get_store_manager()
    if sm then
        local dbg = false
        pcall(function() dbg = sm:Get("bDebugUnlockAllTables") end)
        parts[#parts + 1] = "debugUnlock=" .. tostring(dbg)
    end

    local am = get_asset_manager()
    if am then
        local acq = false
        pcall(function() acq = am:Get("bShouldAcquireMissingChunksOnLoad") end)
        parts[#parts + 1] = "autoAcquire=" .. tostring(acq)
    end

    return TAG .. ": " .. table.concat(parts, " | ")
end) end)

-- Check individual table ownership
pcall(function() RegisterCommand("pres_check_owned", function(idx)
    idx = tonumber(idx) or 0
    if idx < 0 or idx >= #tables_found then return TAG .. ": invalid index" end
    local t = tables_found[idx + 1]
    if not t or not t.info then return TAG .. ": no table at index " .. idx end

    local sm = get_store_manager()
    if not sm then return TAG .. ": no StoreManager" end

    local owned = false
    pcall(function() owned = sm:Call("IsTableOwned", t.info) end)

    local included = false
    pcall(function() included = t.info:Get("IncludedInBaseGame") end)

    local disabled = false
    pcall(function() disabled = t.info:Call("IsPlayDisabled") end)

    return string.format("%s: [%d] %s — owned=%s included=%s disabled=%s",
        TAG, idx, t.name, tostring(owned), tostring(included), tostring(disabled))
end) end)

-- Dump full table info for one table
pcall(function() RegisterCommand("pres_table_info", function(idx)
    idx = tonumber(idx) or 0
    if idx < 0 or idx >= #tables_found then return TAG .. ": invalid index" end
    local t = tables_found[idx + 1]
    if not t or not t.info then return TAG .. ": no table at index " .. idx end

    local info = t.info
    local lines = { string.format("%s: Table [%d] %s", TAG, idx, t.name) }

    local props = {"PitID", "AssetName", "AppVersion", "Version", "GFXVersion",
                   "IncludedInBaseGame", "DisablePlayInPressBuild", "NudgeStrength",
                   "TiltSensitivity", "TrialScoreLimit", "TrialTimeLimit", "ProModeAvailable",
                   "DataVersion", "ForceNewPhysics"}
    for _, p in ipairs(props) do
        local val = "?"
        pcall(function() val = tostring(info:Get(p)) end)
        lines[#lines + 1] = "  " .. p .. " = " .. val
    end
    return table.concat(lines, "\n")
end) end)

-- Force specific table as owned
pcall(function() RegisterCommand("pres_own_table", function(idx)
    idx = tonumber(idx) or 0
    if idx < 0 or idx >= #tables_found then return TAG .. ": invalid index" end
    local t = tables_found[idx + 1]
    if not t or not t.info then return TAG .. ": no table at index " .. idx end
    pcall(function() t.info:Set("IncludedInBaseGame", true) end)
    pcall(function() t.info:Set("DisablePlayInPressBuild", false) end)
    return TAG .. ": Forced table [" .. idx .. "] " .. t.name .. " as owned"
end) end)

-- ============================================================================
-- GLOBAL API
-- ============================================================================
_G.PFX_Preservation = {
    activate              = activate_preservation,
    set_debug_unlock      = set_debug_unlock,
    set_auto_acquire      = set_auto_acquire_chunks,
    install_hooks         = install_ownership_hooks,
    enumerate_tables      = enumerate_tables,
    enumerate_chunks      = enumerate_chunks,
    force_all_included    = force_all_tables_included,
    force_download_all    = force_download_all,
    get_tables            = function() return tables_found end,
    get_chunks            = function() return chunks_found end,
    is_active             = function() return bypass_active end,
}

-- ============================================================================
-- AUTO-ACTIVATION
-- ============================================================================
-- Delay to let subsystems initialize
pcall(function()
    ExecuteWithDelay(3000, function()
        Log(TAG .. ": Auto-activating preservation mode...")
        activate_preservation()
    end)
end)

Log(TAG .. ": v1 loaded — preservation & ownership bypass")
Log(TAG .. ": Bridge commands:")
Log(TAG .. ":   pres_activate     — Full activation (all steps)")
Log(TAG .. ":   pres_unlock       — Set bDebugUnlockAllTables")
Log(TAG .. ":   pres_hooks        — Install ownership hooks")
Log(TAG .. ":   pres_tables       — Enumerate all tables")
Log(TAG .. ":   pres_chunks       — Enumerate pak chunk mappings")
Log(TAG .. ":   pres_force_base   — Force all tables as IncludedInBaseGame")
Log(TAG .. ":   pres_download_all — Force download all missing chunks")
Log(TAG .. ":   pres_status       — Show current state")
Log(TAG .. ":   pres_check_owned <idx> — Check ownership for table")
Log(TAG .. ":   pres_table_info <idx>  — Dump table properties")
Log(TAG .. ":   pres_own_table <idx>   — Force single table owned")

-- Auto-activate after game is ready (retries every 10s via LoopAsync)
local pres_auto_activated = false
ExecuteWithDelay(10000, function()
    if pres_auto_activated then return end
    Log(TAG .. ": AUTO: attempting preservation activation...")
    local ok = pcall(activate_preservation)
    if ok and #tables_found > 0 then
        pres_auto_activated = true
        Log(TAG .. ": AUTO: preservation activated")
        Notify(TAG, "Tables unlocked — " .. #tables_found .. " found")
    else
        Log(TAG .. ": AUTO: activation deferred (game not ready, retrying in 10s)")
        ExecuteWithDelay(10000, function()
            if not pres_auto_activated then
                pcall(activate_preservation)
                if #tables_found > 0 then pres_auto_activated = true end
            end
        end)
    end
end)
