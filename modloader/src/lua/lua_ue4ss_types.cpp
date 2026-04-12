// modloader/src/lua/lua_ue4ss_types.cpp
// UE4SS-compatible UClass, UStruct, UFunction, UEnum, AActor Lua usertypes
// Extends the base UObject usertype with full UE4SS method parity

#include "modloader/lua_ue4ss_types.h"
#include "modloader/lua_uobject.h"
#include "modloader/lua_types.h"
#include "modloader/class_rebuilder.h"
#include "modloader/reflection_walker.h"
#include "modloader/symbols.h"
#include "modloader/logger.h"
#include "modloader/types.h"

#include <sstream>
#include <cstring>

namespace lua_ue4ss_types
{

    // ═══════════════════════════════════════════════════════════════════════
    // UObject Extensions — add missing UE4SS methods to existing UObject type
    // ═══════════════════════════════════════════════════════════════════════

    void extend_uobject(sol::state &lua)
    {
        // We can't modify an existing usertype after creation in sol2,
        // so we register these as global free functions that accept UObject as first arg.
        // UE4SS does many of these as methods — we register both the method-style
        // (via extended_uobject_methods) and free-function style.

        // GetFName(obj) → FName
        lua.set_function("UObject_GetFName", [](sol::this_state ts, const lua_uobject::LuaUObject &self) -> sol::object
                         {
        sol::state_view lua(ts);
        if (!self.ptr) return sol::nil;
        int32_t idx = ue::uobj_get_name_index(self.ptr);
        lua_types::LuaFName fn(idx);
        return sol::make_object(lua, fn); });

        // GetOuter(obj) → UObject
        lua.set_function("UObject_GetOuter", [](sol::this_state ts, const lua_uobject::LuaUObject &self) -> sol::object
                         {
        sol::state_view lua(ts);
        if (!self.ptr) return sol::nil;
        ue::UObject* outer = ue::uobj_get_outer(self.ptr);
        if (!outer) return sol::nil;
        lua_uobject::LuaUObject wrapped;
        wrapped.ptr = outer;
        return sol::make_object(lua, wrapped); });

        // IsA(obj, className) → bool
        lua.set_function("UObject_IsA", [](const lua_uobject::LuaUObject &self, sol::object class_arg) -> bool
                         {
        if (!self.ptr) return false;

        ue::UClass* target_cls = nullptr;

        if (class_arg.is<std::string>()) {
            target_cls = reflection::find_class_ptr(class_arg.as<std::string>());
        } else if (class_arg.is<lua_uobject::LuaUObject>()) {
            target_cls = reinterpret_cast<ue::UClass*>(class_arg.as<lua_uobject::LuaUObject&>().ptr);
        }

        if (!target_cls) return false;

        // Walk the class hierarchy
        ue::UClass* cls = ue::uobj_get_class(self.ptr);
        while (cls) {
            if (cls == target_cls) return true;
            cls = reinterpret_cast<ue::UClass*>(ue::ustruct_get_super(reinterpret_cast<ue::UStruct*>(cls)));
        }
        return false; });

        // HasAllFlags(obj, flags) → bool
        lua.set_function("UObject_HasAllFlags", [](const lua_uobject::LuaUObject &self, int32_t flags) -> bool
                         {
        if (!self.ptr) return false;
        int32_t obj_flags = ue::uobj_get_flags(self.ptr);
        return (obj_flags & flags) == flags; });

        // HasAnyFlags(obj, flags) → bool
        lua.set_function("UObject_HasAnyFlags", [](const lua_uobject::LuaUObject &self, int32_t flags) -> bool
                         {
        if (!self.ptr) return false;
        int32_t obj_flags = ue::uobj_get_flags(self.ptr);
        return (obj_flags & flags) != 0; });

        // GetWorld(obj) → UObject (WorldContext)
        lua.set_function("UObject_GetWorld", [](sol::this_state ts, const lua_uobject::LuaUObject &self) -> sol::object
                         {
        sol::state_view lua(ts);
        if (!self.ptr) return sol::nil;
        if (!symbols::GWorld) return sol::nil;
        ue::UObject* world = *reinterpret_cast<ue::UObject**>(symbols::GWorld);
        if (!world) return sol::nil;
        lua_uobject::LuaUObject wrapped;
        wrapped.ptr = world;
        return sol::make_object(lua, wrapped); });

        // type(obj) → string (the UE4SS "type" that returns "UObject", "AActor", "UClass" etc)
        lua.set_function("UObject_type", [](const lua_uobject::LuaUObject &self) -> std::string
                         {
        if (!self.ptr) return "nil";
        ue::UClass* cls = ue::uobj_get_class(self.ptr);
        if (!cls) return "UObject";
        std::string name = reflection::get_short_name(reinterpret_cast<const ue::UObject*>(cls));
        // Check if it's a Class object itself
        ue::UClass* meta = ue::uobj_get_class(reinterpret_cast<ue::UObject*>(cls));
        if (meta) {
            std::string meta_name = reflection::get_short_name(reinterpret_cast<const ue::UObject*>(meta));
            if (meta_name == "Class") return "UClass";
        }
        return name; });

        // GetPropertyValue(obj, propName) → RemoteUnrealParam-wrapped value
        // UE4SS uses this to get typed property access
        lua.set_function("UObject_GetPropertyValue", [](sol::this_state ts, lua_uobject::LuaUObject &self, const std::string &prop_name) -> sol::object
                         {
        sol::state_view lua(ts);
        if (!self.ptr) return sol::nil;

        ue::UClass* cls = ue::uobj_get_class(self.ptr);
        if (!cls) return sol::nil;

        std::string class_name = reflection::get_short_name(reinterpret_cast<const ue::UObject*>(cls));
        auto* rc = rebuilder::rebuild(class_name);
        if (!rc) return sol::nil;

        auto* rp = rc->find_property(prop_name);
        if (!rp) return sol::nil;

        // Create a RemoteUnrealParam pointing to the property memory
        lua_types::LuaRemoteUnrealParam param;
        param.ptr = reinterpret_cast<uint8_t*>(self.ptr) + rp->offset;
        param.property_type_name = rp->type_name;

        // Map reflection PropType → RemoteUnrealParam::ParamType
        using PT = lua_types::LuaRemoteUnrealParam::ParamType;
        switch (rp->type) {
            case reflection::PropType::ObjectProperty:
            case reflection::PropType::WeakObjectProperty:
            case reflection::PropType::ClassProperty:
                param.param_type = PT::UObjectPtr; break;
            case reflection::PropType::BoolProperty:
                param.param_type = PT::Bool; break;
            case reflection::PropType::IntProperty:
                param.param_type = PT::Int32; break;
            case reflection::PropType::Int64Property:
                param.param_type = PT::Int64; break;
            case reflection::PropType::FloatProperty:
                param.param_type = PT::Float; break;
            case reflection::PropType::DoubleProperty:
                param.param_type = PT::Double; break;
            case reflection::PropType::NameProperty:
                param.param_type = PT::FNameVal; break;
            case reflection::PropType::StrProperty:
                param.param_type = PT::FStringVal; break;
            case reflection::PropType::ByteProperty:
                param.param_type = PT::Byte; break;
            default:
                param.param_type = PT::RawPtr; break;
        }

        return sol::make_object(lua, param); });

        logger::log_info("LUA", "UObject extensions registered (GetFName, GetOuter, IsA, flags, etc.)");
    }

    // ═══════════════════════════════════════════════════════════════════════
    // UClass usertype
    // ═══════════════════════════════════════════════════════════════════════

    void register_uclass_type(sol::state &lua)
    {
        // UClass inherits all UObject methods + adds its own
        // Registered as a separate "UClass" type table with helper functions
        sol::table t = lua.create_named_table("UClassMethods");

        // GetCDO(classObj) → UObject
        t.set_function("GetCDO", [](sol::this_state ts, const lua_uobject::LuaUObject &self) -> sol::object
                       {
        sol::state_view lua(ts);
        if (!self.ptr) return sol::nil;
        std::string name = reflection::get_short_name(self.ptr);
        ue::UObject* cdo = rebuilder::get_cdo(name);
        if (!cdo) return sol::nil;
        lua_uobject::LuaUObject wrapped;
        wrapped.ptr = cdo;
        return sol::make_object(lua, wrapped); });

        // IsChildOf(classObj, parentClassObj) → bool
        t.set_function("IsChildOf", [](const lua_uobject::LuaUObject &self, const lua_uobject::LuaUObject &parent) -> bool
                       {
        if (!self.ptr || !parent.ptr) return false;
        ue::UClass* cls = reinterpret_cast<ue::UClass*>(self.ptr);
        ue::UClass* target = reinterpret_cast<ue::UClass*>(parent.ptr);
        while (cls) {
            if (cls == target) return true;
            cls = reinterpret_cast<ue::UClass*>(
                ue::ustruct_get_super(reinterpret_cast<ue::UStruct*>(cls)));
        }
        return false; });

        // GetSuperStruct(classObj) → UObject (parent class)
        t.set_function("GetSuperStruct", [](sol::this_state ts, const lua_uobject::LuaUObject &self) -> sol::object
                       {
        sol::state_view lua(ts);
        if (!self.ptr) return sol::nil;
        ue::UStruct* super = ue::ustruct_get_super(reinterpret_cast<ue::UStruct*>(self.ptr));
        if (!super) return sol::nil;
        lua_uobject::LuaUObject wrapped;
        wrapped.ptr = reinterpret_cast<ue::UObject*>(super);
        return sol::make_object(lua, wrapped); });

        // ForEachFunction(classObj, callback)
        t.set_function("ForEachFunction", [](const lua_uobject::LuaUObject &self, sol::function callback)
                       {
        if (!self.ptr) return;
        auto funcs = reflection::walk_functions(
            reinterpret_cast<ue::UStruct*>(self.ptr), true);
        for (const auto& f : funcs) {
            lua_uobject::LuaUObject func_wrapped;
            func_wrapped.ptr = reinterpret_cast<ue::UObject*>(f.raw);
            auto result = callback(func_wrapped);
            if (result.valid() && result.get_type() == sol::type::boolean && result.get<bool>()) {
                break; // return true from callback to stop
            }
        } });

        // ForEachProperty(classObj, callback)
        t.set_function("ForEachProperty", [](sol::this_state ts, const lua_uobject::LuaUObject &self, sol::function callback)
                       {
        sol::state_view lua(ts);
        if (!self.ptr) return;
        auto props = reflection::walk_properties(
            reinterpret_cast<ue::UStruct*>(self.ptr), true);
        for (const auto& p : props) {
            sol::table prop_info = lua.create_table();
            prop_info["Name"] = p.name;
            prop_info["Offset"] = p.offset;
            prop_info["Size"] = p.element_size;
            prop_info["Type"] = static_cast<int>(p.type);
            prop_info["TypeName"] = p.inner_type_name;
            auto result = callback(prop_info);
            if (result.valid() && result.get_type() == sol::type::boolean && result.get<bool>()) {
                break;
            }
        } });

        logger::log_info("LUA", "UClassMethods table registered");
    }

    // ═══════════════════════════════════════════════════════════════════════
    // UFunction usertype helpers
    // ═══════════════════════════════════════════════════════════════════════

    void register_ufunction_type(sol::state &lua)
    {
        sol::table t = lua.create_named_table("UFunctionMethods");

        // GetFunctionFlags(funcObj) → int
        t.set_function("GetFunctionFlags", [](const lua_uobject::LuaUObject &self) -> uint32_t
                       {
        if (!self.ptr) return 0;
        return ue::ufunc_get_flags(reinterpret_cast<ue::UFunction*>(self.ptr)); });

        // SetFunctionFlags(funcObj, flags)
        t.set_function("SetFunctionFlags", [](const lua_uobject::LuaUObject &self, uint32_t flags)
                       {
        if (!self.ptr) return;
        ue::write_field(self.ptr, ue::ufunc::FUNCTION_FLAGS_OFF(), flags); });

        // GetNumParms(funcObj) → int
        t.set_function("GetNumParms", [](const lua_uobject::LuaUObject &self) -> int
                       {
        if (!self.ptr) return 0;
        return ue::ufunc_get_num_parms(reinterpret_cast<ue::UFunction*>(self.ptr)); });

        // GetParmsSize(funcObj) → int
        t.set_function("GetParmsSize", [](const lua_uobject::LuaUObject &self) -> int
                       {
        if (!self.ptr) return 0;
        return ue::ufunc_get_parms_size(reinterpret_cast<ue::UFunction*>(self.ptr)); });

        // GetNativeFunc(funcObj) → lightuserdata
        t.set_function("GetNativeFunc", [](const lua_uobject::LuaUObject &self) -> sol::lightuserdata_value
                       {
        if (!self.ptr) return sol::lightuserdata_value(nullptr);
        return sol::lightuserdata_value(
            ue::ufunc_get_func_ptr(reinterpret_cast<ue::UFunction*>(self.ptr))); });

        logger::log_info("LUA", "UFunctionMethods table registered");
    }

    // ═══════════════════════════════════════════════════════════════════════
    // UEnum helpers
    // ═══════════════════════════════════════════════════════════════════════

    void register_uenum_type(sol::state &lua)
    {
        sol::table t = lua.create_named_table("UEnumMethods");

        // GetNameByValue(enumObj, value) → string
        t.set_function("GetNameByValue", [](const lua_uobject::LuaUObject &self, int64_t value) -> std::string
                       {
        if (!self.ptr) return "";
        // Read the enum names array
        void* names_data = ue::read_field<void*>(self.ptr, ue::uenum::NAMES_DATA);
        int32_t names_num = ue::read_field<int32_t>(self.ptr, ue::uenum::NAMES_NUM);
        if (!names_data || names_num <= 0) return "";

        for (int32_t i = 0; i < names_num; i++) {
            const uint8_t* entry = reinterpret_cast<const uint8_t*>(names_data) + i * ue::uenum::ENUM_ENTRY_SIZE;
            int32_t fname_idx = *reinterpret_cast<const int32_t*>(entry);
            int64_t enum_val = *reinterpret_cast<const int64_t*>(entry + 8);
            if (enum_val == value) {
                return reflection::fname_to_string(fname_idx);
            }
        }
        return ""; });

        // GetValueByName(enumObj, name) → int or nil
        t.set_function("GetValueByName", [](sol::this_state ts, const lua_uobject::LuaUObject &self, const std::string &name) -> sol::object
                       {
        sol::state_view lua(ts);
        if (!self.ptr) return sol::nil;
        void* names_data = ue::read_field<void*>(self.ptr, ue::uenum::NAMES_DATA);
        int32_t names_num = ue::read_field<int32_t>(self.ptr, ue::uenum::NAMES_NUM);
        if (!names_data || names_num <= 0) return sol::nil;

        for (int32_t i = 0; i < names_num; i++) {
            const uint8_t* entry = reinterpret_cast<const uint8_t*>(names_data) + i * ue::uenum::ENUM_ENTRY_SIZE;
            int32_t fname_idx = *reinterpret_cast<const int32_t*>(entry);
            int64_t enum_val = *reinterpret_cast<const int64_t*>(entry + 8);
            std::string entry_name = reflection::fname_to_string(fname_idx);
            if (entry_name == name) {
                return sol::make_object(lua, static_cast<double>(enum_val));
            }
        }
        return sol::nil; });

        // ForEachName(enumObj, callback)
        t.set_function("ForEachName", [](const lua_uobject::LuaUObject &self, sol::function callback)
                       {
        if (!self.ptr) return;
        void* names_data = ue::read_field<void*>(self.ptr, ue::uenum::NAMES_DATA);
        int32_t names_num = ue::read_field<int32_t>(self.ptr, ue::uenum::NAMES_NUM);
        if (!names_data || names_num <= 0) return;

        for (int32_t i = 0; i < names_num; i++) {
            const uint8_t* entry = reinterpret_cast<const uint8_t*>(names_data) + i * ue::uenum::ENUM_ENTRY_SIZE;
            int32_t fname_idx = *reinterpret_cast<const int32_t*>(entry);
            int64_t enum_val = *reinterpret_cast<const int64_t*>(entry + 8);
            std::string entry_name = reflection::fname_to_string(fname_idx);

            auto result = callback(entry_name, static_cast<double>(enum_val));
            if (result.valid() && result.get_type() == sol::type::boolean && result.get<bool>()) {
                break;
            }
        } });

        // NumEnums(enumObj) → int
        t.set_function("NumEnums", [](const lua_uobject::LuaUObject &self) -> int
                       {
        if (!self.ptr) return 0;
        return ue::read_field<int32_t>(self.ptr, ue::uenum::NAMES_NUM); });

        logger::log_info("LUA", "UEnumMethods table registered");
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Register ALL extended types
    // ═══════════════════════════════════════════════════════════════════════

    void register_all(sol::state &lua)
    {
        extend_uobject(lua);
        register_uclass_type(lua);
        register_ufunction_type(lua);
        register_uenum_type(lua);

        logger::log_info("LUA", "UE4SS-compatible extended types registered");
    }

} // namespace lua_ue4ss_types
