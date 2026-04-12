--[[
    DebugMenuAPI v20 — Cooperative Enumless Design
    ===============================================
    Lua NEVER blocks or overrides Blueprint input handling.
    Lua APPENDS pages, CREATES widgets, and BINDS callbacks.
    Blueprint handles ALL input: scroll, confirm, back, highlighting.

    ARCHITECTURE:
      PAK injects "Mods" Setting (OptionType=Action) on Main page.
      NO enum changes. DebugMenuType enum is UNTOUCHED.
      Blueprint dispatches via raw byte (EqualEqual_ByteByte).

    HOOKS (all POST — never BLOCK):
      1. PostHook NewMenu  → populate custom pages with widgets
      2. PostHook DoAction  → handle "Mods" nav + custom item actions

    FLOW:
      Main page → Confirm "Mods" → DoAction no-ops → PostHook → NewMenu(100)
      Page 100  → Blueprint clears, auto-adds "Back" → PostHook → Lua adds items
      Confirm item → DoAction no-ops → PostHook → Lua invokes callback
      Back button → Blueprint PreviousMenu() pops stack → navigates back natively

    NATIVE (handled entirely by Blueprint, NOT by Lua):
      ✓ Scrolling (InputActionScrollUp/Down)
      ✓ Highlighting (UpdateOptionHighlight)
      ✓ Back navigation (PreviousMenus TArray stack)
      ✓ "Back" widget (auto-added by NewMenu for pages != 0,1,28)
      ✓ VBoxList scrolling and visibility

    SIMPLE API (static items on root Mods page):
      SharedAPI.DebugMenu.RegisterToggle(mod, name, default, callback)
      SharedAPI.DebugMenu.RegisterAction(mod, name, callback)
      SharedAPI.DebugMenu.RegisterSelector(mod, name, options, callback)

    ADVANCED API (dynamic sub-pages):
      SharedAPI.DebugMenu.RegisterSubMenu(mod, name, callback)
      SharedAPI.DebugMenu.NavigateTo({populate=fn, name=title})
      SharedAPI.DebugMenu.AddItem(name, callback)
      SharedAPI.DebugMenu.Refresh()
]]

local MOD_NAME = "DebugMenuAPI"
local VERSION  = "20.0"

-- ═══════════════════════════════════════════════════════════════════════
-- Constants & State
-- ═══════════════════════════════════════════════════════════════════════

local MODS_ROOT_BYTE = 100       -- Root mods page byte
local next_page_byte = 101       -- Next assignable byte for sub-pages
local custom_pages   = {}        -- byte → {name, items, populate_fn}
local dm_ref         = nil       -- Cached DebugMenu_C
local initialized    = false

-- Build context (set during populate_fn execution)
local _build_page    = nil       -- Active page being built by populate_fn
local _refreshing    = false     -- Guard against recursive refresh
local _submenu_name  = nil       -- Pending page name from RegisterSubMenu

-- Root page (always exists, static items)
custom_pages[MODS_ROOT_BYTE] = {
    name        = "Mods",
    items       = {},
    populate_fn = nil,     -- nil = static page (items persist)
}

-- ═══════════════════════════════════════════════════════════════════════
-- Logging
-- ═══════════════════════════════════════════════════════════════════════

local function Log(msg)
    print("[" .. MOD_NAME .. "] " .. msg)
end

local function Warn(msg)
    print("[" .. MOD_NAME .. "] WARN: " .. msg)
end

-- ═══════════════════════════════════════════════════════════════════════
-- DebugMenu_C Access
-- ═══════════════════════════════════════════════════════════════════════

local function get_dm()
    if dm_ref and dm_ref:IsValid() then return dm_ref end
    local ok, r = pcall(FindFirstOf, "DebugMenu_C")
    if ok and r and r:IsValid() then
        dm_ref = r
        return r
    end
    return nil
end

local function get_active_menu(dm)
    local ok, v = pcall(function() return dm:Get("ActiveMenu") end)
    return (ok and type(v) == "number") and v or nil
end

-- ═══════════════════════════════════════════════════════════════════════
-- Display Helpers
-- ═══════════════════════════════════════════════════════════════════════

local function make_display(item)
    local d = item.name or "?"
    if item.type == "toggle" then
        d = d .. (item.state and " [ON]" or " [OFF]")
    elseif item.type == "selector" and item.options and #item.options > 0 then
        local idx = (item.sel_index % #item.options) + 1
        d = d .. " [" .. tostring(item.options[idx]) .. "]"
    elseif item.type == "submenu" or item.type == "page_link" then
        d = d .. " >>"
    end
    return d
end

-- ═══════════════════════════════════════════════════════════════════════
-- Widget Population
-- ═══════════════════════════════════════════════════════════════════════
-- After NewMenu: Blueprint auto-added "Back" at index 0 for sub-pages.
-- We APPEND our items after Back. Index mapping:
--   0 = Back (auto-added by Blueprint for pages != 0,1,28)
--   1 = items[1]  (first registered item)
--   2 = items[2]
--   ...

local function build_and_render(dm, page)
    -- Dynamic pages: rebuild items via populate_fn each time
    if page.populate_fn then
        page.items = {}
        _build_page = page
        pcall(page.populate_fn)
        _build_page = nil
    end

    -- Render items as CreateActiveOption widgets
    if #page.items == 0 then
        pcall(function() dm:Call("CreateActiveOption", "(No mods registered)") end)
        return
    end

    for _, item in ipairs(page.items) do
        pcall(function() dm:Call("CreateActiveOption", make_display(item)) end)
    end
end

local function set_chrome(dm, page)
    -- Title
    pcall(function()
        local pw = dm:Get("ParentWidget")
        if pw then
            local tt = pw:Get("TitleText")
            if tt then tt:Set("Text", page.name or "Mods") end
        end
    end)
    -- VBoxList settings (raise MaxVisible for long mod lists)
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

-- ═══════════════════════════════════════════════════════════════════════
-- Soft Refresh — Rebuild + re-render without changing page
-- ═══════════════════════════════════════════════════════════════════════

local function refresh_page(dm, page_byte, saved_ci)
    if _refreshing then return end
    _refreshing = true

    local page = custom_pages[page_byte]
    if not page then _refreshing = false; return end

    -- Clear everything (including auto-added "Back")
    pcall(function() dm:Call("ClearWidgets") end)

    -- Re-add "Back" at index 0 (recognized by Blueprint Confirm handler)
    pcall(function() dm:Call("CreateActiveOption", "Back") end)

    -- Rebuild + render items
    build_and_render(dm, page)
    set_chrome(dm, page)

    -- Restore selection
    pcall(function() dm:Set("CurrentIndex", saved_ci or 0) end)
    pcall(function() dm:Call("UpdateOptionHighlight") end)

    _refreshing = false
end

-- ═══════════════════════════════════════════════════════════════════════
-- Read Selected Widget's OptionName
-- ═══════════════════════════════════════════════════════════════════════

local function get_selected_option_name(dm)
    local ok_ci, ci = pcall(function() return dm:Get("CurrentIndex") end)
    if not ok_ci or type(ci) ~= "number" then return nil end

    local ok_w, widgets = pcall(function() return dm:Get("ActiveOptionsWidgets") end)
    if not ok_w or not widgets then return nil end

    -- TArray is 1-indexed, CurrentIndex is 0-based
    local ok_e, widget = pcall(function() return widgets[ci + 1] end)
    if not ok_e or not widget or not widget:IsValid() then return nil end

    local ok_n, name = pcall(function() return widget:Get("OptionName") end)
    return ok_n and tostring(name or "") or nil
end

-- ═══════════════════════════════════════════════════════════════════════
-- Hooks — ALL PostHook, NEVER BLOCK
-- ═══════════════════════════════════════════════════════════════════════

local function setup_hooks()
    local DM = "/Game/Blueprints/Debug/DebugMenu/DebugMenu.DebugMenu_C"

    -- ──────────────────────────────────────────────────────────────────
    -- PostHook NewMenu
    --   Blueprint already ran: ClearWidgets → push PreviousMenus →
    --   set ActiveMenu → populate from DataTable → auto-add "Back"
    --   We APPEND our custom items after Back.
    -- ──────────────────────────────────────────────────────────────────
    RegisterPostHook(DM .. ":NewMenu", function(self, func, parms)
        pcall(function()
            local dm = self:get()
            if not dm then return end

            local am = get_active_menu(dm)
            if not am or not custom_pages[am] then return end

            local page = custom_pages[am]
            build_and_render(dm, page)
            set_chrome(dm, page)

            Log("Populated page " .. am .. " (" .. (page.name or "?")
                .. ") — " .. #page.items .. " items"
                .. (page.populate_fn and " [dynamic]" or " [static]"))
        end)
    end)

    -- ──────────────────────────────────────────────────────────────────
    -- PostHook DoAction
    --   Fires ONLY for OptionType=0 (Action) confirmations.
    --   Blueprint Link → PreviousMenu/NewMenu (not DoAction).
    --   Blueprint Bool/Enum → OptionIncremented (not DoAction).
    --   Blueprint "Back" check → inside Link handler (not DoAction).
    --
    --   Two jobs:
    --     1. Main page (am=1): detect "Mods" → navigate to root page
    --     2. Custom page:      detect item → invoke callback/toggle/etc
    -- ──────────────────────────────────────────────────────────────────
    RegisterPostHook(DM .. ":DoAction", function(self, func, parms)
        pcall(function()
            local dm = self:get()
            if not dm then return end

            local am = get_active_menu(dm)
            if not am then return end

            -- ── Job 1: Main page → "Mods" navigation ──
            if am == 1 then
                local opt = get_selected_option_name(dm)
                if opt == "Mods" then
                    Log("'Mods' selected on Main → page " .. MODS_ROOT_BYTE)
                    ExecuteAsync(function()
                        pcall(function() dm:Call("NewMenu", MODS_ROOT_BYTE) end)
                    end)
                end
                return
            end

            -- ── Job 2: Custom page → item actions ──
            if not custom_pages[am] then return end
            local page = custom_pages[am]

            local ok_ci, ci = pcall(function() return dm:Get("CurrentIndex") end)
            if not ok_ci or type(ci) ~= "number" then return end

            -- Index mapping: Back=0, items[1]=1, items[2]=2, ...
            -- ci=1 → items[1] (Lua 1-indexed). ci=0 is "Back" (Link, not DoAction).
            if ci < 1 or ci > #page.items then return end
            local item = page.items[ci]

            -- ── Static toggle ──
            if item.type == "toggle" then
                item.state = not item.state
                if item.callback then
                    pcall(item.callback, item.state, item)
                end
                refresh_page(dm, am, ci)

            -- ── Static selector ──
            elseif item.type == "selector" and item.options and #item.options > 0 then
                item.sel_index = (item.sel_index + 1) % #item.options
                if item.callback then
                    local val = item.options[item.sel_index + 1]
                    pcall(item.callback, val, item.sel_index, item)
                end
                refresh_page(dm, am, ci)

            -- ── Submenu link (callback calls NavigateTo) ──
            elseif item.type == "submenu" then
                if item.callback then
                    _submenu_name = item.name
                    pcall(item.callback)
                    _submenu_name = nil
                end

            -- ── Static page_link ──
            elseif item.type == "page_link" then
                if item.callback then
                    pcall(item.callback)
                end

            -- ── Action (simple or dynamic page item) ──
            elseif item.type == "action" then
                if item.callback then
                    pcall(item.callback, item)
                end
            end
        end)
    end)

    Log("Hooks: PostHook NewMenu + PostHook DoAction (never BLOCK)")
end

-- ═══════════════════════════════════════════════════════════════════════
-- SharedAPI — Public interface for other mods
-- ═══════════════════════════════════════════════════════════════════════

local function setup_shared_api()
    if not SharedAPI then SharedAPI = {} end

    local api = {}
    api.VERSION = VERSION

    -- ══════════════════════════════════════════════════════════════════
    -- Simple API — Static items on root Mods page
    -- ══════════════════════════════════════════════════════════════════

    --- Register a boolean toggle on the root Mods page.
    -- @param mod_name  string   Mod identifier (e.g. "GodMode")
    -- @param name      string   Display name (e.g. "God Mode")
    -- @param default   bool     Initial state (true/false)
    -- @param callback  function fn(newState, item) called on toggle
    -- @return item table
    function api.RegisterToggle(mod_name, name, default, callback)
        local item = {
            mod = mod_name, name = name, type = "toggle",
            state = default or false, callback = callback,
        }
        table.insert(custom_pages[MODS_ROOT_BYTE].items, item)
        Log("  + toggle [" .. mod_name .. "] " .. name)
        return item
    end

    --- Register an action button on the root Mods page.
    -- @param mod_name  string   Mod identifier
    -- @param name      string   Display name
    -- @param callback  function fn(item) called on confirm
    -- @return item table
    function api.RegisterAction(mod_name, name, callback)
        local item = {
            mod = mod_name, name = name, type = "action",
            callback = callback,
        }
        table.insert(custom_pages[MODS_ROOT_BYTE].items, item)
        Log("  + action [" .. mod_name .. "] " .. name)
        return item
    end

    --- Register a selector (cycles through options) on the root Mods page.
    -- @param mod_name  string   Mod identifier
    -- @param name      string   Display name
    -- @param options   table    {"Opt1", "Opt2", ...}
    -- @param callback  function fn(selectedValue, selectedIndex, item)
    -- @return item table
    function api.RegisterSelector(mod_name, name, options, callback)
        local item = {
            mod = mod_name, name = name, type = "selector",
            options = options, sel_index = 0, callback = callback,
        }
        table.insert(custom_pages[MODS_ROOT_BYTE].items, item)
        Log("  + selector [" .. mod_name .. "] " .. name)
        return item
    end

    -- ══════════════════════════════════════════════════════════════════
    -- Advanced API — Dynamic sub-pages
    -- ══════════════════════════════════════════════════════════════════

    --- Register a sub-menu link on the root Mods page.
    -- When selected, `callback` fires. Inside it, call api.NavigateTo().
    -- @param mod_name  string   Mod identifier
    -- @param name      string   Display title (also used as page title)
    -- @param callback  function Called when submenu is opened
    -- @return item table
    function api.RegisterSubMenu(mod_name, name, callback)
        local item = {
            mod = mod_name, name = name, type = "submenu",
            callback = callback,
        }
        table.insert(custom_pages[MODS_ROOT_BYTE].items, item)
        Log("  + submenu [" .. mod_name .. "] " .. name)
        return item
    end

    --- Navigate to a dynamic page with a populate callback.
    -- Called from inside RegisterSubMenu callback or from AddItem callbacks.
    -- populate() is re-called on every render and Refresh().
    -- Inside populate(), use api.AddItem() to build the page.
    -- @param opts  {populate=function, name=string}
    function api.NavigateTo(opts)
        opts = opts or {}
        local byte = next_page_byte
        next_page_byte = next_page_byte + 1

        if byte > 255 then
            Warn("Page byte overflow (>255)!")
            return
        end

        custom_pages[byte] = {
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

    --- Add an action item to the currently-building dynamic page.
    -- Only valid inside a populate() callback passed to NavigateTo().
    -- @param name      string        Display name
    -- @param callback  function|nil  Called on confirm (nil = label/separator)
    function api.AddItem(name, callback)
        if not _build_page then
            Warn("AddItem called outside populate callback")
            return
        end
        table.insert(_build_page.items, {
            name = name, type = "action", callback = callback,
        })
    end

    --- Force refresh the current custom page display.
    -- For dynamic pages, re-calls populate_fn to rebuild items.
    -- For static pages, re-renders with current item states.
    function api.Refresh()
        local dm = get_dm()
        if not dm then return end
        local am = get_active_menu(dm)
        if not am or not custom_pages[am] then return end
        local ok_ci, ci = pcall(function() return dm:Get("CurrentIndex") end)
        refresh_page(dm, am, (ok_ci and ci) or 0)
    end

    --- Create a static sub-page linked from root.
    -- @param page_id   string  Unique identifier
    -- @param page_name string  Display title
    -- @return page table (add items via api.AddItemToPage)
    function api.AddPage(page_id, page_name)
        local byte = next_page_byte
        next_page_byte = next_page_byte + 1

        custom_pages[byte] = {
            name = page_name, items = {}, populate_fn = nil,
        }

        -- Auto-add navigation link on root page
        table.insert(custom_pages[MODS_ROOT_BYTE].items, {
            name = page_name, type = "page_link",
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
        return custom_pages[byte]
    end

    --- Add an item to a static sub-page returned by AddPage().
    -- @param page      table      Page table from AddPage()
    -- @param mod_name  string     Mod identifier
    -- @param name      string     Display name
    -- @param item_type string     "toggle"|"action"|"selector"
    -- @param opts      table      {default, callback, options, default_index}
    -- @return item table
    function api.AddItemToPage(page, mod_name, name, item_type, opts)
        opts = opts or {}
        local item = {
            mod       = mod_name,
            name      = name,
            type      = item_type,
            state     = opts.default or false,
            options   = opts.options,
            sel_index = opts.default_index or 0,
            callback  = opts.callback,
        }
        table.insert(page.items, item)
        return item
    end

    --- Get all custom pages (read-only snapshot).
    function api.GetPages()
        return custom_pages
    end

    --- Check if a given ActiveMenu byte is a custom page.
    function api.IsCustomPage(byte_val)
        return custom_pages[byte_val] ~= nil
    end

    SharedAPI.DebugMenu = api
    Log("SharedAPI.DebugMenu exposed (simple + advanced API)")
end

-- ═══════════════════════════════════════════════════════════════════════
-- Bridge Commands
-- ═══════════════════════════════════════════════════════════════════════

local function setup_bridge()
    if not RegisterBridgeCommand then return end

    RegisterBridgeCommand("debugmenu_status", function()
        local dm = get_dm()
        local page_count, item_count = 0, 0
        for _, p in pairs(custom_pages) do
            page_count = page_count + 1
            item_count = item_count + #p.items
        end
        local s = {
            version    = VERSION,
            pages      = page_count,
            items      = item_count,
            dm_valid   = dm ~= nil,
            root_byte  = MODS_ROOT_BYTE,
            next_byte  = next_page_byte,
        }
        if dm then
            pcall(function()
                s.active_menu   = tostring(dm:Get("ActiveMenu"))
                s.current_index = dm:Get("CurrentIndex")
            end)
        end
        return s
    end)

    RegisterBridgeCommand("debugmenu_pages", function()
        local r = {}
        for byte, page in pairs(custom_pages) do
            local ii = {}
            for i, item in ipairs(page.items) do
                ii[i] = {
                    name  = item.name,
                    type  = item.type,
                    state = item.state,
                }
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

    RegisterBridgeCommand("debugmenu_refresh", function()
        if SharedAPI and SharedAPI.DebugMenu then
            SharedAPI.DebugMenu.Refresh()
            return { ok = true }
        end
        return { ok = false }
    end)

    Log("Bridge: debugmenu_status, debugmenu_pages, debugmenu_refresh")
end

-- ═══════════════════════════════════════════════════════════════════════
-- Init
-- ═══════════════════════════════════════════════════════════════════════

local function init()
    if initialized then return end
    initialized = true

    Log(string.format("v%s — Cooperative enumless design", VERSION))
    Log("  ALL PostHooks, NEVER BLOCK. Game handles all input.")
    Log("  Back/Scroll/Highlight = 100% Blueprint native.")

    setup_shared_api()
    setup_hooks()
    setup_bridge()

    Log("Ready — mods register via SharedAPI.DebugMenu")
end

init()
