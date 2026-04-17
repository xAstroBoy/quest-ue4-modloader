-- mods/SaddlerAdaFix/main.lua v3.0
-- ═══════════════════════════════════════════════════════════════════════
-- SaddlerAda Fix — fixes em3f (Saddler-Ada) mesh table so the entity
-- is visible when spawned.
--
-- v3.0 — Fixed mesh paths from EnemyFixes_P.pak analysis.
--   The em3f_meshTable asset lives at:
--     /Game/Blueprints/Characters/Bosses/em3f_meshTable
--   This asset is inside EnemyFixes_P.pak. If that PAK is not mounted
--   on the device, em3f spawns will be invisible (no mesh data).
--
--   Also tries EM3F_meshData and EM3F_Poseable_BP as fallbacks.
--
-- REQUIREMENT: EnemyFixes_P.pak must be installed on the device at:
--   /sdcard/Android/data/com.Armature.VR4/files/UE4Game/VR4/Content/Paks/
-- ═══════════════════════════════════════════════════════════════════════
local TAG = "SaddlerAdaFix"
local VERBOSE = false
local function V(...) if VERBOSE then Log(TAG .. " [V] " .. string.format(...)) end end

local EM3F_INDEX = 0x3F

-- ═══════════════════════════════════════════════════════════════════════
-- UE4SS ASSET DISCOVERY — Try to find em3f mesh table via reflection
-- ═══════════════════════════════════════════════════════════════════════

-- ═══════════════════════════════════════════════════════════════════════
-- ASSET PATHS — Verified from EnemyFixes_P.pak contents
-- PAK mount point: ../../../VR4/Content/
-- Assets found:
--   Blueprints/Characters/Bosses/em3f_meshTable.uasset
--   Blueprints/Characters/Bosses/EM3F_meshData.uasset
--   Blueprints/Characters/Bosses/EM3F_Poseable_BP.uasset
-- NOTE: These assets ONLY exist if EnemyFixes_P.pak is mounted!
-- ═══════════════════════════════════════════════════════════════════════
local MESH_TABLE_PATHS = {
    -- Primary: em3f_meshTable (the actual mesh table class from PAK)
    "/Game/Blueprints/Characters/Bosses/em3f_meshTable.em3f_meshTable_C",
    "/Game/Blueprints/Characters/Bosses/em3f_meshTable",
    -- Case variations
    "/Game/Blueprints/Characters/Bosses/Em3f_meshTable.Em3f_meshTable_C",
    "/Game/Blueprints/Characters/Bosses/EM3F_meshTable.EM3F_meshTable_C",
    -- MeshData asset (may serve as mesh table)
    "/Game/Blueprints/Characters/Bosses/EM3F_meshData.EM3F_meshData_C",
    "/Game/Blueprints/Characters/Bosses/EM3F_meshData",
    -- Poseable BP fallback
    "/Game/Blueprints/Characters/Bosses/EM3F_Poseable_BP.EM3F_Poseable_BP_C",
    "/Game/Blueprints/Characters/Bosses/EM3F_Poseable_BP",
}

local cachedClass = nil

-- Pre-search using UE4SS StaticFindObject (safe — no asset loading)
for _, path in ipairs(MESH_TABLE_PATHS) do
    V("Trying StaticFindObject: %s", path)
    local ok, found = pcall(StaticFindObject, path)
    if ok and found and found:IsValid() then
        cachedClass = found:GetAddress()
        Log(TAG .. ": Pre-cached em3f mesh table via StaticFindObject: " .. path)
        break
    end
end

-- NOTE: StaticLoadClass is NOT safe at boot time — it can crash the engine.
-- We defer the StaticLoadClass fallback to the native hook where the engine
-- is guaranteed to be in a stable state (asset system initialized).
if cachedClass then
    Log(TAG .. ": em3f mesh table found @ " .. ToHex(cachedClass))
else
    Log(TAG .. ": ⚠️ em3f mesh table NOT FOUND at boot!")
    Log(TAG .. ": Ensure EnemyFixes_P.pak is installed at:")
    Log(TAG .. ":   /sdcard/Android/data/com.Armature.VR4/files/UE4Game/VR4/Content/Paks/EnemyFixes_P.pak")
    Log(TAG .. ": Will retry via StaticLoadClass in GetEtcModelClass hook (deferred)")
end

-- ═══════════════════════════════════════════════════════════════════════
-- NATIVE HOOKS — GetEtcModelClass override for em3f
-- ═══════════════════════════════════════════════════════════════════════

local sym_AVR4Model_SC = Resolve("AVR4Model_StaticClass", 0x068EF580)
-- Mangled names + fallback offsets from RE4 VR symbol_dump.txt
local sym_StaticFindObj = Resolve("_Z16StaticFindObjectP6UClassP7UObjectPKDsb", 0x06A1AE74)
local sym_StaticLoadCls = Resolve("_Z15StaticLoadClassP6UClassP7UObjectPKDsS4_jP11UPackageMap", 0x06A1DE74)

pcall(function()
RegisterNativeHook("GetEtcModelClass",
    function(self_ptr, index) V("GetEtcModelClass pre-hook index=0x%X", index); return self_ptr, index end,
    function(retval, self_ptr, index)
        V("GetEtcModelClass post-hook index=0x%X retval=%s", index, tostring(retval))
        if index ~= EM3F_INDEX then return retval end
        if retval ~= 0 then
            if not cachedClass then cachedClass = retval end
            return retval
        end
        -- Return cached class from UE4SS pre-search
        if cachedClass then return cachedClass end

        -- Deferred StaticLoadClass (safe at hook time — engine is initialized)
        V("em3f cache miss, trying deferred StaticLoadClass")
        for _, path in ipairs(MESH_TABLE_PATHS) do
            local ok2, loaded = pcall(StaticLoadClass, "Actor", path)
            if ok2 and loaded then
                cachedClass = loaded:GetAddress()
                Log(TAG .. ": Deferred StaticLoadClass found em3f: " .. path)
                return cachedClass
            end
        end

        -- Last resort: manual C++ lookup
        V("em3f cache miss, attempting manual C++ lookup")
        local baseClass = nil
        if sym_AVR4Model_SC then
            baseClass = CallNative(sym_AVR4Model_SC, "p")
        end
        if baseClass == 0 then baseClass = nil end

        for _, path in ipairs(MESH_TABLE_PATHS) do
            if sym_StaticFindObj then
                local found = CallNative(sym_StaticFindObj, "ppps", 0, 0, path)
                if found ~= 0 then cachedClass = found; return found end
            end
            if sym_StaticLoadCls and baseClass then
                local found = CallNative(sym_StaticLoadCls, "ppps", baseClass, 0, path)
                if found ~= 0 then cachedClass = found; return found end
            end
        end
        return retval
    end)
end)

-- Monitor em3f container init
pcall(function()
    RegisterNativeHook("Et3f_init",
        function(self_ptr) V("Et3f_init pre-hook self=%s", tostring(self_ptr)); Log(TAG .. ": em3f container init"); return self_ptr end, nil)
end)

pcall(function()
    RegisterNativeHook("VR4ModelInit",
        function(self_ptr, uclass)
            V("VR4ModelInit pre-hook self=%s uclass=%s", tostring(self_ptr), tostring(uclass))
            if uclass == 0 then LogWarn(TAG .. ": VR4ModelInit null UClass") end
            return self_ptr, uclass
        end, nil)
end)

Log(TAG .. ": v3.0 loaded — UE4SS StaticFindObject/StaticLoadClass + native GetEtcModelClass")
