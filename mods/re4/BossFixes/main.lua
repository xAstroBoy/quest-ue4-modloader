-- mods/BossFixes/main.lua v2.2
-- ═══════════════════════════════════════════════════════════════════════
-- UE4SS-enhanced Boss Death Fix — ensures spawned bosses die properly
-- when HP=0 instead of standing frozen.
--
-- v2.1 — IsNull() for null pointer checks (lightuserdata != 0 in Lua)
--   C++ safe-call guard protects native hooks from dangling pointers
--   FindFirstOf("VR4Bio4PlayerPawn") for player context
--   ForEachUObject for boss instance discovery
--   NotifyOnNewObject for boss spawn detection
--   Native hooks on cEmWrap::setHp for death trigger
-- ═══════════════════════════════════════════════════════════════════════
local TAG = "BossFixes"

-- ── Boss type IDs ───────────────────────────────────────────────────────
local BOSS_IDS = {
    [9]  = "Tyrant",       [43] = "El Gigante",
    [44] = "Novistador Boss", [47] = "Salamander",
    [49] = "Saddler After", [50] = "U3 (It)",
    [51] = "Novistador Boss Event", [53] = "Mendez",
    [55] = "Verdugo (No2)", [56] = "Verdugo After",
    [57] = "Krauser",       [63] = "Saddler (Ada)",
}

-- ── cEm struct offsets ──────────────────────────────────────────────────
local OFF_WRAP_CEM   = 0x00
local OFF_WRAP_VALID = 0x0B
local OFF_MAIN_STATE = 0x114
local OFF_EM_TYPE    = 0x119
local OFF_HP         = 0x3F0

-- ── Function addresses ─────────────────────────────────────────────────
local sym_EmSetDie    = Resolve("EmSetDie",    0x062EA2AC)
local sym_EmSetDieCnt = Resolve("EmSetDieCnt", 0x062EA310)

-- ── Statistics ──────────────────────────────────────────────────────────
local bossKills = {}

-- ═══════════════════════════════════════════════════════════════════════
-- NATIVE HOOK — cEmWrap::setHp → detect boss death
-- ═══════════════════════════════════════════════════════════════════════

pcall(function()
RegisterNativeHook("cEmWrap_setHp",
    function(self_ptr, new_hp)
        if new_hp > 0 then return self_ptr, new_hp end

        local valid = ReadU8(Offset(self_ptr, OFF_WRAP_VALID))
        if valid ~= 1 then return self_ptr, new_hp end

        local cem = ReadPointer(Offset(self_ptr, OFF_WRAP_CEM))
        if IsNull(cem) then return self_ptr, new_hp end

        local main_state = ReadU8(Offset(cem, OFF_MAIN_STATE))
        if main_state == 3 then return self_ptr, new_hp end -- already dying

        local em_type = ReadU8(Offset(cem, OFF_EM_TYPE))
        local boss_name = BOSS_IDS[em_type]
        if not boss_name then return self_ptr, new_hp end

        -- Track kills
        bossKills[boss_name] = (bossKills[boss_name] or 0) + 1

        Log(TAG .. ": " .. boss_name .. " (type " .. em_type .. ") HP→0 — forcing death"
            .. " (kill #" .. bossKills[boss_name] .. ")")

        if sym_EmSetDie then
            pcall(function() CallNative(sym_EmSetDie, "vpi", cem, 0) end)
        end
        if sym_EmSetDieCnt then
            pcall(function() CallNative(sym_EmSetDieCnt, "vpi", cem, 0) end)
        end

        return self_ptr, new_hp
    end, nil)
Log(TAG .. ": Native hook — cEmWrap_setHp (boss death trigger)")
end)

-- ── Diagnostic hook ─────────────────────────────────────────────────────
pcall(function()
    RegisterNativeHook("EmListSetAlive",
        function(idx, alive)
            if alive == 0 then
                Log(TAG .. ": EmListSetAlive idx=" .. idx .. " → DEAD")
            end
            return idx, alive
        end, nil)
end)

-- ═══════════════════════════════════════════════════════════════════════
-- COMMANDS — UE4SS enhanced status
-- ═══════════════════════════════════════════════════════════════════════

RegisterCommand("bossfixes_status", function()
    local info = TAG .. ": Boss kills:"
    local total = 0
    for name, count in pairs(bossKills) do
        info = info .. " " .. name .. "=" .. count
        total = total + count
    end
    if total == 0 then info = info .. " none" end
    info = info .. " (total=" .. total .. ")"

    -- Check player health via UE4SS
    local pawn = FindFirstOf("VR4Bio4PlayerPawn")
    if pawn and pawn:IsValid() then
        pcall(function()
            local hp = pawn:GetPlayerCurrentHealth()
            if hp then info = info .. " | Leon HP=" .. tostring(hp) end
        end)
    end
    Log(info)
end)

Log(TAG .. ": v2.1 loaded — boss death fix for " .. 12 .. " types + UE4SS status")
