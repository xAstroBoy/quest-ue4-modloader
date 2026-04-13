-- mods/EnemySpawner/main.lua v8.0
-- ═══════════════════════════════════════════════════════════════════════
-- Direct EmSetEvent Enemy Spawner — 170+ enemies via native EmSetEvent()
--
-- v8.0 — COMPLETE REWRITE: Direct EmSetEvent() spawning
--   Replaces broken ESL-write approach with direct EmSetEvent() calls
--   Spawns enemies REGARDLESS of level or ESL slots
--   Uses AllocateMemory + CallNative for native function invocation
--   Gets player position from pPL for near-player spawning
--   Distributes spawns in a circle around the player
-- ═══════════════════════════════════════════════════════════════════════
local TAG = "EnemySpawner"
local VERBOSE = false
local function V(...) if VERBOSE then Log(TAG .. " [V] " .. string.format(...)) end end

-- ── HP presets (LE uint16 → {lo, hi}) ───────────────────────────────
local HP_PRESETS = {
    EASY   = { 232, 3 },    -- 1000
    NORMAL = { 184, 11 },   -- 3000
    HARD   = { 76, 29 },    -- 7500
}
local HP_BOSS = {
    EASY   = { 184, 11 },   -- 3000
    NORMAL = { 136, 19 },   -- 5000
    HARD   = { 76, 29 },    -- 7500
}
local HP_GARRADOR = {
    EASY   = { 160, 15 },   -- 4000
    NORMAL = { 136, 19 },   -- 5000
    HARD   = { 124, 21 },   -- 5500
}

local function makeHP(lo, hi) return lo + (hi * 256) end

-- ═══════════════════════════════════════════════════════════════════════
-- COMPLETE ENEMY DATABASE — 170+ enemies from binary EEmID + EEm10Type
-- Each entry: { name, bytes[9], hpType?, removeInvincible? }
-- bytes: {emId, subType, param1, param2, p5, p6, p7, hpLo, hpHi}
-- ═══════════════════════════════════════════════════════════════════════

local VILLAGERS = {
    { "Villager 1500",  {21,0,0,0,24,64,0, 232,3} },
    { "Villager 1501",  {21,0,0,0,8,20,1, 232,3} },
    { "Villager 1502",  {21,0,0,0,2,32,1, 232,3} },
    { "Villager 1503",  {21,3,0,0,0,48,32, 232,3} },
    { "Villager 1504",  {21,3,0,0,1,52,65, 232,3} },
    { "Villager 1505",  {21,3,0,0,68,56,1, 232,3} },
    { "Villager 1506",  {21,4,0,0,96,48,0, 232,3} },
    { "Villager 1507",  {21,4,0,0,104,32,1, 232,3} },
    { "Villager 1508",  {21,4,0,0,105,64,0, 232,3} },
    { "Villager 1509",  {21,11,0,0,0,52,40, 232,3} },
    { "Villager 1510",  {21,11,0,0,0,56,41, 232,3} },
    { "Villager 1511",  {21,11,0,0,0,68,128, 232,3} },
    { "Villager 1600",  {22,2,0,0,0,160,129, 232,3} },
    { "Villager 1601",  {22,2,0,0,0,32,32, 232,3} },
    { "Villager 1602",  {22,2,0,0,0,120,137, 232,3} },
}

local SALVADORS = {
    { "Dr. Salvador 150", {21,0,0,0,96,16,16, 232,3} },
    { "Dr. Salvador 151", {21,0,0,0,1,0,16, 232,3} },
    { "Dr. Salvador 152", {21,0,48,0,2,48,16, 232,3} },
    { "Dr. Salvador 153", {21,3,0,0,1,48,16, 232,3} },
    { "Dr. Salvador 154", {21,3,0,0,2,32,16, 232,3} },
    { "Dr. Salvador 155", {21,3,48,0,0,32,16, 232,3} },
    { "Dr. Salvador 156", {21,4,0,0,96,48,16, 232,3} },
    { "Dr. Salvador 157", {21,4,0,0,97,0,16, 232,3} },
    { "Dr. Salvador 158", {21,4,0,0,99,0,16, 232,3} },
    { "Bella Sister 00",  {21,11,0,0,96,0,16, 232,3} },
    { "Bella Sister 01",  {21,11,0,0,97,16,16, 232,3} },
    { "Bella Sister 02",  {21,11,48,0,99,32,16, 232,3} },
    { "Bella Sister 03",  {22,2,0,0,0,160,16, 232,3} },
    { "Bella Sister 04",  {22,2,0,0,113,32,16, 232,3} },
    { "Bella Sister 05",  {22,2,48,0,0,120,16, 232,3} },
    { "Bella Sister 06",  {22,11,0,0,0,32,16, 232,3} },
    { "Bella Sister 07",  {22,11,0,0,1,124,16, 232,3} },
    { "Bella Sister 08",  {22,11,0,0,2,0,16, 232,3} },
}

local ZEALOTS_1C = {
    { "Zealot 1C00", {28,7,0,0,0,20,0, 232,3} },
    { "Zealot 1C01", {28,7,0,0,16,18,1, 232,3} },
    { "Zealot 1C02", {28,7,0,0,2,2,1, 232,3} },
    { "Zealot 1C03", {28,7,0,0,8,128,0, 232,3} },
    { "Zealot 1C04", {28,7,0,0,8,4,129, 232,3} },
    { "Zealot 1C05", {28,7,0,0,64,20,132, 232,3} },
    { "Zealot 1C06", {28,7,0,0,144,128,0, 232,3} },
    { "Zealot 1C07", {28,7,0,0,144,132,128, 232,3} },
    { "Zealot 1C08", {28,9,0,0,0,20,0, 232,3} },
    { "Zealot 1C09", {28,9,0,0,16,2,1, 232,3} },
    { "Zealot 1C10", {28,9,0,0,2,2,128, 232,3} },
    { "Zealot 1C11", {28,9,0,0,8,144,1, 232,3} },
    { "Zealot 1C12", {28,9,0,0,8,4,128, 232,3} },
    { "Zealot 1C13", {28,9,0,0,64,20,132, 232,3} },
    { "Zealot 1C14", {28,9,0,0,64,4,33, 232,3} },
    { "Zealot 1C15", {28,9,0,0,64,132,160, 232,3} },
}

local ZEALOTS_1A = {
    { "Zealot 1A00", {26,7,0,2,0,16,0, 232,3} },
    { "Zealot 1A01", {26,7,0,2,16,2,1, 232,3} },
    { "Zealot 1A02", {26,7,0,2,2,3,1, 232,3} },
    { "Zealot 1A03", {26,7,0,2,0,128,64, 232,3} },
    { "Zealot 1A04", {26,7,0,2,8,4,0, 232,3} },
    { "Zealot 1A05", {26,7,0,2,64,20,132, 232,3} },
    { "Zealot 1A06", {26,7,0,2,144,128,0, 232,3} },
    { "Zealot 1A07", {26,7,0,2,144,0,128, 232,3} },
    { "Zealot 1A08", {26,8,0,2,0,16,0, 232,3} },
    { "Zealot 1A09", {26,8,0,2,1,2,0, 232,3} },
    { "Zealot 1A10", {26,8,0,2,1,2,129, 232,3} },
    { "Zealot 1A11", {26,8,0,0,8,144,1, 232,3} },
    { "Zealot 1A12", {26,8,0,0,8,4,128, 232,3} },
    { "Zealot 1A13", {26,9,0,2,64,20,132, 232,3} },
    { "Zealot 1A14", {26,9,0,2,64,4,33, 232,3} },
    { "Zealot 1A15", {26,9,0,2,64,132,160, 232,3} },
}

local ZEALOTS_MISC = {
    { "Zealot 110",  {17,8,0,96,2,72,0, 232,3} },
    { "Zealot 111",  {17,7,0,160,128,8,0, 232,3} },
    { "Zealot 112",  {17,8,0,96,128,8,0, 232,3} },
    { "Zealot 113",  {17,8,0,2,66,80,32, 232,3} },
    { "Zealot 114",  {17,7,0,96,2,64,32, 232,3} },
    { "Zealot 140",  {20,8,0,50,1,80,0, 232,3} },
    { "Zealot 141",  {20,7,0,48,64,68,32, 232,3} },
    { "Zealot 142",  {20,8,0,2,0,64,0, 232,3} },
    { "Zealot 143",  {20,8,0,32,64,68,128, 232,3} },
    { "Zealot 144",  {20,8,0,0,208,64,160, 232,3} },
    { "Zealot 145",  {20,9,0,2,0,64,4, 232,3} },
    { "Zealot 146",  {20,9,0,0,0,32,4, 232,3} },
}

local ZEALOTS_1B = {
    { "Zealot 1B1", {27,7,37,32,32,7,0, 232,3} },
    { "Zealot 1B2", {27,7,0,32,144,0,0, 232,3} },
    { "Zealot 1B3", {27,7,0,0,64,128,32, 232,3} },
    { "Zealot 1B4", {27,7,0,0,0,68,128, 232,3} },
    { "Zealot 1B5", {27,7,0,48,66,64,32, 232,3} },
    { "Zealot 1B6", {27,7,0,0,0,48,0, 232,3} },
}

local GARRADORS = {
    { "Garrador",               {28,10,0,0,0,0,0, 160,15}, "garrador" },
    { "Armored Garrador",       {28,13,0,0,0,0,0, 124,21}, "garrador" },
    { "Armored Garrador Plaga", {28,10,48,0,0,0,0, 160,15}, "garrador" },
    { "Garrador 1B",            {27,10,0,0,0,0,0, 232,3}, "garrador" },
}

local MACE_GANADOS = {
    { "Mace Soldier 00", {31,24,0,0,24,0,1, 232,3} },
    { "Mace Soldier 01", {31,24,0,0,72,4,1, 232,3} },
    { "Mace Soldier 02", {31,24,0,0,89,16,1, 232,3} },
    { "Mace Soldier 03", {31,24,0,0,10,0,1, 232,3} },
    { "Mace Soldier 04", {31,24,0,0,91,0,1, 232,3} },
    { "Mace Soldier 05", {31,24,0,0,16,0,33, 232,3} },
    { "Mace Soldier 06", {31,24,0,0,64,0,33, 232,3} },
    { "Mace Soldier 07", {31,24,0,0,16,0,4, 232,3} },
    { "Mace Soldier 08", {31,24,0,0,64,0,5, 232,3} },
    { "Mace Soldier 09", {31,24,0,0,193,16,1, 232,3} },
    { "Mace Soldier 10", {31,24,0,0,16,16,64, 232,3} },
    { "Mace Soldier 11", {31,24,0,0,0,4,1, 232,3} },
}

local SOLDIERS = {
    { "Soldier 1D00", {29,14,0,8,72,1,0, 232,3} },
    { "Soldier 1D01", {29,14,0,0,2,2,129, 232,3} },
    { "Soldier 1D02", {29,16,0,0,17,3,129, 232,3} },
    { "Soldier 1D03", {29,16,0,0,2,2,129, 232,3} },
    { "Soldier 1D04", {29,18,0,0,64,0,4, 232,3} },
    { "Soldier 1D05", {29,18,0,0,72,0,129, 232,3} },
    { "Soldier 1D06", {29,18,0,0,1,2,0, 232,3} },
    { "Soldier 1E00", {30,14,0,8,1,3,160, 232,3} },
    { "Soldier 1E01", {30,14,0,8,1,0,192, 232,3} },
    { "Soldier 1E02", {30,14,0,0,1,193,17, 232,3} },
    { "Soldier 1E03", {30,14,0,0,131,1,0, 232,3} },
    { "Soldier 1E04", {30,14,0,2,16,32,136, 232,3} },
    { "Soldier 1E05", {30,15,0,2,16,32,137, 232,3} },
    { "Soldier 1E06", {30,15,0,0,129,13,0, 232,3} },
    { "Soldier 1E07", {30,15,0,2,96,19,0, 232,3} },
    { "Soldier 1E08", {30,15,0,10,128,21,0, 232,3} },
    { "Soldier 1E09", {30,15,0,2,48,17,8, 232,3} },
    { "Soldier 1E10", {30,15,0,10,128,21,0, 232,3} },
    { "Soldier 1E11", {30,23,0,2,48,17,8, 232,3} },
    { "Soldier 1E12", {30,23,0,2,48,17,136, 232,3} },
    { "Soldier 1E13", {30,23,0,0,128,9,0, 232,3} },
    { "Soldier 1E14", {30,23,0,0,0,0,4, 232,3} },
    { "Soldier 1E15", {30,23,0,0,0,16,5, 232,3} },
    { "Soldier 1E16", {30,23,0,8,1,0,192, 232,3} },
    { "Soldier 1E17", {30,25,0,0,193,17,0, 232,3} },
    { "Soldier 1E18", {30,25,0,2,16,32,136, 232,3} },
    { "Soldier 1E19", {30,25,0,0,0,16,0, 232,3} },
    { "Soldier 1F00", {31,14,0,10,0,80,33, 232,3} },
    { "Soldier 1F01", {31,15,0,0,33,9,9, 232,3} },
    { "Soldier 1F02", {31,16,0,10,17,16,32, 232,3} },
}

local JJS = {
    { "JJ",       {29,2,0,0,0,0,0, 136,19}, "boss" },
    { "JJ Plaga", {29,2,48,0,0,0,0, 136,19}, "boss" },
}

local SUPER_SALVADORS = {
    { "Super Salvador",          {32,22,0,0,0,0,0, 172,13}, "boss" },
    { "Super Salvador Plaga",    {32,22,0,8,8,176,16, 172,13}, "boss" },
    { "Super Salvador Dynamite", {32,22,0,8,8,176,16, 172,13}, "boss" },
}

local DOGS = {
    { "Wolf (Friendly)",   {33,0,0,0,0,0,0, 232,3} },
    { "Colmillos (EmDog)", {34,0,0,0,0,0,0, 232,3} },
}

local ARMADURAS = {
    { "Armadura 00", {60,0,0,0,0,0,0, 232,3} },
    { "Armadura 01", {60,1,0,0,0,0,0, 232,3} },
    { "Armadura 02", {60,2,0,0,0,0,0, 232,3} },
    { "Armadura 03", {60,3,0,0,0,0,0, 232,3} },
}

local DRONES = {
    { "Drone",        {58,0,0,0,0,0,0, 232,3} },
    { "Ground Robot", {58,2,0,0,0,0,0, 232,3} },
}

local ANIMALS = {
    { "Chicken 00",  {40,0,0,0,0,0,0, 232,3} },
    { "Chicken 01",  {40,1,0,0,0,0,0, 232,3} },
    { "Crow",        {35,0,0,0,0,0,0, 232,3} },
    { "Cow 00",      {38,0,0,0,0,0,0, 255,127} },
    { "Cow 01",      {38,0,0,0,0,0,0, 255,127} },
    { "Snake",       {36,0,1,0,0,0,0, 232,3} },
    { "Bat",         {41,0,0,0,0,0,0, 232,3} },
    { "Black Bass",  {39,0,0,0,0,0,0, 232,3} },
    { "Spider",      {46,0,0,0,0,0,0, 232,3} },
}

local TRAPS = {
    { "Walking Parasite", {37,0,0,0,0,0,0, 232,3} },
    { "Bear Trap",        {42,0,0,0,0,0,0, 232,3} },
    { "Wire Trap",        {42,2,0,0,0,0,0, 232,3} },
}

local NOVISTADORES = {
    { "Novistador",            {45,0,0,0,0,0,0, 232,3} },
    { "Novistador Boss",       {44,0,0,0,0,0,0, 232,3}, "boss" },
    { "Novistador Boss Event", {51,0,0,0,0,0,0, 232,3}, "boss" },
}

local REGENERATORS = {
    { "Regenerator", {54,0,0,0,0,0,0, 232,3} },
    { "Iron Maiden", {54,2,0,0,0,0,0, 232,3} },
}

local GIGANTES = {
    { "El Gigante", {43,0,0,0,0,0,0, 232,3}, "boss" },
}

local VERDUGOS = {
    { "Verdugo (No2)",       {55,0,0,0,0,0,0, 232,3}, "boss" },
    { "Verdugo After (No2)", {56,0,0,0,0,0,0, 232,3}, "boss" },
}

local BOSSES = {
    { "Mendez Phase 1",      {53,0,0,0,0,0,0, 232,3}, "boss" },
    { "Mendez Phase 2",      {53,1,0,0,0,0,0, 232,3}, "boss" },
    { "Krauser Knife (No3)", {57,1,0,0,0,0,0, 232,3}, "boss" },
    { "Krauser Mutant",      {57,2,0,0,0,0,0, 232,3}, "boss" },
    { "U3 (It)",             {50,1,0,0,0,0,0, 232,3}, "boss", true },
    { "Saddler After",       {49,0,0,0,0,0,0, 232,3}, "boss" },
    { "Saddler (Ada)",       {63,0,0,0,0,0,0, 232,3}, "boss" },
    { "Salamander",          {47,0,0,0,0,0,0, 232,3}, "boss" },
    { "Tyrant (em09)",       {9,0,0,0,0,0,0, 232,3}, "boss" },
}

local NPCS = {
    { "Merchant",            {24,6,0,0,0,0,0, 232,3} },
    { "Saddler NPC 0",      {48,0,0,0,0,0,0, 232,3}, "boss" },
    { "Saddler NPC 1",      {48,1,0,0,0,0,0, 232,3}, "boss" },
    { "Mendez NPC",          {52,0,0,0,0,0,0, 232,3} },
    { "Salazar NPC",         {52,1,0,0,0,0,0, 232,3} },
    { "Red Cloak Verdugo",   {52,2,0,0,0,0,0, 232,3}, "boss" },
    { "Black Cloak Verdugo", {52,3,0,0,0,0,0, 232,3}, "boss" },
    { "Ashley",              {3,0,0,0,0,0,0, 232,3} },
    { "Ashley 2",            {5,0,0,0,0,0,0, 232,3} },
    { "Ashley 3",            {12,0,0,0,0,0,0, 232,3} },
    { "Luis Sera",           {4,0,0,0,0,0,0, 232,3} },
    { "HUNK",                {6,0,0,0,0,0,0, 232,3} },
    { "Wesker",              {13,0,0,0,0,0,0, 232,3} },
    { "Krauser (NPC)",       {10,0,0,0,0,0,0, 232,3} },
    { "Police",              {7,0,0,0,0,0,0, 232,3} },
}

local ADA_ENEMIES = {
    { "SW Zombie Ganado (em40)",   {64,0,0,0,0,0,0, 232,3} },
    { "SW Zombie Ganado B (em41)", {65,0,0,0,0,0,0, 232,3} },
    { "SW Villager Ganado (em42)", {66,0,0,0,0,0,0, 232,3} },
    { "SW Soldier Ganado (em43)",  {67,0,0,0,0,0,0, 232,3} },
    { "SW Ganado (em44)",          {68,0,0,0,0,0,0, 232,3} },
    { "SW Ganado (em45)",          {69,0,0,0,0,0,0, 232,3} },
    { "SW Ganado (em46)",          {70,0,0,0,0,0,0, 232,3} },
    { "SW Ganado (em47)",          {71,0,0,0,0,0,0, 232,3} },
    { "SW Ganado (em48)",          {72,0,0,0,0,0,0, 232,3} },
    { "SW Ganado (em49)",          {73,0,0,0,0,0,0, 232,3} },
    { "SW Ganado (em4a)",          {74,0,0,0,0,0,0, 232,3} },
    { "SW Ganado (em4c)",          {76,0,0,0,0,0,0, 232,3} },
    { "SW Ganado (em4d)",          {77,0,0,0,0,0,0, 232,3} },
    { "SW Ganado (em4f)",          {79,0,0,0,0,0,0, 232,3} },
}

local VEHICLES = {
    { "Truck",      {59,0,0,0,0,0,0, 232,3}, "boss" },
    { "Helicopter", {61,0,0,0,0,0,0, 232,3}, "boss" },
}

-- ═══════════════════════════════════════════════════════════════════════
-- CATEGORIES — Each is a submenu in the spawner
-- ═══════════════════════════════════════════════════════════════════════
local CATEGORIES = {
    { name = "Villagers",                    enemies = VILLAGERS },
    { name = "Dr. Salvador + Bella Sisters", enemies = SALVADORS },
    { name = "Zealots (1C)",                 enemies = ZEALOTS_1C },
    { name = "Zealots (1A)",                 enemies = ZEALOTS_1A },
    { name = "Zealots (11/14)",              enemies = ZEALOTS_MISC },
    { name = "Zealots (1B)",                 enemies = ZEALOTS_1B },
    { name = "Garradors",                    enemies = GARRADORS },
    { name = "Big Mace Ganados",             enemies = MACE_GANADOS },
    { name = "Island Soldiers",              enemies = SOLDIERS },
    { name = "JJs",                          enemies = JJS },
    { name = "Super Salvadors",              enemies = SUPER_SALVADORS },
    { name = "Dogs (Colmillos)",             enemies = DOGS },
    { name = "Armaduras",                    enemies = ARMADURAS },
    { name = "Drones + Robots",              enemies = DRONES },
    { name = "Animals",                      enemies = ANIMALS },
    { name = "Parasites + Traps",            enemies = TRAPS },
    { name = "Novistadores",                 enemies = NOVISTADORES },
    { name = "Regenerators + Iron Maidens",  enemies = REGENERATORS },
    { name = "El Gigante",                   enemies = GIGANTES },
    { name = "Verdugos (No2)",               enemies = VERDUGOS },
    { name = "Bosses",                       enemies = BOSSES },
    { name = "NPCs",                         enemies = NPCS },
    { name = "Ada Assignment Ganados",       enemies = ADA_ENEMIES },
    { name = "Vehicles + Turrets",           enemies = VEHICLES },
}

-- Build flat lookup tables
local ALL_ENEMIES = {}
local ENEMY_BY_NAME = {}
for _, cat in ipairs(CATEGORIES) do
    for _, em in ipairs(cat.enemies) do
        local entry = {
            name   = em[1],
            bytes  = em[2],
            hpType = em[3] or "normal",
            removeInvincible = em[4] or false,
            category = cat.name,
        }
        ALL_ENEMIES[#ALL_ENEMIES + 1] = entry
        ENEMY_BY_NAME[em[1]] = entry
    end
end

-- ═══════════════════════════════════════════════════════════════════════
-- NATIVE ENGINE — Direct EmSetEvent() spawning
-- ═══════════════════════════════════════════════════════════════════════
-- EmSetEvent(EM_LIST*) -> cEm*
-- Spawns an enemy from a 32-byte EM_LIST struct.
-- Returns the cEm entity pointer, or errEm on failure (pool full / bad emId).
-- Sets EM_LIST[0x00] = 7 on success, cEm[0x92] = 0xFF (event-spawned, not ESL).
-- Does NOT check room/scenario — spawns work on ANY level.
--
-- EM_LIST struct layout (from Binary Ninja decompilation @ 0x062E9E8C):
--   +0x00  u8   status     0=ready, EmSetEvent sets to 7 after spawn
--   +0x01  u8   emId       Enemy type ID (database bytes[1])
--   +0x02  u8   subType    -> cEm+0x119 (database bytes[2])
--   +0x03  u8   param1     -> cEm+0x47d (database bytes[3])
--   +0x04  u8   param2     -> cEm+0x4c4 (database bytes[4])
--   +0x05-07    (unused by EmSetEvent — write database bytes[5-7] for compat)
--   +0x08  s16  hp         HP (LE int16) -> cEm+0x7e, cEm+0x3f2
--   +0x0a       (unused)
--   +0x0b  u8   param3     -> cEm+0x4cc
--   +0x0c  s16  posX       x 10.0 -> world X float at cEm+0xa4
--   +0x0e  s16  posY       x 10.0 -> world Y float (height)
--   +0x10  s16  posZ       x 10.0 -> world Z float at cEm+0xac
--   +0x12  s16  rotY       x (pi/16384) -> facing angle (radians)
--   +0x14  s16  rotX       x (pi/16384) -> pitch
--   +0x16  s16  rotZ       x (pi/16384) -> roll
--   +0x18  s16  scale?     x 1000.0
--   +0x1A  s16  misc

local sym_EmSetEvent = nil
local addr_pPL       = nil   -- BSS: cPlayer* pointer (player position)
local addr_errEm     = nil   -- BSS: cEm* error sentinel
local emListBuf      = nil   -- Reusable 32-byte EM_LIST (AllocateMemory)
local nativeReady    = false

local function initNative()
    if nativeReady then V("initNative: already initialized"); return true end

    V("initNative: resolving native symbols...")
    local base = nil
    pcall(function() base = GetLibBase() end)
    if not base or IsNull(base) then
        V("initNative: GetLibBase() returned nil/null")
        LogWarn(TAG .. ": GetLibBase() failed")
        return false
    end
    V("initNative: base=%s", tostring(base))

    -- Resolve EmSetEvent — try mangled C++ name, then plain, then raw offset
    pcall(function()
        sym_EmSetEvent = Resolve("_Z10EmSetEventP7EM_LIST", 0x062E9E8C)
    end)
    if not sym_EmSetEvent or IsNull(sym_EmSetEvent) then
        pcall(function()
            sym_EmSetEvent = Resolve("EmSetEvent", 0x062E9E8C)
        end)
    end
    if not sym_EmSetEvent or IsNull(sym_EmSetEvent) then
        pcall(function()
            sym_EmSetEvent = Offset(base, 0x062E9E8C)
        end)
    end
    V("initNative: EmSetEvent resolved=%s", tostring(sym_EmSetEvent ~= nil))

    -- pPL: BSS variable holding cPlayer* (player position source)
    -- pPL+0xa4 = posX (float), pPL+0xa8 = posY (float), pPL+0xac = posZ (float)
    pcall(function() addr_pPL = Offset(base, 0x0A94AB40) end)
    V("initNative: pPL=%s", tostring(addr_pPL))

    -- errEm: BSS variable holding cEm* sentinel returned on spawn failure
    pcall(function() addr_errEm = Offset(base, 0x0A954FF0) end)
    V("initNative: errEm=%s", tostring(addr_errEm))

    -- Allocate reusable 32-byte EM_LIST buffer (calloc -> zero-initialized)
    pcall(function() emListBuf = AllocateMemory(32) end)
    V("initNative: emListBuf=%s", tostring(emListBuf))

    if not sym_EmSetEvent or IsNull(sym_EmSetEvent) then
        V("initNative: FAILED - EmSetEvent not resolved")
        LogWarn(TAG .. ": EmSetEvent resolve FAILED")
        return false
    end
    if not emListBuf or IsNull(emListBuf) then
        V("initNative: FAILED - AllocateMemory returned nil/null")
        LogWarn(TAG .. ": AllocateMemory(32) FAILED")
        return false
    end

    nativeReady = true
    Log(TAG .. ": Native engine ready")
    Log(TAG .. ":   EmSetEvent  @ " .. ToHex(sym_EmSetEvent))
    pcall(function() Log(TAG .. ":   pPL var     @ " .. ToHex(addr_pPL)) end)
    pcall(function() Log(TAG .. ":   errEm var   @ " .. ToHex(addr_errEm)) end)
    pcall(function() Log(TAG .. ":   emListBuf   @ " .. ToHex(emListBuf)) end)
    return true
end

-- ═══════════════════════════════════════════════════════════════════════
-- PLAYER POSITION — UE4 reflection (primary) + native pPL fallback
-- ═══════════════════════════════════════════════════════════════════════

local function getPlayerPosition()
    -- Method 1: UE4 reflection — reliable, works as long as pawn is spawned
    -- Try multiple class names (VR4Bio4PlayerPawn is the actual class in RE4 VR)
    local pawn = nil
    for _, cls in ipairs({"VR4Bio4PlayerPawn", "VR4GamePlayerPawn", "Pawn"}) do
        if not pawn then
            pcall(function()
                local p = FindFirstOf(cls)
                if p and p:IsValid() then pawn = p end
            end)
        end
    end

    if pawn then
        -- Try K2_GetActorLocation → returns FVector LuaUStruct {X, Y, Z}
        -- RE4 VR wraps the RE4 engine inside UE4 — actor positions are stored
        -- in RE4 native coords: X=horizontal, Y=height, Z=horizontal
        -- The cEm position at +0xA4/+0xA8/+0xAC matches the UE4 actor location
        local ok, loc = pcall(function() return pawn:Call("K2_GetActorLocation") end)
        if ok and loc then
            local x, y, z
            pcall(function() x = loc.X end)
            pcall(function() y = loc.Y end)
            pcall(function() z = loc.Z end)
            if x and z and (x ~= 0 or y ~= 0 or z ~= 0) then
                V("getPlayerPosition: K2_GetActorLocation=(%.1f, %.1f, %.1f)", x, y or 0, z or 0)
                -- Return RE4 native: (posX, posY_height, posZ)
                return x, y or 0, z or 0
            end
        end

        -- Try reading RootComponent.RelativeLocation
        local ok2, root = pcall(function() return pawn:Get("RootComponent") end)
        if ok2 and root and root:IsValid() then
            local ok3, rloc = pcall(function() return root:Get("RelativeLocation") end)
            if ok3 and rloc then
                local x, y, z
                pcall(function() x = rloc.X end)
                pcall(function() y = rloc.Y end)
                pcall(function() z = rloc.Z end)
                if x and z and (x ~= 0 or y ~= 0 or z ~= 0) then
                    V("getPlayerPosition: RelativeLocation=(%.1f, %.1f, %.1f)", x, y or 0, z or 0)
                    return x, y or 0, z or 0
                end
            end
        end
    end

    -- Fallback: native pPL (original method — may return 0,0,0)
    if addr_pPL then
        local pPL = nil
        pcall(function() pPL = ReadPtr(addr_pPL) end)
        if pPL and not IsNull(pPL) then
            local x, y, z
            pcall(function() x = ReadFloat(Offset(pPL, 0xa4)) end)
            pcall(function() y = ReadFloat(Offset(pPL, 0xa8)) end)
            pcall(function() z = ReadFloat(Offset(pPL, 0xac)) end)
            if x and z and (x ~= 0 or z ~= 0) then
                V("getPlayerPosition: native pPL pos=(%.1f, %.1f, %.1f)", x, y or 0, z)
                return x, y or 0, z
            end
        end
    end

    V("getPlayerPosition: all methods returned nil/zero")
    return nil
end

-- ═══════════════════════════════════════════════════════════════════════
-- STATE
-- ═══════════════════════════════════════════════════════════════════════

local state = {
    difficulty    = "NORMAL",
    spawnCount    = 1,
    spawnDistance  = 200,   -- world units from player (enemies spawn in circle)
    selectedIdx   = 1,
    totalSpawned  = 0,
    lastSpawnedEm = nil,   -- last cEm* returned by EmSetEvent
}

local saved = ModConfig.Load("EnemySpawner")
V("Config: loaded=%s", tostring(saved ~= nil))
if saved then
    if saved.difficulty and HP_PRESETS[saved.difficulty] then
        state.difficulty = saved.difficulty
    end
    if saved.spawnCount then
        state.spawnCount = math.max(1, math.min(5, saved.spawnCount))
    end
    if saved.spawnDistance then
        state.spawnDistance = math.max(50, math.min(2000, saved.spawnDistance))
    end
end

-- ═══════════════════════════════════════════════════════════════════════
-- HP CALCULATION
-- ═══════════════════════════════════════════════════════════════════════

local function getHP(enemy)
    local diff = state.difficulty
    local t = enemy.hpType
    local hp
    if t == "boss" then
        hp = HP_BOSS[diff] or HP_BOSS.NORMAL
    elseif t == "garrador" then
        hp = HP_GARRADOR[diff] or HP_GARRADOR.NORMAL
    else
        hp = HP_PRESETS[diff] or HP_PRESETS.NORMAL
    end
    return hp[1] + hp[2] * 256
end

-- ═══════════════════════════════════════════════════════════════════════
-- SIGNED INT16 HELPER — for WriteU16 with negative position values
-- ═══════════════════════════════════════════════════════════════════════

local function toU16(v)
    v = math.floor(v + 0.5)
    if v < -32768 then v = -32768 end
    if v > 32767 then v = 32767 end
    if v < 0 then v = v + 65536 end  -- two's complement for WriteU16
    return v
end

-- ═══════════════════════════════════════════════════════════════════════
-- SPAWN ENGINE — Direct EmSetEvent() call
-- ═══════════════════════════════════════════════════════════════════════

local function spawnSingle(enemy, offsetX, offsetZ)
    V("spawnSingle: enemy=%s offX=%s offZ=%s", enemy.name, tostring(offsetX), tostring(offsetZ))
    if not initNative() then V("spawnSingle: native init failed"); return false, "Native init failed" end

    -- Get player position in RE4 world coords
    local px, py, pz = getPlayerPosition()
    if not px then V("spawnSingle: no player position"); return false, "No player position — load a level first" end
    V("spawnSingle: playerPos=(%.1f,%.1f,%.1f)", px, py, pz)

    -- Compute HP from preset tables
    local hp = getHP(enemy)
    V("spawnSingle: hp=%d emId=%d subType=%d", hp, enemy.bytes[1], enemy.bytes[2])
    local b = enemy.bytes
    -- bytes: {emId, subType, param1, param2, p5, p6, p7, hpLo, hpHi}

    -- Zero-fill the entire 32-byte EM_LIST buffer
    for i = 0, 31 do
        pcall(function() WriteU8(Offset(emListBuf, i), 0) end)
    end

    -- Fill EM_LIST struct fields from database entry
    -- +0x00: status = 0 (ready for EmSetEvent, it sets to 7 after spawn)
    pcall(function() WriteU8(Offset(emListBuf, 0x01), b[1]) end)       -- emId
    pcall(function() WriteU8(Offset(emListBuf, 0x02), b[2]) end)       -- subType
    pcall(function() WriteU8(Offset(emListBuf, 0x03), b[3]) end)       -- param1 -> cEm+0x47d
    pcall(function() WriteU8(Offset(emListBuf, 0x04), b[4]) end)       -- param2 -> cEm+0x4c4
    pcall(function() WriteU8(Offset(emListBuf, 0x05), b[5] or 0) end)  -- compat (unused by EmSetEvent)
    pcall(function() WriteU8(Offset(emListBuf, 0x06), b[6] or 0) end)  -- compat
    pcall(function() WriteU8(Offset(emListBuf, 0x07), b[7] or 0) end)  -- compat

    -- HP (int16 LE at +0x08)
    pcall(function() WriteU16(Offset(emListBuf, 0x08), hp) end)

    -- Position: convert world coords -> EM_LIST int16 (worldCoord / 10.0)
    local spawnX = (px + (offsetX or 0)) / 10.0
    local spawnY = py / 10.0
    local spawnZ = (pz + (offsetZ or 0)) / 10.0

    pcall(function() WriteU16(Offset(emListBuf, 0x0c), toU16(spawnX)) end)  -- posX
    pcall(function() WriteU16(Offset(emListBuf, 0x0e), toU16(spawnY)) end)  -- posY
    pcall(function() WriteU16(Offset(emListBuf, 0x10), toU16(spawnZ)) end)  -- posZ

    -- Rotation: face toward player (compute angle from spawn pos to player)
    -- RE4 engine: rotY=0 faces +Z axis. Use atan2(dx, dz) for angle from +Z.
    local dx = -(offsetX or 0)
    local dz = -(offsetZ or 0)
    if dx ~= 0 or dz ~= 0 then
        local faceAngle = math.atan(dx, dz)  -- radians from +Z toward player
        -- EM_LIST rotY: EmSetEvent multiplies by pi/16384, so inverse = 16384/pi
        local rotInt = math.floor(faceAngle * 16384.0 / math.pi)
        pcall(function() WriteU16(Offset(emListBuf, 0x12), toU16(rotInt)) end)
    end
    -- Zero rotX and rotZ to prevent upside-down spawns
    pcall(function() WriteU16(Offset(emListBuf, 0x14), 0) end)  -- rotX = 0 (no pitch)
    pcall(function() WriteU16(Offset(emListBuf, 0x16), 0) end)  -- rotZ = 0 (no roll)

    V("spawnSingle: calling EmSetEvent emId=%d subType=%d hp=%d", b[1], b[2], hp)
    -- Call EmSetEvent(EM_LIST*) -> returns cEm* or errEm
    -- Set flag so Randomizer's EmSetEvent hook skips our deliberate spawns
    if SharedAPI then SharedAPI._skipRandomizer = true end
    local result = nil
    local callOk, callErr = pcall(function()
        result = CallNative(sym_EmSetEvent, "pp", emListBuf)
    end)
    if SharedAPI then SharedAPI._skipRandomizer = false end

    if not callOk then
        V("spawnSingle: CallNative crashed: %s", tostring(callErr))
        return false, "CallNative crash: " .. tostring(callErr)
    end
    V("spawnSingle: CallNative returned result=%s", tostring(result))
    if not result or IsNull(result) then
        V("spawnSingle: result is NULL")
        return false, "EmSetEvent returned NULL"
    end

    -- Check against errEm sentinel (spawn failure = pool full or invalid emId)
    local errEm = nil
    pcall(function() errEm = ReadPtr(addr_errEm) end)
    if errEm and not IsNull(errEm) and result == errEm then
        V("spawnSingle: result matches errEm sentinel - pool full/invalid emId")
        return false, "Pool full or invalid emId (errEm)"
    end

    -- Check status byte — EmSetEvent sets EM_LIST[0] = 7 on success
    local statusByte = 0
    pcall(function() statusByte = ReadU8(emListBuf) end)
    V("spawnSingle: statusByte=%d (7=success)", statusByte)
    if statusByte ~= 7 then
        Log(TAG .. ":   Warning: EM_LIST status=" .. statusByte .. " (expected 7)")
    end

    V("spawnSingle: SUCCESS enemy=%s cEm=%s", enemy.name, tostring(result))
    return true, result
end

local function spawnEnemy(enemy, count)
    count = count or state.spawnCount
    V("spawnEnemy: name=%s count=%d diff=%s dist=%d", enemy.name, count, state.difficulty, state.spawnDistance)

    Log(TAG .. ": Spawning " .. enemy.name .. " x" .. count
        .. " | HP=" .. getHP(enemy)
        .. " diff=" .. state.difficulty
        .. " dist=" .. state.spawnDistance)

    local spawned = 0
    local lastErr = nil

    for i = 1, count do
        -- Distribute enemies in a circle around the player
        local angle = (i - 1) * (2 * math.pi / math.max(count, 1))
        if count > 1 then
            angle = angle + math.random() * 0.3  -- slight random jitter
        end
        local dist = state.spawnDistance
        local ox = math.cos(angle) * dist
        local oz = math.sin(angle) * dist
        V("spawnEnemy: #%d/%d angle=%.2f ox=%.1f oz=%.1f", i, count, angle, ox, oz)

        local ok, result = spawnSingle(enemy, ox, oz)
        if ok then
            spawned = spawned + 1
            state.lastSpawnedEm = result
        else
            lastErr = result
            Log(TAG .. ":   #" .. i .. " failed: " .. tostring(result))
        end
    end

    state.totalSpawned = state.totalSpawned + spawned

    if spawned > 0 then
        Log(TAG .. ": Spawned " .. spawned .. "/" .. count .. " " .. enemy.name)
        Notify(TAG, "Spawned " .. spawned .. "x " .. enemy.name)
    else
        LogWarn(TAG .. ": FAILED " .. enemy.name .. ": " .. tostring(lastErr))
        Notify(TAG, "FAILED: " .. tostring(lastErr))
    end

    return spawned
end

local function spawnByName(name, count)
    V("spawnByName: name='%s' count=%s", name, tostring(count))
    local em = ENEMY_BY_NAME[name]
    if not em then
        V("spawnByName: exact match failed, trying fuzzy search")
        -- Fuzzy search
        local lower = name:lower()
        for _, e in ipairs(ALL_ENEMIES) do
            if e.name:lower():find(lower, 1, true) then
                em = e
                break
            end
        end
    end
    if not em then
        V("spawnByName: no match found for '%s'", name)
        LogWarn(TAG .. ": Enemy not found: " .. name)
        return 0
    end
    V("spawnByName: resolved '%s' -> %s", name, em.name)
    return spawnEnemy(em, count)
end

-- ═══════════════════════════════════════════════════════════════════════
-- COMMANDS
-- ═══════════════════════════════════════════════════════════════════════

RegisterCommand("spawn", function(args)
    V("cmd:spawn args='%s'", tostring(args))
    if not args or args == "" then
        Log(TAG .. ": Usage: spawn <name|index> [count]")
        return
    end
    local parts = {}
    for w in args:gmatch("%S+") do parts[#parts + 1] = w end
    local target = parts[1]
    local count = tonumber(parts[#parts])
    if count and #parts > 1 then
        -- Last arg is count, rest is name
        target = table.concat(parts, " ", 1, #parts - 1)
    else
        count = state.spawnCount
        target = table.concat(parts, " ")
    end

    local idx = tonumber(target)
    if idx and idx >= 1 and idx <= #ALL_ENEMIES then
        spawnEnemy(ALL_ENEMIES[idx], count)
    else
        spawnByName(target, count)
    end
end)

RegisterCommand("spawn_difficulty", function(args)
    V("cmd:spawn_difficulty args='%s'", tostring(args))
    local d = (args or "NORMAL"):upper()
    if HP_PRESETS[d] then
        state.difficulty = d
        ModConfig.Save("EnemySpawner", state)
        Log(TAG .. ": Difficulty -> " .. d)
        Notify(TAG, "Difficulty: " .. d)
    else
        Log(TAG .. ": Valid: EASY, NORMAL, HARD (current: " .. state.difficulty .. ")")
    end
end)

RegisterCommand("spawn_count", function(args)
    V("cmd:spawn_count args='%s'", tostring(args))
    local n = tonumber(args) or 1
    state.spawnCount = math.max(1, math.min(5, n))
    ModConfig.Save("EnemySpawner", state)
    Log(TAG .. ": Count -> " .. state.spawnCount)
end)

RegisterCommand("spawn_distance", function(args)
    V("cmd:spawn_distance args='%s'", tostring(args))
    local n = tonumber(args) or 200
    state.spawnDistance = math.max(50, math.min(2000, n))
    ModConfig.Save("EnemySpawner", state)
    Log(TAG .. ": Distance -> " .. state.spawnDistance)
end)

RegisterCommand("spawn_list", function()
    V("cmd:spawn_list")
    Log(TAG .. ": " .. #ALL_ENEMIES .. " enemies in " .. #CATEGORIES .. " categories:")
    for _, cat in ipairs(CATEGORIES) do
        Log("  " .. cat.name .. " (" .. #cat.enemies .. ")")
    end
end)

RegisterCommand("spawner_status", function()
    V("cmd:spawner_status")
    local info = TAG .. ": v8.0"
        .. " | diff=" .. state.difficulty
        .. " | count=" .. state.spawnCount
        .. " | dist=" .. state.spawnDistance
        .. " | spawned=" .. state.totalSpawned
        .. " | enemies=" .. #ALL_ENEMIES
        .. " | native=" .. tostring(nativeReady)

    local px, py, pz = getPlayerPosition()
    if px then
        info = info .. string.format(" | pos=(%.0f,%.0f,%.0f)", px, py, pz)
    else
        info = info .. " | pos=N/A"
    end

    Log(info)
end)

-- ═══════════════════════════════════════════════════════════════════════
-- SHARED API — Expose spawner to other mods
-- ═══════════════════════════════════════════════════════════════════════

V("SharedAPI available=%s", tostring(SharedAPI ~= nil))
if SharedAPI then
    V("Registering SharedAPI.Spawner")
    SharedAPI.Spawner = {
        ALL_ENEMIES    = ALL_ENEMIES,
        CATEGORIES     = CATEGORIES,
        ENEMY_BY_NAME  = ENEMY_BY_NAME,
        spawn          = spawnEnemy,
        spawnByName    = spawnByName,
        spawnSingle    = spawnSingle,
        getPlayerPosition = getPlayerPosition,
        getDifficulty  = function() return state.difficulty end,
        setDifficulty  = function(d) if HP_PRESETS[d] then state.difficulty = d end end,
        getSpawnCount  = function() return state.spawnCount end,
        setSpawnCount  = function(n) state.spawnCount = math.max(1, math.min(10, n)) end,
        getSpawnDistance = function() return state.spawnDistance end,
        setSpawnDistance = function(n) state.spawnDistance = math.max(100, math.min(5000, n)) end,
    }
end

-- ═══════════════════════════════════════════════════════════════════════
-- DEBUG MENU INTEGRATION
-- ═══════════════════════════════════════════════════════════════════════

V("DebugMenu available=%s", tostring(SharedAPI and SharedAPI.DebugMenu ~= nil))
if SharedAPI and SharedAPI.DebugMenu then
    local api = SharedAPI.DebugMenu
    V("Registering DebugMenu sub-menu 'EnemySpawner'")

    api.RegisterSubMenu("EnemySpawner", "Enemy Spawner", function()
        V("DebugMenu: EnemySpawner sub-menu activated")
        api.NavigateTo({ populate = function()
            -- Settings
            api.AddItem("Difficulty: " .. state.difficulty, function()
                V("DebugMenu: cycling difficulty from %s", state.difficulty)
                local modes = {"EASY", "NORMAL", "HARD"}
                local idx = 1
                for i, m in ipairs(modes) do
                    if m == state.difficulty then idx = i; break end
                end
                state.difficulty = modes[(idx % #modes) + 1]
                ModConfig.Save("EnemySpawner", state)
                api.Refresh()
            end)
            api.AddItem("Count: " .. state.spawnCount, function()
                V("DebugMenu: cycling count from %d", state.spawnCount)
                state.spawnCount = (state.spawnCount % 5) + 1
                ModConfig.Save("EnemySpawner", state)
                api.Refresh()
            end)
            api.AddItem("Distance: " .. state.spawnDistance, function()
                V("DebugMenu: cycling distance from %d", state.spawnDistance)
                local dists = {50, 100, 200, 500, 1000, 2000}
                local cur = 1
                for i, d in ipairs(dists) do
                    if d >= state.spawnDistance then cur = i; break end
                end
                state.spawnDistance = dists[(cur % #dists) + 1]
                ModConfig.Save("EnemySpawner", state)
                api.Refresh()
            end)

            api.AddItem("--- QUICK ACTIONS ---", nil)
            api.AddItem(">> Spawn Random <<", function()
                local pick = ALL_ENEMIES[math.random(#ALL_ENEMIES)]
                V("DebugMenu: spawn random picked='%s'", pick.name)
                spawnEnemy(pick, state.spawnCount)
            end)

            -- Category sub-pages
            api.AddItem("--- CATEGORIES (" .. #CATEGORIES .. ") ---", nil)
            for ci, cat in ipairs(CATEGORIES) do
                local catRef = cat
                api.AddItem(catRef.name .. " (" .. #catRef.enemies .. ") >>", function()
                    V("DebugMenu: entering category '%s'", catRef.name)
                    api.NavigateTo({ populate = function()
                        -- Spawn random from this category
                        api.AddItem(">> Spawn Random " .. catRef.name .. " <<", function()
                            local raw = catRef.enemies[math.random(#catRef.enemies)]
                            local entry = ENEMY_BY_NAME[raw[1]]
                            V("DebugMenu: cat random picked='%s' found=%s", raw[1], tostring(entry ~= nil))
                            if entry then
                                spawnEnemy(entry, state.spawnCount)
                            end
                        end)
                        api.AddItem("---", nil)
                        -- Individual enemies
                        for ei, em in ipairs(catRef.enemies) do
                            local emName = em[1]
                            api.AddItem(emName, function()
                                V("DebugMenu: spawn individual '%s'", emName)
                                local entry = ENEMY_BY_NAME[emName]
                                if entry then
                                    spawnEnemy(entry, state.spawnCount)
                                end
                            end)
                        end
                    end })
                end)
            end
        end })
    end)
end

-- ═══════════════════════════════════════════════════════════════════════
-- INITIALIZE
-- ═══════════════════════════════════════════════════════════════════════

-- Try to init native engine on load (non-fatal — will retry on first spawn)
V("Init: calling initNative (non-fatal)")
pcall(initNative)
V("Init: nativeReady=%s", tostring(nativeReady))

Log(TAG .. ": v8.0 loaded — " .. #ALL_ENEMIES .. " enemies, "
    .. #CATEGORIES .. " categories"
    .. " | EmSetEvent direct spawning"
    .. " | diff=" .. state.difficulty
    .. " dist=" .. state.spawnDistance
    .. " | native=" .. tostring(nativeReady))
