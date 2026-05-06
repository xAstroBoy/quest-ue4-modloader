--[[
    DebugMenuAPI v26.0 — In-place widget updates (no ClearWidgets)
    ===================================================
    A mod-menu system that cooperates with the stock DebugMenu_C.
    NEVER blocks or overrides Blueprint behaviour.

    The stock debug menu works 100 % unchanged for every built-in page.
    This mod only takes over for custom byte-IDs (100 +).

    ─────────────────────────────────────────────────────────────────────
    HOW IT WORKS  (v25 — Deferred injection + BndEvt hooks)
    ─────────────────────────────────────────────────────────────────────
    IMPORTANT: Blueprint-internal function calls (DoAction, NewMenu,
    InputActionConfirm etc.) do NOT go through ProcessEvent so they
    CANNOT be hooked. Only delegate-dispatched BndEvt functions on
    VR4PlayerController_BP_C go through ProcessEvent.

    v25 KEY FIX: The PostHook on OpenDebugMenu fires BEFORE Blueprint's
    latent actions complete (e.g. fade-in animations, deferred NewMenu).
    Injecting "Mods" immediately gets wiped by subsequent Blueprint
    execution. We now use ExecuteWithDelay with multiple retry windows
    to inject AFTER all Blueprint activity settles. BndEvt Scroll hooks
    provide lazy injection as a failsafe.

    1.  PostHook on OpenDebugMenu schedules deferred injection of "Mods"
        at 200ms/500ms/1000ms after menu opens (retries until it sticks).
    2.  BndEvt Scroll hooks (IDs 0,1,2,4,5,6,7,11,13) lazily inject
        "Mods" whenever user scrolls on Main page (AM=1).
    3.  PostHook BndEvt Confirm (3, 8, 10) — detects "Mods" selection
        on Main page and navigates to page 100 via Lua.
    4.  PostHook NewMenu — fires ONLY when called via ProcessEvent
        (i.e. from our Lua dm:Call). APPENDS custom option widgets.
    5.  PostHook BndEvt Back (9) — when Blueprint navigates back to a
        custom page, rebuilds it.

    BLUEPRINT HANDLES (never touched by Lua):
        ✓  Scrolling          InputActionScrollUp / Down
        ✓  Highlighting       UpdateOptionHighlight
        ✓  Back navigation    PreviousMenus TArray stack
        ✓  "Back" widget      auto-added by NewMenu for pages ≠ 0,1,28
        ✓  VBoxList scroll    FirstVisible / MaxVisible

    ─────────────────────────────────────────────────────────────────────
    PUBLIC API  (SharedAPI.DebugMenu)
    ─────────────────────────────────────────────────────────────────────
    SIMPLE  (static items on root Mods page):
        .RegisterToggle (mod, name, getter, setter)
        .RegisterAction (mod, name, callback)
        .RegisterSelector(mod, name, options, callback)

    ADVANCED (dynamic sub-pages):
        .RegisterSubMenu(mod, name, onEnter)
        .NavigateTo({populate = fn, name = title})
        .AddItem(name, callback)
        .Refresh()
        .AddPage(pageId, pageName) → page
        .AddItemToPage(page, mod, name, type, opts)
]]

local MOD_NAME = "DebugMenuAPI"
local VERSION  = "26.0"

local ipairs   = ipairs
local math     = math
local pairs    = pairs
local pcall    = pcall
local print    = print
local rawget   = rawget
local string   = string
local table    = table
local tonumber = tonumber
local tostring = tostring
local type     = type

-- ═══════════════════════════════════════════════════════════════════════
-- STATE
-- ═══════════════════════════════════════════════════════════════════════

local MODS_ROOT_BYTE = 100            -- root "Mods" page
local next_page_byte = 101            -- auto-incremented for sub-pages
local pages          = {}             -- byte → page table
local dm_cache       = nil            -- cached DebugMenu_C reference
local initialised    = false

-- Build context (only valid inside a populate_fn)
local _build_page    = nil
local _refreshing    = false
local _submenu_name  = nil
local RegisterBridgeCommand = rawget(_G, "RegisterBridgeCommand")

-- Root page — always exists, items are persistent (static)
pages[MODS_ROOT_BYTE] = {
    name        = "Mods",
    items       = {},
    populate_fn = nil,   -- nil → static page
}

-- ═══════════════════════════════════════════════════════════════════════
-- LOGGING
-- ═══════════════════════════════════════════════════════════════════════

local function Log(msg)
    print("[" .. MOD_NAME .. "] " .. tostring(msg))
end

local function Warn(msg)
    print("[" .. MOD_NAME .. "] WARN: " .. tostring(msg))
end

local function Err(msg)
    print("[" .. MOD_NAME .. "] ERROR: " .. tostring(msg))
end

local TAG = "[DebugMenuAPI]"
local VERBOSE = true
local function V(...) if VERBOSE then print(TAG .. " [V] " .. string.format(...)) end end

local function isDefaultObject(obj)
    if not obj then return false end
    local ok, name = pcall(function() return obj:GetName() end)
    return ok and type(name) == "string" and name:sub(1, 9) == "Default__"
end

local function findFirstNonDefault(className)
    local first = nil
    pcall(function() first = FindFirstOf(className) end)
    if first and first:IsValid() and not isDefaultObject(first) then
        return first
    end
    local all = nil
    pcall(function() all = FindAllOf(className) end)
    if all then
        for _, obj in ipairs(all) do
            if obj and obj:IsValid() and not isDefaultObject(obj) then
                return obj
            end
        end
    end
    return nil
end

-- ═══════════════════════════════════════════════════════════════════════
-- DEBUGMENU_C ACCESS
-- ═══════════════════════════════════════════════════════════════════════

--- Return the live DebugMenu_C singleton (cached).
local function get_dm()
    if dm_cache and dm_cache:IsValid() then return dm_cache end
    V("get_dm: cache miss, calling FindFirstOf('DebugMenu_C')")
    local ok, r = pcall(findFirstNonDefault, "DebugMenu_C")
    if ok and r and r:IsValid() then
        dm_cache = r
        V("get_dm: found %s", tostring(r:GetName()))
        return r
    end
    V("get_dm: NOT FOUND (ok=%s, r=%s)", tostring(ok), tostring(r))
    dm_cache = nil
    return nil
end

--- Read ActiveMenu byte from DebugMenu_C.
local function get_active_menu(dm)
    local ok, v = pcall(function() return dm:Get("ActiveMenu") end)
    return (ok and type(v) == "number") and v or nil
end

--- Read CurrentIndex from DebugMenu_C.
local function get_current_index(dm)
    local ok, v = pcall(function() return dm:Get("CurrentIndex") end)
    return (ok and type(v) == "number") and v or nil
end

local function get_widgets(dm)
    local ok, widgets = pcall(function() return dm:Get("ActiveOptionsWidgets") end)
    if not ok or not widgets then return nil end
    return widgets
end

local function get_widget_count(widgets)
    if not widgets then return 0 end
    local count = 0
    pcall(function() count = #widgets end)
    return count
end

local function get_widget_at_zero_based(widgets, zero_based_index)
    if not widgets or type(zero_based_index) ~= "number" or zero_based_index < 0 then
        return nil
    end

    local arr_index = zero_based_index + 1
    local count = get_widget_count(widgets)
    if arr_index < 1 or arr_index > count then
        V("widget access OOB: zero_based=%s arr_index=%s count=%s",
            tostring(zero_based_index), tostring(arr_index), tostring(count))
        return nil
    end

    local ok, widget = pcall(function() return widgets[arr_index] end)
    if not ok or not widget or not widget:IsValid() then
        return nil
    end
    return widget
end

-- ═══════════════════════════════════════════════════════════════════════
-- VIEWPORT HELPERS
-- ═══════════════════════════════════════════════════════════════════════

--- Ensure the WidgetComponent's render target is large enough.
--- The PAK mod increases SizeBox_90 HeightOverride from 1200 to 2000,
--- and bDrawAtDesiredSize causes the render target to auto-adjust.
--- This function forces a redraw after dynamic content changes.
local function ensure_viewport(dm)
    pcall(function()
        local widget_comp = dm:Get("Widget")
        if widget_comp and widget_comp:IsValid() then
            widget_comp:Call("RequestRedraw")
        end
    end)
end

-- ═══════════════════════════════════════════════════════════════════════
-- MODS OPTION INJECTION (programmatic — replaces old PAK DataTable row)
-- ═══════════════════════════════════════════════════════════════════════

--- Append a "Mods" option widget to the Main page (AM=1) if not present.
--- Called after Blueprint builds the stock 24-item Main page.
--- Returns true if "Mods" is now present, false if injection failed.
local function ensure_mods_option(dm)
    local widgets = get_widgets(dm)
    if not widgets then V("ensure_mods: no widgets array"); return false end

    local count = get_widget_count(widgets)

    -- Need at least the stock items to be present before injecting
    if count < 10 then
        V("ensure_mods: only %d widgets, too early (waiting for stock items)", count)
        return false
    end

    -- Scan for existing "Mods" widget
    for i = 1, count do
        local ok2, w = pcall(function() return widgets[i] end)
        if ok2 and w and w:IsValid() then
            local ok3, name = pcall(function() return w:Get("OptionName") end)
            if ok3 and tostring(name) == "Mods" then
                V("ensure_mods: already present at index %d", i)
                return true
            end
        end
    end

    -- Not found → inject as last option
    local ok_create = pcall(function() dm:Call("CreateActiveOption", "Mods") end)
    if ok_create then
        Log("Injected 'Mods' option on Main page (item #" .. (count + 1) .. ")")

        -- Increase MaxVisible so user can scroll to see "Mods"
        pcall(function()
            local pw = dm:Get("ParentWidget")
            if pw then
                local vbl = pw:Get("DebugVBoxList")
                if vbl then
                    vbl:Set("MaxVisible", 30)
                    vbl:Call("UpdateListView")
                end
            end
        end)

        -- Force render target update
        ensure_viewport(dm)
    else
        Warn("Failed to inject 'Mods' option widget")
    end
    return ok_create
end

--- Schedule deferred injection of "Mods" on the Main page.
--- Uses multiple retry windows to survive Blueprint latent actions
--- that may rebuild/clear the widget list after OpenDebugMenu returns.
local function schedule_mods_injection()
    local delays = {200, 500, 1000, 2000}
    for _, ms in ipairs(delays) do
        ExecuteWithDelay(ms, function()
            pcall(function()
                local dm = get_dm()
                if not dm then return end
                local am = get_active_menu(dm)
                if am ~= 1 then return end
                ensure_mods_option(dm)
            end)
        end)
    end
end

-- ═══════════════════════════════════════════════════════════════════════
-- DISPLAY HELPERS
-- ═══════════════════════════════════════════════════════════════════════

--- Build the display string for a menu item.
--- Properly calls getter() for toggles so the label stays in sync.
local function make_display(item)
    local label = item.name or "?"

    if item.type == "toggle" then
        -- Call the getter for the live state
        local state_val = false
        if type(item.getter) == "function" then
            local ok, v = pcall(item.getter)
            state_val = ok and (v == true) or false
        end
        label = label .. (state_val and "  [ON]" or "  [OFF]")

    elseif item.type == "selector" and item.options and #item.options > 0 then
        local idx = ((item.sel_index or 0) % #item.options) + 1
        label = label .. "  [" .. tostring(item.options[idx]) .. "]"

    elseif item.type == "submenu" or item.type == "page_link" then
        label = label .. "  >>"
    end

    return label
end

-- ═══════════════════════════════════════════════════════════════════════
-- PAGE BUILDING
-- ═══════════════════════════════════════════════════════════════════════

--- Create option widgets for every item in the page and set the title.
--- Called inside PostHook NewMenu after Blueprint has finished its work.
local function build_page(dm, page)
    V("build_page: name=%s, items_before=%d, dynamic=%s", page.name or "?", #page.items, tostring(page.populate_fn ~= nil))
    -- Dynamic pages: re-create item list via populate_fn each time
    if page.populate_fn then
        page.items = {}
        _build_page = page
        pcall(page.populate_fn)
        _build_page = nil
    end

    -- Create one widget per item
    if #page.items == 0 then
        V("build_page: no items, creating placeholder")
        pcall(function() dm:Call("CreateActiveOption", "(No mods registered)") end)
    else
        V("build_page: creating %d option widgets", #page.items)
        for _, item in ipairs(page.items) do
            pcall(function() dm:Call("CreateActiveOption", make_display(item)) end)
        end
    end

    -- Cosmetics: title + VBoxList settings
    pcall(function()
        local pw = dm:Get("ParentWidget")
        V("build_page: ParentWidget=%s", tostring(pw))
        if pw then
            local tt = pw:Get("TitleText")
            if tt then tt:Set("Text", page.name or "Mods") end
        end
    end)
    pcall(function()
        local pw = dm:Get("ParentWidget")
        if pw then
            local vbl = pw:Get("DebugVBoxList")
            V("build_page: DebugVBoxList=%s", tostring(vbl))
            if vbl then
                vbl:Set("MaxVisible", 50)
                vbl:Set("FirstVisible", 0)
                vbl:Call("UpdateListView")
            end
        end
    end)

    -- Force render target update after dynamic content change
    ensure_viewport(dm)
end

--- Soft-refresh: clear and re-render the current custom page without
--- navigating away.  Preserves the selection cursor.
local function refresh_page(dm, page_byte, cursor)
    if _refreshing then return end
    _refreshing = true
    V("refresh_page: byte=%d, cursor=%s", page_byte, tostring(cursor))

    local page = pages[page_byte]
    if not page then _refreshing = false; return end

    -- Wipe widgets (including the auto-added "Back")
    V("refresh_page: ClearWidgets")
    pcall(function() dm:Call("ClearWidgets") end)

    -- Re-add "Back" at index 0 (Blueprint recognises it by name)
    pcall(function() dm:Call("CreateActiveOption", "Back") end)

    -- Rebuild + render items
    build_page(dm, page)

    -- Restore cursor
    pcall(function() dm:Set("CurrentIndex", cursor or 0) end)
    pcall(function() dm:Call("UpdateOptionHighlight") end)

    -- Force render target update
    ensure_viewport(dm)

    _refreshing = false
end

--- Full rebuild of a custom page from scratch.
--- Used when Blueprint's internal NewMenu call didn't trigger our PostHook
--- (e.g. PreviousMenu back-navigation to a custom page).
local function rebuild_custom_page(dm, page_byte)
    V("rebuild_custom_page: byte=%d", page_byte)
    local page = pages[page_byte]
    if not page then V("rebuild_custom_page: no page for byte %d", page_byte); return end

    -- Wipe whatever Blueprint left (might be empty or just "Back")
    V("rebuild_custom_page: ClearWidgets")
    pcall(function() dm:Call("ClearWidgets") end)

    -- Re-add "Back" at index 0 (Blueprint recognises it by name)
    pcall(function() dm:Call("CreateActiveOption", "Back") end)

    -- Build items + cosmetics
    build_page(dm, page)

    -- Reset cursor to "Back" and update highlight
    pcall(function() dm:Set("CurrentIndex", 0) end)
    pcall(function() dm:Call("UpdateOptionHighlight") end)
end

-- ═══════════════════════════════════════════════════════════════════════
-- SELECTED WIDGET NAME
-- ═══════════════════════════════════════════════════════════════════════

--- Read the OptionName string of the widget at CurrentIndex.
local function get_selected_option_name(dm)
    local ci = get_current_index(dm)
    if not ci then return nil end

    local widgets = get_widgets(dm)
    if not widgets then return nil end

    local widget = get_widget_at_zero_based(widgets, ci)
    if not widget then return nil end

    local ok_n, name = pcall(function() return widget:Get("OptionName") end)
    return ok_n and tostring(name or "") or nil
end

-- ═══════════════════════════════════════════════════════════════════════
-- ITEM ACTION DISPATCH
-- ═══════════════════════════════════════════════════════════════════════

--- Handle a confirm action on the currently-selected item.
local function dispatch_item(dm, page_byte)
    local page = pages[page_byte]
    if not page then return end

    local ci = get_current_index(dm)
    if not ci then return end

    -- Index mapping: Back = 0, items[1] = 1, items[2] = 2 …
    -- ci = 0 is "Back" (handled by Blueprint as a Link, not DoAction).
    if ci < 1 or ci > #page.items then return end
    local item = page.items[ci]
    V("dispatch_item: page=%d, ci=%d, item=%s, type=%s", page_byte, ci, item.name or "?", item.type or "?")

    -- ── Toggle ──────────────────────────────────────────────────
    if item.type == "toggle" then
        -- Read current state from getter
        local current = false
        if type(item.getter) == "function" then
            local ok, v = pcall(item.getter)
            current = ok and (v == true) or false
        end
        -- Invert and call setter
        local new_state = not current
        if item.setter then
            pcall(item.setter, new_state)
        end
        -- Update widget label in-place (works for ALL page types — never ClearWidgets)
        pcall(function()
            local widgets = get_widgets(dm)
            local w = get_widget_at_zero_based(widgets, ci)
            if w then
                w:Set("OptionName", make_display(item))
                pcall(function() w:Call("Setup") end)
            end
        end)
        ensure_viewport(dm)

    -- ── Selector ────────────────────────────────────────────────
    elseif item.type == "selector" and item.options and #item.options > 0 then
        item.sel_index = ((item.sel_index or 0) + 1) % #item.options
        if item.callback then
            local val = item.options[item.sel_index + 1]
            pcall(item.callback, val, item.sel_index, item)
        end
        -- Update widget label in-place (works for ALL page types — never ClearWidgets)
        pcall(function()
            local widgets = get_widgets(dm)
            local w = get_widget_at_zero_based(widgets, ci)
            if w then
                w:Set("OptionName", make_display(item))
                pcall(function() w:Call("Setup") end)
            end
        end)
        ensure_viewport(dm)

    -- ── SubMenu link ────────────────────────────────────────────
    elseif item.type == "submenu" then
        if item.callback then
            _submenu_name = item.name
            pcall(item.callback)
            _submenu_name = nil
        end

    -- ── Page link ───────────────────────────────────────────────
    elseif item.type == "page_link" then
        if item.callback then pcall(item.callback) end

    -- ── Action ──────────────────────────────────────────────────
    elseif item.type == "action" then
        if item.callback then pcall(item.callback, item) end
    end
end

-- ═══════════════════════════════════════════════════════════════════════
-- HOOKS — ALL POST-HOOKS, NEVER BLOCK
-- ═══════════════════════════════════════════════════════════════════════

local function setup_hooks()
    local DM_PATH = "/Game/Blueprints/Debug/DebugMenu/DebugMenu.DebugMenu_C"

    -- BndEvt function name template on VR4PlayerController_BP_C.
    -- These are the ONLY functions called through ProcessEvent for
    -- debug menu input (delegate-dispatched from the input system).
    -- Blueprint-internal calls (DoAction, NewMenu, InputActionConfirm)
    -- do NOT go through ProcessEvent and CANNOT be hooked.
    local function bndevt(n)
        return "VR4PlayerController_BP_C:"
            .. "BndEvt__DebugInput_K2Node_ComponentBoundEvent_"
            .. n
            .. "_DebugKeyMulticastDelegate__DelegateSignature"
    end

    -- ┌─────────────────────────────────────────────────────────────────┐
    -- │ PostHook : NewMenu                                             │
    -- │ Fires ONLY when NewMenu is called via ProcessEvent — i.e.      │
    -- │ from our Lua dm:Call("NewMenu", byte).                         │
    -- │ Blueprint-internal NewMenu calls (from DoAction, PreviousMenu) │
    -- │ do NOT trigger this hook.                                      │
    -- │ We APPEND our items after Blueprint has finished its work.      │
    -- └─────────────────────────────────────────────────────────────────┘
    RegisterPostHook(DM_PATH .. ":NewMenu", function(self, func, parms)
        V("PostHook NewMenu fired")
        pcall(function()
            local dm = get_dm()
            if not dm then V("PostHook NewMenu: get_dm() returned nil"); return end

            local am = get_active_menu(dm)
            V("PostHook NewMenu: AM=%s, is_custom=%s", tostring(am), tostring(am and pages[am] ~= nil))
            if not am or not pages[am] then return end

            local page = pages[am]
            build_page(dm, page)

            Log("Page " .. am .. " (" .. (page.name or "?") .. ") — "
                .. #page.items .. " items"
                .. (page.populate_fn and " [dynamic]" or ""))
        end)
    end)

    -- ┌─────────────────────────────────────────────────────────────────┐
    -- │ PostHook : OpenDebugMenu                                       │
    -- │ Fires when menu is opened via interface message from            │
    -- │ VR4PlayerController_BP_C:DebugMenuButtonPressed.               │
    -- │ Blueprint has latent actions that build widgets AFTER this      │
    -- │ function returns, so direct injection gets wiped.              │
    -- │ We schedule DEFERRED injection at multiple delay windows.      │
    -- └─────────────────────────────────────────────────────────────────┘
    pcall(function()
        RegisterPostHook(DM_PATH .. ":OpenDebugMenu", function(self, func, parms)
            Log("PostHook OpenDebugMenu FIRED — scheduling deferred injection")
            V("PostHook OpenDebugMenu fired — scheduling deferred injection")
            schedule_mods_injection()
        end)
        Log("PostHook: OpenDebugMenu → deferred 'Mods' injection (200/500/1000/2000ms)")
    end)

    -- ┌─────────────────────────────────────────────────────────────────┐
    -- │ PostHook : BndEvt Scroll (IDs 0,1,2,4,5,6,7,11,13)            │
    -- │ Lazy injection: whenever user scrolls on Main page (AM=1),    │
    -- │ check if "Mods" exists and inject if missing. This is a       │
    -- │ failsafe for the deferred injection above.                    │
    -- └─────────────────────────────────────────────────────────────────┘
    local SCROLL_IDS = {0, 1, 2, 4, 5, 6, 7, 11, 13}

    local function on_scroll_post(self, func, parms)
        pcall(function()
            local dm = get_dm()
            if not dm then return end
            local am = get_active_menu(dm)
            if am ~= 1 then return end
            ensure_mods_option(dm)
        end)
    end

    for _, id in ipairs(SCROLL_IDS) do
        pcall(function() RegisterPostHook(bndevt(id), on_scroll_post) end)
    end
    Log("PostHook: BndEvt Scroll (" .. #SCROLL_IDS .. " IDs) → lazy 'Mods' injection on page 1")

    -- ┌─────────────────────────────────────────────────────────────────┐
    -- │ PostHook : BndEvt Confirm (IDs 3, 8, 10)                       │
    -- │ L Trigger, A button, R Trigger — all fire InputActionConfirm   │
    -- │ inside the Blueprint, which calls DoAction internally.         │
    -- │                                                                │
    -- │ Our PostHook fires AFTER the entire BndEvt handler completes   │
    -- │ (including DoAction, ProcessNewSetting, etc.).                  │
    -- │                                                                │
    -- │ Two jobs:                                                      │
    -- │   1. Main page (AM=1): detect "Mods" → navigate to page 100   │
    -- │   2. Custom page (AM≥100): dispatch item action, or rebuild    │
    -- │      if we just landed here via "Back" confirm navigation.     │
    -- └─────────────────────────────────────────────────────────────────┘
    local CONFIRM_IDS = {3, 8, 10}

    local function on_confirm_post(self, func, parms)
        local pressed = ReadU8(parms)
        V("BndEvt Confirm: pressed=%d", pressed)
        if pressed == 0 then return end   -- release event, skip

        pcall(function()
            local dm = get_dm()
            if not dm then V("BndEvt Confirm: dm=nil, aborting"); return end

            local am = get_active_menu(dm)
            V("BndEvt Confirm: AM=%s, ci=%s", tostring(am), tostring(get_current_index(dm)))
            if not am then return end

            -- Job 1: Main page → navigate to Mods root
            if am == 1 then
                -- Lazy injection fallback (in case deferred injection missed)
                ensure_mods_option(dm)
                local opt = get_selected_option_name(dm)
                V("BndEvt Confirm: Main page, selected=%s", tostring(opt))
                if opt == "Mods" then
                    Log("'Mods' selected on Main → page " .. MODS_ROOT_BYTE)
                    ExecuteAsync(function()
                        pcall(function() dm:Call("NewMenu", MODS_ROOT_BYTE) end)
                    end)
                end
                return
            end

            -- Job 2: Custom page
            if pages[am] then
                local ci = get_current_index(dm)
                V("BndEvt Confirm: custom page %d, ci=%s", am, tostring(ci))
                if ci and ci >= 1 then
                    -- Normal item confirm → dispatch action
                    dispatch_item(dm, am)
                else
                    V("BndEvt Confirm: ci=0 on custom page, rebuilding")
                    -- ci=0: user confirmed "Back" and Blueprint navigated
                    -- us to this custom page via PreviousMenu → NewMenu.
                    -- Since NewMenu was called internally (no ProcessEvent),
                    -- our PostHook didn't fire. Rebuild the page now.
                    rebuild_custom_page(dm, am)
                end
            end
        end)
    end

    for _, id in ipairs(CONFIRM_IDS) do
        RegisterPostHook(bndevt(id), on_confirm_post)
    end

    -- ┌─────────────────────────────────────────────────────────────────┐
    -- │ PostHook : BndEvt Back (ID 9)                                  │
    -- │ B button — fires InputActionBack inside the Blueprint, which   │
    -- │ calls PreviousMenu → NewMenu(previous_page) internally.        │
    -- │                                                                │
    -- │ If we land on a custom page after going back, we need to       │
    -- │ rebuild it because the internal NewMenu call didn't trigger     │
    -- │ our PostHook.                                                  │
    -- └─────────────────────────────────────────────────────────────────┘
    RegisterPostHook(bndevt(9), function(self, func, parms)
        local pressed = ReadU8(parms)
        V("BndEvt Back: pressed=%d", pressed)
        if pressed == 0 then return end

        pcall(function()
            local dm = get_dm()
            if not dm then V("BndEvt Back: dm=nil, aborting"); return end

            local am = get_active_menu(dm)
            V("BndEvt Back: AM=%s, is_custom=%s", tostring(am), tostring(am and pages[am] ~= nil))
            if not am then return end

            -- Returned to Main page → schedule "Mods" injection
            if am == 1 then
                schedule_mods_injection()
                return
            end

            -- Custom page → rebuild
            if pages[am] then
                rebuild_custom_page(dm, am)
            end
        end)
    end)

    Log("Hooks installed (PostHook NewMenu + OpenDebugMenu(deferred) + BndEvt Scroll×9 + Confirm×3 + Back)")
end

-- ═══════════════════════════════════════════════════════════════════════
-- SHARED API — PUBLIC INTERFACE FOR OTHER MODS
-- ═══════════════════════════════════════════════════════════════════════

local function setup_shared_api()
    if not SharedAPI then SharedAPI = {} end
    local api = {}
    api.VERSION = VERSION

    -- ── Simple API ──────────────────────────────────────────────────

    --- Register a boolean toggle on the root Mods page.
    ---@param mod_name  string   Unique mod identifier
    ---@param name      string   Display label
    ---@param getter    function () → bool    returns current state
    ---@param setter    function (bool) → void  applies new state
    ---@return table item handle
    function api.RegisterToggle(mod_name, name, getter, setter)
        local item = {
            mod    = mod_name,
            name   = name,
            type   = "toggle",
            getter = getter,   -- fn() → bool
            setter = setter,   -- fn(bool)
        }
        table.insert(pages[MODS_ROOT_BYTE].items, item)
        Log("  + toggle  [" .. mod_name .. "] " .. name)
        return item
    end

    --- Register a one-shot action button on the root Mods page.
    ---@param mod_name  string
    ---@param name      string
    ---@param callback  function ()
    ---@return table item handle
    function api.RegisterAction(mod_name, name, callback)
        local item = {
            mod      = mod_name,
            name     = name,
            type     = "action",
            callback = callback,
        }
        table.insert(pages[MODS_ROOT_BYTE].items, item)
        Log("  + action  [" .. mod_name .. "] " .. name)
        return item
    end

    --- Register a cycle-selector on the root Mods page.
    ---@param mod_name  string
    ---@param name      string
    ---@param options   string[]  {"Opt1", "Opt2", …}
    ---@param callback  function (value, index, item)
    ---@return table item handle
    function api.RegisterSelector(mod_name, name, options, callback)
        local item = {
            mod       = mod_name,
            name      = name,
            type      = "selector",
            options   = options or {},
            sel_index = 0,
            callback  = callback,
        }
        table.insert(pages[MODS_ROOT_BYTE].items, item)
        Log("  + select  [" .. mod_name .. "] " .. name)
        return item
    end

    -- ── Advanced API ────────────────────────────────────────────────

    --- Register a sub-menu link on the root Mods page.
    --- When selected, `onEnter` fires.  Inside it, call api.NavigateTo().
    ---@param mod_name  string
    ---@param name      string  (also becomes the sub-page title)
    ---@param onEnter   function ()
    ---@return table item handle
    function api.RegisterSubMenu(mod_name, name, onEnter)
        local item = {
            mod      = mod_name,
            name     = name,
            type     = "submenu",
            callback = onEnter,
        }
        table.insert(pages[MODS_ROOT_BYTE].items, item)
        Log("  + submenu [" .. mod_name .. "] " .. name)
        return item
    end

    --- Navigate to a dynamic sub-page.
    --- Call inside RegisterSubMenu's onEnter or from AddItem callbacks.
    --- populate() is re-invoked on every render / Refresh().
    ---@param opts {populate: function, name?: string}
    function api.NavigateTo(opts)
        opts = opts or {}
        local byte = next_page_byte
        next_page_byte = next_page_byte + 1
        V("NavigateTo: byte=%d, name=%s", byte, tostring(opts.name or _submenu_name or "?"))
        if byte > 254 then
            Warn("Page byte overflow (> 254)")
            return
        end

        pages[byte] = {
            name        = opts.name or _submenu_name or "Mods",
            items       = {},
            populate_fn = opts.populate,
        }

        local dm = get_dm()
        if dm then
            ExecuteAsync(function()
                pcall(function() dm:Call("NewMenu", byte) end)
            end)
        end
    end

    --- Add an action item to the page currently being built by
    --- a populate() callback inside NavigateTo.
    ---@param name      string         Display label
    ---@param callback  function|nil   Confirm action (nil = label / separator)
    function api.AddItem(name, callback)
        if not _build_page then
            Warn("AddItem() called outside a populate() callback")
            return
        end
        table.insert(_build_page.items, {
            name     = name,
            type     = "action",
            callback = callback,
        })
    end

    --- Force-refresh the current custom page (re-renders labels in-place).
    --- For dynamic pages this re-invokes populate() then updates each
    --- widget's OptionName without ClearWidgets (preserves input state).
    function api.Refresh()
        V("api.Refresh called")
        if _refreshing then V("api.Refresh: re-entrant, skipping"); return end
        _refreshing = true

        local dm = get_dm()
        if not dm then V("api.Refresh: dm=nil"); _refreshing = false; return end
        local am = get_active_menu(dm)
        if not am or not pages[am] then
            V("api.Refresh: AM=%s not custom", tostring(am))
            _refreshing = false; return
        end

        local page = pages[am]
        local cursor = get_current_index(dm) or 0

        -- For dynamic pages, re-run populate_fn to get updated items/labels
        if page.populate_fn then
            page.items = {}
            _build_page = page
            pcall(page.populate_fn)
            _build_page = nil
        end

        -- In-place update: walk widgets and update each label
        -- Widget layout: [1]=Back, [2]=items[1], [3]=items[2], ...
        pcall(function()
            local widgets = get_widgets(dm)
            if not widgets then
                V("api.Refresh: widgets=nil — falling back")
                _refreshing = false
                refresh_page(dm, am, cursor)
                return
            end

            local wcount = get_widget_count(widgets)
            local expected = #page.items + 1  -- +1 for Back
            if wcount ~= expected then
                V("api.Refresh: widget count mismatch (got %d, expected %d) — falling back", wcount, expected)
                -- Fallback: full rebuild (rare — only if item count changed)
                _refreshing = false
                refresh_page(dm, am, cursor)
                return
            end
            for i, item in ipairs(page.items) do
                local w = get_widget_at_zero_based(widgets, i)
                if w then
                    w:Set("OptionName", make_display(item))
                    pcall(function() w:Call("Setup") end)
                end
            end
        end)

        -- Restore cursor position and highlight
        pcall(function() dm:Set("CurrentIndex", cursor) end)
        pcall(function() dm:Call("UpdateOptionHighlight") end)
        ensure_viewport(dm)

        _refreshing = false
    end

    --- Create a named static sub-page and add a navigation link on
    --- the root Mods page.  Returns the page table.
    ---@param page_id   string  Unique page identifier
    ---@param page_name string  Display title
    ---@return table|nil page handle (pass to AddItemToPage)
    function api.AddPage(page_id, page_name)
        local byte = next_page_byte
        next_page_byte = next_page_byte + 1
        if byte > 254 then
            Warn("Page byte overflow (> 254)")
            return nil
        end

        pages[byte] = {
            name        = page_name,
            items       = {},
            populate_fn = nil,
        }

        -- Auto-add navigation link on root page
        table.insert(pages[MODS_ROOT_BYTE].items, {
            name = page_name,
            type = "page_link",
            callback = function()
                local dm = get_dm()
                if dm then
                    ExecuteAsync(function()
                        pcall(function() dm:Call("NewMenu", byte) end)
                    end)
                end
            end,
        })
        Log("  + page [" .. page_id .. "] " .. page_name .. " → byte " .. byte)
        return pages[byte]
    end

    --- Add an item to a static sub-page returned by AddPage().
    ---@param page      table     Page from AddPage()
    ---@param mod_name  string    Mod identifier
    ---@param name      string    Display label
    ---@param item_type string    "toggle" | "action" | "selector"
    ---@param opts      table     {getter, setter, callback, options, default_index}
    ---@return table|nil item handle
    function api.AddItemToPage(page, mod_name, name, item_type, opts)
        if not page then Warn("AddItemToPage: nil page"); return nil end
        opts = opts or {}
        local item = {
            mod       = mod_name,
            name      = name,
            type      = item_type,
            getter    = opts.getter,
            setter    = opts.setter,
            callback  = opts.callback,
            options   = opts.options,
            sel_index = opts.default_index or 0,
        }
        table.insert(page.items, item)
        return item
    end

    --- Read-only snapshot of all registered pages.
    function api.GetPages()     return pages end

    --- Check whether a given byte is one of our custom pages.
    function api.IsCustomPage(b) return pages[b] ~= nil end

    SharedAPI.DebugMenu = api
    Log("SharedAPI.DebugMenu ready  (v" .. VERSION .. ")")
end

-- ═══════════════════════════════════════════════════════════════════════
-- BRIDGE COMMANDS  (ADB console → modloader bridge)
-- ═══════════════════════════════════════════════════════════════════════

local function setup_bridge()
    if not RegisterBridgeCommand then return end

    -- ── Status ──────────────────────────────────────────────────────
    RegisterBridgeCommand("debugmenu_status", function()
        local dm = get_dm()
        local page_count, item_count = 0, 0
        for _, p in pairs(pages) do
            page_count = page_count + 1
            item_count = item_count + #p.items
        end
        local r = {
            version   = VERSION,
            pages     = page_count,
            items     = item_count,
            dm_valid  = dm ~= nil,
            root_byte = MODS_ROOT_BYTE,
            next_byte = next_page_byte,
        }
        if dm then
            pcall(function() r.active_menu   = dm:Get("ActiveMenu") end)
            pcall(function() r.current_index = dm:Get("CurrentIndex") end)
        end
        return r
    end)

    -- ── Page dump ───────────────────────────────────────────────────
    RegisterBridgeCommand("debugmenu_pages", function()
        local r = {}
        for byte, page in pairs(pages) do
            local ii = {}
            for i, item in ipairs(page.items) do
                local entry = { name = item.name, type = item.type }
                if item.type == "toggle" and type(item.getter) == "function" then
                    local ok, v = pcall(item.getter)
                    entry.state = ok and v or nil
                end
                if item.type == "selector" then
                    entry.sel_index = item.sel_index
                    entry.options   = item.options
                end
                ii[i] = entry
            end
            r[tostring(byte)] = {
                name       = page.name,
                byte       = byte,
                item_count = #page.items,
                items      = ii,
                dynamic    = page.populate_fn ~= nil,
            }
        end
        return r
    end)

    -- ── Refresh ─────────────────────────────────────────────────────
    RegisterBridgeCommand("debugmenu_refresh", function()
        if SharedAPI and SharedAPI.DebugMenu then
            SharedAPI.DebugMenu.Refresh()
            return { ok = true }
        end
        return { ok = false, err = "SharedAPI.DebugMenu not available" }
    end)

    -- ── Direct open ─────────────────────────────────────────────────
    -- Opens the Mods page directly (useful for testing without
    -- navigating through the stock menu).
    RegisterBridgeCommand("debugmenu_open", function()
        local dm = get_dm()
        if not dm then return { ok = false, err = "DebugMenu_C not found" } end
        pcall(function() dm:Call("NewMenu", MODS_ROOT_BYTE) end)
        return { ok = true, page = MODS_ROOT_BYTE }
    end)

    -- ── Toggle a specific mod by id ─────────────────────────────────
    RegisterBridgeCommand("debugmenu_toggle", function(args)
        local mod_id = args and args.mod
        if not mod_id then return { ok = false, err = "missing 'mod' arg" } end
        local root = pages[MODS_ROOT_BYTE]
        for _, item in ipairs(root.items) do
            if item.mod == mod_id and item.type == "toggle" then
                local cur = false
                if type(item.getter) == "function" then
                    local ok, v = pcall(item.getter)
                    cur = ok and (v == true) or false
                end
                if item.setter then pcall(item.setter, not cur) end
                return { ok = true, mod = mod_id, state = not cur }
            end
        end
        return { ok = false, err = "toggle '" .. mod_id .. "' not found" }
    end)

    Log("Bridge: debugmenu_status, debugmenu_pages, debugmenu_refresh, "
        .. "debugmenu_open, debugmenu_toggle")
end

-- ═══════════════════════════════════════════════════════════════════════
-- INIT
-- ═══════════════════════════════════════════════════════════════════════

local function init()
    if initialised then return end
    initialised = true

    Log("v" .. VERSION .. " — Deferred injection + BndEvt hook architecture")
    V("VERBOSE logging enabled")
    Log("  BndEvt hooks on VR4PlayerController_BP_C (ProcessEvent path)")
    Log("  'Mods' injection: deferred 200/500/1000/2000ms + lazy on scroll")

    setup_shared_api()
    setup_hooks()
    setup_bridge()

    -- Schedule a startup injection attempt (in case menu is already open)
    schedule_mods_injection()

    Log("Ready — mods register via SharedAPI.DebugMenu")
end

init()
