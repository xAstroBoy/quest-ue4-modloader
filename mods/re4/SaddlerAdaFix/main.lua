-- mods/SaddlerAdaFix/main.lua v2.1
-- ═══════════════════════════════════════════════════════════════════════
-- UE4SS-style SaddlerAda Fix — uses StaticFindObject/StaticLoadClass
-- directly from UE4SS API instead of manual CallNative.
--
-- v2.0 — Full UE4SS API:
--   StaticFindObject(path) for UObject lookup (no CallNative wrapper)
--   StaticLoadClass(base, path) for asset loading
--   Native hooks on GetEtcModelClass for mesh table override
-- ═══════════════════════════════════════════════════════════════════════
local TAG = "SaddlerAdaFix"
local VERBOSE = true
local function V(...) if VERBOSE then Log(TAG .. " [V] " .. string.format(...)) end end

local EM3F_INDEX = 0x3F

-- ═══════════════════════════════════════════════════════════════════════
-- UE4SS ASSET DISCOVERY — Try to find em3f mesh table via reflection
-- ═══════════════════════════════════════════════════════════════════════

local MESH_TABLE_PATHS = {
    "/Game/Blueprints/Characters/Bosses/em3f_meshTable.em3f_meshTable_C",
    "/Game/Blueprints/Characters/Bosses/Em3f_meshTable.Em3f_meshTable_C",
    "/Game/Blueprints/Characters/Bosses/em3f_MeshTable.em3f_MeshTable_C",
    "/Game/Blueprints/Characters/Bosses/Em3f_MeshTable.Em3f_MeshTable_C",
    "/Game/Blueprints/Characters/Enemies/Em3f/em3f_meshTable.em3f_meshTable_C",
    "/Game/Blueprints/Characters/Enemies/em3f_meshTable.em3f_meshTable_C",
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
    Log(TAG .. ": em3f mesh table not pre-cached — will use StaticLoadClass in hook (deferred)")
end

-- ═══════════════════════════════════════════════════════════════════════
-- NATIVE HOOKS — GetEtcModelClass override for em3f
-- ═══════════════════════════════════════════════════════════════════════

local sym_AVR4Model_SC = Resolve("AVR4Model_StaticClass", 0x068EF580)
local sym_StaticFindObj = Resolve("StaticFindObject")
local sym_StaticLoadCls = Resolve("StaticLoadClass")

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

Log(TAG .. ": v2.1 loaded — UE4SS StaticFindObject/StaticLoadClass + native GetEtcModelClass")
