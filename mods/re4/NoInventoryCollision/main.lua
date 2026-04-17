-- mods/NoInventoryCollision/main.lua v3.1
-- ═══════════════════════════════════════════════════════════════════════
-- UE4SS-style Unlimited Inventory — all placement checks return true.
-- Items overlay on each other (no grid collision).
--
-- v3.0 — Full UE4SS API:
--   RegisterPostHook on AttacheCase/VR4ItemReceptacle/Slot UFunctions
--   FindFirstOf("AttacheCase") for inventory introspection
--   FindAllOf("VR4ItemPickup") for item listing
--   NotifyOnNewObject("VR4ItemPickup") for item spawn tracking
--   Native hooks for collision bypass
-- ═══════════════════════════════════════════════════════════════════════
local TAG = "NoInventoryCollision"
local VERBOSE = false
local function V(...) if VERBOSE then Log(TAG .. " [V] " .. string.format(...)) end end

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

local state = {
    enabled = true,
}

local saved = ModConfig.Load("NoInventoryCollision")
if saved then
    if saved.enabled ~= nil then state.enabled = saved.enabled end
end

-- ═══════════════════════════════════════════════════════════════════════
-- UE4SS POST-HOOKS — Force all CanPlaceItem → true
-- ═══════════════════════════════════════════════════════════════════════

-- AttacheCase::CanPlaceItem → force true
RegisterPostHook("/Script/Game.AttacheCase:CanPlaceItem", function(self, func, parms)
    if not state.enabled then return end
    V("PostHook AttacheCase:CanPlaceItem -> true")
    local p = CastParms(parms, "AttacheCase:CanPlaceItem")
    if p then p:SetReturnValue(true) end
end)
Log(TAG .. ": RegisterPostHook — AttacheCase:CanPlaceItem → true")

-- VR4ItemReceptacle::CanPlaceItem → force true
RegisterPostHook("/Script/Game.VR4ItemReceptacle:CanPlaceItem", function(self, func, parms)
    if not state.enabled then return end
    V("PostHook VR4ItemReceptacle:CanPlaceItem -> true")
    local p = CastParms(parms, "VR4ItemReceptacle:CanPlaceItem")
    if p then p:SetReturnValue(true) end
end)
Log(TAG .. ": RegisterPostHook — VR4ItemReceptacle:CanPlaceItem → true")

-- VR4ItemReceptacle::CanPlaceItemId → force true
RegisterPostHook("/Script/Game.VR4ItemReceptacle:CanPlaceItemId", function(self, func, parms)
    if not state.enabled then return end
    V("PostHook VR4ItemReceptacle:CanPlaceItemId -> true")
    local p = CastParms(parms, "VR4ItemReceptacle:CanPlaceItemId")
    if p then p:SetReturnValue(true) end
end)
Log(TAG .. ": RegisterPostHook — VR4ItemReceptacle:CanPlaceItemId → true")

-- VR4ItemReceptacleSlot::CanPlaceItem → force true
RegisterPostHook("/Script/Game.VR4ItemReceptacleSlot:CanPlaceItem", function(self, func, parms)
    if not state.enabled then return end
    V("PostHook VR4ItemReceptacleSlot:CanPlaceItem -> true")
    local p = CastParms(parms, "VR4ItemReceptacleSlot:CanPlaceItem")
    if p then p:SetReturnValue(true) end
end)
Log(TAG .. ": RegisterPostHook — VR4ItemReceptacleSlot:CanPlaceItem → true")

-- VR4ItemPickup::OnInventoryFull — monitor
RegisterHook("/Script/Game.VR4ItemPickup:OnInventoryFull", function(Context, Parms)
    if state.enabled then
        Log(TAG .. ": OnInventoryFull fired (bypassed by CanPlaceItem)")
    end
end)
Log(TAG .. ": RegisterHook — VR4ItemPickup:OnInventoryFull (monitor)")

-- VR4GamePlayerPawn::EquipProp — monitor equip events
RegisterHook("/Script/Game.VR4GamePlayerPawn:EquipProp", function(Context, Parms)
    if not state.enabled then return end
    local self = Context:get()
    if self and self:IsValid() then
        pcall(function()
            local slot = self:GetCurrentEquippedPropSlot()
            if slot then Log(TAG .. ": EquipProp slot=" .. tostring(slot)) end
        end)
    end
end)
Log(TAG .. ": RegisterHook — VR4GamePlayerPawn:EquipProp (monitor)")

-- ═══════════════════════════════════════════════════════════════════════
-- UE4SS ITEM SPAWN TRACKING
-- ═══════════════════════════════════════════════════════════════════════

local itemsTracked = 0
NotifyOnNewObject("VR4ItemPickup", function(obj)
    if obj and obj:IsValid() then
        itemsTracked = itemsTracked + 1
        pcall(function()
            local id = obj:GetItemID()
            if id then Log(TAG .. ": Item spawned — ID " .. tostring(id) .. " (total tracked: " .. itemsTracked .. ")") end
        end)
    end
end)
Log(TAG .. ": NotifyOnNewObject — VR4ItemPickup (item spawn tracking)")

-- ═══════════════════════════════════════════════════════════════════════
-- NATIVE HOOKS — Item collision bypass
-- ═══════════════════════════════════════════════════════════════════════

local sym_intersect = Resolve("ItemRegionsIntersect", 0x06735678)
if sym_intersect then
    pcall(function()
    RegisterNativeHookAt(sym_intersect, "ItemRegionsIntersect", nil,
        function(retval)
            if not state.enabled then return retval end
            V("Native ItemRegionsIntersect -> 0 (allow overlay)")
            return 0
        end)
    Log(TAG .. ": Native hook — ItemRegionsIntersect → false (allow overlay)")
    end)
end

-- NOTE: ItemRegionsIntersectOrAdjacent does NOT exist in the RE4 binary
-- (confirmed via bindump analysis — 0x064AEFC4 was actually execMakeItemRegion).
-- ItemRegionsIntersect alone handles all inventory collision bypass.

-- ═══════════════════════════════════════════════════════════════════════
-- COMMANDS — UE4SS inventory introspection
-- ═══════════════════════════════════════════════════════════════════════

RegisterCommand("NoInventoryCollision", function()
    state.enabled = not state.enabled
    V("toggle: enabled=%s", tostring(state.enabled))
    ModConfig.Save("NoInventoryCollision", state)
    Log(TAG .. ": " .. (state.enabled and "ENABLED" or "DISABLED"))
    Notify(TAG, state.enabled and "ON" or "OFF")
end)

RegisterCommand("inventory_info", function()
    local info = TAG .. ": enabled=" .. tostring(state.enabled)
    local case = findFirstNonDefault("AttacheCase")
    V("FindFirstOf AttacheCase -> %s", tostring(case))
    if case and case:IsValid() then
        pcall(function()
            local w = case.Width
            local h = case.Height
            if w and h then info = info .. " | Grid: " .. tostring(w) .. "x" .. tostring(h) end
        end)
        pcall(function()
            local items = case:GetItems()
            if items then info = info .. " | Items: " .. tostring(#items) end
        end)
    else
        info = info .. " | AttacheCase: not found"
    end
    -- Count all pickup items in the world
    local pickups = FindAllOf("VR4ItemPickup")
    if pickups then info = info .. " | World items: " .. #pickups end
    Log(info)
end)

if SharedAPI and SharedAPI.DebugMenu then
    SharedAPI.DebugMenu.RegisterToggle("NoInventoryCollision", "Unlimited Inventory",
        function() return state.enabled end,
        function(v) state.enabled = v; ModConfig.Save("NoInventoryCollision", state) end)
end

Log(TAG .. ": v3.0 loaded — UE4SS PostHook CanPlaceItem + NotifyOnNewObject item tracking"
    .. " + FindFirstOf inventory + native collision bypass")
