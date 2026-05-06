-- ============================================================================
-- PFX_ModMenu v4 — In-Game Mod Menu via Legal Section Takeover
-- ============================================================================
-- Fixes vs v3:
--   * find_named: BFS (depth=nil recursive bug gone)
--   * Text set AFTER AddChild (WidgetTree not ready until in hierarchy)
--   * Button text via WBP_Button_Base.ButtonText TextBlock (SetText/SetLabel did nothing)
--   * Click dispatch uses ToolTipText (Tooltip property is nil on these widgets)
--   * Panel build triggered on WBP_Button_Legal CLICK, not Construct
--     (Construct fires on zero-size instances first; desiredSize not ready)
--   * rename_legal_button_all() still runs in Construct for early label update
-- ============================================================================
local TAG = "PFX_ModMenu"
local VERBOSE = true
local function V(...) if VERBOSE then Log(TAG .. " [V] " .. string.format(...)) end end
Log(TAG .. ": Loading v4...")

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

-- BFS widget search by name (no recursion depth bug)
local function find_named(w, name)
    if not is_live(w) then return nil end
    local queue = {w}
    local head  = 1
    while head <= #queue do
        local cur = queue[head]; head = head + 1
        local ok, nm = pcall(function() return cur:GetName() end)
        if ok and nm == name then return cur end
        local ok2, cnt = pcall(function() return cur:Call("GetChildrenCount") end)
        if ok2 and type(cnt) == "number" and cnt > 0 then
            for i = 0, cnt - 1 do
                local ok3, c = pcall(function() return cur:Call("GetChildAt", i) end)
                if ok3 and c then queue[#queue + 1] = c end
            end
        end
    end
    return nil
end

-- Get WidgetTree RootWidget of a UUserWidget
local function widget_root(w)
    local wt = nil
    pcall(function() wt = w:Get("WidgetTree") end)
    if not wt then return nil end
    local root = nil
    pcall(function() root = wt:Get("RootWidget") end)
    return root
end

-- Set visible text on a WBP_Button_Default_C via SetTitleText (blueprint callable)
local function set_button_text(btn, text)
    if not is_live(btn) then return false end
    pcall(function() btn:Call("SetTitleText", text) end)
    -- Also write the property directly as fallback
    pcall(function() btn:Set("Button Title Text", text) end)
    return true
end

-- Clear the icon texture on a WBP_Button_Default_C (hide the category icon)
local function clear_button_icon(btn)
    if not is_live(btn) then return end
    pcall(function() btn:Set("Icon Texture", nil) end)
    -- Also hide the NotificationMark image if present
    pcall(function()
        local nm = btn:Get("NotificationMark")
        if nm then nm:Call("SetVisibility", 1) end  -- 1 = Collapsed
    end)
end

-- ============================================================================
-- STATE
-- ============================================================================
local ball_save_on   = true
local log_states_on  = true
local panel_built    = false
local last_settings  = nil
local action_buttons = {}
local click_stats = {
    default_hook_hits = 0,
    base_hook_hits    = 0,
    ctx_extract_hits  = 0,
    ctx_extract_miss  = 0,
    legal_hits        = 0,
    action_hits       = 0,
    unknown_hits      = 0,
    last_source       = "",
    last_name         = "",
}
local last_trigger = {
    key = "",
    t   = 0,
}

-- ============================================================================
-- ACTIONS
-- ============================================================================
local ACTIONS = {
    -- ── Max / Unlock ────────────────────────────────────────────────────────
    {
        id    = "max_all",
        label = "Max All + Unlock All",
        fn = function()
            if PFX_Max then pcall(PFX_Max.run); return "MaxAll: done" end
            -- fallback: cheat manager
            pcall(function()
                local cm = FindFirstOf("PFXCheatManager")
                if is_live(cm) then
                    cm:Call("PFXDebug_TablePerkMaxAll")
                    cm:Call("PFXDebug_TableMasteryMaxAll")
                    cm:Call("PFXDebug_Collectibles_UnlockAll", false)
                end
            end)
            return "MaxAll: done (fallback)"
        end,
    },
    {
        id    = "fix_trophies",
        label = "Fix Trophies (holo->physical)",
        fn = function()
            if PFX_Max then
                local n = 0
                pcall(function() n = PFX_Max.fix_trophies() or 0 end)
                return "Trophy fix: " .. n .. " swapped"
            end
            return "Trophy fix: PFX_Max not loaded"
        end,
    },
    -- ── Randomize ALL ───────────────────────────────────────────────────────
    {
        id    = "rand_all",
        label = "Randomize All Hub Slots",
        fn = function()
            if PFX_Rand then
                local n = 0
                pcall(function() n = PFX_Rand.scramble_all() or 0 end)
                return "Rand all: " .. n .. " slots"
            end
            return "Rand all: PFX_Rand not loaded"
        end,
    },
    {
        id    = "rand_tables",
        label = "Randomize Table Cosmetics",
        fn = function()
            if PFX_Rand then
                pcall(PFX_Rand.scramble_tables)
                return "Rand tables: done"
            end
            return "Rand tables: PFX_Rand not loaded"
        end,
    },
    -- ── Per-category scramble ───────────────────────────────────────────────
    {
        id    = "rand_wall",
        label = "Randomize Walls",
        fn = function()
            if PFX_Rand then
                local n = 0
                pcall(function() n = PFX_Rand.scramble_cat(PFX_Rand.CAT_WALL) or 0 end)
                return "Rand Wall: " .. n .. " slots"
            end
            return "Rand Wall: PFX_Rand not loaded"
        end,
    },
    {
        id    = "rand_floor",
        label = "Randomize Floors",
        fn = function()
            if PFX_Rand then
                local n = 0
                pcall(function() n = PFX_Rand.scramble_cat(PFX_Rand.CAT_FLOOR) or 0 end)
                return "Rand Floor: " .. n .. " slots"
            end
            return "Rand Floor: PFX_Rand not loaded"
        end,
    },
    {
        id    = "rand_poster",
        label = "Randomize Posters",
        fn = function()
            if PFX_Rand then
                local n = 0
                pcall(function() n = PFX_Rand.scramble_cat(PFX_Rand.CAT_POSTER) or 0 end)
                return "Rand Poster: " .. n .. " slots"
            end
            return "Rand Poster: PFX_Rand not loaded"
        end,
    },
    {
        id    = "rand_statue",
        label = "Randomize Statues",
        fn = function()
            if PFX_Rand then
                local n = 0
                pcall(function() n = PFX_Rand.scramble_cat(PFX_Rand.CAT_STATUE) or 0 end)
                return "Rand Statue: " .. n .. " slots"
            end
            return "Rand Statue: PFX_Rand not loaded"
        end,
    },
    {
        id    = "rand_gadget",
        label = "Randomize Gadgets",
        fn = function()
            if PFX_Rand then
                local n = 0
                pcall(function() n = PFX_Rand.scramble_cat(PFX_Rand.CAT_GADGET) or 0 end)
                return "Rand Gadget: " .. n .. " slots"
            end
            return "Rand Gadget: PFX_Rand not loaded"
        end,
    },
    {
        id    = "rand_hub",
        label = "Randomize Hub Interior",
        fn = function()
            if PFX_Rand then
                local n = 0
                pcall(function() n = PFX_Rand.scramble_cat(PFX_Rand.CAT_HUB) or 0 end)
                return "Rand Hub: " .. n .. " slots"
            end
            return "Rand Hub: PFX_Rand not loaded"
        end,
    },

    -- ── Cheats ──────────────────────────────────────────────────────────────
    {
        id    = "cheat_saveball",
        label = "Cheat: Save Ball NOW",
        fn = function()
            local api = (_G and _G.PFX_Cheats) or PFX_Cheats
            if api and api.save_ball then
                local ok, msg = api.save_ball()
                return "SaveBall: " .. tostring(msg)
            end
            return "SaveBall: PFX_Cheats not loaded"
        end,
    },
    {
        id    = "cheat_pause",
        label = "Cheat: Pause/Resume",
        fn = function()
            local api = (_G and _G.PFX_Cheats) or PFX_Cheats
            if api and api.pause_resume then
                local ok, msg = api.pause_resume()
                return "Pause: " .. tostring(msg)
            end
            return "Pause: PFX_Cheats not loaded"
        end,
    },
    {
        id    = "cheat_restartball",
        label = "Cheat: Restart Ball",
        fn = function()
            local api = (_G and _G.PFX_Cheats) or PFX_Cheats
            if api and api.restart_ball then
                local ok, msg = api.restart_ball()
                return "Restart: " .. tostring(msg)
            end
            return "Restart: PFX_Cheats not loaded"
        end,
    },
    {
        id     = "cheat_logstates",
        toggle = true,
        label  = function()
            local api = (_G and _G.PFX_Cheats) or PFX_Cheats
            if api and api.cheats then
                log_states_on = not not api.cheats.log_game_states
            end
            return "Cheat: Log States " .. (log_states_on and "[ON]" or "[OFF]")
        end,
        fn = function()
            local api = (_G and _G.PFX_Cheats) or PFX_Cheats
            if api and api.toggle_log_states then
                log_states_on = not not api.toggle_log_states()
                return "LogStates: " .. (log_states_on and "ON" or "OFF")
            end
            log_states_on = not log_states_on
            return "LogStates: " .. (log_states_on and "ON" or "OFF") .. " (local only)"
        end,
    },

    -- ── Ball Save toggle ─────────────────────────────────────────────────────
    {
        id     = "ball_save",
        toggle = true,
        label  = function()
            local api = (_G and _G.PFX_Cheats) or PFX_Cheats
            if api and api.cheats then
                ball_save_on = not not api.cheats.infinite_ball_save
            end
            return "Ball Save: " .. (ball_save_on and "[ON]" or "[OFF]")
        end,
        fn = function()
            local api = (_G and _G.PFX_Cheats) or PFX_Cheats
            if api and api.toggle_ball_save then
                ball_save_on = not not api.toggle_ball_save()
            else
                ball_save_on = not ball_save_on
                pcall(function()
                    if PFX_Cheats and PFX_Cheats.cheats then
                        PFX_Cheats.cheats.infinite_ball_save = ball_save_on
                    end
                end)
            end
            return "Ball Save: " .. (ball_save_on and "ON" or "OFF")
        end,
    },
}

local function get_action_label(idx)
    local a = ACTIONS[idx]
    if not a then return "?" end
    if type(a.label) == "function" then return a.label() end
    return a.label
end

-- ToolTipText stamp per action — used for click dispatch (Tooltip property is nil)
local function make_tip(idx) return "[M:" .. ACTIONS[idx].id .. "]" end

local function refresh_toggle_button(idx)
    local btn = action_buttons[idx]
    if not is_live(btn) then return end
    set_button_text(btn, get_action_label(idx))
end

local function find_action_index_by_button(btn)
    if not is_live(btn) then return nil end
    for idx, ab in ipairs(action_buttons) do
        if is_live(ab) and ab == btn then return idx end
    end

    -- Fallback by ToolTipText marker if identity check misses.
    local tip = ""
    pcall(function() tip = tostring(btn:Get("ToolTipText") or "") end)
    for idx, _ in ipairs(ACTIONS) do
        if tip == make_tip(idx) then return idx end
    end
    return nil
end

-- ============================================================================
-- RENAME the "Legal" nav button across ALL settings instances
-- ============================================================================
local function rename_legal_button_all()
    V("rename_legal_button_all")
    local all = nil
    pcall(function() all = FindAllOf("WBP_WristMenuSettings_C") end)
    if not all then return end
    for _, w in ipairs(all) do
        if is_live(w) then
            pcall(function()
                local root = widget_root(w)
                if not root then return end
                local lb = find_named(root, "WBP_Button_Legal")
                if not is_live(lb) then return end
                set_button_text(lb, "MODS")
                clear_button_icon(lb)
            end)
        end
    end
end

local function find_settings_for_legal_button(btn)
    if not is_live(btn) then return nil end
    local all = nil
    pcall(function() all = FindAllOf("WBP_WristMenuSettings_C") end)
    if not all then return nil end
    for _, w in ipairs(all) do
        if is_live(w) then
            local root = widget_root(w)
            if root then
                local lb = find_named(root, "WBP_Button_Legal")
                if is_live(lb) and lb == btn then return w end
            end
        end
    end
    return nil
end

local function is_legal_nav_button(btn)
    return is_live(find_settings_for_legal_button(btn))
end

local function resolve_default_button(obj)
    if not is_live(obj) then return nil end

    local nm = ""
    pcall(function() nm = obj:GetName() end)
    if nm:find("^WBP_Button_Default") then return obj end

    -- Click path can start from inner UButton/ButtonBase internals.
    -- Walk outer chain and return first WBP_Button_Default_* object.
    local cur = obj
    for _ = 1, 8 do
        local outer = nil
        pcall(function() outer = cur:GetOuter() end)
        if not outer then break end
        cur = outer
        local onm = ""
        pcall(function() onm = cur:GetName() end)
        if onm:find("^WBP_Button_Default") then return cur end
    end
    return nil
end

local function extract_click_object(raw_ctx)
    if is_live(raw_ctx) then return raw_ctx end

    local c = nil
    pcall(function() c = raw_ctx:get() end)
    if is_live(c) then return c end

    local s = nil
    pcall(function() s = raw_ctx:Get("self") end)
    if is_live(s) then return s end

    local o = nil
    pcall(function() o = raw_ctx:GetOuter() end)
    if is_live(o) then return o end

    return nil
end

-- ============================================================================
-- BUILD THE MOD PANEL
-- Called when user clicks the Legal/MODS nav button — panel is definitely live.
-- ============================================================================
local function find_big_settings()
    local all = nil
    pcall(function() all = FindAllOf("WBP_WristMenuSettings_C") end)
    if not all then return nil end
    for _, w in ipairs(all) do
        if is_live(w) then
            local ok, ds = pcall(function() return w:Call("GetDesiredSize") end)
            if ok and ds then
                local ok2, x = pcall(function() return ds.X end)
                if ok2 and type(x) == "number" and x > 500 then return w end
            end
        end
    end
    return nil
end

local function build_mod_panel(target_settings)
    V("build_mod_panel target_settings=%s", tostring(target_settings))
    local big = target_settings
    if not is_live(big) then
        big = find_big_settings()
    end
    if not is_live(big) then
        Log(TAG .. ": build_mod_panel: no big settings widget — open Settings first")
        return
    end

    if panel_built and last_settings == big then
        Log(TAG .. ": panel already built for this instance")
        return
    end

    last_settings  = big
    panel_built    = false
    action_buttons = {}

    local root = widget_root(big)
    if not root then Log(TAG .. ": no root widget"); return end

    -- Rename nav button
    local lb = find_named(root, "WBP_Button_Legal")
    if is_live(lb) then
        set_button_text(lb, "MODS")
        Log(TAG .. ": WBP_Button_Legal -> MODS")
    end

    -- Clear LegalLineContainer (33 copyright RichTextBlocks)
    local llc = find_named(root, "LegalLineContainer")
    if not is_live(llc) then Log(TAG .. ": LegalLineContainer not found"); return end
    pcall(function() llc:Call("ClearChildren") end)
    Log(TAG .. ": LLC cleared")

    local pc = nil
    pcall(function() pc = FindFirstOf("BP_PlayerController_C") end)
    if not pc then Log(TAG .. ": no PlayerController"); return end

    -- Header (non-interactive)
    local hdr = nil
    pcall(function() hdr = CreateWidget("WBP_Button_Default_C", pc) end)
    if is_live(hdr) then
        pcall(function() llc:Call("AddChild", hdr) end)
        set_button_text(hdr, "=== PFX Mod Menu ===")
        clear_button_icon(hdr)
        pcall(function() hdr:Set("ToolTipText", "[M:header]") end)
    end

    -- Action buttons
    for idx, _ in ipairs(ACTIONS) do
        local btn = nil
        pcall(function() btn = CreateWidget("WBP_Button_Default_C", pc) end)
        if is_live(btn) then
            pcall(function() llc:Call("AddChild", btn) end)
            set_button_text(btn, get_action_label(idx))
            clear_button_icon(btn)
            pcall(function() btn:Set("ToolTipText", make_tip(idx)) end)
            action_buttons[idx] = btn
            Log(TAG .. ": [" .. idx .. "] " .. get_action_label(idx))
        else
            Log(TAG .. ": CreateWidget failed for action " .. idx)
        end
    end

    panel_built = true
    local final = 0
    pcall(function() final = llc:Call("GetChildrenCount") end)
    Log(TAG .. ": panel built — " .. final .. " items in LLC")
end

-- ============================================================================
-- HOOK: WBP_WristMenuSettings_C:Construct
-- Rename Legal button on every instance. Do NOT build panel here —
-- Construct fires on zero-size instances first; desiredSize not ready yet.
-- ============================================================================
pcall(function()
    RegisterHook("WBP_WristMenuSettings_C:Construct", function(_ctx)
        V("WBP_WristMenuSettings_C:Construct hook fired")
        panel_built    = false
        last_settings  = nil
        action_buttons = {}
        pcall(rename_legal_button_all)
    end)
    Log(TAG .. ": WBP_WristMenuSettings_C:Construct hook registered")
end)

-- ============================================================================
-- HOOK: WBP_Button_Default_C click
--   Role 1: name == "WBP_Button_Legal"  -> build panel NOW (big instance is live)
--   Role 2: ToolTipText starts "[M:"    -> dispatch to action
-- ============================================================================
pcall(function()
    local function handle_click(source, raw_ctx)
        V("handle_click source=%s", source)
        local self_obj = extract_click_object(raw_ctx)
        if not is_live(self_obj) then
            click_stats.ctx_extract_miss = click_stats.ctx_extract_miss + 1
            return
        end
        click_stats.ctx_extract_hits = click_stats.ctx_extract_hits + 1

        local btn = resolve_default_button(self_obj) or self_obj
        if not is_live(btn) then return end

        local nm = ""
        pcall(function() nm = btn:GetName() end)

        -- De-dup bursts from multiple hooks (clicked/pressed/released/delegate)
        local now = 0
        pcall(function() now = os.clock() end)
        local k = tostring(nm)
        if last_trigger.key == k and (now - (last_trigger.t or 0)) < 0.20 then
            return
        end
        last_trigger.key = k
        last_trigger.t   = now

        click_stats.last_source = source
        click_stats.last_name   = nm

        -- Role 1: Legal/MODS nav button clicked → build panel and BLOCK original legal page
        if is_legal_nav_button(btn) then
            click_stats.legal_hits = click_stats.legal_hits + 1
            Log(TAG .. ": Legal/MODS nav clicked via " .. source .. " — building mod panel + BLOCK")
            pcall(function()
                local owner_settings = find_settings_for_legal_button(btn)
                build_mod_panel(owner_settings)
            end)
            return "BLOCK"
        end

        -- Role 2: one of our action buttons clicked
        local idx = find_action_index_by_button(btn)
        if not idx then
            click_stats.unknown_hits = click_stats.unknown_hits + 1
            return
        end

        local action = ACTIONS[idx]
        if not action then return end
        click_stats.action_hits = click_stats.action_hits + 1

        local result = "?"
        pcall(function() result = tostring(action.fn() or get_action_label(idx)) end)
        Log(TAG .. ": action[" .. idx .. "] " .. action.id .. " -> " .. result)

        -- Always refresh: toggles update label, others briefly flash result as button text
        if action.toggle then
            pcall(function() refresh_toggle_button(idx) end)
        else
            local capturedIdx = idx
            local capturedResult = result
            pcall(function()
                local b = action_buttons[capturedIdx]
                if is_live(b) then set_button_text(b, capturedResult) end
            end)
            pcall(function()
                LoopAsync(2000, function()
                    local b = action_buttons[capturedIdx]
                    if is_live(b) then
                        set_button_text(b, get_action_label(capturedIdx))
                    end
                    return true
                end)
            end)
        end

        return "BLOCK"
    end

    RegisterHook("WBP_Button_Default_C:BndEvt__WBP_Button_Default_WBP_Button_Base_K2Node_ComponentBoundEvent_0_OnButtonClicked__DelegateSignature", function(ctx)
        click_stats.default_hook_hits = click_stats.default_hook_hits + 1
        return handle_click("default_bndevt", ctx)
    end)
    Log(TAG .. ": WBP_Button_Default_C BndEvt click hook registered")

    RegisterHook("WBP_Button_Base_C:BndEvt__WBP_Button_Base_Button_112_K2Node_ComponentBoundEvent_0_OnButtonClickedEvent__DelegateSignature", function(ctx)
        click_stats.base_hook_hits = click_stats.base_hook_hits + 1
        return handle_click("base_bndevt", ctx)
    end)
    Log(TAG .. ": WBP_Button_Base_C BndEvt click hook registered")

    RegisterHook("WBP_Button_Base_C:OnButtonClicked__DelegateSignature", function(ctx)
        click_stats.base_hook_hits = click_stats.base_hook_hits + 1
        return handle_click("base_delegate", ctx)
    end)
    Log(TAG .. ": WBP_Button_Base_C delegate click hook registered")

    RegisterHook("WBP_Button_Base_C:BndEvt__WBP_Button_Base_Button_112_K2Node_ComponentBoundEvent_3_OnButtonPressedEvent__DelegateSignature", function(ctx)
        click_stats.base_hook_hits = click_stats.base_hook_hits + 1
        return handle_click("base_pressed", ctx)
    end)
    Log(TAG .. ": WBP_Button_Base_C pressed hook registered")

    RegisterHook("WBP_Button_Base_C:OnMouseReleased_Event", function(ctx)
        click_stats.base_hook_hits = click_stats.base_hook_hits + 1
        return handle_click("base_mouse_released", ctx)
    end)
    Log(TAG .. ": WBP_Button_Base_C mouse-release hook registered")
end)

-- ============================================================================
-- BRIDGE COMMANDS
-- ============================================================================
pcall(function()
    RegisterCommand("modmenu_status", function()
        V("modmenu_status command fired")
        local lines = { TAG .. " v4 | " .. #ACTIONS .. " actions | panel=" .. tostring(panel_built) }
        lines[#lines + 1] = "clicks: default=" .. tostring(click_stats.default_hook_hits)
            .. " base=" .. tostring(click_stats.base_hook_hits)
            .. " extracted=" .. tostring(click_stats.ctx_extract_hits)
            .. " miss=" .. tostring(click_stats.ctx_extract_miss)
            .. " legal=" .. tostring(click_stats.legal_hits)
            .. " action=" .. tostring(click_stats.action_hits)
            .. " unknown=" .. tostring(click_stats.unknown_hits)
            .. " last=" .. tostring(click_stats.last_source) .. ":" .. tostring(click_stats.last_name)
        for i = 1, #ACTIONS do
            lines[#lines + 1] = "  " .. i .. ". " .. get_action_label(i)
        end
        return table.concat(lines, "\n")
    end)
end)

pcall(function()
    RegisterCommand("modmenu_exec", function(args)
        V("modmenu_exec args=%s", tostring(args))
        local idx = tonumber(args)
        if idx and ACTIONS[idx] then
            local result = "?"
            pcall(function() result = tostring(ACTIONS[idx].fn() or get_action_label(idx)) end)
            if ACTIONS[idx].toggle then pcall(function() refresh_toggle_button(idx) end) end
            return TAG .. ": " .. result
        end
        local lines = { "Usage: modmenu_exec <1-" .. #ACTIONS .. ">" }
        for i = 1, #ACTIONS do lines[#lines + 1] = "  " .. i .. ". " .. get_action_label(i) end
        return table.concat(lines, "\n")
    end)
end)

pcall(function()
    RegisterCommand("modmenu_rebuild", function()
        V("modmenu_rebuild command fired")
        panel_built    = false
        last_settings  = nil
        action_buttons = {}
        pcall(build_mod_panel)
        local big = find_big_settings()
        return big and (TAG .. ": rebuilt on " .. big:GetName()) or TAG .. ": open Settings first"
    end)
end)

pcall(function()
    RegisterCommand("modmenu_rename", function()
        V("modmenu_rename command fired")
        pcall(rename_legal_button_all)
        return TAG .. ": rename attempted on all instances"
    end)
end)

-- Immediately rename any already-open Legal buttons (hot-reload case: widget already constructed)
pcall(function()
    rename_legal_button_all()
    Log(TAG .. ": initial rename_legal_button_all() called on load")
end)

Log(TAG .. ": v4 ready")
Log(TAG .. ": Open wrist menu -> Settings (gear) -> tap MODS to build panel")
Log(TAG .. ": Bridge: modmenu_status | modmenu_exec <N> | modmenu_rebuild | modmenu_rename")
