-- ============================================================================
-- PFX_ModMenu v10 — In-Game Mod Menu via Legal Section Takeover
-- ============================================================================
-- v10 CLICK FIX:
--   BndEvt hooks were using the wrong function name. Decompiled blueprints show
--   "ComponentBoundEvent_0" but the actual runtime UFunction is named
--   "K2Node_ComponentBoundEvent_0". PE trace (638k calls, 74s) confirmed this:
--   BndEvt__..._K2Node_ComponentBoundEvent_0_OnButtonClicked appeared 16 times.
--   BndEvt__..._ComponentBoundEvent_0 (without K2Node_) was never called.
--   Same fix applied to the slider BndEvt (K2Node_ComponentBoundEvent_2).
--   ExecuteUbergraph_WBP_Button_Default never appeared in PE trace — it is
--   called via EX_LocalFinalFunction which skips ProcessEvent entirely.
--
-- v9 ROOT CAUSE FIX #2:
--   btn_to_idx stored UObject identity as Lua table key.
--   UE4SS creates a NEW Lua wrapper object every time you touch a UObject
--   (ctx, ctx:get(), etc.), so identity comparison ALWAYS fails => lookup
--   always returns nil => no action dispatches even when the hook fires.
--
--   FIX: name_to_idx[widget:GetName()] — string keys work correctly.
--   FIX: always rebuild on MODS tab click (handles widget lifecycle).
--   FIX: extensive per-step logging so failures are visible in logcat.
-- ============================================================================
local TAG = "PFX_ModMenu"
Log(TAG .. ": Loading v10...")

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

-- Get the name of a hook ctx argument (works for both raw UObject and RemoteUnrealParam).
local function ctx_name(obj)
    if not obj then return "" end
    local nm = ""
    pcall(function() local n = obj:GetName(); if n and n ~= "" then nm = n end end)
    if nm ~= "" then return nm end
    pcall(function()
        local inner = obj:get()
        if inner then
            local n = inner:GetName()
            if n and n ~= "" then nm = n end
        end
    end)
    return nm
end

-- Get the UObject from a hook ctx (handles both raw UObject and RemoteUnrealParam).
local function ctx_obj(obj)
    if not obj then return nil end
    local ok1, v = pcall(function() return obj:IsValid() end)
    if ok1 and v then return obj end
    local ok2, inner = pcall(function() return obj:get() end)
    if ok2 and is_live(inner) then return inner end
    return nil
end

local function set_button_text(btn, text)
    if not is_live(btn) then return false end
    pcall(function() btn:Call("SetTitleText", text) end)
    pcall(function() btn:Set("Button Title Text", text) end)
    return true
end

local function clear_button_icon(btn)
    if not is_live(btn) then return end
    pcall(function() btn:Set("Icon Texture", nil) end)
end

-- ============================================================================
-- STATE
-- ============================================================================
local ball_save_on  = true
local log_states_on = true

local action_buttons = {}  -- [idx] = widget object (for syncing display)
local name_to_idx    = {}  -- [widget:GetName() string] = idx  (O(1), string-keyed)
local syncing_guard  = {}  -- [widget_name string] = true while programmatically syncing

local click_stats = { hits=0, actions=0, misses=0, last_id="", last_src="" }

-- ============================================================================
-- ACTIONS
-- ============================================================================
local ACTIONS = {

    -- ── CHEATS ───────────────────────────────────────────────────────────────
    { id="hdr_cheats", header=true, label="── CHEATS ──" },

    {
        id="ball_save", toggle=true, label="Ball Save (Infinite)",
        fn=function()
            local api = rawget(_G, "PFX_Cheats")
            if api and api.toggle_ball_save then
                ball_save_on = not not api.toggle_ball_save()
                return "Ball Save: " .. (ball_save_on and "ON" or "OFF")
            end
            ball_save_on = not ball_save_on
            return "Ball Save: " .. (ball_save_on and "ON" or "OFF") .. " (local)"
        end,
    },
    {
        id="big_ball", toggle=true, label="Big Ball",
        fn=function()
            local api = rawget(_G, "PFX_Cheats")
            if api and api.toggle_big_ball then
                local on, msg = api.toggle_big_ball()
                return "BigBall: " .. tostring(msg or on)
            end
            return "BigBall: PFX_Cheats not loaded"
        end,
    },
    {
        id="large_flippers", toggle=true, label="Large Flippers",
        fn=function()
            local api = rawget(_G, "PFX_Cheats")
            if api and api.toggle_large_flippers then
                local on, msg = api.toggle_large_flippers()
                return "Flippers: " .. tostring(msg or on)
            end
            return "Flippers: PFX_Cheats not loaded"
        end,
    },
    {
        id="flipper_length", slider=true, label="Flipper Length",
        min=1.0, max=5.0, step=0.5,
        get_value=function()
            local api = rawget(_G, "PFX_Cheats")
            return (api and api.get_flipper_scale_x and api.get_flipper_scale_x()) or 1.0
        end,
        fn=function(val)
            local api = rawget(_G, "PFX_Cheats")
            if api and api.set_flipper_length_x then
                api.set_flipper_length_x(val)
                Log(TAG .. ": [slider] flipper_length=" .. string.format("%.2f", val))
            end
        end,
    },
    {
        id="finger_mode", toggle=true, label="Finger Mode",
        fn=function()
            local api = rawget(_G, "PFX_FingerMode")
            if api and api.toggle then
                local on = api.toggle()
                return "Finger: " .. (on and "ON" or "OFF")
            end
            return "Finger: PFX_FingerMode not loaded"
        end,
    },
    {
        id="cheat_logstates", toggle=true, label="Log Game States",
        fn=function()
            local api = rawget(_G, "PFX_Cheats")
            if api and api.toggle_log_states then
                log_states_on = not not api.toggle_log_states()
                return "LogStates: " .. (log_states_on and "ON" or "OFF")
            end
            log_states_on = not log_states_on
            return "LogStates: " .. (log_states_on and "ON" or "OFF") .. " (local)"
        end,
    },

    -- ── ACTIONS ───────────────────────────────────────────────────────────────
    { id="hdr_actions", header=true, label="── ACTIONS ──" },

    {
        id="cheat_saveball", label="Save Ball NOW",
        fn=function()
            local api = rawget(_G, "PFX_Cheats")
            if api and api.save_ball then return "SaveBall: " .. tostring(select(2, api.save_ball())) end
            return "SaveBall: PFX_Cheats not loaded"
        end,
    },
    {
        id="cheat_pause", label="Pause / Resume",
        fn=function()
            local api = rawget(_G, "PFX_Cheats")
            if api and api.pause_resume then return "Pause: " .. tostring(select(2, api.pause_resume())) end
            return "Pause: PFX_Cheats not loaded"
        end,
    },
    {
        id="cheat_restartball", label="Restart Ball",
        fn=function()
            local api = rawget(_G, "PFX_Cheats")
            if api and api.restart_ball then return "Restart: " .. tostring(select(2, api.restart_ball())) end
            return "Restart: PFX_Cheats not loaded"
        end,
    },

    -- ── MAX & UNLOCK ──────────────────────────────────────────────────────────
    { id="hdr_max", header=true, label="── MAX & UNLOCK ──" },

    {
        id="max_all", label="Max All + Unlock All",
        fn=function()
            local api = rawget(_G, "PFX_Max")
            if api and api.run then pcall(api.run); return "MaxAll: done" end
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
        id="fix_trophies", label="Fix Trophies -> Physical",
        fn=function()
            local api = rawget(_G, "PFX_Max")
            if api and api.fix_trophies then
                local n=0; pcall(function() n = api.fix_trophies() or 0 end)
                return "Trophies: " .. n .. " swapped"
            end
            return "Trophies: PFX_Max not loaded"
        end,
    },

    -- ── RANDOMIZE ─────────────────────────────────────────────────────────────
    { id="hdr_rand", header=true, label="── RANDOMIZE ──" },

    {
        id="rand_all", label="Randomize All Hub Slots",
        fn=function()
            local api = rawget(_G, "PFX_Rand")
            if api then local n=0; pcall(function() n=api.scramble_all() or 0 end); return "Rand all: "..n end
            return "Rand all: PFX_Rand not loaded"
        end,
    },
    {
        id="rand_tables", label="Randomize Table Cosmetics",
        fn=function()
            local api = rawget(_G, "PFX_Rand")
            if api then pcall(api.scramble_tables); return "Rand tables: done" end
            return "Rand tables: PFX_Rand not loaded"
        end,
    },
    {
        id="rand_wall", label="Randomize Walls",
        fn=function()
            local api = rawget(_G, "PFX_Rand")
            if api then local n=0; pcall(function() n=api.scramble_cat(api.CAT_WALL) or 0 end); return "Rand Wall: "..n end
            return "Rand Wall: PFX_Rand not loaded"
        end,
    },
    {
        id="rand_floor", label="Randomize Floors",
        fn=function()
            local api = rawget(_G, "PFX_Rand")
            if api then local n=0; pcall(function() n=api.scramble_cat(api.CAT_FLOOR) or 0 end); return "Rand Floor: "..n end
            return "Rand Floor: PFX_Rand not loaded"
        end,
    },
    {
        id="rand_poster", label="Randomize Posters",
        fn=function()
            local api = rawget(_G, "PFX_Rand")
            if api then local n=0; pcall(function() n=api.scramble_cat(api.CAT_POSTER) or 0 end); return "Rand Poster: "..n end
            return "Rand Poster: PFX_Rand not loaded"
        end,
    },
    {
        id="rand_statue", label="Randomize Statues",
        fn=function()
            local api = rawget(_G, "PFX_Rand")
            if api then local n=0; pcall(function() n=api.scramble_cat(api.CAT_STATUE) or 0 end); return "Rand Statue: "..n end
            return "Rand Statue: PFX_Rand not loaded"
        end,
    },
    {
        id="rand_gadget", label="Randomize Gadgets",
        fn=function()
            local api = rawget(_G, "PFX_Rand")
            if api then local n=0; pcall(function() n=api.scramble_cat(api.CAT_GADGET) or 0 end); return "Rand Gadget: "..n end
            return "Rand Gadget: PFX_Rand not loaded"
        end,
    },
    {
        id="rand_hub", label="Randomize Hub Interior",
        fn=function()
            local api = rawget(_G, "PFX_Rand")
            if api then local n=0; pcall(function() n=api.scramble_cat(api.CAT_HUB) or 0 end); return "Rand Hub: "..n end
            return "Rand Hub: PFX_Rand not loaded"
        end,
    },
}

-- ============================================================================
-- ACTION HELPERS
-- ============================================================================
local function get_action_label(idx)
    local a = ACTIONS[idx]
    if not a then return "?" end
    if type(a.label) == "function" then return a.label() end
    return tostring(a.label)
end

local function action_toggle_state(idx)
    local a = ACTIONS[idx]
    if not a or not a.toggle then return false end
    local id = a.id
    local api = rawget(_G, "PFX_Cheats")
    if id == "ball_save" then
        return (api and api.cheats and not not api.cheats.infinite_ball_save) or ball_save_on
    elseif id == "big_ball" then
        return (api and api.cheats and not not api.cheats.big_ball) or false
    elseif id == "large_flippers" then
        return (api and api.cheats and not not api.cheats.large_flippers) or false
    elseif id == "finger_mode" then
        local fapi = rawget(_G, "PFX_FingerMode")
        if fapi then
            if fapi.is_enabled then local ok,v = pcall(fapi.is_enabled); if ok then return not not v end end
            if fapi.enabled ~= nil then return not not fapi.enabled end
        end
        return false
    elseif id == "cheat_logstates" then
        return (api and api.cheats and not not api.cheats.log_game_states) or log_states_on
    end
    return false
end

-- Sync the OptionsEntry Type=1 carousel to the actual toggle state.
-- Uses syncing_guard (string-keyed) to prevent the hook from re-firing.
local function sync_switcher_widget(idx)
    local btn = action_buttons[idx]
    if not is_live(btn) then return end
    local on = action_toggle_state(idx)
    local target = on and 1 or 0
    local nm = ""
    pcall(function() nm = btn:GetName() end)
    if nm ~= "" then syncing_guard[nm] = true end
    pcall(function()
        local os = btn:Get("OptionSwitcher")
        if is_live(os) then
            pcall(function() os:Call("SetSelectedIndex", target) end)
            pcall(function() os:Set("SelectedIndex", target) end)
        end
        pcall(function() btn:Set("SelectedOptionIndex", target) end)
        pcall(function() btn:Call("UpdateSelection") end)
    end)
    if nm ~= "" then syncing_guard[nm] = nil end
end

-- Core dispatcher: logs result, updates button display.
local function dispatch_action(idx, source)
    local a = ACTIONS[idx]
    if not a or a.header or a.slider then return end

    click_stats.actions = click_stats.actions + 1
    click_stats.last_id  = a.id
    click_stats.last_src = source

    local result = "?"
    local ok = pcall(function() result = tostring(a.fn() or get_action_label(idx)) end)
    if not ok then result = "ERROR" end

    Log(TAG .. ": [" .. source .. "] [" .. a.id .. "] -> " .. result)

    local btn = action_buttons[idx]
    if a.toggle then
        if is_live(btn) then sync_switcher_widget(idx) end
    else
        if is_live(btn) then set_button_text(btn, result) end
        pcall(function()
            ExecuteWithDelay(2000, function()
                local b = action_buttons[idx]
                if is_live(b) then set_button_text(b, get_action_label(idx)) end
            end)
        end)
    end
end

-- ============================================================================
-- SETTINGS WIDGET ACCESS
-- ============================================================================
local function get_legal_button(w)
    local lb = nil
    pcall(function() lb = w:Get("WBP_Button_Legal") end)
    return is_live(lb) and lb or nil
end

local function get_llc(w)
    local llc = nil
    pcall(function() llc = w:Get("LegalLineContainer") end)
    return is_live(llc) and llc or nil
end

local function rename_legal_button_all()
    local all = nil
    pcall(function() all = FindAllOf("WBP_WristMenuSettings_C") end)
    if not all then return end
    for _, w in ipairs(all) do
        if is_live(w) then
            pcall(function()
                local lb = get_legal_button(w)
                if not lb then return end
                set_button_text(lb, "MODS")
                clear_button_icon(lb)
                pcall(function() lb:Call("SetHasNotificationMark", true) end)
            end)
        end
    end
end

-- ============================================================================
-- BUILD MOD PANEL (v9)
-- Always clears and rebuilds — handles widget lifecycle correctly.
-- Uses name_to_idx (string-keyed) for reliable dispatch.
-- ============================================================================
local function build_mod_panel_on(w)
    if not is_live(w) then
        Log(TAG .. ": build: settings widget not live")
        return false
    end

    local wname = ""
    pcall(function() wname = w:GetName() end)
    Log(TAG .. ": build: starting on " .. wname)

    -- Rename nav button
    pcall(function()
        local lb = get_legal_button(w)
        if is_live(lb) then
            set_button_text(lb, "MODS")
            clear_button_icon(lb)
            pcall(function() lb:Call("SetHasNotificationMark", true) end)
        end
    end)

    -- Get LegalLineContainer
    local llc = get_llc(w)
    if not is_live(llc) then
        -- Nudge the switcher to make it accessible
        pcall(function()
            local ss = w:Get("SubmenuSwitcher")
            if is_live(ss) then
                local nw = ss:Call("GetNumWidgets") or 0
                if nw > 0 then ss:Call("SetActiveWidgetIndex", nw - 1) end
            end
        end)
        llc = get_llc(w)
    end
    if not is_live(llc) then
        Log(TAG .. ": build: LegalLineContainer not found on " .. wname)
        return false
    end

    -- Clear old state BEFORE creating new widgets
    for k in pairs(name_to_idx)    do name_to_idx[k]    = nil end
    for k in pairs(action_buttons) do action_buttons[k] = nil end
    for k in pairs(syncing_guard)  do syncing_guard[k]  = nil end

    pcall(function() llc:Call("ClearChildren") end)

    local pc = nil
    pcall(function() pc = FindFirstOf("BP_PlayerController_C") end)
    if not pc then
        Log(TAG .. ": build: no player controller found")
        return false
    end

    local registered = 0

    local function reg_widget(widget, idx)
        if not is_live(widget) then return end
        local nm = ""
        pcall(function() nm = widget:GetName() end)
        if nm == "" then
            Log(TAG .. ": build: widget for idx=" .. idx .. " has no name — will not dispatch")
            return
        end
        name_to_idx[nm] = idx
        action_buttons[idx] = widget
        registered = registered + 1
        Log(TAG .. ": build: idx=" .. idx .. " id=" .. ACTIONS[idx].id .. " name=" .. nm)
    end

    -- Title banner
    local title = nil
    pcall(function() title = CreateWidget("WBP_Button_Default_C", pc) end)
    if is_live(title) then
        pcall(function() llc:Call("AddChild", title) end)
        set_button_text(title, "PFX MOD MENU  v10")
        clear_button_icon(title)
        pcall(function() title:Set("IsEnabled", false) end)
    end

    -- Action rows
    for idx = 1, #ACTIONS do
        local a = ACTIONS[idx]
        if not a then goto continue end

        if a.header then
            local sep = nil
            pcall(function() sep = CreateWidget("WBP_Button_Default_C", pc) end)
            if is_live(sep) then
                pcall(function() llc:Call("AddChild", sep) end)
                set_button_text(sep, a.label)
                clear_button_icon(sep)
                pcall(function() sep:Set("IsEnabled", false) end)
            end

        elseif a.toggle then
            -- WBP_OptionsEntry_C Type=1 — native OFF/ON carousel
            local entry = nil
            pcall(function() entry = CreateWidget("WBP_OptionsEntry_C", pc) end)
            if is_live(entry) then
                pcall(function() llc:Call("AddChild", entry) end)
                pcall(function() entry:Set("Type", 1) end)
                pcall(function() entry:Set("Title", a.label) end)
                local init_idx = action_toggle_state(idx) and 1 or 0
                pcall(function()
                    local os = entry:Get("OptionSwitcher")
                    if is_live(os) then
                        pcall(function() os:Call("SetOptions", {"OFF", "ON"}) end)
                        pcall(function() os:Call("SetSelectedIndex", init_idx) end)
                        pcall(function() os:Set("SelectedIndex", init_idx) end)
                    end
                end)
                pcall(function() entry:Set("SelectedOptionIndex", init_idx) end)
                pcall(function() entry:Call("SetupEntry") end)
                pcall(function() entry:Call("PreInitButtons") end)
                pcall(function() entry:Call("UpdateSelection") end)
                reg_widget(entry, idx)
            else
                Log(TAG .. ": build: CreateWidget WBP_OptionsEntry_C failed for " .. a.id)
            end

        elseif a.slider then
            -- WBP_OptionsEntry_C Type=2 — numeric slider
            local entry = nil
            pcall(function() entry = CreateWidget("WBP_OptionsEntry_C", pc) end)
            if is_live(entry) then
                pcall(function() llc:Call("AddChild", entry) end)
                pcall(function() entry:Set("Type", 2) end)
                pcall(function() entry:Set("Title", a.label) end)
                pcall(function() entry:Set("SliderMinValue", a.min) end)
                pcall(function() entry:Set("SliderMaxValue", a.max) end)
                pcall(function() entry:Set("SliderStepSize", a.step) end)
                local cv = 1.0
                pcall(function() cv = a.get_value() end)
                pcall(function() entry:Set("SelectedSliderValue", cv) end)
                pcall(function() entry:Call("SetupEntry") end)
                pcall(function() entry:Call("RefreshValue") end)
                reg_widget(entry, idx)
            else
                Log(TAG .. ": build: CreateWidget WBP_OptionsEntry_C (slider) failed for " .. a.id)
            end

        else
            -- WBP_Button_Default_C — plain action button
            local btn = nil
            pcall(function() btn = CreateWidget("WBP_Button_Default_C", pc) end)
            if is_live(btn) then
                pcall(function() llc:Call("AddChild", btn) end)
                set_button_text(btn, get_action_label(idx))
                clear_button_icon(btn)
                reg_widget(btn, idx)
            else
                Log(TAG .. ": build: CreateWidget WBP_Button_Default_C failed for " .. a.id)
            end
        end

        ::continue::
    end

    local final = 0
    pcall(function() final = llc:Call("GetChildrenCount") end)
    Log(TAG .. ": build: done on " .. wname .. " — " .. final .. " children, " .. registered .. " registered")

    pcall(function() w:Call("ResetScroll") end)
    return registered > 0
end

-- ============================================================================
-- HOOKS
-- All wrapped in pcall; errors are logged, not swallowed silently.
-- ============================================================================

-- Construct: rename Legal→MODS
local ok, err = pcall(function()
    RegisterHook("WBP_WristMenuSettings_C:Construct", function(ctx)
        pcall(rename_legal_button_all)
    end)
end)
Log(TAG .. ": Construct hook: " .. (ok and "OK" or ("FAILED: " .. tostring(err))))

-- OnSubmenuButtonClicked: fires when any nav tab is tapped.
-- Check which widget was clicked; rebuild our panel regardless
-- (always-rebuild ensures name_to_idx stays fresh after widget lifecycle events).
local ok2, err2 = pcall(function()
    RegisterHook("WBP_WristMenuSettings_C:OnSubmenuButtonClicked", function(ctx, widget_param)
        local settings = ctx_obj(ctx)
        if not is_live(settings) then return end

        -- Log which tab was clicked
        local clicked = ctx_name(widget_param)
        Log(TAG .. ": NAV_CLICK clicked=" .. clicked)

        -- Always rebuild: handles widget lifecycle (destroy/recreate on tab switch)
        pcall(function() build_mod_panel_on(settings) end)

        pcall(function() settings:Call("ResetScroll") end)
    end)
end)
Log(TAG .. ": OnSubmenuButtonClicked hook: " .. (ok2 and "OK" or ("FAILED: " .. tostring(err2))))

-- SelectStartingCategory
local ok3, err3 = pcall(function()
    RegisterHook("WBP_WristMenuSettings_C:SelectStartingCategory", function(ctx)
        local settings = ctx_obj(ctx)
        if is_live(settings) then pcall(function() build_mod_panel_on(settings) end) end
    end)
end)
Log(TAG .. ": SelectStartingCategory hook: " .. (ok3 and "OK" or ("FAILED: " .. tostring(err3))))

-- ── WBP_Button_Default_C CLICK ──────────────────────────────────────────────
-- PE trace confirmed: BndEvt functions DO go through ProcessEvent when the
-- runtime name includes "K2Node_" — decompiled blueprints omit it.
-- Correct runtime name: K2Node_ComponentBoundEvent_0 (not ComponentBoundEvent_0)
local ok4, err4 = pcall(function()
    RegisterHook(
        "WBP_Button_Default_C:BndEvt__WBP_Button_Default_WBP_Button_Base_K2Node_ComponentBoundEvent_0_OnButtonClicked__DelegateSignature",
        function(ctx)
            click_stats.hits = click_stats.hits + 1
            local nm = ctx_name(ctx)
            Log(TAG .. ": BTN_CLICK nm=[" .. nm .. "] hits=" .. click_stats.hits)

            local idx = name_to_idx[nm]
            if not idx then
                click_stats.misses = click_stats.misses + 1
                Log(TAG .. ": BTN_CLICK miss: nm=[" .. nm .. "] (not a mod menu button)")
                return
            end

            dispatch_action(idx, "btn_click")
        end
    )
end)
Log(TAG .. ": WBP_Button_Default_C click hook: " .. (ok4 and "OK" or ("FAILED: " .. tostring(err4))))

-- ── WBP_OptionsEntry_C TOGGLE (carousel Type=1) ─────────────────────────────
local ok5, err5 = pcall(function()
    RegisterHook("WBP_OptionsEntry_C:HandleSwitcherValueChanged", function(ctx)
        local nm = ctx_name(ctx)
        Log(TAG .. ": SWITCHER_CHANGE nm=[" .. nm .. "]")

        if syncing_guard[nm] then
            Log(TAG .. ": SWITCHER_CHANGE guarded, skip")
            return
        end

        local idx = name_to_idx[nm]
        if not idx then return end
        local a = ACTIONS[idx]
        if not a or not a.toggle then return end

        local entry = ctx_obj(ctx)
        local opt_idx = 0
        pcall(function()
            local os = entry:Get("OptionSwitcher")
            if is_live(os) then
                opt_idx = tonumber(os:Get("SelectedIndex")) or 0
            else
                opt_idx = tonumber(entry:Get("SelectedOptionIndex")) or 0
            end
        end)

        local new_state = (opt_idx == 1)
        local cur_state = action_toggle_state(idx)

        if new_state ~= cur_state then
            click_stats.actions = click_stats.actions + 1
            click_stats.last_id  = a.id
            click_stats.last_src = "switcher"
            local result = "?"
            pcall(function() result = tostring(a.fn() or a.id) end)
            Log(TAG .. ": [switcher] [" .. a.id .. "] " .. (new_state and "ON" or "OFF") .. " -> " .. result)
        end

        sync_switcher_widget(idx)
    end)
end)
Log(TAG .. ": HandleSwitcherValueChanged hook: " .. (ok5 and "OK" or ("FAILED: " .. tostring(err5))))

-- ── WBP_OptionsEntry_C SLIDER (Type=2) ──────────────────────────────────────
local ok6, err6 = pcall(function()
    RegisterHook(
        "WBP_OptionsEntry_C:BndEvt__WBP_OptionsEntry_Slider_K2Node_ComponentBoundEvent_2_OnValueChanged__DelegateSignature",
        function(ctx)
            local nm = ctx_name(ctx)
            local idx = name_to_idx[nm]
            if not idx then return end
            local a = ACTIONS[idx]
            if not a or not a.slider then return end

            local entry = ctx_obj(ctx)
            local val = 1.0
            if is_live(entry) then
                pcall(function() val = tonumber(entry:Get("SelectedSliderValue")) or 1.0 end)
            end
            Log(TAG .. ": [slider] [" .. a.id .. "] val=" .. string.format("%.2f", val))
            pcall(a.fn, val)
        end
    )
end)
Log(TAG .. ": slider hook: " .. (ok6 and "OK" or ("FAILED: " .. tostring(err6))))

-- ============================================================================
-- BRIDGE COMMANDS
-- ============================================================================
pcall(function()
    RegisterCommand("modmenu_status", function()
        local lines = {
            string.format("%s v10 | actions=%d", TAG, #ACTIONS),
            string.format("clicks: hits=%d actions=%d misses=%d last=[%s]@%s",
                click_stats.hits, click_stats.actions, click_stats.misses,
                click_stats.last_id, click_stats.last_src),
            string.format("name_to_idx entries: %d", (function() local n=0; for _ in pairs(name_to_idx) do n=n+1 end; return n end)()),
        }
        for nm, idx in pairs(name_to_idx) do
            local a = ACTIONS[idx]
            if a then
                lines[#lines+1] = "  [" .. nm .. "] => " .. idx .. " (" .. a.id .. ")"
            end
        end
        return table.concat(lines, "\n")
    end)
end)

pcall(function()
    RegisterCommand("modmenu_exec", function(args)
        local idx = tonumber(args)
        if idx and ACTIONS[idx] and not ACTIONS[idx].header then
            dispatch_action(idx, "bridge_exec")
            return TAG .. ": exec done — " .. click_stats.last_id
        end
        local lines = { "Usage: modmenu_exec <1-" .. #ACTIONS .. ">" }
        for i = 1, #ACTIONS do
            local a = ACTIONS[i]
            if not a.header then
                lines[#lines+1] = "  " .. i .. ". " .. get_action_label(i)
            end
        end
        return table.concat(lines, "\n")
    end)
end)

pcall(function()
    RegisterCommand("modmenu_rebuild", function()
        for k in pairs(name_to_idx)    do name_to_idx[k]    = nil end
        for k in pairs(action_buttons) do action_buttons[k] = nil end
        local all = nil; pcall(function() all = FindAllOf("WBP_WristMenuSettings_C") end)
        local n = 0
        if all then
            for _, w in ipairs(all) do
                if is_live(w) and build_mod_panel_on(w) then n = n + 1 end
            end
        end
        return n > 0 and (TAG .. ": rebuilt on " .. n .. " instance(s)")
            or TAG .. ": open Settings -> MODS tab first"
    end)
end)

pcall(function()
    RegisterCommand("modmenu_rename", function()
        pcall(rename_legal_button_all)
        return TAG .. ": rename attempted"
    end)
end)

-- Hot-reload: rename any already-open Legal buttons immediately
pcall(function()
    rename_legal_button_all()
    Log(TAG .. ": initial rename done")
end)

-- ============================================================================
Log(TAG .. ": v10 ready — " .. #ACTIONS .. " items")
Log(TAG .. ": OPEN wrist menu -> Settings (gear) -> tap MODS tab")
Log(TAG .. ": BRIDGE: modmenu_status | modmenu_exec <N> | modmenu_rebuild")
Log(TAG .. ": KEY FIX v10: ExecuteUbergraph ep=10 hook replaces broken BndEvt hook")
