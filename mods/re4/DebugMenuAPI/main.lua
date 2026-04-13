--[[
    DebugMenuAPI v21 — Clean Cooperative Design
    ============================================
    A mod-menu system that cooperates with the stock DebugMenu_C.
    NEVER blocks or overrides Blueprint behaviour.

    The stock debug menu works 100 % unchanged for every built-in page.
    This mod only takes over for custom byte-IDs (100 +).

    ─────────────────────────────────────────────────────────────────────
    HOW IT WORKS
    ─────────────────────────────────────────────────────────────────────
    1.  A PAK-injected DataTable row adds "Mods" (OptionType = Action)
        to the stock Main page.
    2.  PostHook DoAction  — detects when the player confirms "Mods"
        on the Main page and navigates to byte 100.
    3.  PostHook NewMenu   — after Blueprint clears & auto-adds "Back",
        Lua APPENDS our custom option widgets.
    4.  On custom pages Lua reads the selected widget name, resolves
        the item, and invokes its getter / setter / callback.

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
local VERSION  = "21.0"

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

-- ═══════════════════════════════════════════════════════════════════════
-- DEBUGMENU_C ACCESS
-- ═══════════════════════════════════════════════════════════════════════

--- Return the live DebugMenu_C singleton (cached).
local function get_dm()
    if dm_cache and dm_cache:IsValid() then return dm_cache end
    local ok, r = pcall(FindFirstOf, "DebugMenu_C")
    if ok and r and r:IsValid() then
        dm_cache = r
        return r
    end
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
    -- Dynamic pages: re-create item list via populate_fn each time
    if page.populate_fn then
        page.items = {}
        _build_page = page
        pcall(page.populate_fn)
        _build_page = nil
    end

    -- Create one widget per item
    if #page.items == 0 then
        pcall(function() dm:Call("CreateActiveOption", "(No mods registered)") end)
    else
        for _, item in ipairs(page.items) do
            pcall(function() dm:Call("CreateActiveOption", make_display(item)) end)
        end
    end

    -- Cosmetics: title + VBoxList settings
    pcall(function()
        local pw = dm:Get("ParentWidget")
        if pw then
            local tt = pw:Get("TitleText")
            if tt then tt:Set("Text", page.name or "Mods") end
        end
    end)
    pcall(function()
        local pw = dm:Get("ParentWidget")
        if pw then
            local vbl = pw:Get("DebugVBoxList")
            if vbl then
                vbl:Set("MaxVisible", 50)
                vbl:Set("FirstVisible", 0)
                vbl:Call("UpdateListView")
            end
        end
    end)
end

--- Soft-refresh: clear and re-render the current custom page without
--- navigating away.  Preserves the selection cursor.
local function refresh_page(dm, page_byte, cursor)
    if _refreshing then return end
    _refreshing = true

    local page = pages[page_byte]
    if not page then _refreshing = false; return end

    -- Wipe widgets (including the auto-added "Back")
    pcall(function() dm:Call("ClearWidgets") end)

    -- Re-add "Back" at index 0 (Blueprint recognises it by name)
    pcall(function() dm:Call("CreateActiveOption", "Back") end)

    -- Rebuild + render items
    build_page(dm, page)

    -- Restore cursor
    pcall(function() dm:Set("CurrentIndex", cursor or 0) end)
    pcall(function() dm:Call("UpdateOptionHighlight") end)

    _refreshing = false
end

-- ═══════════════════════════════════════════════════════════════════════
-- SELECTED WIDGET NAME
-- ═══════════════════════════════════════════════════════════════════════

--- Read the OptionName string of the widget at CurrentIndex.
local function get_selected_option_name(dm)
    local ci = get_current_index(dm)
    if not ci then return nil end

    local ok_w, widgets = pcall(function() return dm:Get("ActiveOptionsWidgets") end)
    if not ok_w or not widgets then return nil end

    -- TArray is 1-indexed, CurrentIndex is 0-based → widget = arr[ci + 1]
    local ok_e, widget = pcall(function() return widgets[ci + 1] end)
    if not ok_e or not widget or not widget:IsValid() then return nil end

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
        refresh_page(dm, page_byte, ci)

    -- ── Selector ────────────────────────────────────────────────
    elseif item.type == "selector" and item.options and #item.options > 0 then
        item.sel_index = ((item.sel_index or 0) + 1) % #item.options
        if item.callback then
            local val = item.options[item.sel_index + 1]
            pcall(item.callback, val, item.sel_index, item)
        end
        refresh_page(dm, page_byte, ci)

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

    -- ┌─────────────────────────────────────────────────────────────────┐
    -- │ PostHook : NewMenu                                             │
    -- │ Blueprint already: ClearWidgets → push PreviousMenus →         │
    -- │ set ActiveMenu → populate from DataTable → auto-add "Back"     │
    -- │ We APPEND our items after Blueprint is done.                    │
    -- └─────────────────────────────────────────────────────────────────┘
    RegisterPostHook(DM_PATH .. ":NewMenu", function(self, func, parms)
        pcall(function()
            local dm = self:get()
            if not dm then return end

            local am = get_active_menu(dm)
            if not am or not pages[am] then return end

            local page = pages[am]
            build_page(dm, page)

            Log("Page " .. am .. " (" .. (page.name or "?") .. ") — "
                .. #page.items .. " items"
                .. (page.populate_fn and " [dynamic]" or ""))
        end)
    end)

    -- ┌─────────────────────────────────────────────────────────────────┐
    -- │ PostHook : DoAction                                            │
    -- │ Fires ONLY for OptionType = Action confirmations.              │
    -- │ Two jobs:                                                      │
    -- │   1. Main page (AM = 1): detect "Mods" → NewMenu(100)         │
    -- │   2. Custom page (AM ≥ 100): dispatch item action              │
    -- └─────────────────────────────────────────────────────────────────┘
    RegisterPostHook(DM_PATH .. ":DoAction", function(self, func, parms)
        pcall(function()
            local dm = self:get()
            if not dm then return end

            local am = get_active_menu(dm)
            if not am then return end

            -- Job 1: Main page → navigate to Mods root
            if am == 1 then
                local opt = get_selected_option_name(dm)
                if opt == "Mods" then
                    Log("'Mods' selected → page " .. MODS_ROOT_BYTE)
                    ExecuteAsync(function()
                        pcall(function() dm:Call("NewMenu", MODS_ROOT_BYTE) end)
                    end)
                end
                return
            end

            -- Job 2: Custom page → item action
            if pages[am] then
                dispatch_item(dm, am)
            end
        end)
    end)

    Log("Hooks installed (PostHook NewMenu + PostHook DoAction)")
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

    --- Force-refresh the current custom page (re-renders labels).
    --- For dynamic pages this re-invokes populate().
    function api.Refresh()
        local dm = get_dm()
        if not dm then return end
        local am = get_active_menu(dm)
        if not am or not pages[am] then return end
        refresh_page(dm, am, get_current_index(dm) or 0)
    end

    --- Create a named static sub-page and add a navigation link on
    --- the root Mods page.  Returns the page table.
    ---@param page_id   string  Unique page identifier
    ---@param page_name string  Display title
    ---@return table page handle (pass to AddItemToPage)
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
    ---@return table item handle
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

    Log("v" .. VERSION .. " — Clean cooperative design")
    Log("  PostHook only — Blueprint handles all native input")
    Log("  Stock menu is 100%% unmodified for built-in pages")

    setup_shared_api()
    setup_hooks()
    setup_bridge()

    Log("Ready — mods register via SharedAPI.DebugMenu")
end

init()
