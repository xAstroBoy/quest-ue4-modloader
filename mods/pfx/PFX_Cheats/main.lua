-- ============================================================================
-- PFX_Cheats v1 — In-game Cheat Menu + Infinite Ball Save
-- ============================================================================
-- Features:
--   1. Infinite Ball Save — hooks OnGameStateChanged on PFXGameflowObject_PlayTable
--      and calls SkipPrepareGameEnd() to cancel drain. Logs all state transitions
--      so we can tune the exact state number.
--   2. Bridge commands — toggle cheats at runtime via TCP bridge console
--   3. Wrist-menu stub — hooks BP_WristMenu_C:Open to inject cheat entries
-- ============================================================================
local TAG = "PFX_Cheats"
local VERBOSE = true
local function V(...) if VERBOSE then Log(TAG .. " [V] " .. string.format(...)) end end
Log(TAG .. ": Loading v1...")

-- ============================================================================
-- STATE
-- ============================================================================
local cheats = {
    infinite_ball_save  = true,   -- toggle via bridge: cheats_ballsave 0|1
    log_game_states     = true,   -- log every OnGameStateChanged transition
    big_ball            = false,  -- scale active table ball up
    large_flippers      = false,  -- scale flipper actors up
}

local stats = {
    ball_saves          = 0,
    state_changes       = 0,
    game_end_blocked    = 0,
    big_ball_applies    = 0,
    flipper_applies     = 0,
}

-- Ball scales uniformly. Flippers now scale on actor Y (table width axis)
-- so collider + visible mesh widen together.
local BIG_BALL_SCALE   = 2.50
local FLIPPER_SCALE_X  = 10.00  -- current X stretch (runtime-mutable via slider)
local FLIPPER_SCALE_Y  = 1.0    -- keep width unchanged
local FLIPPER_SCALE_Z  = 1.0    -- keep height unchanged

-- Cache of original BoxComponent half-extents (populated on first scale).
-- Allows restoring the exact original collision shape before each re-scale
-- so actor-level (sx,1,1) scale never compounds with direct BoxExtent calls.
local flipper_box_extents_cache = {}
local flipper_mesh_scales_cache = {}
local flipper_axis_cache = {}

local TABLE_FOLDER_TO_FLIPPER_PREFIX = {
    BALLY_Attack_from_Mars = "119_AttackFromMars",
    BALLY_Black_Lagoon = "131_BlackLagoon",
    BALLY_Black_Rose = "118_BlackRose",
    BALLY_Champion_Pub = "121_ChampionPub",
    BALLY_Elvira_and_the_Party_Monsters = "197_Elvira",
    BALLY_Party_Zone = "120_PartyZone",
    BALLY_Safe_Cracker = "122_SafeCracker",
    BALLY_Scared_Stiff = "198_ScaredStiff",
    BALLY_TheatreOfMagic = "117_TheatreOfMagic",
    BALLY_Twilight_Zone = "162_TwilightZone",
    BALLY_WHO_Dunnit = "195_WhoDunnit",
    BALLY_World_Cup_Soccer = "157_WorldCup",
    BETHESDA_Fallout = "90_Fallout",
    Bally_Addams_Family = "156_AddamsFamily",
    Bethesda_Doom = "92_Doom",
    Bethesda_Skyrim = "91_Skyrim",
    CD_Tomb_Raider_Chronicles = "182_TombRaider_Chronicles",
    CD_Tomb_Raider_Croft_Manor = "192_TombRaider_Manor",
    LEGENDARY_Godzilla = "158_Godzilla",
    LEGENDARY_Godzilla_vs_Kong = "160_GvK",
    LEGENDARY_Kong = "153_Kong",
    PEANUTS_Charlie_Brown_CS = "181_CharlieBrown",
    UNIVERSAL_BSG = "170_BattlestarGalactica",
    UNIVERSAL_Knight_Rider = "177_KnightRider",
    UNIVERSAL_Xena = "167_Xena",
    WMS_Comet = "199_Comet",
    WMS_Diner = "201_Diner",
    WMS_Fire = "200_Fire",
    WMS_Getaway = "111_Getaway",
    WMS_Indiana_Jones = "133_IndianaJones",
    WMS_Junkyard = "110_Junkyard",
    WMS_Medieval_Madness = "109_MedievalMadness",
    WMS_Monster_Bash = "130_MonsterBash",
    WMS_Pinbot = "194_PinBot",
    WMS_Start_Trek_TNG = "163_StarTrekTNG",
    WMS_Taxi = "196_Taxi",
    ZEN_Egypt = "112_CurseOfTheMummy",
    ZEN_Noir = "123_PinballNoir",
    ZEN_Pirates = "113_SkyPirates",
}

-- Flipper actors are table-specific subclasses of BP_Collectible_FlipperArm_Base_C
-- (e.g. BP_113_SkyPirates_Flippers0_C). They live in LVL_VR_Main.PersistentLevel.
-- Avoid broad ForEachUObject bridge scans here; they are expensive on the live game thread.
-- Prefer direct current-table class probes and exact/subclass lookups instead.




-- Known game state values (discovered via live logging — expand as needed)
-- State 3 = "PrepareBallEnd" / drain in most YUP pinball builds
-- We block on any state > 1 that is not "Paused" (usually 2)
local DRAIN_STATES = { [3]=true, [5]=true, [6]=true, [7]=true }
local PAUSE_STATE  = 2

-- ============================================================================
-- HELPERS
-- ============================================================================
local function is_live(obj)
    if not obj then return false end
    local ok, valid = pcall(function() return obj:IsValid() end)
    if not ok or not valid then return false end
    local ok2, name = pcall(function() return obj:GetName() end)
    if not ok2 or not name then return false end
    return not name:match("^Default__") and not name:match("^REINST_")
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

local function get_play_table()
    return find_live("PFXGameflowObject_PlayTable")
end

local function in_table_gameplay()
    local pt = find_live("GFO_PlayTable_C", "PFXGameflowObject_PlayTable")
    if not pt then return false end

    local paused = false
    pcall(function() paused = pt:Call("IsPaused") end)

    local tr = nil
    pcall(function() tr = pt:Get("m_tableReference") end)
    if tr and is_live(tr) then
        local running = false
        pcall(function() running = tr:Call("IsTableGameRunning") end)
        return running == true and not paused
    end

    return not paused
end

local wrist_guard = {
    enabled = true,
    last_open_t = 0,
    blocked_close = 0,
}

local function force_wrist_menu_open_short(wm)
    if not wrist_guard.enabled then return end
    if not is_live(wm) then return end
    if not in_table_gameplay() then return end

    local t0 = os.clock()
    wrist_guard.last_open_t = t0

    for i = 1, 16 do
        ExecuteWithDelay(i * 120, function()
            if not is_live(wm) then return end
            local dt = os.clock() - (wrist_guard.last_open_t or 0)
            if dt > 2.2 then return end
            pcall(function() wm:Call("SetOpenState", true, false) end)
            pcall(function() wm:Call("SetOpenState", true) end)
        end)
    end
end

local function cheats_set_ballsave(enable)
    V("cheats_set_ballsave enable=%s", tostring(enable))
    cheats.infinite_ball_save = not not enable
    Log(TAG .. ": infinite_ball_save = " .. tostring(cheats.infinite_ball_save))
    return cheats.infinite_ball_save
end

local function cheats_toggle_ballsave()
    V("cheats_toggle_ballsave")
    return cheats_set_ballsave(not cheats.infinite_ball_save)
end

local function cheats_set_logstates(enable)
    V("cheats_set_logstates enable=%s", tostring(enable))
    cheats.log_game_states = not not enable
    return cheats.log_game_states
end

local function cheats_toggle_logstates()
    V("cheats_toggle_logstates")
    return cheats_set_logstates(not cheats.log_game_states)
end

local function cheats_saveball()
    V("cheats_saveball")
    local pt = get_play_table()
    if not pt then return false, "not in game" end
    local ok = pcall(function() pt:Call("SkipPrepareGameEnd") end)
    if ok then
        stats.ball_saves = stats.ball_saves + 1
        return true, "ball saved (total=" .. stats.ball_saves .. ")"
    end
    return false, "SkipPrepareGameEnd failed"
end

local function cheats_restartball()
    V("cheats_restartball")
    local pt = get_play_table()
    if not pt then return false, "not in game" end
    local ok = pcall(function() pt:Call("RestartGame") end)
    return ok, "RestartGame ok=" .. tostring(ok)
end

local function cheats_pause_resume()
    V("cheats_pause_resume")
    local pt = get_play_table()
    if not pt then return false, "not in game" end
    local paused = false
    pcall(function() paused = pt:Call("IsPaused") end)
    if paused then
        pcall(function() pt:Call("ResumeGame") end)
        return true, "Game RESUMED"
    else
        pcall(function() pt:Call("PauseGame") end)
        return true, "Game PAUSED"
    end
end

-- ============================================================================
-- SCALE CHEATS (Big Ball / Large Flippers)
-- Collision-safe approach:
--   * Actor scale is applied via SetActorScale3D
--   * Collision is explicitly re-enabled on actor and root primitive
-- ============================================================================

-- scale_vec: {X, Y, Z} table for non-uniform scale, or nil to use {sx, sx, sx}
local function apply_actor_scale_with_collision(actor, sx, scale_vec)
    if not is_live(actor) then return false end
    local sv = scale_vec or { X = sx, Y = sx, Z = sx }

    local ok_scale = false
    pcall(function()
        actor:Call("SetActorScale3D", sv)
        ok_scale = true
    end)

    pcall(function() actor:Call("SetActorEnableCollision", true) end)

    local root = nil
    pcall(function() root = actor:Call("K2_GetRootComponent") end)
    if root and is_live(root) then
        pcall(function() root:Set("CollisionEnabled", 3) end) -- QueryAndPhysics
        pcall(function() root:Call("SetCollisionEnabled", 3) end)
        pcall(function() root:Call("SetGenerateOverlapEvents", true) end)
        pcall(function() root:Call("SetWorldScale3D", sv) end)
    end

    -- Some gameplay visuals do not visibly resize from actor/root scale alone.
    -- Force primitive component scaling as well.
    pcall(function()
        local pcls = FindClass("PrimitiveComponent")
        if not pcls then return end
        local comps = actor:Call("K2_GetComponentsByClass", pcls)
        if not comps then return end
        for i = 1, #comps do
            local comp = comps[i]
            if is_live(comp) then
                pcall(function() comp:Call("SetRelativeScale3D", sv) end)
                pcall(function() comp:Call("SetWorldScale3D", sv) end)
            end
        end
    end)

    return ok_scale
end

local function refresh_collision_components(actor, component_class_name)
    if not is_live(actor) then return 0 end

    local cls = nil
    pcall(function() cls = FindClass(component_class_name) end)
    if not cls then return 0 end

    local comps = nil
    pcall(function() comps = actor:Call("K2_GetComponentsByClass", cls) end)
    if not comps or #comps == 0 then return 0 end

    local count = 0
    for i = 1, #comps do
        local comp = comps[i]
        if is_live(comp) then
            pcall(function() comp:Call("SetCollisionEnabled", 3) end)
            pcall(function() comp:Call("SetGenerateOverlapEvents", true) end)
            count = count + 1
        end
    end
    return count
end

local function find_active_ball()
    local candidates = {
        "ball_C",
        "Ball_C",
        "ball",
        "Ball",
    }

    for _, class_name in ipairs(candidates) do
        local balls = nil
        pcall(function() balls = FindAllOf(class_name) end)
        if balls then
            for _, b in ipairs(balls) do
                if is_live(b) then return b end
            end
        end
    end

    local fallback = nil
    pcall(function()
        local smas = FindAllOf("StaticMeshActor")
        if not smas then return end
        for _, sma in ipairs(smas) do
            if is_live(sma) then
                local n = ""
                pcall(function() n = sma:GetName() end)
                local nn = n:lower()
                if nn:find("ball", 1, true) and not nn:find("flipper", 1, true) then
                    fallback = sma
                    return
                end
            end
        end
    end)
    return fallback
end

local function get_active_table_folder_from_ball_class()
    local ball = find_active_ball()
    if ball == nil then return nil end
    if not is_live(ball) then return nil end
    local live_ball = ball
    local class_full = ""
    local ok_cls, cls = pcall(function() return live_ball:GetClass() end)
    if ok_cls and cls then
        local ok_name, full_name = pcall(function() return cls:GetFullName() end)
        if ok_name and full_name then
            class_full = tostring(full_name)
        end
    end
    if class_full == "" then return nil end
    return class_full:match("/Game/Tables/([^/]+)/")
end

local function get_active_table_level_name()
    local folder = get_active_table_folder_from_ball_class()
    if not folder then return nil, nil end
    local level = folder:match("^[^_]+_(.+)$") or folder
    if level == "" then return nil, nil end
    return folder, level
end

local function get_active_table_folder_path()
    local folder = get_active_table_folder_from_ball_class()
    if not folder then return nil end
    return "/Game/Tables/" .. folder .. "/"
end

local function get_active_table_level_prefix()
    local ball = find_active_ball()
    if not ball or not is_live(ball) then return nil end

    local fn = ""
    pcall(function() fn = tostring(ball:GetFullName() or "") end)
    if fn == "" then return nil end

    local prefix = fn:match("^(.-%.PersistentLevel%.)")
    if prefix and prefix ~= "" then
        return prefix
    end
    return nil
end

local function get_active_table_reference()
    local pt = find_live("GFO_PlayTable_C", "PFXGameflowObject_PlayTable")
    if not pt then return nil end

    local tr = nil
    pcall(function() tr = pt:Get("m_tableReference") end)
    if tr and is_live(tr) then return tr end

    pcall(function() tr = pt:Call("GetTableReference") end)
    if tr and is_live(tr) then return tr end

    tr = find_live("PFXTableReference", "YUPTableReference")
    if tr and is_live(tr) then return tr end

    return nil
end

local function collect_table_level_actors()
    local tr = get_active_table_reference()
    if not tr then return nil end

    local actors = nil
    pcall(function()
        actors = tr:Call("GetTableLevelActors")
    end)

    if type(actors) == "table" and #actors > 0 then
        return actors
    end
    return nil
end

local function collect_live_balls()
    local out = {}
    local seen = {}
    local candidates = {
        "ball_C",
        "Ball_C",
        "ball",
        "Ball",
    }

    for _, class_name in ipairs(candidates) do
        local balls = nil
        pcall(function() balls = FindAllOf(class_name) end)
        if balls then
            for _, b in ipairs(balls) do
                if is_live(b) then
                    local n = ""
                    pcall(function() n = b:GetName() end)
                    if n ~= "" and not seen[n] then
                        seen[n] = true
                        out[#out + 1] = b
                    end
                end
            end
        end
    end

    return out
end

-- User-configurable flipper class (set via cheats_set_flipper_class bridge command).
-- Empty = auto-detect via ForEachUObject class-name scan.
local flipper_class_override = ""
local flipper_class_pattern_cache = {
    classes = nil,
    last_t = 0,
}
local flipper_actor_cache = {
    actors = nil,
    last_t = 0,
}

local function collect_flipper_actors()
    local now = os.clock()
    if flipper_actor_cache.actors and (now - flipper_actor_cache.last_t) < 2.0 then
        local cached = {}
        for _, a in ipairs(flipper_actor_cache.actors) do
            if is_live(a) then
                cached[#cached + 1] = a
            end
        end
        if #cached > 0 then
            return cached
        end
    end

    local out = {}
    local seen = {}

    local function add_from_table_level_flippers()
        local level_prefix = get_active_table_level_prefix()
        if not level_prefix then
            level_prefix = get_active_table_folder_path()
        end
        if not level_prefix then return end
        V("collect_flipper_actors(level_prefix=%s)", tostring(level_prefix))

        local actors = nil
        pcall(function()
            actors = FindAllOf("StaticMeshActor", true)
        end)
        if not actors then return end

        local function add_visible_flipper(a, n, fn)
            local key = fn ~= "" and fn or n
            if key == "" or seen[key] then return end
            seen[key] = true
            out[#out + 1] = a
        end

        -- Pass 1: prefer the plain flipper actors; on this table they carry the
        -- collider path and their Y scale widens both the render and physics.
        for _, a in ipairs(actors) do
            if is_live(a) then
                local n = ""
                local fn = ""
                pcall(function() n = tostring(a:GetName() or "") end)
                pcall(function() fn = tostring(a:GetFullName() or "") end)
                local nn = n:lower()
                if fn:find(level_prefix, 1, true)
                    and nn:find("flipper_", 1, true)
                    and not nn:find("obj_", 1, true)
                    and not nn:find("sensor_", 1, true)
                then
                    add_visible_flipper(a, n, fn)
                end
            end
        end

        if #out > 0 then return end

        -- Pass 2: fallback to the obj_flipper proxies only if the plain flippers
        -- are not exposed by the current table.
        for _, a in ipairs(actors) do
            if is_live(a) then
                local n = ""
                local fn = ""
                pcall(function() n = tostring(a:GetName() or "") end)
                pcall(function() fn = tostring(a:GetFullName() or "") end)

                local nn = n:lower()
                if fn:find(level_prefix, 1, true)
                    and nn:find("obj_flipper_", 1, true)
                then
                    add_visible_flipper(a, n, fn)
                end
            end
        end
    end

    local function is_actor_instance(obj)
        local ok_isa, isa_actor = pcall(function() return IsA(obj, "Actor") end)
        if not ok_isa or not isa_actor then return false end
        local ofn = ""
        pcall(function() ofn = tostring(obj:GetFullName() or "") end)
        if ofn == "" then return false end
        return ofn:find(".PersistentLevel.", 1, true) ~= nil
    end

    local function in_valid_table_path(a)
        local af = ""
        pcall(function() af = tostring(a:GetFullName() or "") end)
        local afl = af:lower()

        -- IMPORTANT: Do NOT reject by class path under /TableRewards/.
        -- Many valid flipper classes are defined there but instantiated in
        -- /Game/Hub/LVL_VR_Main.PersistentLevel during gameplay.
        -- Keep only runtime level instances.
        if not afl:find(".persistentlevel.", 1, true) then
            return false
        end
        return true
    end

    local function has_flipper_components(a)
        local has_box = false
        pcall(function()
            local bcls = FindClass("BoxComponent")
            if not bcls then return end
            local arr = a:Call("K2_GetComponentsByClass", bcls)
            has_box = arr and #arr > 0
        end)
        if has_box then return true end

        local has_mesh = false
        pcall(function()
            local mcls = FindClass("StaticMeshComponent")
            if not mcls then return end
            local arr = a:Call("K2_GetComponentsByClass", mcls)
            has_mesh = arr and #arr > 0
        end)
        return has_mesh
    end

    local function add(a)
        if not is_live(a) then return end
        if not is_actor_instance(a) then return end
        if not in_valid_table_path(a) then return end
        if not has_flipper_components(a) then return end

        local n = ""
        local fn = ""
        pcall(function() n = tostring(a:GetName() or "") end)
        pcall(function() fn = tostring(a:GetFullName() or "") end)

        local cn = ""
        pcall(function() cn = tostring(a:GetClass():GetName() or "") end)
        local is_flipper_base_subclass = false
        pcall(function() is_flipper_base_subclass = IsA(a, "BP_Collectible_FlipperArm_Base_C") end)
        if not is_flipper_base_subclass then
            pcall(function() is_flipper_base_subclass = IsA(a, "ABP_Collectible_FlipperArm_Base_C") end)
        end
        local is_table_flipper_bp = (cn:match("^BP_.-_Flippers%d+_C$") ~= nil)
        local key_text = (n .. " " .. cn):lower()
        if not is_table_flipper_bp and not is_flipper_base_subclass then
            if not key_text:find("flipper", 1, true) then return end
            if key_text:find("button", 1, true) then return end
            if key_text:find("widget", 1, true) then return end
            if key_text:find("entry", 1, true) then return end
            if key_text:find("light", 1, true) then return end
            if key_text:find("manager", 1, true) then return end
            if key_text:find("controller", 1, true) then return end
        end

        local key = fn ~= "" and fn or n
        if key == "" or seen[key] then return end
        seen[key] = true
        out[#out + 1] = a
        V("collect_flipper: %s cls=%s", n,
            (function() local c="?"; pcall(function() c=a:GetClass():GetName() end); return c end)())
    end

    local function add_all(arr)
        if not arr then return end
        for _, a in ipairs(arr) do
            add(a)
        end
    end

    -- Primary runtime path: exact playfield flipper actors from the current
    -- table level. These are the visible in-table flippers the player sees.
    pcall(add_from_table_level_flippers)
    if #out > 0 then
        V("collect_flipper_actors(table-playfield flippers): found=%d", #out)
        flipper_actor_cache.actors = out
        flipper_actor_cache.last_t = os.clock()
        return out
    end

    -- Primary runtime path: use the current table reference's actor list.
    -- This is the actual gameplay table actor set and is far smaller than a full
    -- GUObjectArray walk, so it avoids bridge lag/timeouts.
    pcall(function()
        add_all(collect_table_level_actors())
    end)
    if #out > 0 then
        V("collect_flipper_actors(table-level actors): found=%d", #out)
        flipper_actor_cache.actors = out
        flipper_actor_cache.last_t = os.clock()
        return out
    end

    -- Deterministic current-table path: derive the exact flipper class prefix from
    -- the active ball's table folder, then probe Flippers0/1/2 directly.
    pcall(function()
        local folder = get_active_table_folder_from_ball_class()
        local prefix = folder and TABLE_FOLDER_TO_FLIPPER_PREFIX[folder] or nil
        if not prefix then return end
        for i = 0, 2 do
            local cn = string.format("BP_%s_Flippers%d_C", prefix, i)
            add(FindObjectByType(cn, true))
        end
    end)
    if #out > 0 then
        V("collect_flipper_actors(table-prefix-direct): found=%d", #out)
        flipper_actor_cache.actors = out
        flipper_actor_cache.last_t = os.clock()
        return out
    end

    -- Fast fallback after exact table-prefix probing: grab live subclasses of the
    -- flipper base class directly. This can include other loaded tables, so it is
    -- only used when the current-table path fails to produce any results.
    pcall(function()
        add_all(FindAllOf("BP_Collectible_FlipperArm_Base_C", true))
    end)
    pcall(function()
        add_all(FindAllOf("ABP_Collectible_FlipperArm_Base_C", true))
    end)
    if #out > 0 then
        V("collect_flipper_actors(base-subclass-scan): found=%d", #out)
        flipper_actor_cache.actors = out
        flipper_actor_cache.last_t = os.clock()
        return out
    end

    local function get_cached_table_flipper_classes()
        local now = os.clock()
        if flipper_class_pattern_cache.classes and (now - flipper_class_pattern_cache.last_t) < 30 then
            return flipper_class_pattern_cache.classes
        end

        local classes = {}
        flipper_class_pattern_cache.classes = classes
        flipper_class_pattern_cache.last_t = now
        return classes
    end

    -- Explicit override: user identified the class via dump_actor_classes.
    if flipper_class_override ~= "" then
        pcall(function()
            add_all(FindAllOf(flipper_class_override, true))
        end)
        if #out == 0 then
            pcall(function()
                add(FindObjectByType(flipper_class_override, true))
            end)
        end
        if #out == 0 then
            pcall(function()
                ForEachUObject(function(obj, _)
                    if IsA(obj, flipper_class_override) then add(obj) end
                end)
            end)
        end
        V("collect_flipper_actors(override=%s): found=%d", flipper_class_override, #out)
        flipper_actor_cache.actors = out
        flipper_actor_cache.last_t = os.clock()
        return out
    end

    -- Preferred auto path: class names discovered from BlueprintGeneratedClass
    -- objects, then resolved exactly like the manual override path.
    pcall(function()
        local classes = get_cached_table_flipper_classes()
        if not classes then return end
        for _, cn in ipairs(classes) do
            pcall(function() add_all(FindAllOf(cn, true)) end)
            if #out == 0 then
                pcall(function() add(FindObjectByType(cn, true)) end)
            end
        end
    end)
    if #out > 0 then
        V("collect_flipper_actors(class-override-auto): found=%d", #out)
        flipper_actor_cache.actors = out
        flipper_actor_cache.last_t = os.clock()
        return out
    end

    -- Fast table-agnostic path: scan live Actor instances only.
    -- This avoids heavy BlueprintGeneratedClass + FindAllOf-per-class scans.
    pcall(function()
        local actors = FindAllOf("Actor", true)
        if not actors then return end
        for _, a in ipairs(actors) do
            if is_live(a) then
                local cn = ""
                local an = ""
                pcall(function() cn = tostring(a:GetClass():GetName() or "") end)
                pcall(function() an = tostring(a:GetName() or "") end)
                local t = (cn .. " " .. an):lower()
                local is_table_flipper_bp = (cn:match("^BP_.-_Flippers%d+_C$") ~= nil)
                if is_table_flipper_bp
                    or (t:find("flipper", 1, true)
                        and not t:find("button", 1, true)
                        and not t:find("widget", 1, true)
                        and not t:find("entry", 1, true)
                        and not t:find("manager", 1, true)
                        and not t:find("controller", 1, true))
                then
                    add(a)
                end
            end
        end
    end)
    if #out > 0 then
        V("collect_flipper_actors(actor-scan): found=%d", #out)
        flipper_actor_cache.actors = out
        flipper_actor_cache.last_t = os.clock()
        return out
    end

    -- Subclass path: FindAllOf("Actor") is exact-class only on this runtime,
    -- so walk UObject instances and keep only Actor subclasses with flipper-like class names.
    pcall(function()
        for _, obj in ipairs(collect_table_level_actors() or {}) do
            add(obj)
        end
    end)
    if #out > 0 then
        V("collect_flipper_actors(table-level subclass-scan): found=%d", #out)
        flipper_actor_cache.actors = out
        flipper_actor_cache.last_t = os.clock()
        return out
    end

    -- NOTE: Deliberately no full ForEachUObject fallback here.
    -- It's too expensive for bridge-driven interactive calls.
    V("collect_flipper_actors: found=%d", #out)
    flipper_actor_cache.actors = out
    flipper_actor_cache.last_t = os.clock()
    return out
end

local function probe_scale_targets()
    local lines = {}

    local ball = find_active_ball()
    if ball and is_live(ball) then
        local bname = "?"
        local bclass = "?"
        pcall(function() bname = tostring(ball:GetName()) end)
        pcall(function() bclass = tostring(ball:GetClass():GetFullName()) end)
        lines[#lines + 1] = "ball=" .. bname
        lines[#lines + 1] = "ball_class=" .. bclass
    else
        lines[#lines + 1] = "ball=none"
    end

    local flippers = collect_flipper_actors()
    lines[#lines + 1] = "flippers_found=" .. tostring(flippers and #flippers or 0)

    if flippers then
        for i = 1, math.min(#flippers, 6) do
            local f = flippers[i]
            local n = "?"
            local c = "?"
            local s = "?"
            local box_count = 0
            local mesh_count = 0
            pcall(function() n = tostring(f:GetName()) end)
            pcall(function() c = tostring(f:GetClass():GetFullName()) end)
            pcall(function()
                local rc = f:Call("K2_GetRootComponent")
                local rs = rc and rc:Get("RelativeScale3D") or nil
                if rs then
                    s = string.format("(%.2f,%.2f,%.2f)", rs.X, rs.Y, rs.Z)
                end
            end)
            pcall(function()
                local bcls = FindClass("BoxComponent")
                local arr = bcls and f:Call("K2_GetComponentsByClass", bcls) or nil
                box_count = arr and #arr or 0
            end)
            pcall(function()
                local mcls = FindClass("StaticMeshComponent")
                local arr = mcls and f:Call("K2_GetComponentsByClass", mcls) or nil
                mesh_count = arr and #arr or 0
            end)
            lines[#lines + 1] = string.format("f%d=%s|%s|scale=%s|box=%d|mesh=%d", i, n, c, s, box_count, mesh_count)
        end
    end

    return table.concat(lines, " || ")
end

local function apply_big_ball()
    local target_scale = cheats.big_ball and BIG_BALL_SCALE or 1.0
    local balls = collect_live_balls()
    if not balls or #balls == 0 then
        local one = find_active_ball()
        if one then balls = { one } else balls = {} end
    end

    if #balls == 0 then
        return false, "no active ball"
    end

    local ok_count = 0
    for _, ball in ipairs(balls) do
        if apply_actor_scale_with_collision(ball, target_scale) then
            refresh_collision_components(ball, "StaticMeshComponent")
            ok_count = ok_count + 1
        end
    end

    stats.big_ball_applies = stats.big_ball_applies + ok_count
    if ok_count > 0 then
        return true, "ball scale=" .. string.format("%.2f", target_scale) .. " (actors=" .. tostring(ok_count) .. ")"
    end
    return false, "failed to scale ball"
end

local function apply_large_flippers()
    local flippers = collect_flipper_actors()

    if not flippers or #flippers == 0 then
        return false, "no flipper actors"
    end

    local sx = cheats.large_flippers and FLIPPER_SCALE_X or 1.0

    local ok_count = 0
    for _, f in ipairs(flippers) do
        if is_live(f) then
            local applied_actor = false
            local actor_name = ""
            pcall(function() actor_name = tostring(f:GetName() or "") end)

            local actor_scale = { X = 1.0, Y = sx, Z = 1.0 }

            -- Scale the actor itself on Y so the collider follows the same width
            -- change as the visible mesh.
            pcall(function() f:Call("SetActorScale3D", actor_scale) end)
            pcall(function() f:Call("SetActorEnableCollision", true) end)

            -- Keep the root/world transform in sync with the actor scale.
            pcall(function()
                local root = f:Call("K2_GetRootComponent")
                if root then
                    root:Call("SetRelativeScale3D", actor_scale)
                    root:Call("SetWorldScale3D", actor_scale)
                    root:Call("UpdateComponentToWorld")
                    root:Call("UpdateBounds")
                    root:Call("MarkRenderTransformDirty")
                    root:Call("MarkRenderStateDirty")
                end
            end)

            -- Keep mesh components at their cached LOCAL base scale.
            -- Actor Y scale already applies the intended width and keeps
            -- collider + visuals in sync. Multiplying mesh Y here would
            -- compound world scale (e.g. actor 10 * mesh 10 => 100).
            pcall(function()
                local mcls = FindClass("StaticMeshComponent")
                if not mcls then return end
                local comps = f:Call("K2_GetComponentsByClass", mcls)
                if not comps then return end
                for i = 1, #comps do
                    local mc = comps[i]
                    if is_live(mc) then
                        local n = ""
                        pcall(function() n = tostring(mc:GetName() or "") end)
                        if n == "" then n = tostring(i) end
                        local key = actor_name .. "::" .. n
                        if not flipper_mesh_scales_cache[key] then
                            pcall(function()
                                local rs = mc:Get("RelativeScale3D")
                                if rs then
                                    flipper_mesh_scales_cache[key] = { X = rs.X, Y = rs.Y, Z = rs.Z }
                                end
                            end)
                        end
                        local base = flipper_mesh_scales_cache[key]
                        if base then
                            local mx, my, mz = base.X, base.Y, base.Z
                            pcall(function() mc:Set("RelativeScale3D", { X = mx, Y = my, Z = mz }) end)
                            pcall(function() mc:Call("UpdateComponentToWorld") end)
                            pcall(function() mc:Call("UpdateBounds") end)
                            pcall(function() mc:Call("MarkRenderTransformDirty") end)
                            pcall(function() mc:Call("MarkRenderStateDirty") end)
                            applied_actor = true
                        end
                    end
                end
            end)

            if applied_actor then
                ok_count = ok_count + 1
            end
        end
    end

    stats.flipper_applies = stats.flipper_applies + ok_count
    return ok_count > 0, string.format("flippers scaled=%d @(1.00,%.2f,1.00)", ok_count, sx)
end

local function schedule_large_flipper_reapply_burst()
    if not cheats.large_flippers then return end
    local delays = { 100, 250, 500, 900, 1500 }
    for _, delay_ms in ipairs(delays) do
        ExecuteWithDelay(delay_ms, function()
            if cheats.large_flippers then
                pcall(apply_large_flippers)
            end
        end)
    end
end

local function cheats_set_big_ball(enable)
    cheats.big_ball = not not enable
    local ok, msg = apply_big_ball()
    return cheats.big_ball, (ok and msg) or ("pending: " .. tostring(msg))
end

local function cheats_toggle_big_ball()
    return cheats_set_big_ball(not cheats.big_ball)
end

local function cheats_set_large_flippers(enable)
    cheats.large_flippers = not not enable
    local ok, msg = apply_large_flippers()
    if cheats.large_flippers then
        schedule_large_flipper_reapply_burst()
    end
    return cheats.large_flippers, (ok and msg) or ("pending: " .. tostring(msg))
end

local function cheats_toggle_large_flippers()
    return cheats_set_large_flippers(not cheats.large_flippers)
end

local function get_flipper_scale_x()
    return FLIPPER_SCALE_X
end

-- Set flipper length (X scale, 1.0 = none/default, up to 5.0).
-- Immediately re-applies flipper scale if flippers are currently active.
local function set_flipper_length_x(val)
    val = math.max(1.0, math.min(10.0, tonumber(val) or 10.0))
    FLIPPER_SCALE_X = val
    if val > 1.0 then
        cheats.large_flippers = true
    else
        cheats.large_flippers = false
    end
    local ok, msg = apply_large_flippers()
    return val, (ok and msg) or ("pending: " .. tostring(msg))
end

local function cheats_reset_scale()
    cheats.big_ball = false
    cheats.large_flippers = false
    local bok, bmsg = apply_big_ball()
    local fok, fmsg = apply_large_flippers()
    return (bok and fok), "reset ball=" .. tostring(bmsg) .. " flippers=" .. tostring(fmsg)
end

local function cheats_status_line()
    local drain_list = {}
    for k in pairs(DRAIN_STATES) do drain_list[#drain_list+1] = k end
    table.sort(drain_list)
    return string.format(
        "%s v1: ballsave=%s logstates=%s big_ball=%s large_flippers=%s | saves=%d end_blocked=%d states_seen=%d | apply(ball=%d,flipper=%d) | drain_states={%s}",
        TAG,
        tostring(cheats.infinite_ball_save),
        tostring(cheats.log_game_states),
        tostring(cheats.big_ball),
        tostring(cheats.large_flippers),
        stats.ball_saves,
        stats.game_end_blocked,
        stats.state_changes,
        stats.big_ball_applies,
        stats.flipper_applies,
        table.concat(drain_list, ",")
    )
end

-- ============================================================================
-- INFINITE BALL SAVE
-- Hook: PFXGameflowObject_PlayTable:OnGameStateChanged (BlueprintEvent)
-- Pre-hook fires BEFORE the BP graph runs — we can inspect the new state
-- and call SkipPrepareGameEnd() to cancel drain sequences.
-- ============================================================================

-- Pre-hook: intercept state change BEFORE the BP graph handles it
pcall(function()
    RegisterHook("PFXGameflowObject_PlayTable:OnGameStateChanged", function(ctx, state_raw)
        V("OnGameStateChanged hook fired")
        local newState = 0
        pcall(function() newState = ReadU8(state_raw) end)
        stats.state_changes = stats.state_changes + 1

        if cheats.log_game_states then
            Log(TAG .. ": [GameState] -> " .. tostring(newState))
        end

        if cheats.infinite_ball_save and DRAIN_STATES[newState] then
            -- Try to skip prepare-game-end (cancels the drain sequence)
            local pt = nil
            pcall(function() pt = ctx end)
            if pt and is_live(pt) then
                local ok = pcall(function() pt:Call("SkipPrepareGameEnd") end)
                if ok then
                    stats.ball_saves = stats.ball_saves + 1
                    Log(TAG .. ": [BallSave] Blocked drain at state=" .. newState
                        .. " (saves=" .. stats.ball_saves .. ")")
                end
            end
        end

        -- Reapply scale cheats after state transitions / ball respawns
        if cheats.big_ball then pcall(apply_big_ball) end
        if cheats.large_flippers then pcall(apply_large_flippers) end
    end)
    Log(TAG .. ": OnGameStateChanged hook registered")
end)

pcall(function()
    RegisterHook("GFO_PlayTable_C:OnTableRestart__DelegateSignature", function()
        -- Table restart recreates actors; re-apply active scale cheats shortly after
        ExecuteWithDelay(1200, function()
            if cheats.big_ball then pcall(apply_big_ball) end
            if cheats.large_flippers then pcall(apply_large_flippers) end
        end)
    end)
    Log(TAG .. ": OnTableRestart hook registered (scale reapply)")
end)

pcall(function()
    RegisterHook("GFO_PlayTable_C:OnTableStartBegin", function()
        -- BP table-start path (decompiled) recreates runtime actors; reapply shortly after
        ExecuteWithDelay(900, function()
            if cheats.big_ball then pcall(apply_big_ball) end
            if cheats.large_flippers then pcall(apply_large_flippers) end
        end)
    end)
    Log(TAG .. ": OnTableStartBegin hook registered (scale reapply)")
end)

pcall(function()
    RegisterHook("GFO_PlayTable_C:OnGameResumed", function()
        -- Resume path can rebuild/correct actor state; reapply active scales
        ExecuteWithDelay(400, function()
            if cheats.big_ball then pcall(apply_big_ball) end
            if cheats.large_flippers then pcall(apply_large_flippers) end
        end)
    end)
    Log(TAG .. ": OnGameResumed hook registered (scale reapply)")
end)

pcall(function()
    RegisterHook("GFO_PlayTable_C:BP_OnActivated", function()
        -- PlayTable activation grabs handlers and initializes runtime actors
        ExecuteWithDelay(1600, function()
            if cheats.big_ball then pcall(apply_big_ball) end
            if cheats.large_flippers then pcall(apply_large_flippers) end
        end)
    end)
    Log(TAG .. ": BP_OnActivated hook registered (scale reapply)")
end)

-- Post-hook on OnGameEnd: if ball save enabled and game ends, restart
-- (catches cases where drain slipped through state hook)
pcall(function()
    RegisterHook("PFXGameflowObject_PlayTable:OnGameEnd", function(ctx)
        V("OnGameEnd hook fired, ballsave=%s", tostring(cheats.infinite_ball_save))
        if not cheats.infinite_ball_save then return end
        local pt = nil
        pcall(function() pt = ctx end)
        if pt and is_live(pt) then
            local ok = pcall(function() pt:Call("SkipPrepareGameEnd") end)
            if ok then
                stats.game_end_blocked = stats.game_end_blocked + 1
                Log(TAG .. ": [BallSave] OnGameEnd blocked (total=" .. stats.game_end_blocked .. ")")
            end
        end
    end)
    Log(TAG .. ": OnGameEnd hook registered")
end)

-- ============================================================================
-- WRIST MENU INJECTION (stub — hooks BP_WristMenu_C when it opens in-game)
-- This logs available entries and attempts to inject a Cheats tab.
-- ============================================================================
pcall(function()
    RegisterHook("BP_WristMenu_C:Open", function(ctx)
        V("BP_WristMenu_C:Open hook fired")
        Log(TAG .. ": [WristMenu] Opened — cheats available via bridge commands")
        -- Future: inject cheat entries into the wrist menu entries array
        -- For now just log so we can probe the menu structure
        pcall(function()
            local wm = ctx
            if not is_live(wm) then return end
            local wn = "?"
            pcall(function() wn = wm:GetName() end)
            Log(TAG .. ": [WristMenu] instance=" .. wn)
            force_wrist_menu_open_short(wm)
        end)
    end)
    Log(TAG .. ": WristMenu:Open hook registered")
end)

pcall(function()
    RegisterPreHook("BP_Pawn_Base_C:SetWristMenuEnabled", function(self, funcPtr, parms)
        if not wrist_guard.enabled then return end
        if not in_table_gameplay() then return end

        local now = os.clock()
        local dt = now - (wrist_guard.last_open_t or 0)
        if dt < 0 or dt > 2.2 then return end

        local bEnable = 1
        pcall(function() bEnable = ReadU8(parms) end)
        if bEnable == 0 then
            wrist_guard.blocked_close = wrist_guard.blocked_close + 1
            return "BLOCK"
        end
    end)
    Log(TAG .. ": Wrist menu close guard hook registered")
end)

-- ============================================================================
-- BRIDGE COMMANDS
-- ============================================================================

-- Toggle infinite ball save
pcall(function()
    RegisterCommand("cheats_ballsave", function(args)
        local enable = (args == nil or args == "" or args == "1" or args == "true")
        cheats_set_ballsave(enable)
        return TAG .. ": infinite_ball_save = " .. tostring(cheats.infinite_ball_save)
    end)
end)

-- Toggle state logging
pcall(function()
    RegisterCommand("cheats_logstates", function(args)
        local enable = (args == nil or args == "" or args == "1" or args == "true")
        cheats_set_logstates(enable)
        return TAG .. ": log_game_states = " .. tostring(cheats.log_game_states)
    end)
end)

-- Add/remove drain state numbers
pcall(function()
    RegisterCommand("cheats_drainstate", function(args)
        local n = tonumber(args)
        if not n then return "usage: cheats_drainstate <number>" end
        DRAIN_STATES[math.floor(n)] = true
        Log(TAG .. ": Added drain state " .. n)
        local states = {}
        for k in pairs(DRAIN_STATES) do states[#states+1] = k end
        table.sort(states)
        return TAG .. ": drain states = {" .. table.concat(states, ",") .. "}"
    end)
end)

-- Manual ball save trigger (call while in game if auto-hook misses)
pcall(function()
    RegisterCommand("cheats_saveball", function()
        local ok, msg = cheats_saveball()
        if ok then return TAG .. ": " .. msg end
        return TAG .. ": " .. msg
    end)
end)

-- Force restart current ball (if drain already happened)
pcall(function()
    RegisterCommand("cheats_restartball", function()
        local _, msg = cheats_restartball()
        return TAG .. ": " .. msg
    end)
end)

-- Pause/resume game
pcall(function()
    RegisterCommand("cheats_pause", function()
        local _, msg = cheats_pause_resume()
        return TAG .. ": " .. msg
    end)
end)

pcall(function()
    RegisterCommand("cheats_bigball", function(args)
        local enable = (args == nil or args == "" or args == "1" or args == "true")
        local on, msg = cheats_set_big_ball(enable)
        return TAG .. ": big_ball=" .. tostring(on) .. " | " .. tostring(msg)
    end)
end)

pcall(function()
    RegisterCommand("cheats_flippers", function(args)
        local enable = (args == nil or args == "" or args == "1" or args == "true")
        local on, msg = cheats_set_large_flippers(enable)
        return TAG .. ": large_flippers=" .. tostring(on) .. " | " .. tostring(msg)
    end)
end)

pcall(function()
    RegisterCommand("cheats_reapply_scale", function()
        local aok, amsg = apply_big_ball()
        local fok, fmsg = apply_large_flippers()
        return TAG .. ": reapply big_ball=" .. tostring(aok) .. "(" .. tostring(amsg) .. ")"
            .. " flippers=" .. tostring(fok) .. "(" .. tostring(fmsg) .. ")"
    end)
end)

pcall(function()
    RegisterCommand("cheats_reset_scale", function()
        local ok, msg = cheats_reset_scale()
        return TAG .. ": reset_scale ok=" .. tostring(ok) .. " | " .. tostring(msg)
    end)
end)

pcall(function()
    RegisterCommand("cheats_flipper_length", function(args)
        local val, msg = set_flipper_length_x(tonumber(args) or 1.0)
        return TAG .. ": flipper_length=" .. string.format("%.2f", val) .. " | " .. tostring(msg)
    end)
end)

pcall(function()
    RegisterCommand("cheats_probe_scale", function()
        return TAG .. ": " .. probe_scale_targets()
    end)
end)

-- Dump all unique actor class names present in GUObjectArray (run while in a table).
-- Use this to identify the real flipper actor class, then call cheats_set_flipper_class.
pcall(function()
    RegisterCommand("dump_actor_classes", function()
        local counts = {}
        local lines = {}
        local actors = collect_flipper_actors()
        if not actors or #actors == 0 then
            return "actor_classes(0): no flipper actors found"
        end

        for _, obj in ipairs(actors) do
            local cn = "?"
            local fn = "?"
            pcall(function() cn = tostring(obj:GetClass():GetName() or "?") end)
            pcall(function() fn = tostring(obj:GetFullName() or "?") end)
            counts[cn] = (counts[cn] or 0) + 1
            lines[#lines + 1] = string.format("%s :: %s", cn, fn)
        end

        local sorted = {}
        for cn, c in pairs(counts) do sorted[#sorted+1] = {cn, c} end
        table.sort(sorted, function(a, b) return a[1] < b[1] end)
        local summary = {}
        for _, v in ipairs(sorted) do
            summary[#summary+1] = string.format("%s x%d", v[1], v[2])
        end
        return "actor_classes(" .. #sorted .. "):\n"
            .. table.concat(summary, "\n")
            .. "\n---\n"
            .. table.concat(lines, "\n")
    end)
end)

-- Override the flipper class used by collect_flipper_actors().
-- Run dump_actor_classes first to find the right class name.
pcall(function()
    RegisterCommand("cheats_set_flipper_class", function(args)
        flipper_class_override = tostring(args or "")
        return TAG .. ": flipper_class_override=" .. tostring(flipper_class_override)
    end)
end)

-- Status dump
pcall(function()
    RegisterCommand("cheats_status", function()
        return cheats_status_line()
            .. string.format(" | wrist_guard=%s blocked=%d", tostring(wrist_guard.enabled), wrist_guard.blocked_close)
    end)
end)

pcall(function()
    RegisterCommand("cheats_wristguard", function(args)
        if args == "0" or args == "false" or args == "off" then
            wrist_guard.enabled = false
        elseif args == "1" or args == "true" or args == "on" or args == nil or args == "" then
            wrist_guard.enabled = true
        else
            wrist_guard.enabled = not wrist_guard.enabled
        end
        return TAG .. ": wrist_guard=" .. tostring(wrist_guard.enabled)
            .. " blocked=" .. tostring(wrist_guard.blocked_close)
    end)
end)

-- Global API for other mods (e.g. PFX_ModMenu)
PFX_Cheats = nil
_G.PFX_Cheats = {
    cheats = cheats,
    stats = stats,
    set_ball_save = cheats_set_ballsave,
    toggle_ball_save = cheats_toggle_ballsave,
    set_log_states = cheats_set_logstates,
    toggle_log_states = cheats_toggle_logstates,
    set_big_ball = cheats_set_big_ball,
    toggle_big_ball = cheats_toggle_big_ball,
    set_large_flippers = cheats_set_large_flippers,
    toggle_large_flippers = cheats_toggle_large_flippers,
    get_flipper_scale_x = get_flipper_scale_x,
    set_flipper_length_x = set_flipper_length_x,
    reset_scale = cheats_reset_scale,
    apply_big_ball = apply_big_ball,
    apply_large_flippers = apply_large_flippers,
    probe_scale_targets = probe_scale_targets,
    save_ball = cheats_saveball,
    restart_ball = cheats_restartball,
    pause_resume = cheats_pause_resume,
    status = cheats_status_line,
}
Log(TAG .. ": global API exported (_G.PFX_Cheats)")

-- Max all perks right now via CheatManager (works in-game)
pcall(function()
    RegisterCommand("cheats_maxperks", function()
        local cm = find_live("PFXCheatManager")
        if not cm then return "PFXCheatManager not found" end
        local ok = pcall(function() cm:Call("PFXDebug_TablePerkMaxAll") end)
        return TAG .. ": PFXDebug_TablePerkMaxAll ok=" .. tostring(ok)
    end)
end)

-- Max all masteries via CheatManager
pcall(function()
    RegisterCommand("cheats_maxmasteries", function()
        local cm = find_live("PFXCheatManager")
        if not cm then return "PFXCheatManager not found" end
        local ok = pcall(function() cm:Call("PFXDebug_TableMasteryMaxAll") end)
        return TAG .. ": PFXDebug_TableMasteryMaxAll ok=" .. tostring(ok)
    end)
end)

-- Unlock all collectibles via CheatManager
pcall(function()
    RegisterCommand("cheats_unlockall", function()
        local cm = find_live("PFXCheatManager")
        if not cm then return "PFXCheatManager not found" end
        local ok = pcall(function() cm:Call("PFXDebug_Collectibles_UnlockAll", false) end)
        return TAG .. ": PFXDebug_Collectibles_UnlockAll ok=" .. tostring(ok)
    end)
end)

-- Max all championship via CheatManager
pcall(function()
    RegisterCommand("cheats_maxchamp", function()
        local cm = find_live("PFXCheatManager")
        if not cm then return "PFXCheatManager not found" end
        local ok = pcall(function() cm:Call("PFXDebug_Championship_MaxAll") end)
        return TAG .. ": PFXDebug_Championship_MaxAll ok=" .. tostring(ok)
    end)
end)

Log(TAG .. ": v1 loaded — ball_save=" .. tostring(cheats.infinite_ball_save)
    .. " log_states=" .. tostring(cheats.log_game_states)
    .. " big_ball=" .. tostring(cheats.big_ball)
    .. " large_flippers=" .. tostring(cheats.large_flippers))
Log(TAG .. ": Bridge cmds: cheats_status, cheats_ballsave, cheats_saveball, cheats_restartball")
Log(TAG .. ": Bridge cmds: cheats_pause, cheats_logstates, cheats_drainstate <n>")
Log(TAG .. ": Bridge cmds: cheats_bigball 0|1, cheats_flippers 0|1, cheats_reapply_scale")
Log(TAG .. ": Bridge cmds: cheats_maxperks, cheats_maxmasteries, cheats_unlockall, cheats_maxchamp")
