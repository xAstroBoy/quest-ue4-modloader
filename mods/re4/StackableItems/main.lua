-- mods/StackableItems/main.lua v3.3
-- ═══════════════════════════════════════════════════════════════════════
-- UE4SS-enhanced Stackable Items — removes stack limits for all
-- non-weapon items (ammo, health, grenades, eggs, etc.)
--
-- v3.3 — Uses modloader IsNull() for null pointer detection.
--   C++ modloader safe-call guard protects against crashes in
--   original itemInfo when outInfo is NULL or dangling.
--   RegisterNativeHook on itemInfo for capacity override
--   FindAllOf("VR4ItemPickup") for item discovery
--   ModConfig for persistence
--
-- ITEM_INFO struct at return:
--   +0x0  uint16_t  unknown
--   +0x2  uint8_t   type (1=weapon, 2=ammo, 3=healing, 5=egg, 8=grenade, 9=treasure)
--   +0x3  uint8_t   defaultBatchCount
--   +0x4  uint16_t  maxCapacity ← WE OVERRIDE THIS
-- ═══════════════════════════════════════════════════════════════════════
local TAG = "StackableItems"

local state = {
    enabled = true,
    maxStack = 9999,
    overrideCount = 0,
    uniqueItems = {},
}

local saved = ModConfig.Load("StackableItems")
if saved then
    if saved.enabled ~= nil then state.enabled = saved.enabled end
    if saved.maxStack then state.maxStack = saved.maxStack end
end

-- ═══════════════════════════════════════════════════════════════════════
-- NATIVE HOOK — itemInfo post-hook: override maxCapacity
-- NOTE: C++ safe-call guard protects original from null/dangling crashes
-- ═══════════════════════════════════════════════════════════════════════
local sym_itemInfo = Resolve("itemInfo", 0x0635F714)

-- Dobby hook for capacity override (post-hook reads filled ITEM_INFO)
pcall(function()
    RegisterNativeHook("itemInfo",
        function(itemId, outInfoPtr)
            -- BLOCK if outInfo is null — prevent write to NULL
            if IsNull(outInfoPtr) then
                return "BLOCK"
            end
            return itemId, outInfoPtr
        end,
        function(retval, itemId, outInfoPtr)
            if not state.enabled then return retval end
            if IsNull(outInfoPtr) then return retval end

            pcall(function()
                local itemType = ReadU8(Offset(outInfoPtr, 2))
                local maxCap = ReadU16(Offset(outInfoPtr, 4))

                -- Skip weapons (type 1) and invalid types (type 0)
                if itemType == 0 or itemType == 1 then return end

                -- Skip items with max <= 1 (key items, single-count)
                if maxCap <= 1 then return end

                -- Skip if already at our target
                if maxCap >= state.maxStack then return end

                -- Override max capacity
                WriteU16(Offset(outInfoPtr, 4), state.maxStack)

                -- Track unique items overridden
                local id = itemId & 0xFFFF
                if not state.uniqueItems[id] then
                    state.uniqueItems[id] = true
                    state.overrideCount = state.overrideCount + 1
                    Log(TAG .. ": Unlocked stack: itemId=0x"
                        .. string.format("%X", id)
                        .. " type=" .. itemType
                        .. " oldMax=" .. maxCap
                        .. " → " .. state.maxStack)
                end
            end)

            return retval
        end)
    Log(TAG .. ": RegisterNativeHook — itemInfo (Dobby, capacity override)")
end)

-- ═══════════════════════════════════════════════════════════════════════
-- COMMANDS
-- ═══════════════════════════════════════════════════════════════════════

RegisterCommand("stackable", function()
    state.enabled = not state.enabled
    ModConfig.Save("StackableItems", state)
    Log(TAG .. ": " .. (state.enabled and "ON" or "OFF"))
    Notify(TAG, state.enabled and "ON" or "OFF")
end)

RegisterCommand("stackable_max", function(args)
    local n = tonumber(args)
    if n and n >= 1 and n <= 99999 then
        state.maxStack = n
        ModConfig.Save("StackableItems", state)
        Log(TAG .. ": Max stack → " .. n)
        Notify(TAG, "Max: " .. n)
    else
        Log(TAG .. ": Usage: stackable_max <1-99999> (current: " .. state.maxStack .. ")")
    end
end)

RegisterCommand("stackable_status", function()
    local info = TAG .. ": enabled=" .. tostring(state.enabled)
        .. " maxStack=" .. state.maxStack
        .. " uniqueItems=" .. state.overrideCount

    -- Check inventory via UE4SS
    local attachCase = FindFirstOf("AttacheCase")
    if attachCase and attachCase:IsValid() then
        pcall(function()
            local items = attachCase:GetItems()
            if items then info = info .. " | caseItems=" .. tostring(#items) end
        end)
    end

    Log(info)
end)

if SharedAPI and SharedAPI.DebugMenu then
    SharedAPI.DebugMenu.RegisterToggle("StackableItems", "Stackable Items",
        function() return state.enabled end,
        function(v) state.enabled = v; ModConfig.Save("StackableItems", state) end)
end

Log(TAG .. ": v3.3 loaded — null-safe ToHex | maxStack=" .. state.maxStack
    .. " | Weapons (type 1) + key items (max<=1) untouched"
    .. " | UE4SS RegisterNativeHook + FindFirstOf")
