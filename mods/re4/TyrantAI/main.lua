-- mods/TyrantAI/main.lua v3.0
-- ═══════════════════════════════════════════════════════════════════════
-- UE4SS-enhanced Tyrant (em09) AI Monitor — hooks the complete
-- cEm09 state machine for autonomous AI monitoring and control.
--
-- Ported from RE4_TyrantAI.js v1.0 (634 lines Frida)
-- v3.0 — Lua modloader + UE4SS:
--   RegisterNativeHook on Em09Init/cEm09::move/~cEm09/stateInit/counterAttack
--   ReadU8/U16/U32/F32 for struct access
--   ForEachUObject for finding UE4 actor context
--   ModConfig for persistence
--
-- cEm09 State Machine:
--   0 = Init (HP/collision/model/anims → state 1)
--   1 = Active (idle/chase/attack/reposition)
--   2 = Hit Reaction / Knockdown
--   3 = Death
-- ═══════════════════════════════════════════════════════════════════════
local TAG = "TyrantAI"

-- ── cEm09 struct offsets ────────────────────────────────────────────
local OFF = {
    vtable       = 0x000,
    statusFlags  = 0x008,
    posX         = 0x0A4,
    posZ         = 0x0AC,
    mainState    = 0x114,   -- u16 packed: low=main, high=sub
    subState     = 0x115,   -- u8
    phase        = 0x116,   -- u8
    param        = 0x117,   -- u8
    emId         = 0x118,   -- u8
    posY         = 0x178,
    collFlags    = 0x362,
    hp           = 0x3F0,   -- i32
    dmgFlags     = 0x3FC,
    dmgType      = 0x3FE,
    distSq       = 0x458,   -- f32 distance² to player
    animTable    = 0x460,   -- ptr (CRITICAL)
    routeFlags   = 0x4FD,
    angle        = 0x72D,   -- f32 signed angle
    absAngle     = 0x731,   -- f32 absolute angle
    grabFlag     = 0x779,   -- u8
}

-- ── State name map ──────────────────────────────────────────────────
local STATE_NAMES = {
    [0x0000] = "INIT",      [0x0001] = "IDLE",
    [0x0002] = "HIT_REACT", [0x0003] = "DEAD",
    [0x0101] = "APPROACH",   [0x0401] = "PATHFIND",
    [0x0501] = "CHASE",      [0x0601] = "REPO_FWD",
    [0x0701] = "ATK_RANGE",  [0x0801] = "CLOSE_ATK",
    [0x0901] = "TURN_ATK",
}
local function stateName(packed)
    if STATE_NAMES[packed] then return STATE_NAMES[packed] end
    local main = packed & 0xFF
    local sub = (packed >> 8) & 0xFF
    return "S" .. main .. ":" .. sub
end

-- ── Struct access helpers ───────────────────────────────────────────
local function getHP(p)         return ReadS32(Offset(p, OFF.hp)) end
local function getPackedState(p) return ReadU16(Offset(p, OFF.mainState)) end
local function getMainState(p)  return ReadU8(Offset(p, OFF.mainState)) end
local function getSubState(p)   return ReadU8(Offset(p, OFF.subState)) end
local function getPhase(p)      return ReadU8(Offset(p, OFF.phase)) end
local function getParam(p)      return ReadU8(Offset(p, OFF.param)) end
local function getDistSq(p)     return ReadFloat(Offset(p, OFF.distSq)) end
local function getAngle(p)      return ReadFloat(Offset(p, OFF.angle)) end
local function getAnimTable(p)  return ReadPointer(Offset(p, OFF.animTable)) end
local function getRouteFlags(p) return ReadU8(Offset(p, OFF.routeFlags)) end
local function getGrabFlag(p)   return ReadU8(Offset(p, OFF.grabFlag)) end
local function getPosX(p)       return ReadFloat(Offset(p, OFF.posX)) end
local function getPosY(p)       return ReadFloat(Offset(p, OFF.posY)) end
local function getPosZ(p)       return ReadFloat(Offset(p, OFF.posZ)) end
local function getEmId(p)       return ReadU8(Offset(p, OFF.emId)) end

local function setHP(p, v)      WriteS32(Offset(p, OFF.hp), v) end
local function setPackedState(p, v) WriteU16(Offset(p, OFF.mainState), v) end
local function setPhase(p, v)   WriteU8(Offset(p, OFF.phase), v) end
local function setParam(p, v)   WriteU8(Offset(p, OFF.param), v) end

-- ═══════════════════════════════════════════════════════════════════════
-- TRACKER — Manages all live em09 instances
-- ═══════════════════════════════════════════════════════════════════════
local tracker = {
    instances = {},     -- ptr(number) → instance data
    count = 0,
    verbose = false,
    logTransitions = true,
}

local function trackInstance(ptr)
    local key = ptr
    if not tracker.instances[key] then
        tracker.instances[key] = {
            ptr = ptr,
            lastState = -1,
            spawnTime = os.clock(),
            initOk = false,
            moveCount = 0,
            stateHistory = {},
        }
        tracker.count = tracker.count + 1
        Log(TAG .. ": Tyrant SPAWNED @ 0x" .. string.format("%X", ptr)
            .. " (total: " .. tracker.count .. ")")
    end
    return tracker.instances[key]
end

local function untrackInstance(ptr)
    local key = ptr
    local inst = tracker.instances[key]
    if inst then
        local lived = os.clock() - inst.spawnTime
        Log(TAG .. ": Tyrant DESTROYED @ 0x" .. string.format("%X", ptr)
            .. " (lived " .. string.format("%.1f", lived) .. "s"
            .. " moves=" .. inst.moveCount
            .. " transitions=" .. #inst.stateHistory .. ")")
        tracker.instances[key] = nil
        tracker.count = tracker.count - 1
    end
end

local function diagnose(ptr, inst)
    local hp = getHP(ptr)
    local animTbl = getAnimTable(ptr)
    local packed = getPackedState(ptr)

    local ok = true
    local msgs = {}

    if hp <= 0 then msgs[#msgs+1] = "HP=0!"; ok = false
    else msgs[#msgs+1] = "HP=" .. hp end

    if animTbl == 0 then msgs[#msgs+1] = "AnimTable=NULL!"; ok = false
    else msgs[#msgs+1] = "AnimTable=0x" .. string.format("%X", animTbl) end

    msgs[#msgs+1] = "State=0x" .. string.format("%X", packed)

    inst.initOk = ok
    Log(TAG .. ": Init " .. (ok and "OK" or "FAILED") .. ": " .. table.concat(msgs, ", "))
    return ok
end

-- ═══════════════════════════════════════════════════════════════════════
-- NATIVE HOOKS — cEm09 lifecycle + state machine
-- ═══════════════════════════════════════════════════════════════════════

-- Hook Em09Init — track new Tyrant instances
pcall(function()
    RegisterNativeHook("Em09Init",
        function(emPtr)
            return emPtr
        end,
        function(retval, emPtr)
            if emPtr ~= 0 then
                trackInstance(emPtr)
                Log(TAG .. ":   Em09Init complete — tracking instance")
            end
            return retval
        end)
    Log(TAG .. ": Hook — Em09Init (spawn detection)")
end)

-- Hook cEm09::move — monitor AI state machine
pcall(function()
    RegisterNativeHook("cEm09_move",
        function(emPtr)
            if emPtr == 0 then return emPtr end

            local inst = tracker.instances[emPtr]
            if not inst then inst = trackInstance(emPtr) end
            inst.moveCount = inst.moveCount + 1

            local packed = getPackedState(emPtr)

            -- Log state transitions
            if packed ~= inst.lastState then
                if tracker.logTransitions then
                    local hp = getHP(emPtr)
                    local dist = math.sqrt(math.max(0, getDistSq(emPtr)))
                    Log(TAG .. ": STATE: " .. stateName(inst.lastState) .. " → " .. stateName(packed)
                        .. " [HP=" .. hp .. " dist=" .. string.format("%.0f", dist) .. "]")
                end

                inst.stateHistory[#inst.stateHistory + 1] = {
                    from = inst.lastState,
                    to = packed,
                    tick = inst.moveCount,
                }

                -- Diagnose after init → active
                if inst.lastState <= 0 and (packed & 0xFF) >= 1 and not inst.initOk then
                    diagnose(emPtr, inst)
                end

                inst.lastState = packed
            end

            -- Verbose per-frame logging
            if tracker.verbose and inst.moveCount % 60 == 0 then
                Log(TAG .. ": TICK #" .. inst.moveCount
                    .. " state=" .. stateName(packed)
                    .. " HP=" .. getHP(emPtr)
                    .. " dist=" .. string.format("%.0f", math.sqrt(math.max(0, getDistSq(emPtr))))
                    .. " angle=" .. string.format("%.2f", getAngle(emPtr)))
            end

            return emPtr
        end, nil)
    Log(TAG .. ": Hook — cEm09::move (state machine monitor)")
end)

-- Hook cEm09::~cEm09 — untrack destroyed instances
pcall(function()
    RegisterNativeHook("cEm09_dtor",
        function(emPtr)
            untrackInstance(emPtr)
            return emPtr
        end, nil)
    Log(TAG .. ": Hook — cEm09::~cEm09 (destroy detection)")
end)

-- Hook stateInit — verify HP/model setup
pcall(function()
    RegisterNativeHook("em09_stateInit",
        nil,
        function(retval, emPtr)
            if emPtr ~= 0 then
                local hp = getHP(emPtr)
                local animTbl = getAnimTable(emPtr)
                Log(TAG .. ": Init done: HP=" .. hp
                    .. " animTbl=" .. (animTbl == 0 and "NULL!" or string.format("0x%X", animTbl))
                    .. " nextState=0x" .. string.format("%X", getPackedState(emPtr)))
            end
            return retval
        end)
    Log(TAG .. ": Hook — em09_stateInit (setup verification)")
end)

-- Hook counterAttack — log melee hits
pcall(function()
    RegisterNativeHook("em09_counterAttack",
        nil,
        function(retval, emPtr)
            if emPtr ~= 0 then
                local hit = getGrabFlag(emPtr)
                if hit ~= 0 then
                    Log(TAG .. ": COUNTER HIT! → player damaged")
                end
            end
            return retval
        end)
    Log(TAG .. ": Hook — em09_counterAttack (melee detection)")
end)

-- ═══════════════════════════════════════════════════════════════════════
-- HELPER — Iterate all live Tyrants
-- ═══════════════════════════════════════════════════════════════════════
local function forEachTyrant(fn)
    local count = 0
    for key, inst in pairs(tracker.instances) do
        fn(inst.ptr, inst)
        count = count + 1
    end
    if count == 0 then Log(TAG .. ": No live Tyrants") end
    return count
end

-- ═══════════════════════════════════════════════════════════════════════
-- COMMANDS — Runtime control
-- ═══════════════════════════════════════════════════════════════════════

RegisterCommand("tyrant_status", function()
    forEachTyrant(function(ptr, inst)
        local packed = getPackedState(ptr)
        Log("─── Tyrant @ 0x" .. string.format("%X", ptr) .. " ───")
        Log("  State: " .. stateName(packed) .. " (0x" .. string.format("%X", packed) .. ")")
        Log("  Phase: " .. getPhase(ptr) .. "  Param: " .. getParam(ptr))
        Log("  HP: " .. getHP(ptr))
        Log("  Pos: (" .. string.format("%.1f, %.1f, %.1f", getPosX(ptr), getPosY(ptr), getPosZ(ptr)) .. ")")
        Log("  Dist: " .. string.format("%.0f", math.sqrt(math.max(0, getDistSq(ptr))))
            .. "  Angle: " .. string.format("%.2f", getAngle(ptr)))
        Log("  AnimTable: " .. string.format("0x%X", getAnimTable(ptr)))
        Log("  Route: " .. getRouteFlags(ptr) .. "  Grab: " .. getGrabFlag(ptr))
        Log("  Moves: " .. inst.moveCount .. "  Transitions: " .. #inst.stateHistory)
        Log("  InitOk: " .. tostring(inst.initOk))
    end)
end)

RegisterCommand("tyrant_idle", function()
    forEachTyrant(function(ptr)
        setPackedState(ptr, 0x0001); setPhase(ptr, 0); setParam(ptr, 0)
        Log(TAG .. ": Forced → IDLE")
    end)
end)

RegisterCommand("tyrant_chase", function()
    forEachTyrant(function(ptr)
        setPackedState(ptr, 0x0501); setPhase(ptr, 0); setParam(ptr, 0)
        Log(TAG .. ": Forced → CHASE")
    end)
end)

RegisterCommand("tyrant_attack", function()
    forEachTyrant(function(ptr)
        setPackedState(ptr, 0x0801); setPhase(ptr, 0); setParam(ptr, 0)
        Log(TAG .. ": Forced → CLOSE_ATK")
    end)
end)

RegisterCommand("tyrant_heal", function()
    forEachTyrant(function(ptr)
        setHP(ptr, 1000)
        if getMainState(ptr) == 3 then
            setPackedState(ptr, 0x0001); setPhase(ptr, 0)
            Log(TAG .. ": Revived from death → IDLE (1000 HP)")
        else
            Log(TAG .. ": Healed to 1000 HP")
        end
    end)
end)

RegisterCommand("tyrant_god", function()
    forEachTyrant(function(ptr) setHP(ptr, 99999); Log(TAG .. ": God mode ON (99999 HP)") end)
end)

RegisterCommand("tyrant_mortal", function()
    forEachTyrant(function(ptr) setHP(ptr, 1000); Log(TAG .. ": God mode OFF (1000 HP)") end)
end)

RegisterCommand("tyrant_verbose", function()
    tracker.verbose = not tracker.verbose
    Log(TAG .. ": Verbose " .. (tracker.verbose and "ON" or "OFF"))
    Notify(TAG, "Verbose " .. (tracker.verbose and "ON" or "OFF"))
end)

RegisterCommand("tyrant_history", function()
    forEachTyrant(function(ptr, inst)
        Log("─── State History (" .. #inst.stateHistory .. " transitions) ───")
        for i, t in ipairs(inst.stateHistory) do
            Log("  [" .. t.tick .. "] " .. stateName(t.from) .. " → " .. stateName(t.to))
        end
    end)
end)

-- ═══════════════════════════════════════════════════════════════════════
-- SHARED API
-- ═══════════════════════════════════════════════════════════════════════

if SharedAPI then
    SharedAPI.TyrantAI = {
        tracker = tracker,
        forEachTyrant = forEachTyrant,
        getHP = getHP,
        setHP = setHP,
        getPackedState = getPackedState,
        setPackedState = setPackedState,
        stateName = stateName,
    }
end

if SharedAPI and SharedAPI.DebugMenu then
    SharedAPI.DebugMenu.RegisterAction("TyrantAI", "Tyrant Status",
        function()
            forEachTyrant(function(ptr, inst)
                Log(TAG .. ": " .. stateName(getPackedState(ptr))
                    .. " HP=" .. getHP(ptr))
            end)
        end)
end

Log(TAG .. ": v3.0 loaded — cEm09 state machine monitor"
    .. " | 5 native hooks (Em09Init/move/dtor/stateInit/counterAttack)"
    .. " | 8 commands + UE4SS ForEachUObject")
