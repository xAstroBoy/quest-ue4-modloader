// modloader/src/lua/lua_cast.cpp
// ═══════════════════════════════════════════════════════════════════════════
// Cast Layer — Typed access to UObjects and hook parameters
//
// 1. obj:Cast("ClassName") — Returns the same UObject but forces property
//    resolution against the specified class. Validates IsA first.
//
// 2. CastParms(parms_lightuserdata, "Class:Function") — Returns a typed
//    params table with named get/set access to all UFunction parameters.
//    This is the killer feature for hook callbacks — instead of:
//      local val = ReadU32(parms + 0x08)  -- WRONG per copilot-instructions
//    You do:
//      local p = CastParms(parms, "BP_Player_C:TakeDamage")
//      local damage = p.Damage           -- reads float via reflection
//      p.Damage = 0                      -- blocks all damage
//
// 3. CastStruct(address, "StructName") — Returns a LuaUStruct for any
//    in-memory struct at a known address.
//
// All reads/writes go through the reflection system. No raw memory offsets.
// ═══════════════════════════════════════════════════════════════════════════

#include "modloader/lua_cast.h"
#include "modloader/lua_uobject.h"
#include "modloader/lua_ustruct.h"
#include "modloader/class_rebuilder.h"
#include "modloader/reflection_walker.h"
#include "modloader/process_event_hook.h"
#include "modloader/symbols.h"
#include "modloader/logger.h"
#include "modloader/types.h"

#include <sol/sol.hpp>
#include <string>
#include <cstring>

// Forward-declare read_property_value and write_property_value from lua_uobject
// These are in an anonymous namespace so we need to call through the public API
// Instead, we'll use the LuaUObject Get/Set which go through class_rebuilder

namespace lua_cast
{

    // ═══════════════════════════════════════════════════════════════════════
    // LuaCastUObject: A UObject wrapped with a forced class for property resolution
    // ═══════════════════════════════════════════════════════════════════════

    struct LuaCastUObject
    {
        ue::UObject *ptr = nullptr;
        std::string cast_class_name; // forced class for property resolution
    };

    // ═══════════════════════════════════════════════════════════════════════
    // LuaParmsAccessor: Named access to UFunction parameters in-memory
    // ═══════════════════════════════════════════════════════════════════════

    struct LuaParmsAccessor
    {
        void *parms = nullptr;                               // raw parms buffer from ProcessEvent
        std::string func_path;                               // "ClassName:FuncName" for reflection
        std::shared_ptr<rebuilder::RebuiltClass> rc;         // cached class
        std::shared_ptr<rebuilder::RebuiltFunction> rf_data; // cached function info
    };

    // Read a single param from the parms buffer using reflection
    static sol::object read_param(sol::state_view lua, void *parms,
                                  const reflection::PropertyInfo &pi)
    {
        if (!parms)
            return sol::nil;
        uint8_t *base = static_cast<uint8_t *>(parms) + pi.offset;

        switch (pi.type)
        {
        case reflection::PropType::BoolProperty:
        {
            // Bool properties can be bitfield-packed
            if (pi.element_size == 0)
            {
                // Native bool (1 byte)
                return sol::make_object(lua, *base != 0);
            }
            // Use field mask for bitfield bools
            uint8_t mask = pi.bool_field_mask;
            if (mask == 0)
                mask = 0xFF;
            return sol::make_object(lua, (*base & mask) != 0);
        }

        case reflection::PropType::Int8Property:
            return sol::make_object(lua, static_cast<int>(*reinterpret_cast<int8_t *>(base)));
        case reflection::PropType::Int16Property:
            return sol::make_object(lua, static_cast<int>(*reinterpret_cast<int16_t *>(base)));
        case reflection::PropType::IntProperty:
            return sol::make_object(lua, *reinterpret_cast<int32_t *>(base));
        case reflection::PropType::Int64Property:
            return sol::make_object(lua, *reinterpret_cast<int64_t *>(base));
        case reflection::PropType::ByteProperty:
            return sol::make_object(lua, static_cast<int>(*base));
        case reflection::PropType::UInt16Property:
            return sol::make_object(lua, static_cast<int>(*reinterpret_cast<uint16_t *>(base)));
        case reflection::PropType::UInt32Property:
            return sol::make_object(lua, *reinterpret_cast<uint32_t *>(base));
        case reflection::PropType::UInt64Property:
            return sol::make_object(lua, *reinterpret_cast<uint64_t *>(base));
        case reflection::PropType::FloatProperty:
            return sol::make_object(lua, *reinterpret_cast<float *>(base));
        case reflection::PropType::DoubleProperty:
            return sol::make_object(lua, *reinterpret_cast<double *>(base));

        case reflection::PropType::NameProperty:
        {
            int32_t name_idx = *reinterpret_cast<int32_t *>(base);
            return sol::make_object(lua, reflection::fname_to_string(name_idx));
        }

        case reflection::PropType::StrProperty:
        {
            // FString: { char16_t* Data, int32 Num, int32 Max }
            struct FStr
            {
                char16_t *data;
                int32_t num, max;
            };
            auto *fs = reinterpret_cast<FStr *>(base);
            if (!fs->data || fs->num <= 0 || !ue::is_mapped_ptr(fs->data))
                return sol::make_object(lua, std::string(""));
            std::string result;
            int count = fs->num - 1; // exclude null terminator
            for (int i = 0; i < count; i++)
            {
                char16_t c = fs->data[i];
                if (c < 0x80)
                    result += static_cast<char>(c);
                else if (c < 0x800)
                {
                    result += static_cast<char>(0xC0 | (c >> 6));
                    result += static_cast<char>(0x80 | (c & 0x3F));
                }
                else
                {
                    result += static_cast<char>(0xE0 | (c >> 12));
                    result += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                    result += static_cast<char>(0x80 | (c & 0x3F));
                }
            }
            return sol::make_object(lua, result);
        }

        case reflection::PropType::ObjectProperty:
        case reflection::PropType::WeakObjectProperty:
        case reflection::PropType::SoftObjectProperty:
        case reflection::PropType::LazyObjectProperty:
        case reflection::PropType::InterfaceProperty:
        {
            ue::UObject *obj = *reinterpret_cast<ue::UObject **>(base);
            if (!obj || !ue::is_valid_ptr(obj))
                return sol::nil;
            return lua_uobject::wrap_or_nil(lua, obj);
        }

        case reflection::PropType::ClassProperty:
        {
            ue::UClass *cls = *reinterpret_cast<ue::UClass **>(base);
            if (!cls || !ue::is_valid_ptr(reinterpret_cast<ue::UObject *>(cls)))
                return sol::nil;
            lua_uobject::LuaUObject w;
            w.ptr = reinterpret_cast<ue::UObject *>(cls);
            return sol::make_object(lua, w);
        }

        case reflection::PropType::EnumProperty:
        {
            if (pi.element_size == 1)
                return sol::make_object(lua, static_cast<int>(*base));
            if (pi.element_size == 2)
                return sol::make_object(lua, static_cast<int>(*reinterpret_cast<int16_t *>(base)));
            if (pi.element_size == 4)
                return sol::make_object(lua, *reinterpret_cast<int32_t *>(base));
            if (pi.element_size == 8)
                return sol::make_object(lua, *reinterpret_cast<int64_t *>(base));
            return sol::make_object(lua, static_cast<int>(*base));
        }

        case reflection::PropType::StructProperty:
        {
            // Return a LuaUStruct pointing into the parms buffer (non-owning)
            if (pi.raw)
            {
                // Get the inner UStruct from the StructProperty
                ue::UStruct *inner_struct = ue::read_field<ue::UStruct *>(
                    pi.raw, ue::fprop::STRUCT_INNER_STRUCT_OFF());
                if (inner_struct && ue::is_valid_ptr(reinterpret_cast<ue::UObject *>(inner_struct)))
                {
                    auto wrapped = lua_ustruct::wrap(base, inner_struct, pi.element_size);
                    return sol::make_object(lua, wrapped);
                }
            }
            // Fallback: return raw address
            return sol::make_object(lua, sol::lightuserdata_value(base));
        }

        default:
            // For types we don't explicitly handle, return the raw address
            return sol::make_object(lua, sol::lightuserdata_value(base));
        }
    }

    // Write a value to a param slot in the parms buffer
    static bool write_param(void *parms, const reflection::PropertyInfo &pi, sol::object value)
    {
        if (!parms)
            return false;
        uint8_t *base = static_cast<uint8_t *>(parms) + pi.offset;

        switch (pi.type)
        {
        case reflection::PropType::BoolProperty:
        {
            bool val = value.as<bool>();
            uint8_t mask = pi.bool_field_mask;
            if (mask == 0 || mask == 0xFF)
            {
                *base = val ? 1 : 0;
            }
            else
            {
                if (val)
                    *base |= mask;
                else
                    *base &= ~mask;
            }
            return true;
        }

        case reflection::PropType::Int8Property:
            *reinterpret_cast<int8_t *>(base) = static_cast<int8_t>(value.as<int>());
            return true;
        case reflection::PropType::Int16Property:
            *reinterpret_cast<int16_t *>(base) = static_cast<int16_t>(value.as<int>());
            return true;
        case reflection::PropType::IntProperty:
            *reinterpret_cast<int32_t *>(base) = value.as<int32_t>();
            return true;
        case reflection::PropType::Int64Property:
            *reinterpret_cast<int64_t *>(base) = value.as<int64_t>();
            return true;
        case reflection::PropType::ByteProperty:
            *base = static_cast<uint8_t>(value.as<int>());
            return true;
        case reflection::PropType::UInt16Property:
            *reinterpret_cast<uint16_t *>(base) = static_cast<uint16_t>(value.as<int>());
            return true;
        case reflection::PropType::UInt32Property:
            *reinterpret_cast<uint32_t *>(base) = value.as<uint32_t>();
            return true;
        case reflection::PropType::UInt64Property:
            *reinterpret_cast<uint64_t *>(base) = value.as<uint64_t>();
            return true;
        case reflection::PropType::FloatProperty:
            *reinterpret_cast<float *>(base) = value.as<float>();
            return true;
        case reflection::PropType::DoubleProperty:
            *reinterpret_cast<double *>(base) = value.as<double>();
            return true;

        case reflection::PropType::ObjectProperty:
        case reflection::PropType::WeakObjectProperty:
        {
            if (value.is<lua_uobject::LuaUObject>())
            {
                auto &w = value.as<lua_uobject::LuaUObject &>();
                *reinterpret_cast<ue::UObject **>(base) = w.ptr;
                return true;
            }
            if (value == sol::nil)
            {
                *reinterpret_cast<ue::UObject **>(base) = nullptr;
                return true;
            }
            return false;
        }

        case reflection::PropType::EnumProperty:
        {
            if (pi.element_size == 1)
            {
                *base = static_cast<uint8_t>(value.as<int>());
                return true;
            }
            if (pi.element_size == 2)
            {
                *reinterpret_cast<int16_t *>(base) = static_cast<int16_t>(value.as<int>());
                return true;
            }
            if (pi.element_size == 4)
            {
                *reinterpret_cast<int32_t *>(base) = value.as<int32_t>();
                return true;
            }
            if (pi.element_size == 8)
            {
                *reinterpret_cast<int64_t *>(base) = value.as<int64_t>();
                return true;
            }
            return false;
        }

        default:
            logger::log_warn("CAST", "Cannot write param '%s' of type %d",
                             pi.name.c_str(), static_cast<int>(pi.type));
            return false;
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Register all Cast Lua APIs
    // ═══════════════════════════════════════════════════════════════════════

    void register_all(sol::state &lua)
    {
        // ─── CastParms: typed access to ProcessEvent parameters ─────────
        // Usage in hook callback:
        //   RegisterPreHook("PlayerCharacter_C:TakeDamage", function(self, func, parms)
        //       local p = CastParms(parms, "PlayerCharacter_C:TakeDamage")
        //       Log("Damage: " .. p.DamageAmount)
        //       p.DamageAmount = 0  -- nullify damage
        //       return false        -- don't block, let modified parms through
        //   end)
        lua.new_usertype<LuaParmsAccessor>("LuaParmsAccessor", sol::no_constructor,

                                           // Named param access via __index
                                           sol::meta_function::index, [](sol::this_state ts, LuaParmsAccessor &self, const std::string &key) -> sol::object
                                           {
            sol::state_view lua_view(ts);
            if (!self.parms || !self.rc) return sol::nil;

            auto* rf = self.rc->find_function(self.func_path.substr(self.func_path.find(':') + 1));
            if (!rf) return sol::nil;

            // Search params for matching name
            for (const auto& pi : rf->params) {
                if (pi.name == key) {
                    return read_param(lua_view, self.parms, pi);
                }
            }
            // Also check return value
            if (rf->return_prop && rf->return_prop->name == key) {
                return read_param(lua_view, self.parms, *rf->return_prop);
            }
            return sol::nil; },

                                           // Named param write via __newindex
                                           sol::meta_function::new_index, [](LuaParmsAccessor &self, const std::string &key, sol::object value)
                                           {
            if (!self.parms || !self.rc) return;

            auto* rf = self.rc->find_function(self.func_path.substr(self.func_path.find(':') + 1));
            if (!rf) return;

            for (const auto& pi : rf->params) {
                if (pi.name == key) {
                    write_param(self.parms, pi, value);
                    return;
                }
            }
            // Also allow writing return value
            if (rf->return_prop && rf->return_prop->name == key) {
                write_param(self.parms, *rf->return_prop, value);
            } },

                                           // Get all param names
                                           "GetParamNames", [](sol::this_state ts, const LuaParmsAccessor &self) -> sol::table
                                           {
            sol::state_view lua_view(ts);
            sol::table t = lua_view.create_table();
            if (!self.rc) return t;

            auto func_name = self.func_path.substr(self.func_path.find(':') + 1);
            auto* rf = self.rc->find_function(func_name);
            if (!rf) return t;

            int idx = 1;
            for (const auto& pi : rf->params) {
                sol::table param = lua_view.create_table();
                param["name"] = pi.name;
                param["type"] = static_cast<int>(pi.type);
                param["offset"] = pi.offset;
                param["size"] = pi.element_size;
                param["is_return"] = (pi.flags & ue::CPF_ReturnParm) != 0;
                param["is_out"] = (pi.flags & ue::CPF_OutParm) != 0;
                param["is_const"] = (pi.flags & ue::CPF_ConstParm) != 0;
                t[idx++] = param;
            }
            return t; },

                                           // Get return value specifically
                                           "GetReturnValue", [](sol::this_state ts, const LuaParmsAccessor &self) -> sol::object
                                           {
            sol::state_view lua_view(ts);
            if (!self.parms || !self.rc) return sol::nil;

            auto func_name = self.func_path.substr(self.func_path.find(':') + 1);
            auto* rf = self.rc->find_function(func_name);
            if (!rf || !rf->return_prop) return sol::nil;

            return read_param(lua_view, self.parms, *rf->return_prop); },

                                           // Set return value
                                           "SetReturnValue", [](LuaParmsAccessor &self, sol::object value) -> bool
                                           {
            if (!self.parms || !self.rc) return false;

            auto func_name = self.func_path.substr(self.func_path.find(':') + 1);
            auto* rf = self.rc->find_function(func_name);
            if (!rf || !rf->return_prop) return false;

            return write_param(self.parms, *rf->return_prop, value); },

                                           // Get all values as a table
                                           "ToTable", [](sol::this_state ts, const LuaParmsAccessor &self) -> sol::table
                                           {
            sol::state_view lua_view(ts);
            sol::table t = lua_view.create_table();
            if (!self.parms || !self.rc) return t;

            auto func_name = self.func_path.substr(self.func_path.find(':') + 1);
            auto* rf = self.rc->find_function(func_name);
            if (!rf) return t;

            for (const auto& pi : rf->params) {
                if (pi.flags & ue::CPF_ReturnParm) continue; // separate
                auto val = read_param(lua_view, self.parms, pi);
                if (val != sol::nil) {
                    t[pi.name] = val;
                }
            }
            return t; },

                                           // Raw parms address
                                           "GetAddress", [](const LuaParmsAccessor &self) -> uintptr_t
                                           { return reinterpret_cast<uintptr_t>(self.parms); },

                                           sol::meta_function::to_string, [](const LuaParmsAccessor &self) -> std::string
                                           {
            char buf[128];
            snprintf(buf, sizeof(buf), "ParmsAccessor(%s @ %p)", self.func_path.c_str(), self.parms);
            return std::string(buf); });

        // ─── CastParms global function ──────────────────────────────────
        lua.set_function("CastParms", [](sol::this_state ts, sol::object parms_obj, const std::string &func_path) -> sol::object
                         {
            sol::state_view lua_view(ts);

            void* parms = nullptr;
            if (parms_obj.is<sol::lightuserdata>()) {
                parms = parms_obj.as<void*>();
            } else if (parms_obj.is<uintptr_t>()) {
                parms = reinterpret_cast<void*>(parms_obj.as<uintptr_t>());
            }
            if (!parms) {
                logger::log_warn("CAST", "CastParms: null parms pointer");
                return sol::nil;
            }

            // Parse "ClassName:FuncName"
            auto colon = func_path.find(':');
            if (colon == std::string::npos) {
                logger::log_warn("CAST", "CastParms: expected 'ClassName:FuncName' format, got '%s'",
                                 func_path.c_str());
                return sol::nil;
            }

            std::string class_name = func_path.substr(0, colon);
            std::string func_name = func_path.substr(colon + 1);

            auto* rc = rebuilder::rebuild(class_name);
            if (!rc) {
                logger::log_warn("CAST", "CastParms: class '%s' not found", class_name.c_str());
                return sol::nil;
            }

            auto* rf = rc->find_function(func_name);
            if (!rf) {
                logger::log_warn("CAST", "CastParms: function '%s' not found on '%s'",
                                 func_name.c_str(), class_name.c_str());
                return sol::nil;
            }

            LuaParmsAccessor accessor;
            accessor.parms = parms;
            accessor.func_path = func_path;
            accessor.rc = std::shared_ptr<rebuilder::RebuiltClass>(rc, [](rebuilder::RebuiltClass*) {});
            // Note: shared_ptr with no-op deleter since rebuilder owns the memory

            return sol::make_object(lua_view, accessor); });

        // ─── Cast global function — validates IsA then wraps ────────────
        // Usage: local pc = Cast(someObj, "PlayerController")
        // Returns the same UObject with full property access against the cast class.
        // Returns nil if the object is not an instance of the target class.
        lua.set_function("Cast", [](sol::this_state ts, sol::object obj_arg, const std::string &class_name) -> sol::object
                         {
            sol::state_view lua_view(ts);

            ue::UObject* obj = nullptr;
            if (obj_arg.is<lua_uobject::LuaUObject>()) {
                obj = obj_arg.as<lua_uobject::LuaUObject&>().ptr;
            } else if (obj_arg.is<sol::lightuserdata>()) {
                obj = reinterpret_cast<ue::UObject*>(obj_arg.as<void*>());
            }
            if (!obj || !ue::is_valid_ptr(obj)) return sol::nil;

            // Validate IsA
            ue::UClass* target_cls = reflection::find_class_ptr(class_name);
            if (!target_cls) {
                // Try with _C suffix for Blueprint classes
                target_cls = reflection::find_class_ptr(class_name + "_C");
            }
            if (target_cls) {
                // Walk class hierarchy
                ue::UClass* cls = ue::uobj_get_class(obj);
                bool found = false;
                while (cls) {
                    if (cls == target_cls) { found = true; break; }
                    cls = reinterpret_cast<ue::UClass*>(
                        ue::ustruct_get_super(reinterpret_cast<ue::UStruct*>(cls)));
                }
                if (!found) {
                    logger::log_warn("CAST", "Cast failed: object is not a '%s'", class_name.c_str());
                    return sol::nil;
                }
            }
            // If class not found in reflection, allow the cast anyway (user knows best)
            // This handles cases where the class hasn't been rebuilt yet

            // Return a standard LuaUObject — the user can access any property
            // because our reflection system resolves the full class hierarchy
            return lua_uobject::wrap_or_nil(lua_view, obj); });

        // ─── UnsafeCast — no IsA validation ─────────────────────────────
        // For when you KNOW the type but the class hierarchy isn't available
        lua.set_function("UnsafeCast", [](sol::this_state ts, sol::object obj_arg) -> sol::object
                         {
            sol::state_view lua_view(ts);

            ue::UObject* obj = nullptr;
            if (obj_arg.is<lua_uobject::LuaUObject>()) {
                obj = obj_arg.as<lua_uobject::LuaUObject&>().ptr;
            } else if (obj_arg.is<sol::lightuserdata>()) {
                obj = reinterpret_cast<ue::UObject*>(obj_arg.as<void*>());
            }
            if (!obj || !ue::is_valid_ptr(obj)) return sol::nil;

            return lua_uobject::wrap_or_nil(lua_view, obj); });

        // ─── IsA global function ────────────────────────────────────────
        lua.set_function("IsA", [](sol::object obj_arg, const std::string &class_name) -> bool
                         {
            ue::UObject* obj = nullptr;
            if (obj_arg.is<lua_uobject::LuaUObject>()) {
                obj = obj_arg.as<lua_uobject::LuaUObject&>().ptr;
            }
            if (!obj || !ue::is_valid_ptr(obj)) return false;

            ue::UClass* target_cls = reflection::find_class_ptr(class_name);
            if (!target_cls) target_cls = reflection::find_class_ptr(class_name + "_C");
            if (!target_cls) return false;

            ue::UClass* cls = ue::uobj_get_class(obj);
            while (cls) {
                if (cls == target_cls) return true;
                cls = reinterpret_cast<ue::UClass*>(
                    ue::ustruct_get_super(reinterpret_cast<ue::UStruct*>(cls)));
            }
            return false; });

        // ─── GetClassHierarchy — returns {"Actor", "Object"} for an AActor ──
        lua.set_function("GetClassHierarchy", [](sol::this_state ts, sol::object obj_arg) -> sol::table
                         {
            sol::state_view lua_view(ts);
            sol::table t = lua_view.create_table();

            ue::UObject* obj = nullptr;
            if (obj_arg.is<lua_uobject::LuaUObject>()) {
                obj = obj_arg.as<lua_uobject::LuaUObject&>().ptr;
            }
            if (!obj || !ue::is_valid_ptr(obj)) return t;

            ue::UClass* cls = ue::uobj_get_class(obj);
            int idx = 1;
            while (cls) {
                std::string name = reflection::get_short_name(reinterpret_cast<const ue::UObject*>(cls));
                t[idx++] = name;
                cls = reinterpret_cast<ue::UClass*>(
                    ue::ustruct_get_super(reinterpret_cast<ue::UStruct*>(cls)));
            }
            return t; });

        // ─── GetAllProperties — dump all properties of a class ──────────
        lua.set_function("GetAllProperties", [](sol::this_state ts, sol::object obj_arg) -> sol::table
                         {
            sol::state_view lua_view(ts);
            sol::table t = lua_view.create_table();

            ue::UObject* obj = nullptr;
            std::string class_name;

            if (obj_arg.is<lua_uobject::LuaUObject>()) {
                obj = obj_arg.as<lua_uobject::LuaUObject&>().ptr;
                if (obj) {
                    ue::UClass* cls = ue::uobj_get_class(obj);
                    class_name = reflection::get_short_name(reinterpret_cast<const ue::UObject*>(cls));
                }
            } else if (obj_arg.is<std::string>()) {
                class_name = obj_arg.as<std::string>();
            }

            if (class_name.empty()) return t;

            auto* rc = rebuilder::rebuild(class_name);
            if (!rc) return t;

            auto& props = rc->all_properties;
            int idx = 1;
            for (const auto& rp : props) {
                sol::table prop = lua_view.create_table();
                prop["name"] = rp.name;
                prop["type"] = static_cast<int>(rp.type);
                prop["type_name"] = rp.type_name;
                prop["offset"] = rp.offset;
                prop["size"] = rp.element_size;
                t[idx++] = prop;
            }
            return t; });

        // ─── GetAllFunctions — dump all functions of a class ────────────
        lua.set_function("GetAllFunctions", [](sol::this_state ts, sol::object obj_arg) -> sol::table
                         {
            sol::state_view lua_view(ts);
            sol::table t = lua_view.create_table();

            std::string class_name;
            if (obj_arg.is<lua_uobject::LuaUObject>()) {
                auto* obj = obj_arg.as<lua_uobject::LuaUObject&>().ptr;
                if (obj) {
                    ue::UClass* cls = ue::uobj_get_class(obj);
                    class_name = reflection::get_short_name(reinterpret_cast<const ue::UObject*>(cls));
                }
            } else if (obj_arg.is<std::string>()) {
                class_name = obj_arg.as<std::string>();
            }

            if (class_name.empty()) return t;

            auto* rc = rebuilder::rebuild(class_name);
            if (!rc) return t;

            auto& funcs = rc->all_functions;
            int idx = 1;
            for (const auto& rf : funcs) {
                sol::table func = lua_view.create_table();
                func["name"] = rf.name;
                func["flags"] = rf.flags;
                func["num_params"] = static_cast<int>(rf.params.size());

                // List parameters
                sol::table params = lua_view.create_table();
                int pidx = 1;
                for (const auto& pi : rf.params) {
                    sol::table p = lua_view.create_table();
                    p["name"] = pi.name;
                    p["type"] = static_cast<int>(pi.type);
                    p["type_name"] = pi.inner_type_name;
                    p["offset"] = pi.offset;
                    p["is_return"] = (pi.flags & ue::CPF_ReturnParm) != 0;
                    p["is_out"] = (pi.flags & ue::CPF_OutParm) != 0;
                    params[pidx++] = p;
                }
                func["params"] = params;
                t[idx++] = func;
            }
            return t; });

        // ─── SpawnActor — Create an actor in the world ──────────────────
        // Usage: local actor = SpawnActor("BP_Enemy_C", {X=100, Y=200, Z=300})
        lua.set_function("SpawnActor", [](sol::this_state ts, const std::string &class_name, sol::optional<sol::table> location, sol::optional<sol::table> rotation) -> sol::object
                         {
            sol::state_view lua_view(ts);

            // Find the class
            ue::UClass* actor_class = reflection::find_class_ptr(class_name);
            if (!actor_class) actor_class = reflection::find_class_ptr(class_name + "_C");
            if (!actor_class) {
                logger::log_warn("SPAWN", "SpawnActor: class '%s' not found", class_name.c_str());
                return sol::nil;
            }

            // Get world
            ue::UObject* world = nullptr;
            if (symbols::GWorld) {
                world = *reinterpret_cast<ue::UObject**>(symbols::GWorld);
            }
            if (!world || !ue::is_valid_ptr(world)) {
                logger::log_warn("SPAWN", "SpawnActor: GWorld not available");
                return sol::nil;
            }

            // Build the spawn via GameplayStatics::BeginDeferredActorSpawnFromClass
            // or via UWorld::SpawnActor. Since SpawnActor is a C++ template and not
            // exposed as a UFunction, we use GameplayStatics which IS a UFunction.

            // Look for GameplayStatics::BeginDeferredActorSpawnFromClass
            auto* gps_rc = rebuilder::rebuild("GameplayStatics");
            if (!gps_rc) {
                logger::log_warn("SPAWN", "SpawnActor: GameplayStatics class not found");
                return sol::nil;
            }

            auto* spawn_func = gps_rc->find_function("BeginDeferredActorSpawnFromClass");

            if (!spawn_func || !spawn_func->raw) {
                // Fallback: use console command "Summon ClassName"
                logger::log_info("SPAWN", "Using Summon fallback for '%s'", class_name.c_str());
                // Find PlayerController
                auto* pc_rc = rebuilder::rebuild("PlayerController");
                if (!pc_rc) return sol::nil;
                auto* pc = pc_rc->get_first_instance();
                if (!pc) return sol::nil;

                // Execute Summon console command
                auto* cc_func = pe_hook::resolve_func_path("PlayerController:ConsoleCommand");
                if (!cc_func) return sol::nil;

                uint16_t parms_size = ue::ufunc_get_parms_size(cc_func);
                std::vector<uint8_t> parms(parms_size > 0 ? parms_size : 128, 0);

                std::string cmd = "Summon " + class_name;
                std::u16string u16cmd(cmd.begin(), cmd.end());
                u16cmd.push_back(u'\0');
                struct FString { const char16_t* data; int32_t num, max; };
                FString fs;
                fs.data = u16cmd.c_str();
                fs.num = static_cast<int32_t>(u16cmd.size());
                fs.max = fs.num;
                std::memcpy(parms.data(), &fs, sizeof(FString));

                auto pe = pe_hook::get_original();
                if (!pe) pe = symbols::ProcessEvent;
                if (pe) pe(pc, cc_func, parms.data());

                logger::log_info("SPAWN", "Summoned: %s", class_name.c_str());
                return sol::nil; // Summon doesn't return the actor reference
            }

            // TODO: Full SpawnActor via BeginDeferredActorSpawnFromClass ProcessEvent
            // This requires constructing FTransform in the params buffer
            // For now, use the Summon fallback above
            logger::log_info("SPAWN", "BeginDeferredActorSpawnFromClass found but params serialization "
                             "not yet implemented — using Summon fallback");
            return sol::nil; });

        // ─── DestroyActor — Destroy an actor ────────────────────────────
        lua.set_function("DestroyActor", [](sol::object obj_arg) -> bool
                         {
            ue::UObject* obj = nullptr;
            if (obj_arg.is<lua_uobject::LuaUObject>()) {
                obj = obj_arg.as<lua_uobject::LuaUObject&>().ptr;
            }
            if (!obj || !ue::is_valid_ptr(obj)) return false;

            // Call K2_DestroyActor (Blueprint-callable version of DestroyActor)
            auto* func = pe_hook::resolve_func_path("Actor:K2_DestroyActor");
            if (!func) {
                logger::log_warn("SPAWN", "K2_DestroyActor not found");
                return false;
            }

            auto pe = pe_hook::get_original();
            if (!pe) pe = symbols::ProcessEvent;
            if (!pe) return false;

            pe(obj, func, nullptr);
            logger::log_info("SPAWN", "Destroyed actor @ %p", (void*)obj);
            return true; });

        // ─── ConstructObject — StaticConstructObject wrapper ────────────
        // Already exists in lua_bindings as ConstructObject, but add a typed version
        lua.set_function("NewObject", [](sol::this_state ts, const std::string &class_name, sol::optional<sol::object> outer_arg) -> sol::object
                         {
            sol::state_view lua_view(ts);

            if (!symbols::StaticConstructObject) {
                logger::log_warn("SPAWN", "NewObject: StaticConstructObject not resolved");
                return sol::nil;
            }

            ue::UClass* cls = reflection::find_class_ptr(class_name);
            if (!cls) cls = reflection::find_class_ptr(class_name + "_C");
            if (!cls) {
                logger::log_warn("SPAWN", "NewObject: class '%s' not found", class_name.c_str());
                return sol::nil;
            }

            // Outer: use GetTransientPackage if not specified
            ue::UObject* outer = nullptr;
            if (outer_arg && outer_arg->is<lua_uobject::LuaUObject>()) {
                outer = outer_arg->as<lua_uobject::LuaUObject&>().ptr;
            }
            if (!outer && symbols::GetTransientPackage) {
                outer = symbols::GetTransientPackage();
            }

            ue::FName name_none = {0, 0};
            ue::UObject* result = symbols::StaticConstructObject(
                cls, outer, name_none, 0, 0, nullptr, false, nullptr, false);

            if (!result) {
                logger::log_warn("SPAWN", "StaticConstructObject returned NULL");
                return sol::nil;
            }

            logger::log_info("SPAWN", "NewObject: created '%s' @ %p",
                             reflection::get_short_name(result).c_str(), (void*)result);
            return lua_uobject::wrap_or_nil(lua_view, result); });

        // ─── FreeMemory — counterpart to AllocateMemory ─────────────────
        lua.set_function("FreeMemory", [](uintptr_t addr)
                         {
            if (addr == 0) return;
            void* ptr = reinterpret_cast<void*>(addr);
            // We can only safely free memory allocated by our AllocateMemory (which uses new[])
            delete[] static_cast<uint8_t*>(ptr);
            logger::log_info("MEM", "Freed memory at 0x%lX", static_cast<unsigned long>(addr)); });

        // ─── MarkPendingKill ────────────────────────────────────────────
        lua.set_function("MarkPendingKill", [](sol::object obj_arg) -> bool
                         {
            ue::UObject* obj = nullptr;
            if (obj_arg.is<lua_uobject::LuaUObject>()) {
                obj = obj_arg.as<lua_uobject::LuaUObject&>().ptr;
            }
            if (!obj || !ue::is_valid_ptr(obj)) return false;

            // Set RF_BeginDestroyed flag on the object
            int32_t flags = ue::uobj_get_flags(obj);
            flags |= ue::RF_BeginDestroyed;
            ue::write_field<int32_t>(obj, ue::uobj::OBJECT_FLAGS, flags);
            logger::log_info("SPAWN", "MarkPendingKill: %p (flags now 0x%X)",
                             (void*)obj, flags);
            return true; });

        logger::log_info("LUA", "Cast layer + SpawnActor + NewObject + FreeMemory + introspection API registered");

        // ═══════════════════════════════════════════════════════════════════
        // HarmonyX-style Hook() API
        //
        // Usage:
        //   Hook("BP_Player_C:TakeDamage", {
        //       Prefix = function(ctx)
        //           -- ctx.instance is the UObject being called on
        //           -- ctx.args is a CastParms accessor for named parameter access
        //           -- ctx.func_name is the function path
        //           -- return false to BLOCK the original function
        //           -- return true or nil to let it through
        //           local damage = ctx.args.DamageAmount
        //           if damage > 100 then return false end -- block lethal damage
        //       end,
        //       Postfix = function(ctx)
        //           -- ctx.result is available here (if function has a return value)
        //           -- Postfix ALWAYS runs even if Prefix returned false
        //           Log("TakeDamage completed on " .. ctx.instance:GetName())
        //       end
        //   })
        //
        // Returns: { PreId = <id>, PostId = <id>, Unhook = function() }
        // ═══════════════════════════════════════════════════════════════════

        lua.set_function("Hook", [](sol::this_state ts, const std::string &func_path, sol::table opts) -> sol::table
                         {
            sol::state_view lua_view(ts);

            // Normalize path: /Script/Module.Class:Func → Class:Func
            std::string path = func_path;
            if (path.size() > 2 && path[0] == '/') {
                size_t colon = path.rfind(':');
                if (colon != std::string::npos) {
                    std::string path_part = path.substr(0, colon);
                    std::string func_part = path.substr(colon + 1);
                    size_t dot = path_part.rfind('.');
                    std::string class_part = (dot != std::string::npos) ? path_part.substr(dot + 1) : path_part;
                    path = class_part + ":" + func_part;
                }
            }

            sol::object prefix_obj = opts.get_or<sol::object>("Prefix", sol::nil);
            sol::object postfix_obj = opts.get_or<sol::object>("Postfix", sol::nil);
            // Also accept lowercase
            if (prefix_obj == sol::nil) prefix_obj = opts.get_or<sol::object>("prefix", sol::nil);
            if (postfix_obj == sol::nil) postfix_obj = opts.get_or<sol::object>("postfix", sol::nil);

            uint64_t pre_id = 0;
            uint64_t post_id = 0;

            if (prefix_obj.is<sol::function>()) {
                sol::function prefix_fn = prefix_obj.as<sol::function>();
                pre_id = pe_hook::register_pre(path,
                    [prefix_fn, path, lua_view](ue::UObject* self, ue::UFunction* func, void* parms) mutable -> bool {
                        // Build ctx table
                        sol::table ctx = lua_view.create_table();

                        // ctx.instance = wrapped UObject
                        lua_uobject::LuaUObject wrapped;
                        wrapped.ptr = self;
                        ctx["instance"] = wrapped;
                        ctx["__instance"] = wrapped; // HarmonyX compat

                        // ctx.func_name
                        ctx["func_name"] = path;

                        // ctx.args = CastParms accessor for named access
                        if (parms) {
                            auto colon = path.find(':');
                            if (colon != std::string::npos) {
                                std::string class_name = path.substr(0, colon);
                                std::string func_name = path.substr(colon + 1);
                                auto* rc = rebuilder::rebuild(class_name);
                                if (rc) {
                                    auto* rf = rc->find_function(func_name);
                                    if (rf) {
                                        LuaParmsAccessor accessor;
                                        accessor.parms = parms;
                                        accessor.func_path = path;
                                        accessor.rc = std::shared_ptr<rebuilder::RebuiltClass>(rc, [](rebuilder::RebuiltClass*) {});
                                        ctx["args"] = accessor;
                                        ctx["__args"] = accessor; // HarmonyX compat
                                    }
                                }
                            }
                        }

                        // ctx.parms = raw parms lightuserdata (fallback)
                        ctx["parms"] = sol::lightuserdata_value(parms);

                        // ctx.state = persistent per-hook state table
                        ctx["state"] = lua_view.create_table();
                        ctx["__state"] = ctx["state"]; // HarmonyX compat

                        auto result = prefix_fn(ctx);
                        if (result.valid()) {
                            if (result.get_type() == sol::type::boolean) {
                                bool val = result.get<bool>();
                                // HarmonyX: return false = BLOCK
                                return !val;
                            }
                            if (result.get_type() == sol::type::string) {
                                std::string s = result;
                                if (s == "BLOCK") return true;
                            }
                        } else {
                            sol::error err = result;
                            logger::log_error("HOOK", "Hook(%s) Prefix error: %s",
                                              path.c_str(), err.what());
                        }
                        return false; // don't block
                    });
            }

            if (postfix_obj.is<sol::function>()) {
                sol::function postfix_fn = postfix_obj.as<sol::function>();
                post_id = pe_hook::register_post(path,
                    [postfix_fn, path, lua_view](ue::UObject* self, ue::UFunction* func, void* parms) mutable {
                        // Build ctx table
                        sol::table ctx = lua_view.create_table();

                        lua_uobject::LuaUObject wrapped;
                        wrapped.ptr = self;
                        ctx["instance"] = wrapped;
                        ctx["__instance"] = wrapped;
                        ctx["func_name"] = path;

                        if (parms) {
                            auto colon = path.find(':');
                            if (colon != std::string::npos) {
                                std::string class_name = path.substr(0, colon);
                                std::string func_name = path.substr(colon + 1);
                                auto* rc = rebuilder::rebuild(class_name);
                                if (rc) {
                                    auto* rf = rc->find_function(func_name);
                                    if (rf) {
                                        LuaParmsAccessor accessor;
                                        accessor.parms = parms;
                                        accessor.func_path = path;
                                        accessor.rc = std::shared_ptr<rebuilder::RebuiltClass>(rc, [](rebuilder::RebuiltClass*) {});
                                        ctx["args"] = accessor;
                                        ctx["__args"] = accessor;

                                        // ctx.result = return value (if any)
                                        if (rf->return_prop) {
                                            ctx["result"] = read_param(lua_view, parms, *rf->return_prop);
                                            ctx["__result"] = ctx["result"];
                                        }
                                    }
                                }
                            }
                        }

                        ctx["parms"] = sol::lightuserdata_value(parms);
                        ctx["state"] = lua_view.create_table();
                        ctx["__state"] = ctx["state"];

                        auto result = postfix_fn(ctx);
                        if (!result.valid()) {
                            sol::error err = result;
                            logger::log_error("HOOK", "Hook(%s) Postfix error: %s",
                                              path.c_str(), err.what());
                        }
                    });
            }

            // Return result table with IDs and Unhook function
            sol::table result = lua_view.create_table();
            result["PreId"] = pre_id;
            result["PostId"] = post_id;
            result["Unhook"] = [pre_id, post_id]() {
                if (pre_id) pe_hook::unregister(pre_id);
                if (post_id) pe_hook::unregister(post_id);
            };

            logger::log_info("HOOK", "Hook(%s): PreId=%lu PostId=%lu",
                             path.c_str(), (unsigned long)pre_id, (unsigned long)post_id);
            return result; });

        logger::log_info("LUA", "HarmonyX-style Hook() API registered");
    }

} // namespace lua_cast
