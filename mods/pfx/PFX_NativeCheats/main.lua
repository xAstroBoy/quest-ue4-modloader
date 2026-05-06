-- ============================================================================
-- PFX_NativeCheats v2 — Full Native Cheat Manager + YUP Engine + Table/Input/VR Control
-- ============================================================================
-- Exposes EVERY reflected CheatManager function as callable actions,
-- and provides a Lua API layer for YUP engine control.
--
-- Hierarchy (all reflected):
--   CheatManager (UE base)
--     └─ YUPCheatManager  → ToggleROMDebug()
--         └─ PFXCheatManager → 40+ PFXDebug_* functions
--             └─ BP_CheatManager_C → 20+ PFXDEBUG_* blueprint functions
--
-- CheatManager lives on PlayerController at offset 0x0400.
-- If not created yet, we call EnableCheats() to instantiate it.
-- ============================================================================
local TAG = "PFX_NativeCheats"
Log(TAG .. ": Loading v2...")

-- ============================================================================
-- HELPERS
-- ============================================================================
local function is_live(obj)
    if not obj then return false end
    local ok, valid = pcall(function() return obj:IsValid() end)
    if not ok or not valid then return false end
    local ok2, name = pcall(function() return obj:GetName() end)
    return ok2 and name and not name:match("^Default__") and not name:match("^REINST_")
end

-- ============================================================================
-- CHEAT MANAGER ACQUISITION
-- ============================================================================
local cm_cache = nil

-- Force-enable cheats on GameMode so EnableCheats() works in shipping builds
local function force_enable_cheats_flag()
    -- UE5 EnableCheats() checks GameMode.bCheatsEnabled or GIsEditor
    -- In shipping builds, we must set the flag manually
    pcall(function()
        local gm = FindFirstOf("GameModeBase")
        if is_live(gm) then
            pcall(function() gm:Set("bCheatsEnabled", true) end)
        end
    end)
    pcall(function()
        local gm = FindFirstOf("GameMode")
        if is_live(gm) then
            pcall(function() gm:Set("bCheatsEnabled", true) end)
        end
    end)
end

local function get_cheat_manager()
    if cm_cache and is_live(cm_cache) then return cm_cache end
    cm_cache = nil

    -- Step 1: Try FindFirstOf for existing CheatManager instances
    pcall(function()
        local cm = FindFirstOf("BP_CheatManager_C")
        if is_live(cm) then cm_cache = cm end
    end)
    if cm_cache then return cm_cache end

    pcall(function()
        local cm = FindFirstOf("PFXCheatManager")
        if is_live(cm) then cm_cache = cm end
    end)
    if cm_cache then return cm_cache end

    pcall(function()
        local cm = FindFirstOf("YUPCheatManager")
        if is_live(cm) then cm_cache = cm end
    end)
    if cm_cache then return cm_cache end

    -- Step 2: Try via PlayerController.CheatManager property
    local pc = nil
    pcall(function()
        pc = FindFirstOf("BP_PlayerController_C")
        if not is_live(pc) then pc = FindFirstOf("PFXPlayerController") end
        if not is_live(pc) then pc = FindFirstOf("PlayerController") end
    end)
    if is_live(pc) then
        pcall(function()
            local cm = pc:Get("CheatManager")
            if is_live(cm) then cm_cache = cm end
        end)
        if cm_cache then return cm_cache end
    end

    -- Step 3: Force bCheatsEnabled + EnableCheats
    if is_live(pc) then
        Log(TAG .. ": No CheatManager found — forcing cheats enabled...")
        force_enable_cheats_flag()
        pcall(function() pc:Call("EnableCheats") end)
        pcall(function()
            local cm = pc:Get("CheatManager")
            if is_live(cm) then
                cm_cache = cm
                Log(TAG .. ": CheatManager via EnableCheats: " .. cm:GetFullName())
            end
        end)
        if cm_cache then return cm_cache end
    end

    -- Step 4: Manual ConstructObject with BP_CheatManager_C class
    if is_live(pc) then
        Log(TAG .. ": EnableCheats failed — trying ConstructObject...")
        -- Try BP_CheatManager_C first (most-derived, has all PFXDEBUG_ functions)
        pcall(function()
            local cls = FindClass("BP_CheatManager_C")
            if cls then
                local cm = ConstructObject(cls, pc)
                if is_live(cm) then
                    pc:Set("CheatManager", cm)
                    cm_cache = cm
                    Log(TAG .. ": CheatManager via ConstructObject(BP_CheatManager_C): " .. cm:GetFullName())
                end
            end
        end)
        if cm_cache then return cm_cache end

        -- Fallback: PFXCheatManager (C++ class, has PFXDebug_ functions)
        pcall(function()
            local cls = FindClass("PFXCheatManager")
            if cls then
                local cm = ConstructObject(cls, pc)
                if is_live(cm) then
                    pc:Set("CheatManager", cm)
                    cm_cache = cm
                    Log(TAG .. ": CheatManager via ConstructObject(PFXCheatManager): " .. cm:GetFullName())
                end
            end
        end)
        if cm_cache then return cm_cache end

        -- Last fallback: YUPCheatManager
        pcall(function()
            local cls = FindClass("YUPCheatManager")
            if cls then
                local cm = ConstructObject(cls, pc)
                if is_live(cm) then
                    pc:Set("CheatManager", cm)
                    cm_cache = cm
                    Log(TAG .. ": CheatManager via ConstructObject(YUPCheatManager): " .. cm:GetFullName())
                end
            end
        end)
    end

    if not cm_cache then
        Log(TAG .. ": FAILED to create CheatManager by any method")
    end
    return cm_cache
end

-- ============================================================================
-- YUP ENGINE ACCESS LAYER
-- ============================================================================
local yup_cache = {}

local function get_game_subsystem()
    if yup_cache.gs and is_live(yup_cache.gs) then return yup_cache.gs end
    pcall(function()
        local gs = FindFirstOf("BP_GameSubsystem_C")
        if not is_live(gs) then gs = FindFirstOf("PFXGameSubsystem") end
        if not is_live(gs) then gs = FindFirstOf("YUPGameSubsystem") end
        if is_live(gs) then yup_cache.gs = gs end
    end)
    return yup_cache.gs
end

local function get_game_handler()
    if yup_cache.gh and is_live(yup_cache.gh) then return yup_cache.gh end
    pcall(function()
        local gs = get_game_subsystem()
        if gs then
            yup_cache.gh = gs:Call("GetGameHandler")
        end
    end)
    if not yup_cache.gh then
        pcall(function()
            local gh = FindFirstOf("PFXGameHandler")
            if is_live(gh) then yup_cache.gh = gh end
        end)
    end
    return yup_cache.gh
end

local function get_game_info()
    if yup_cache.gi and is_live(yup_cache.gi) then return yup_cache.gi end
    pcall(function()
        local gh = get_game_handler()
        if gh then yup_cache.gi = gh:Get("m_GameInfo") end
    end)
    if not yup_cache.gi then
        pcall(function()
            local gi = FindFirstOf("PFXPinballGameInfo")
            if not is_live(gi) then gi = FindFirstOf("YUPPinballGameInfo") end
            if is_live(gi) then yup_cache.gi = gi end
        end)
    end
    return yup_cache.gi
end

local function get_game_result()
    if yup_cache.gr and is_live(yup_cache.gr) then return yup_cache.gr end
    pcall(function()
        local gh = get_game_handler()
        if gh then yup_cache.gr = gh:Get("m_GameResult") end
    end)
    return yup_cache.gr
end

local function get_table_reference()
    if yup_cache.tr and is_live(yup_cache.tr) then return yup_cache.tr end
    pcall(function()
        local gs = get_game_subsystem()
        if gs then yup_cache.tr = gs:Get("m_TableReference") end
    end)
    if not yup_cache.tr then
        pcall(function()
            local tr = FindFirstOf("PFXTableReference")
            if not is_live(tr) then tr = FindFirstOf("YUPTableReference") end
            if is_live(tr) then yup_cache.tr = tr end
        end)
    end
    return yup_cache.tr
end

local function get_gameflow_manager()
    if yup_cache.gfm and is_live(yup_cache.gfm) then return yup_cache.gfm end
    pcall(function()
        local gfm = FindFirstOf("BP_GameflowManager_C")
        if not is_live(gfm) then gfm = FindFirstOf("PFXGameflowManager") end
        if is_live(gfm) then yup_cache.gfm = gfm end
    end)
    return yup_cache.gfm
end

local function get_game_instance()
    if yup_cache.gii and is_live(yup_cache.gii) then return yup_cache.gii end
    pcall(function()
        local gi = FindFirstOf("BP_PFXGameInstance_C")
        if not is_live(gi) then gi = FindFirstOf("PFXGameInstance") end
        if is_live(gi) then yup_cache.gii = gi end
    end)
    return yup_cache.gii
end

local function invalidate_yup_cache()
    yup_cache = {}
    cm_cache = nil
end

-- ============================================================================
-- SAFE CHEAT CALL HELPER
-- ============================================================================
local function call_cheat(func_name, ...)
    local cm = get_cheat_manager()
    if not cm then return false, "no CheatManager" end
    local args = table.pack(...)
    local ok, err = pcall(function() cm:Call(func_name, table.unpack(args, 1, args.n)) end)
    if ok then
        Log(TAG .. ": " .. func_name .. " OK")
        return true, func_name .. " executed"
    else
        Log(TAG .. ": " .. func_name .. " FAILED: " .. tostring(err))
        return false, func_name .. " failed: " .. tostring(err)
    end
end

-- ============================================================================
-- CHEAT ACTION DEFINITIONS — every reflected function from SDK dump
-- ============================================================================
-- Categories: UE, YUP, PFX, BP
-- Each entry: { id, label, category, fn }
local CHEAT_ACTIONS = {}

-- Helper to add a cheat
local function add(id, label, cat, fn)
    CHEAT_ACTIONS[#CHEAT_ACTIONS + 1] = { id = id, label = label, category = cat, fn = fn }
end

-- ── YUPCheatManager ─────────────────────────────────────────────────────────
add("toggle_rom_debug", "Toggle ROM Debug", "YUP", function()
    return call_cheat("ToggleROMDebug")
end)

-- ── PFXCheatManager — Inventory ─────────────────────────────────────────────
add("unlock_all_tables", "Unlock All Tables", "PFX_Inventory", function()
    return call_cheat("PFXDebug_UserInventory_UnLockAllTables")
end)
add("relock_all_tables", "Re-Lock All Tables", "PFX_Inventory", function()
    return call_cheat("PFXDebug_UserInventory_ReLockAllTables")
end)

-- ── PFXCheatManager — Table Perks ───────────────────────────────────────────
add("perk_max_all", "Max All Perks", "PFX_Perks", function()
    return call_cheat("PFXDebug_TablePerkMaxAll")
end)
add("perk_reset_all", "Reset All Perks", "PFX_Perks", function()
    return call_cheat("PFXDebug_TablePerkResetAll")
end)

-- ── PFXCheatManager — Table Mastery ─────────────────────────────────────────
add("mastery_max_all", "Max All Mastery", "PFX_Mastery", function()
    return call_cheat("PFXDebug_TableMasteryMaxAll")
end)
add("mastery_reset_all", "Reset All Mastery", "PFX_Mastery", function()
    return call_cheat("PFXDebug_TableMasteryResetAll")
end)

-- ── PFXCheatManager — Table Awards ──────────────────────────────────────────
add("awards_unlock_all", "Unlock All Awards", "PFX_Awards", function()
    return call_cheat("PFXDebug_TableAwardUnlockAll")
end)
add("awards_lock_all", "Lock All Awards", "PFX_Awards", function()
    return call_cheat("PFXDebug_TableAwardLockAll")
end)

-- ── PFXCheatManager — Collectibles ──────────────────────────────────────────
add("collectibles_unlock_all", "Unlock All Collectibles", "PFX_Collectibles", function()
    return call_cheat("PFXDebug_Collectibles_UnlockAll", true)
end)
add("collectibles_unlock_all_quiet", "Unlock All (No Brand New)", "PFX_Collectibles", function()
    return call_cheat("PFXDebug_Collectibles_UnlockAll", false)
end)
add("collectibles_lock_all", "Lock All Collectibles", "PFX_Collectibles", function()
    return call_cheat("PFXDebug_Collectibles_LockAll")
end)

-- ── PFXCheatManager — Achievements ──────────────────────────────────────────
add("achievements_unlock_all", "Unlock All Achievements", "PFX_Achievements", function()
    return call_cheat("PFXDebug_Achievement_UnlockAll")
end)

-- ── PFXCheatManager — Championship ──────────────────────────────────────────
add("championship_max_all", "Max All Championship", "PFX_Championship", function()
    return call_cheat("PFXDebug_Championship_MaxAll")
end)
add("championship_enable", "Enable Championship", "PFX_Championship", function()
    return call_cheat("PFXDebug_Championship_Enable", true)
end)
add("championship_disable", "Disable Championship", "PFX_Championship", function()
    return call_cheat("PFXDebug_Championship_Enable", false)
end)
add("championship_mastery_max", "Max Championship Mastery", "PFX_Championship", function()
    return call_cheat("PFXDebug_Championship_MasteryMaxAll")
end)
add("championship_mastery_reset", "Reset Championship Mastery", "PFX_Championship", function()
    return call_cheat("PFXDebug_Championship_MasteryResetAll")
end)

-- ── PFXCheatManager — Player ────────────────────────────────────────────────
add("profile_reset", "Reset Player Profile", "PFX_Player", function()
    return call_cheat("PFXDebug_PlayerProfile_Reset")
end)
add("onboarding_reset", "Reset Onboarding", "PFX_Player", function()
    return call_cheat("PFXDebug_Onboarding_Reset")
end)

-- ── PFXCheatManager — Performance ───────────────────────────────────────────
add("perf_table_isolation_on", "Table Isolation ON", "PFX_Performance", function()
    return call_cheat("PFXDebug_Performance_TableIsolation", true)
end)
add("perf_table_isolation_off", "Table Isolation OFF", "PFX_Performance", function()
    return call_cheat("PFXDebug_Performance_TableIsolation", false)
end)
add("perf_niagara_activate", "Activate Niagara Effects", "PFX_Performance", function()
    return call_cheat("PFXDebug_Performance_ActivateNiagaraEffects")
end)
add("perf_72hz", "Set 72Hz", "PFX_Performance", function()
    return call_cheat("PFXDebug_Performance_SetTargetDisplayFrequency", 72)
end)
add("perf_90hz", "Set 90Hz", "PFX_Performance", function()
    return call_cheat("PFXDebug_Performance_SetTargetDisplayFrequency", 90)
end)
add("perf_120hz", "Set 120Hz", "PFX_Performance", function()
    return call_cheat("PFXDebug_Performance_SetTargetDisplayFrequency", 120)
end)

-- ── PFXCheatManager — Pawn / Camera ─────────────────────────────────────────
add("pawn_toggle_capsule", "Toggle Capsule Visibility", "PFX_Pawn", function()
    return call_cheat("PFXDebug_Pawn_ToggleCapsuleVisibility")
end)
add("pawn_fade_0", "Camera Fade 0 (clear)", "PFX_Pawn", function()
    return call_cheat("PFXDebug_Pawn_SetCameraFade", 0.0)
end)
add("pawn_fade_50", "Camera Fade 50%", "PFX_Pawn", function()
    return call_cheat("PFXDebug_Pawn_SetCameraFade", 0.5)
end)
add("pawn_fade_100", "Camera Fade 100% (black)", "PFX_Pawn", function()
    return call_cheat("PFXDebug_Pawn_SetCameraFade", 1.0)
end)

-- ── PFXCheatManager — Audio ─────────────────────────────────────────────────
add("audio_test_440", "Audio Test Tone 440Hz", "PFX_Audio", function()
    return call_cheat("PFXDebug_Audio_TestTone", 440.0, 0.5)
end)
add("audio_test_1k", "Audio Test Tone 1kHz", "PFX_Audio", function()
    return call_cheat("PFXDebug_Audio_TestTone", 1000.0, 0.5)
end)

-- ── UE CheatManager (base) ──────────────────────────────────────────────────
add("ue_god", "God Mode", "UE_Base", function()
    return call_cheat("God")
end)
add("ue_ghost", "Ghost (noclip)", "UE_Base", function()
    return call_cheat("Ghost")
end)
add("ue_fly", "Fly Mode", "UE_Base", function()
    return call_cheat("Fly")
end)
add("ue_walk", "Walk Mode", "UE_Base", function()
    return call_cheat("Walk")
end)
add("ue_teleport", "Teleport", "UE_Base", function()
    return call_cheat("Teleport")
end)
add("ue_slomo_05", "Slomo 0.5x", "UE_Base", function()
    return call_cheat("Slomo", 0.5)
end)
add("ue_slomo_1", "Slomo 1.0x (normal)", "UE_Base", function()
    return call_cheat("Slomo", 1.0)
end)
add("ue_slomo_2", "Slomo 2.0x", "UE_Base", function()
    return call_cheat("Slomo", 2.0)
end)
add("ue_slomo_01", "Slomo 0.1x (ultra slow)", "UE_Base", function()
    return call_cheat("Slomo", 0.1)
end)
add("ue_players_only", "Players Only (freeze AI)", "UE_Base", function()
    return call_cheat("PlayersOnly")
end)
add("ue_toggle_debug_cam", "Toggle Debug Camera", "UE_Base", function()
    return call_cheat("ToggleDebugCamera")
end)
add("ue_freeze_1s", "Freeze Frame 1s", "UE_Base", function()
    return call_cheat("FreezeFrame", 1.0)
end)
add("ue_freeze_5s", "Freeze Frame 5s", "UE_Base", function()
    return call_cheat("FreezeFrame", 5.0)
end)
add("ue_destroy_target", "Destroy Target", "UE_Base", function()
    return call_cheat("DestroyTarget")
end)
add("ue_flush_log", "Flush Log", "UE_Base", function()
    return call_cheat("FlushLog")
end)

-- ── BP_CheatManager_C — Blueprint debug functions ───────────────────────────
add("bp_pin_wrist_menu", "Pin Wrist Menu", "BP_Debug", function()
    return call_cheat("PFXDEBUG_PinWristMenu", true)
end)
add("bp_unpin_wrist_menu", "Unpin Wrist Menu", "BP_Debug", function()
    return call_cheat("PFXDEBUG_PinWristMenu", false)
end)
add("bp_show_arms", "Show Arms", "BP_Debug", function()
    return call_cheat("PFXDEBUG_ShowArms", true)
end)
add("bp_hide_arms", "Hide Arms", "BP_Debug", function()
    return call_cheat("PFXDEBUG_ShowArms", false)
end)
add("bp_show_hand_collisions", "Show Hand Collisions", "BP_Debug", function()
    return call_cheat("PFXDEBUG_ShowHandCollisions", true)
end)
add("bp_hide_hand_collisions", "Hide Hand Collisions", "BP_Debug", function()
    return call_cheat("PFXDEBUG_ShowHandCollisions", false)
end)
add("bp_show_controller_pos", "Show Controller Position", "BP_Debug", function()
    return call_cheat("PFXDEBUG_ShowControllerPosition", true)
end)
add("bp_hide_controller_pos", "Hide Controller Position", "BP_Debug", function()
    return call_cheat("PFXDEBUG_ShowControllerPosition", false)
end)
add("bp_show_phys_handle", "Show Phys Handle Target", "BP_Debug", function()
    return call_cheat("PFXDEBUG_ShowPhysHandleTarget", true)
end)
add("bp_hide_phys_handle", "Hide Phys Handle Target", "BP_Debug", function()
    return call_cheat("PFXDEBUG_ShowPhysHandleTarget", false)
end)
add("bp_table_isolation_on", "Table Isolation ON", "BP_Debug", function()
    return call_cheat("PFXDEBUG_SetIsolationOnTable", true)
end)
add("bp_table_isolation_off", "Table Isolation OFF", "BP_Debug", function()
    return call_cheat("PFXDEBUG_SetIsolationOnTable", false)
end)
add("bp_isolation_mode_on", "Isolation Mode ON", "BP_Debug", function()
    return call_cheat("PFXDEBUG_SetIsolationModeActive", true)
end)
add("bp_isolation_mode_off", "Isolation Mode OFF", "BP_Debug", function()
    return call_cheat("PFXDEBUG_SetIsolationModeActive", false)
end)
add("bp_constraint_mode_on", "Constraint Mode ON", "BP_Debug", function()
    return call_cheat("PFXDEBUG_SetConstraintModeActive", true)
end)
add("bp_constraint_mode_off", "Constraint Mode OFF", "BP_Debug", function()
    return call_cheat("PFXDEBUG_SetConstraintModeActive", false)
end)
add("bp_hide_all_actors", "Hide All Actors", "BP_Debug", function()
    return call_cheat("PFXDEBUG_HideAllActors", true)
end)
add("bp_show_all_actors", "Show All Actors", "BP_Debug", function()
    return call_cheat("PFXDEBUG_HideAllActors", false)
end)
add("bp_hide_static_meshes", "Hide All Static Meshes", "BP_Debug", function()
    return call_cheat("PFXDEBUG_HideAllStaticMeshActors", true)
end)
add("bp_show_static_meshes", "Show All Static Meshes", "BP_Debug", function()
    return call_cheat("PFXDEBUG_HideAllStaticMeshActors", false)
end)
add("bp_hide_signs", "Hide All Signs", "BP_Debug", function()
    return call_cheat("PFXDEBUG_HideAllSignActors", true)
end)
add("bp_hide_lamps", "Hide All Lamps", "BP_Debug", function()
    return call_cheat("PFXDEBUG_HideAllLampActors", true)
end)
add("bp_hide_speakers", "Hide All Speakers", "BP_Debug", function()
    return call_cheat("PFXDEBUG_HideAllSpeakerActors", true)
end)
add("bp_passthrough_on", "Passthrough Layer ON", "BP_Debug", function()
    return call_cheat("PFXDEBUG_SetPassthroughLayerEnabled", true)
end)
add("bp_passthrough_off", "Passthrough Layer OFF", "BP_Debug", function()
    return call_cheat("PFXDEBUG_SetPassthroughLayerEnabled", false)
end)
add("bp_pawn_collision_on", "Pawn Collision Overlap ON", "BP_Debug", function()
    return call_cheat("PFXDEBUG_SetPawnCollisionOverlap", true)
end)
add("bp_pawn_collision_off", "Pawn Collision Overlap OFF", "BP_Debug", function()
    return call_cheat("PFXDEBUG_SetPawnCollisionOverlap", false)
end)
add("bp_reset_hmd", "Reset HMD Offset", "BP_Debug", function()
    return call_cheat("PFXDEBUG_ResetHmdOffset")
end)
add("bp_reset_orient", "Reset Orientation", "BP_Debug", function()
    return call_cheat("PFXDEBUG_ResetOrientation", true)
end)
add("bp_version", "Show Version", "BP_Debug", function()
    return call_cheat("PFXDEBUG_Version")
end)
add("bp_loading_screen", "Show Loading Screen", "BP_Debug", function()
    return call_cheat("PFXDEBUG_ShowLoadingScreen")
end)
add("bp_dissolve_on", "Dissolve Effect ON", "BP_Debug", function()
    return call_cheat("PFXDEBUG_SetDissolveEffectOnInventoryCollectibles", true)
end)
add("bp_dissolve_off", "Dissolve Effect OFF", "BP_Debug", function()
    return call_cheat("PFXDEBUG_SetDissolveEffectOnInventoryCollectibles", false)
end)
add("bp_toggle_mr_vr", "Toggle MR/VR", "BP_Debug", function()
    return call_cheat("ToggleMRVR")
end)
add("bp_depth_occlusion_0", "Depth Occlusion Type 0", "BP_Debug", function()
    return call_cheat("PFXDEBUG_SetDepthOcclusionEnabled", 0)
end)
add("bp_depth_occlusion_1", "Depth Occlusion Type 1", "BP_Debug", function()
    return call_cheat("PFXDEBUG_SetDepthOcclusionEnabled", 1)
end)
add("bp_depth_occlusion_2", "Depth Occlusion Type 2", "BP_Debug", function()
    return call_cheat("PFXDEBUG_SetDepthOcclusionEnabled", 2)
end)
add("bp_cabinet_mlod_on", "Cabinet Preview MLOD ON", "BP_Debug", function()
    return call_cheat("PFXDEBUG_SetCabinetPreviewMLOD", true)
end)
add("bp_cabinet_mlod_off", "Cabinet Preview MLOD OFF", "BP_Debug", function()
    return call_cheat("PFXDEBUG_SetCabinetPreviewMLOD", false)
end)
add("bp_table_iso_mode_on", "Table Isolation Mode ON", "BP_Debug", function()
    return call_cheat("PFXDEBUG_SetTableIsolationMode", true)
end)
add("bp_table_iso_mode_off", "Table Isolation Mode OFF", "BP_Debug", function()
    return call_cheat("PFXDEBUG_SetTableIsolationMode", false)
end)

-- ── YUP Engine — Game State / Flow ──────────────────────────────────────────
add("yup_get_score", "Get Current Score", "YUP_Engine", function()
    local gr = get_game_result()
    if not gr then return false, "no GameResult" end
    local score = ""
    pcall(function() score = gr:Call("GetScoreAsFormattedString") end)
    Log(TAG .. ": Score = " .. tostring(score))
    return true, "Score: " .. tostring(score)
end)
add("yup_get_game_state", "Get Game State", "YUP_Engine", function()
    local state = "unknown"
    pcall(function()
        local gs = FindFirstOf("GFO_PlayTable_C")
        if is_live(gs) then
            state = tostring(gs:Get("GameState"))
        end
    end)
    Log(TAG .. ": GameState = " .. state)
    return true, "GameState: " .. state
end)
add("yup_game_info_flags", "Dump GameInfo Flags", "YUP_Engine", function()
    local gi = get_game_info()
    if not gi then return false, "no GameInfo" end
    local flags = {}
    for i = 0, 31 do
        local ok, has = pcall(function() return gi:Call("GetFlag_bp", i) end)
        if ok and has then flags[#flags + 1] = "flag" .. i end
    end
    local msg = "Active flags: " .. (#flags > 0 and table.concat(flags, ",") or "none")
    Log(TAG .. ": " .. msg)
    return true, msg
end)
add("yup_set_flag", "Set GameInfo Flag 0", "YUP_Engine", function()
    local gi = get_game_info()
    if not gi then return false, "no GameInfo" end
    pcall(function() gi:Call("SetFlag_bp", 0) end)
    return true, "SetFlag_bp(0)"
end)
add("yup_remove_flag", "Remove GameInfo Flag 0", "YUP_Engine", function()
    local gi = get_game_info()
    if not gi then return false, "no GameInfo" end
    pcall(function() gi:Call("RemoveFlag_bp", 0) end)
    return true, "RemoveFlag_bp(0)"
end)

-- ── YUP Engine — Gameflow Control ───────────────────────────────────────────
add("yup_is_playing", "Is Playing On Table?", "YUP_Engine", function()
    local gs = get_game_subsystem()
    if not gs then return false, "no GameSubsystem" end
    local playing = false
    pcall(function() playing = gs:Call("IsPlayingOnTable") end)
    return true, "IsPlayingOnTable: " .. tostring(playing)
end)
add("yup_environment", "Get Environment", "YUP_Engine", function()
    local gfm = get_gameflow_manager()
    if not gfm then return false, "no GameflowManager" end
    local env = "unknown"
    pcall(function() env = tostring(gfm:Call("GetEnvironment")) end)
    return true, "Environment: " .. env
end)
add("yup_is_mr", "Is Mixed Reality?", "YUP_Engine", function()
    local gfm = get_gameflow_manager()
    if not gfm then return false, "no GameflowManager" end
    local gi = get_game_instance()
    local mr = false
    pcall(function() mr = gfm:Call("IsMixedReality", gi) end)
    return true, "IsMixedReality: " .. tostring(mr)
end)

-- ── Table Control (PFXGameflowObject_PlayTable / GFO_PlayTable_C) ───────────
add("table_pause", "Pause Game", "Table_Control", function()
    local pt = FindFirstOf("GFO_PlayTable_C")
    if not is_live(pt) then return false, "no PlayTable" end
    pcall(function() pt:Call("PauseGame") end)
    return true, "PauseGame"
end)
add("table_resume", "Resume Game", "Table_Control", function()
    local pt = FindFirstOf("GFO_PlayTable_C")
    if not is_live(pt) then return false, "no PlayTable" end
    pcall(function() pt:Call("ResumeGame") end)
    return true, "ResumeGame"
end)
add("table_restart", "Restart Game", "Table_Control", function()
    local pt = FindFirstOf("GFO_PlayTable_C")
    if not is_live(pt) then return false, "no PlayTable" end
    pcall(function() pt:Call("RestartGame") end)
    return true, "RestartGame"
end)
add("table_exit", "Exit Game", "Table_Control", function()
    local pt = FindFirstOf("GFO_PlayTable_C")
    if not is_live(pt) then return false, "no PlayTable" end
    pcall(function() pt:Call("ExitGame") end)
    return true, "ExitGame"
end)
add("table_guide_on", "Table Guide ON", "Table_Control", function()
    local pt = FindFirstOf("GFO_PlayTable_C")
    if not is_live(pt) then return false, "no PlayTable" end
    pcall(function() pt:Call("SetTableGuideActive", true) end)
    return true, "TableGuide ON"
end)
add("table_guide_off", "Table Guide OFF", "Table_Control", function()
    local pt = FindFirstOf("GFO_PlayTable_C")
    if not is_live(pt) then return false, "no PlayTable" end
    pcall(function() pt:Call("SetTableGuideActive", false) end)
    return true, "TableGuide OFF"
end)
add("table_env_hide", "Hide Environment", "Table_Control", function()
    local pt = FindFirstOf("GFO_PlayTable_C")
    if not is_live(pt) then return false, "no PlayTable" end
    pcall(function() pt:Call("SetEnvironmentVisibility", false, true) end)
    return true, "Environment HIDDEN"
end)
add("table_env_show", "Show Environment", "Table_Control", function()
    local pt = FindFirstOf("GFO_PlayTable_C")
    if not is_live(pt) then return false, "no PlayTable" end
    pcall(function() pt:Call("SetEnvironmentVisibility", true, false) end)
    return true, "Environment VISIBLE"
end)
add("table_is_paused", "Is Paused?", "Table_Control", function()
    local pt = FindFirstOf("GFO_PlayTable_C")
    if not is_live(pt) then return false, "no PlayTable" end
    local paused = false
    pcall(function() paused = pt:Call("IsPaused") end)
    return true, "IsPaused: " .. tostring(paused)
end)

-- ── Input Simulation (PFXPlayTableInputHandler) ─────────────────────────────
add("input_left_press", "Left Flipper PRESS", "Input", function()
    local ih = FindFirstOf("PFXPlayTableInputHandler")
    if not is_live(ih) then return false, "no InputHandler" end
    pcall(function() ih:Call("LeftFlipperPressed") end)
    return true, "LeftFlipperPressed"
end)
add("input_left_release", "Left Flipper RELEASE", "Input", function()
    local ih = FindFirstOf("PFXPlayTableInputHandler")
    if not is_live(ih) then return false, "no InputHandler" end
    pcall(function() ih:Call("LeftFlipperReleased") end)
    return true, "LeftFlipperReleased"
end)
add("input_right_press", "Right Flipper PRESS", "Input", function()
    local ih = FindFirstOf("PFXPlayTableInputHandler")
    if not is_live(ih) then return false, "no InputHandler" end
    pcall(function() ih:Call("RightFlipperPressed") end)
    return true, "RightFlipperPressed"
end)
add("input_right_release", "Right Flipper RELEASE", "Input", function()
    local ih = FindFirstOf("PFXPlayTableInputHandler")
    if not is_live(ih) then return false, "no InputHandler" end
    pcall(function() ih:Call("RightFlipperReleased") end)
    return true, "RightFlipperReleased"
end)
add("input_launch_press", "Digital Launch PRESS", "Input", function()
    local ih = FindFirstOf("PFXPlayTableInputHandler")
    if not is_live(ih) then return false, "no InputHandler" end
    pcall(function() ih:Call("DigitalLaunchPressed") end)
    return true, "DigitalLaunchPressed"
end)
add("input_launch_release", "Digital Launch RELEASE", "Input", function()
    local ih = FindFirstOf("PFXPlayTableInputHandler")
    if not is_live(ih) then return false, "no InputHandler" end
    pcall(function() ih:Call("DigitalLaunchReleased") end)
    return true, "DigitalLaunchReleased"
end)
add("input_launch_pressure", "Analogue Launch 100%", "Input", function()
    local ih = FindFirstOf("PFXPlayTableInputHandler")
    if not is_live(ih) then return false, "no InputHandler" end
    pcall(function() ih:Call("OnAnalogueLaunchPressure", 1.0) end)
    return true, "OnAnalogueLaunchPressure(1.0)"
end)
add("input_nudge_left", "Nudge LEFT", "Input", function()
    local ih = FindFirstOf("PFXPlayTableInputHandler")
    if not is_live(ih) then return false, "no InputHandler" end
    pcall(function() ih:Call("OnNudgeGesture", -1.0, 0.0) end)
    return true, "NudgeGesture(-1, 0)"
end)
add("input_nudge_right", "Nudge RIGHT", "Input", function()
    local ih = FindFirstOf("PFXPlayTableInputHandler")
    if not is_live(ih) then return false, "no InputHandler" end
    pcall(function() ih:Call("OnNudgeGesture", 1.0, 0.0) end)
    return true, "NudgeGesture(1, 0)"
end)
add("input_nudge_forward", "Nudge FORWARD", "Input", function()
    local ih = FindFirstOf("PFXPlayTableInputHandler")
    if not is_live(ih) then return false, "no InputHandler" end
    pcall(function() ih:Call("OnNudgeGesture", 0.0, 1.0) end)
    return true, "NudgeGesture(0, 1)"
end)
add("input_powerup_press", "Powerup PRESS", "Input", function()
    local ih = FindFirstOf("PFXPlayTableInputHandler")
    if not is_live(ih) then return false, "no InputHandler" end
    pcall(function() ih:Call("OnUsePowerupPressed") end)
    return true, "UsePowerupPressed"
end)
add("input_extras_press", "Visual Extras PRESS", "Input", function()
    local ih = FindFirstOf("PFXPlayTableInputHandler")
    if not is_live(ih) then return false, "no InputHandler" end
    pcall(function() ih:Call("OnVisualExtrasPressed") end)
    return true, "VisualExtrasPressed"
end)

-- ── VR Pawn Control ─────────────────────────────────────────────────────────
add("pawn_blink", "Blink (fade out/in)", "VR_Pawn", function()
    local pawn = FindFirstOf("BP_VR_Pawn_C")
    if not is_live(pawn) then pawn = FindFirstOf("PFXVRPawn") end
    if not is_live(pawn) then return false, "no VR Pawn" end
    pcall(function() pawn:Call("Blink", nil, 0.5, 0.1) end)
    return true, "Blink(0.5s, 0.1s hold)"
end)
add("pawn_disable_actions", "Disable Player Actions", "VR_Pawn", function()
    local pawn = FindFirstOf("BP_VR_Pawn_C")
    if not is_live(pawn) then pawn = FindFirstOf("PFXVRPawn") end
    if not is_live(pawn) then return false, "no VR Pawn" end
    pcall(function() pawn:Call("DisableActions") end)
    return true, "DisableActions"
end)
add("pawn_enable_actions", "Enable Player Actions", "VR_Pawn", function()
    local pawn = FindFirstOf("BP_VR_Pawn_C")
    if not is_live(pawn) then pawn = FindFirstOf("PFXVRPawn") end
    if not is_live(pawn) then return false, "no VR Pawn" end
    pcall(function() pawn:Call("EnableActions") end)
    return true, "EnableActions"
end)
add("pawn_hide_hints", "Hide Control Hints", "VR_Pawn", function()
    local pawn = FindFirstOf("BP_VR_Pawn_C")
    if not is_live(pawn) then pawn = FindFirstOf("PFXVRPawn") end
    if not is_live(pawn) then return false, "no VR Pawn" end
    pcall(function() pawn:Call("SetControlHintVisibility", false) end)
    return true, "Hints HIDDEN"
end)
add("pawn_show_hints", "Show Control Hints", "VR_Pawn", function()
    local pawn = FindFirstOf("BP_VR_Pawn_C")
    if not is_live(pawn) then pawn = FindFirstOf("PFXVRPawn") end
    if not is_live(pawn) then return false, "no VR Pawn" end
    pcall(function() pawn:Call("SetControlHintVisibility", true) end)
    return true, "Hints VISIBLE"
end)
add("pawn_fade_enable", "Enable Fade", "VR_Pawn", function()
    local pawn = FindFirstOf("BP_VR_Pawn_C")
    if not is_live(pawn) then pawn = FindFirstOf("PFXVRPawn") end
    if not is_live(pawn) then return false, "no VR Pawn" end
    pcall(function() pawn:Call("SetFadeEnabled", true) end)
    return true, "Fade ENABLED"
end)
add("pawn_fade_disable", "Disable Fade", "VR_Pawn", function()
    local pawn = FindFirstOf("BP_VR_Pawn_C")
    if not is_live(pawn) then pawn = FindFirstOf("PFXVRPawn") end
    if not is_live(pawn) then return false, "no VR Pawn" end
    pcall(function() pawn:Call("SetFadeEnabled", false) end)
    return true, "Fade DISABLED"
end)
add("pawn_loading_on", "Loading Widget ON", "VR_Pawn", function()
    local pawn = FindFirstOf("BP_VR_Pawn_C")
    if not is_live(pawn) then pawn = FindFirstOf("PFXVRPawn") end
    if not is_live(pawn) then return false, "no VR Pawn" end
    pcall(function() pawn:Call("SetLoadingWidgetActive", true) end)
    return true, "LoadingWidget ON"
end)
add("pawn_loading_off", "Loading Widget OFF", "VR_Pawn", function()
    local pawn = FindFirstOf("BP_VR_Pawn_C")
    if not is_live(pawn) then pawn = FindFirstOf("PFXVRPawn") end
    if not is_live(pawn) then return false, "no VR Pawn" end
    pcall(function() pawn:Call("SetLoadingWidgetActive", false) end)
    return true, "LoadingWidget OFF"
end)

-- ── Table Info Queries ──────────────────────────────────────────────────────
add("yup_table_count", "Get Table Count", "YUP_TableInfo", function()
    local gs = get_game_subsystem()
    if not gs then return false, "no GameSubsystem" end
    local holder = nil
    pcall(function() holder = gs:Call("GetTableInfoHolder") end)
    if not holder then return false, "no TableInfoHolder" end
    local count = 0
    pcall(function() count = holder:Call("GetTablesNum") end)
    return true, "Tables: " .. tostring(count)
end)
add("yup_table_started", "Is Table Started?", "YUP_TableInfo", function()
    local tr = get_table_reference()
    if not tr then return false, "no TableReference" end
    local started = false
    pcall(function() started = tr:Call("IsTableStarted") end)
    return true, "IsTableStarted: " .. tostring(started)
end)
add("yup_table_running", "Is Game Running?", "YUP_TableInfo", function()
    local tr = get_table_reference()
    if not tr then return false, "no TableReference" end
    local running = false
    pcall(function() running = tr:Call("IsTableGameRunning") end)
    return true, "IsTableGameRunning: " .. tostring(running)
end)
add("yup_table_gameover", "Is Game Over?", "YUP_TableInfo", function()
    local tr = get_table_reference()
    if not tr then return false, "no TableReference" end
    local over = false
    pcall(function() over = tr:Call("IsTableGameOver") end)
    return true, "IsTableGameOver: " .. tostring(over)
end)
add("yup_table_nudge_str", "Get Nudge Strength", "YUP_TableInfo", function()
    local gs = get_game_subsystem()
    if not gs then return false, "no GameSubsystem" end
    local holder = nil
    pcall(function() holder = gs:Call("GetTableInfoHolder") end)
    if not holder then return false, "no holder" end
    local info = nil
    pcall(function() info = holder:Call("GetTableInfoByIndex", 0) end)
    if not info then return false, "no info" end
    local ns = 0
    pcall(function() ns = info:Get("NudgeStrength") end)
    return true, "NudgeStrength: " .. tostring(ns)
end)
add("yup_table_tilt_sens", "Get Tilt Sensitivity", "YUP_TableInfo", function()
    local gs = get_game_subsystem()
    if not gs then return false, "no GameSubsystem" end
    local holder = nil
    pcall(function() holder = gs:Call("GetTableInfoHolder") end)
    if not holder then return false, "no holder" end
    local info = nil
    pcall(function() info = holder:Call("GetTableInfoByIndex", 0) end)
    if not info then return false, "no info" end
    local ts = 0
    pcall(function() ts = info:Get("TiltSensitivity") end)
    return true, "TiltSensitivity: " .. tostring(ts)
end)

-- ── PFXCheatManager — Per-Table Perks ────────────────────────────────────────
add("perk_max_table", "Max Perks (Current Table)", "PFX_Perks", function()
    return call_cheat("PFXDebug_TablePerkMaxTable")
end)
add("perk_reset_table", "Reset Perks (Current Table)", "PFX_Perks", function()
    return call_cheat("PFXDebug_TablePerkResetTable")
end)
add("perk_level_set", "Set Perk Level", "PFX_Perks", function()
    return call_cheat("PFXDebug_TablePerkLevelSet")
end)

-- ── PFXCheatManager — Per-Table Mastery ─────────────────────────────────────
add("mastery_max_table", "Max Mastery (Current Table)", "PFX_Mastery", function()
    return call_cheat("PFXDebug_TableMasteryMaxTable")
end)
add("mastery_reset_table", "Reset Mastery (Current Table)", "PFX_Mastery", function()
    return call_cheat("PFXDebug_TableMasteryResetTable")
end)
add("mastery_level_set", "Set Mastery Level", "PFX_Mastery", function()
    return call_cheat("PFXDebug_TableMasteryLevelSet")
end)

-- ── PFXCheatManager — Per-Table Awards ──────────────────────────────────────
add("awards_unlock_table", "Unlock Awards (Current Table)", "PFX_Awards", function()
    return call_cheat("PFXDebug_TableAwardUnlockTable")
end)
add("awards_lock_table", "Lock Awards (Current Table)", "PFX_Awards", function()
    return call_cheat("PFXDebug_TableAwardLockTable")
end)
add("awards_unlock_single", "Unlock Single Award", "PFX_Awards", function()
    return call_cheat("PFXDebug_TableAwardUnlock")
end)
add("awards_lock_single", "Lock Single Award", "PFX_Awards", function()
    return call_cheat("PFXDebug_TableAwardLock")
end)

-- ── PFXCheatManager — Per-Entry Collectibles ────────────────────────────────
add("collectibles_unlock_entry", "Unlock Single Entry", "PFX_Collectibles", function()
    return call_cheat("PFXDebug_Collectibles_UnlockEntry")
end)
add("collectibles_lock_entry", "Lock Single Entry", "PFX_Collectibles", function()
    return call_cheat("PFXDebug_Collectibles_LockEntry")
end)
add("collectibles_unlock_bundle", "Unlock All In Bundle (Brand New)", "PFX_Collectibles", function()
    return call_cheat("PFXDebug_Collectibles_UnlockAllEntriesInBundle", "", true)
end)
add("collectibles_lock_bundle", "Lock All In Bundle", "PFX_Collectibles", function()
    return call_cheat("PFXDebug_Collectibles_LockAllEntriesInBundle")
end)

-- ── PFXCheatManager — Per-Achievement ───────────────────────────────────────
add("achievement_unlock_single", "Unlock Single Achievement", "PFX_Achievements", function()
    return call_cheat("PFXDebug_Achievement_Unlock")
end)
add("achievement_set_progress", "Set Achievement Progress", "PFX_Achievements", function()
    return call_cheat("PFXDebug_Achievement_SetProgress")
end)
add("achievement_add_progress", "Add Achievement Progress", "PFX_Achievements", function()
    return call_cheat("PFXDebug_Achievement_AddProgress")
end)

-- ── PFXCheatManager — Championship Extras ───────────────────────────────────
add("championship_max_league", "Max League", "PFX_Championship", function()
    return call_cheat("PFXDebug_Championship_MaxLeague")
end)
add("championship_set_debug_match", "Set Debug Match", "PFX_Championship", function()
    return call_cheat("PFXDebug_Championship_SetDebugMatch")
end)
add("championship_mission_reward", "Mission Reward", "PFX_Championship", function()
    return call_cheat("PFXDebug_Championship_MissionReward")
end)
add("championship_mastery_level_set", "Set Championship Mastery Level", "PFX_Championship", function()
    return call_cheat("PFXDebug_Championship_MasteryLevelSet")
end)
add("championship_benefit_grant", "Grant Championship Benefit", "PFX_Championship", function()
    return call_cheat("PFXDebug_Championship_BenefitGrant")
end)
add("championship_benefit_calc", "Benefit Effect Calculator", "PFX_Championship", function()
    return call_cheat("PFXDebug_Championship_BenefitEffectCalculator")
end)

-- ── PFXCheatManager — Audio Extras ──────────────────────────────────────────
add("audio_music_cutoff_500", "Music Cutoff 500Hz", "PFX_Audio", function()
    return call_cheat("PFXDebug_Audio_SetMusicCutoffFrequency", 500.0)
end)
add("audio_music_cutoff_2k", "Music Cutoff 2kHz", "PFX_Audio", function()
    return call_cheat("PFXDebug_Audio_SetMusicCutoffFrequency", 2000.0)
end)
add("audio_music_cutoff_20k", "Music Cutoff 20kHz (full)", "PFX_Audio", function()
    return call_cheat("PFXDebug_Audio_SetMusicCutoffFrequency", 20000.0)
end)

-- ── PFXCheatManager — Sequence / Onboarding ─────────────────────────────────
add("sequence_set_playback", "Set Sequence Playback Position", "PFX_Misc", function()
    return call_cheat("PFXDebug_Sequence_SetPlaybackPosition")
end)
add("reward_wheel_spin", "Add Reward Wheel Spin", "PFX_Misc", function()
    return call_cheat("PFXDebug_RewardWheel_AddSpin")
end)
add("onboarding_hint_by_name", "Show Hint Panel By Name", "PFX_Misc", function()
    return call_cheat("PFXDebug_Onboarding_ShowHintPanelByName")
end)
add("onboarding_hint_by_index", "Show Hint Panel By Index", "PFX_Misc", function()
    return call_cheat("PFXDebug_Onboarding_ShowHintPanelByIndex")
end)

-- ── BP_CheatManager_C — MR / Camera ────────────────────────────────────────
add("bp_toggle_mr_camera", "Toggle MR Test Camera", "BP_Debug", function()
    return call_cheat("PFXDEBUG_ToggleTestCameraViewMR")
end)
add("bp_simulate_mr_capture", "Simulate MR Scene Capture", "BP_Debug", function()
    return call_cheat("PFXDEBUG_SimulateMRSceneCapture")
end)
add("bp_add_pawn_rotation", "Add Pawn Rotation", "BP_Debug", function()
    return call_cheat("PFXDEBUG_AddPawnRotation")
end)
add("bp_add_camera_offset", "Add Relative Camera Offset", "BP_Debug", function()
    return call_cheat("PFXDEBUG_AddRelativeCameraOffset")
end)
add("bp_update_cabinet_lods", "Update Cabinet LODs", "BP_Debug", function()
    return call_cheat("PFXDEBUG_UpdateCabinetLODs")
end)
add("bp_set_tracking_origin", "Set Tracking Origin", "BP_Debug", function()
    return call_cheat("PFXDEBUG_SetTrackingOrigin")
end)
add("bp_show_hide_loading", "Show+Hide Loading Screen", "BP_Debug", function()
    return call_cheat("ShowAndHideLoadingScreen")
end)

-- ── UE CheatManager — Extra Useful ──────────────────────────────────────────
add("ue_view_self", "View Self", "UE_Base", function()
    return call_cheat("ViewSelf")
end)
add("ue_enable_debug_cam", "Enable Debug Camera", "UE_Base", function()
    return call_cheat("EnableDebugCamera")
end)
add("ue_disable_debug_cam", "Disable Debug Camera", "UE_Base", function()
    return call_cheat("DisableDebugCamera")
end)
add("ue_log_loc", "Log Location", "UE_Base", function()
    return call_cheat("LogLoc")
end)
add("ue_change_size_half", "Change Size 0.5x", "UE_Base", function()
    return call_cheat("ChangeSize", 0.5)
end)
add("ue_change_size_2x", "Change Size 2.0x", "UE_Base", function()
    return call_cheat("ChangeSize", 2.0)
end)
add("ue_change_size_normal", "Change Size 1.0x", "UE_Base", function()
    return call_cheat("ChangeSize", 1.0)
end)
add("ue_damage_target_100", "Damage Target (100)", "UE_Base", function()
    return call_cheat("DamageTarget", 100.0)
end)
add("ue_toggle_ai_log", "Toggle AI Logging", "UE_Base", function()
    return call_cheat("ToggleAILogging")
end)
add("ue_test_collision", "Test Collision Distance", "UE_Base", function()
    return call_cheat("TestCollisionDistance")
end)
add("ue_destroy_all_pawns", "Destroy All Pawns Except Target", "UE_Base", function()
    return call_cheat("DestroyAllPawnsExceptTarget")
end)
add("ue_set_world_origin", "Set World Origin", "UE_Base", function()
    return call_cheat("SetWorldOrigin")
end)
add("ue_update_safe_area", "Update Safe Area", "UE_Base", function()
    return call_cheat("UpdateSafeArea")
end)
add("ue_capsule_sweep", "Debug Capsule Sweep", "UE_Base", function()
    return call_cheat("DebugCapsuleSweep")
end)
add("ue_capsule_sweep_clear", "Clear Capsule Sweep", "UE_Base", function()
    return call_cheat("DebugCapsuleSweepClear")
end)

-- ── Finger Mode (PFX_FingerMode integration) ────────────────────────────────
add("finger_toggle", "Toggle Finger Mode (Sweep)", "Finger_Mode", function()
    if _G.PFX_FingerMode then
        local on = _G.PFX_FingerMode.toggle()
        return true, "FingerMode: " .. tostring(on)
    end
    return false, "PFX_FingerMode not loaded"
end)
add("finger_enable", "Enable Finger Mode", "Finger_Mode", function()
    if _G.PFX_FingerMode then _G.PFX_FingerMode.enable(); return true, "FingerMode ON" end
    return false, "PFX_FingerMode not loaded"
end)
add("finger_disable", "Disable Finger Mode", "Finger_Mode", function()
    if _G.PFX_FingerMode then _G.PFX_FingerMode.disable(); return true, "FingerMode OFF" end
    return false, "PFX_FingerMode not loaded"
end)

-- ── Gameflow Manager ────────────────────────────────────────────────────────
add("gfm_show_loading", "Show Loading Screen", "Gameflow", function()
    local gfm = get_gameflow_manager()
    if not gfm then return false, "no GameflowManager" end
    pcall(function() gfm:Call("BP_ShowLoadingScreen") end)
    return true, "BP_ShowLoadingScreen"
end)
add("gfm_hide_loading", "Hide Loading Screen", "Gameflow", function()
    local gfm = get_gameflow_manager()
    if not gfm then return false, "no GameflowManager" end
    pcall(function() gfm:Call("BP_HideLoadingScreen") end)
    return true, "BP_HideLoadingScreen"
end)

-- ============================================================================
-- BUILD CATEGORY INDEX
-- ============================================================================
local categories = {}
local cat_order = {}
local cat_set = {}
for _, a in ipairs(CHEAT_ACTIONS) do
    if not cat_set[a.category] then
        cat_set[a.category] = true
        cat_order[#cat_order + 1] = a.category
        categories[a.category] = {}
    end
    categories[a.category][#categories[a.category] + 1] = a
end

-- ============================================================================
-- GLOBAL API — for PFX_ModMenu integration and bridge commands
-- ============================================================================
local api = {
    -- Core
    get_cheat_manager     = get_cheat_manager,
    call_cheat            = call_cheat,
    invalidate_cache      = invalidate_yup_cache,

    -- YUP access
    get_game_subsystem    = get_game_subsystem,
    get_game_handler      = get_game_handler,
    get_game_info         = get_game_info,
    get_game_result       = get_game_result,
    get_table_reference   = get_table_reference,
    get_gameflow_manager  = get_gameflow_manager,
    get_game_instance     = get_game_instance,

    -- Action registry
    actions               = CHEAT_ACTIONS,
    categories            = categories,
    cat_order             = cat_order,

    -- Run action by ID
    run = function(action_id)
        for _, a in ipairs(CHEAT_ACTIONS) do
            if a.id == action_id then
                local ok, msg = a.fn()
                return ok, msg
            end
        end
        return false, "unknown action: " .. tostring(action_id)
    end,

    -- List all actions
    list = function()
        local lines = {}
        for _, cat in ipairs(cat_order) do
            lines[#lines + 1] = "── " .. cat .. " ──"
            for _, a in ipairs(categories[cat]) do
                lines[#lines + 1] = "  " .. a.id .. " : " .. a.label
            end
        end
        return table.concat(lines, "\n")
    end,

    -- Status
    status = function()
        local cm = get_cheat_manager()
        local cm_name = cm and cm:GetFullName() or "NONE"
        local gh = get_game_handler()
        local gh_name = gh and gh:GetName() or "NONE"
        return TAG .. " | CM=" .. cm_name .. " | GH=" .. gh_name .. " | " .. #CHEAT_ACTIONS .. " actions"
    end,
}

_G.PFX_NativeCheats = api
Log(TAG .. ": Global API exported as PFX_NativeCheats")

-- ============================================================================
-- BRIDGE COMMANDS
-- ============================================================================
pcall(function()
    RegisterCommand("nc_status", function()
        return api.status()
    end)
end)
pcall(function()
    RegisterCommand("nc_list", function()
        return api.list()
    end)
end)
pcall(function()
    RegisterCommand("nc_run", function(action_id)
        local ok, msg = api.run(action_id)
        return TAG .. ": " .. tostring(ok) .. " — " .. tostring(msg)
    end)
end)
pcall(function()
    RegisterCommand("nc_cm", function()
        local cm = get_cheat_manager()
        if cm then
            return TAG .. ": CM=" .. cm:GetFullName() .. " class=" .. cm:GetClass():GetName()
        end
        return TAG .. ": no CheatManager found"
    end)
end)
pcall(function()
    RegisterCommand("nc_call", function(func_name)
        if not func_name or func_name == "" then return TAG .. ": usage: nc_call <FunctionName>" end
        local ok, msg = call_cheat(func_name)
        return TAG .. ": " .. tostring(ok) .. " — " .. tostring(msg)
    end)
end)
pcall(function()
    RegisterCommand("nc_yup", function()
        local parts = {}
        local gs = get_game_subsystem()
        parts[#parts + 1] = "GS=" .. (gs and gs:GetName() or "nil")
        local gh = get_game_handler()
        parts[#parts + 1] = "GH=" .. (gh and gh:GetName() or "nil")
        local gi = get_game_info()
        parts[#parts + 1] = "GI=" .. (gi and gi:GetName() or "nil")
        local gr = get_game_result()
        parts[#parts + 1] = "GR=" .. (gr and gr:GetName() or "nil")
        local tr = get_table_reference()
        parts[#parts + 1] = "TR=" .. (tr and tr:GetName() or "nil")
        local gfm = get_gameflow_manager()
        parts[#parts + 1] = "GFM=" .. (gfm and gfm:GetName() or "nil")
        return TAG .. ": " .. table.concat(parts, " | ")
    end)
end)

-- Quick-access bridge commands for most common cheats
pcall(function() RegisterCommand("nc_unlock_all", function()
    local r = {}
    local ok1, m1 = api.run("unlock_all_tables"); r[#r+1] = m1
    local ok2, m2 = api.run("perk_max_all"); r[#r+1] = m2
    local ok3, m3 = api.run("mastery_max_all"); r[#r+1] = m3
    local ok4, m4 = api.run("collectibles_unlock_all"); r[#r+1] = m4
    local ok5, m5 = api.run("awards_unlock_all"); r[#r+1] = m5
    local ok6, m6 = api.run("achievements_unlock_all"); r[#r+1] = m6
    local ok7, m7 = api.run("championship_max_all"); r[#r+1] = m7
    return TAG .. ": UNLOCK ALL\n" .. table.concat(r, "\n")
end) end)
pcall(function() RegisterCommand("nc_reset_all", function()
    local r = {}
    local ok1, m1 = api.run("relock_all_tables"); r[#r+1] = m1
    local ok2, m2 = api.run("perk_reset_all"); r[#r+1] = m2
    local ok3, m3 = api.run("mastery_reset_all"); r[#r+1] = m3
    local ok4, m4 = api.run("collectibles_lock_all"); r[#r+1] = m4
    local ok5, m5 = api.run("awards_lock_all"); r[#r+1] = m5
    local ok6, m6 = api.run("championship_mastery_reset"); r[#r+1] = m6
    local ok7, m7 = api.run("profile_reset"); r[#r+1] = m7
    return TAG .. ": RESET ALL\n" .. table.concat(r, "\n")
end) end)
pcall(function() RegisterCommand("nc_slomo", function(speed)
    local s = tonumber(speed) or 1.0
    return TAG .. ": " .. tostring(select(2, call_cheat("Slomo", s)))
end) end)
pcall(function() RegisterCommand("nc_hz", function(hz)
    local h = tonumber(hz) or 90
    return TAG .. ": " .. tostring(select(2, call_cheat("PFXDebug_Performance_SetTargetDisplayFrequency", h)))
end) end)
pcall(function() RegisterCommand("nc_score", function()
    local _, m = api.run("yup_get_score")
    return TAG .. ": " .. tostring(m)
end) end)

Log(TAG .. ": v2 loaded — " .. #CHEAT_ACTIONS .. " cheat actions across " .. #cat_order .. " categories")
Log(TAG .. ": Categories: " .. table.concat(cat_order, ", "))
Log(TAG .. ": Bridge: nc_status, nc_list, nc_run <id>, nc_cm, nc_call <func>, nc_yup")
Log(TAG .. ":   Quick: nc_unlock_all, nc_reset_all, nc_slomo <speed>, nc_hz <freq>, nc_score")
