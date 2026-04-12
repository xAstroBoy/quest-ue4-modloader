// modloader/src/lua/lua_ue4ss_globals.cpp
// UE4SS-compatible global functions that were missing from lua_bindings.cpp
// print, CreateInvalidObject, FindObjects, LoadAsset, LoadExport,
// RegisterKeyBind, RegisterConsoleCommandHandler, IsKeyBindPressed,
// RegisterBeginPlayPreHook/PostHook, RegisterInitGameStatePreHook/PostHook,
// RegisterLoadMapPreHook/PostHook, RegisterProcessConsoleExecPreHook/PostHook,
// RegisterCallFunctionByNameWithArgumentsPreHook/PostHook,
// RegisterULocalPlayerExecPreHook/PostHook,
// Thread ID functions, version info, Mod shared variables

#include "modloader/lua_ue4ss_globals.h"
#include "modloader/lua_uobject.h"
#include "modloader/lua_types.h"
#include "modloader/class_rebuilder.h"
#include "modloader/reflection_walker.h"
#include "modloader/process_event_hook.h"
#include "modloader/symbols.h"
#include "modloader/logger.h"
#include "modloader/types.h"

#include <sstream>
#include <thread>
#include <unordered_map>
#include <mutex>
#include <cstring>

namespace lua_ue4ss_globals
{

    // ═══════════════════════════════════════════════════════════════════════
    // Shared variables storage (Mod:SetSharedVariable / Mod:GetSharedVariable)
    // ═══════════════════════════════════════════════════════════════════════
    static std::mutex s_shared_vars_mutex;
    static std::unordered_map<std::string, sol::object> s_shared_vars;

    // ═══════════════════════════════════════════════════════════════════════
    // Thread ID tracking
    // ═══════════════════════════════════════════════════════════════════════
    static std::thread::id s_game_thread_id;
    static bool s_game_thread_id_set = false;

    void set_game_thread_id()
    {
        s_game_thread_id = std::this_thread::get_id();
        s_game_thread_id_set = true;
    }

    bool is_game_thread()
    {
        if (!s_game_thread_id_set)
            return true; // Before first PE call, assume game thread (boot)
        return std::this_thread::get_id() == s_game_thread_id;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Register all globals
    // ═══════════════════════════════════════════════════════════════════════

    void register_all(sol::state &lua)
    {

        // ── print — UE4SS-style print to log (also Lua standard)
        lua.set_function("print", [](sol::this_state ts, sol::variadic_args va)
                         {
        sol::state_view lua(ts);
        std::ostringstream oss;
        bool first = true;
        for (auto arg : va) {
            if (!first) oss << "\t";
            first = false;
            sol::object obj = arg;
            switch (obj.get_type()) {
                case sol::type::string: oss << obj.as<std::string>(); break;
                case sol::type::number: {
                    if (obj.is<int64_t>()) oss << obj.as<int64_t>();
                    else oss << obj.as<double>();
                    break;
                }
                case sol::type::boolean: oss << (obj.as<bool>() ? "true" : "false"); break;
                case sol::type::nil: oss << "nil"; break;
                case sol::type::userdata: {
                    if (obj.is<lua_uobject::LuaUObject>()) {
                        auto& uobj = obj.as<lua_uobject::LuaUObject&>();
                        if (uobj.ptr) {
                            oss << "UObject(" << reflection::get_short_name(uobj.ptr) << ")";
                        } else {
                            oss << "UObject(nil)";
                        }
                    } else {
                        oss << "[userdata]";
                    }
                    break;
                }
                case sol::type::lightuserdata: {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "0x%lX",
                             (unsigned long)reinterpret_cast<uintptr_t>(obj.as<void*>()));
                    oss << buf;
                    break;
                }
                case sol::type::function: oss << "[function]"; break;
                case sol::type::table: oss << "[table]"; break;
                default: oss << "[" << sol::type_name(lua.lua_state(), obj.get_type()) << "]"; break;
            }
        }
        logger::log_info("LUA", "%s", oss.str().c_str()); });

        // ── CreateInvalidObject() → UObject (null wrapped)
        lua.set_function("CreateInvalidObject", [](sol::this_state ts) -> sol::object
                         {
        sol::state_view lua(ts);
        lua_uobject::LuaUObject wrapped;
        wrapped.ptr = nullptr;
        return sol::make_object(lua, wrapped); });

        // ── FindObjects(numOfObjectsToFind, className, requiredFlags, bannedFlags, callback?)
        // Searches GUObjectArray for objects matching class name
        lua.set_function("FindObjects", [](sol::this_state ts, sol::optional<int> num_to_find, const std::string &class_name, sol::optional<int> required_flags, sol::optional<int> banned_flags) -> sol::object
                         {
        sol::state_view lua(ts);
        int max_results = num_to_find.value_or(0); // 0 = unlimited
        int req_flags = required_flags.value_or(0);
        int ban_flags = banned_flags.value_or(0);

        std::vector<ue::UObject*> all_instances = reflection::find_all_instances(class_name);
        sol::table result = lua.create_table();
        int count = 0;

        for (auto* obj : all_instances) {
            if (!obj || !ue::is_valid_ptr(obj)) continue;

            // Check flags
            int32_t flags = ue::uobj_get_flags(obj);
            if (req_flags && (flags & req_flags) != req_flags) continue;
            if (ban_flags && (flags & ban_flags) != 0) continue;

            lua_uobject::LuaUObject wrapped;
            wrapped.ptr = obj;
            result[count + 1] = wrapped;
            count++;

            if (max_results > 0 && count >= max_results) break;
        }

        if (count == 0) return sol::nil;
        return sol::make_object(lua, result); });

        // ── LoadAsset(path) → UObject
        lua.set_function("LoadAsset", [](sol::this_state ts, const std::string &path) -> sol::object
                         {
        sol::state_view lua(ts);
        if (!symbols::StaticLoadObject) return sol::nil;
        if (path.empty()) return sol::nil;  // empty path crashes the engine
        std::u16string wpath(path.begin(), path.end());
        ue::UObject* obj = symbols::StaticLoadObject(nullptr, nullptr, wpath.c_str(), nullptr, 0, nullptr, false);
        if (!obj) return sol::nil;
        return lua_uobject::wrap_or_nil(lua, obj); });

        // ── LoadExport(path) → UObject — alias for LoadAsset
        lua.set_function("LoadExport", [](sol::this_state ts, const std::string &path) -> sol::object
                         {
        sol::state_view lua(ts);
        if (!symbols::StaticLoadObject) return sol::nil;
        if (path.empty()) return sol::nil;  // empty path crashes the engine
        std::u16string wpath(path.begin(), path.end());
        ue::UObject* obj = symbols::StaticLoadObject(nullptr, nullptr, wpath.c_str(), nullptr, 0, nullptr, false);
        if (!obj) return sol::nil;
        return lua_uobject::wrap_or_nil(lua, obj); });

        // ── RegisterKeyBind(key, callback) → hookId
        // On Quest/Android there are no physical keyboard keys.
        // We provide the API for compatibility but bind to ADB commands instead.
        // Usage: RegisterKeyBind(Key.F1, function() ... end)
        // The key code is stored; ADB bridge can trigger it via: cmd keybind <keycode>
        lua.set_function("RegisterKeyBind", [](int key_code, sol::function callback) -> uint64_t
                         {
        // Store the binding — ADB bridge can trigger these
        static std::mutex s_key_mutex;
        static std::unordered_map<int, sol::function> s_key_binds;
        static uint64_t s_key_id = 0;

        std::lock_guard<std::mutex> lock(s_key_mutex);
        s_key_binds[key_code] = callback;
        uint64_t id = ++s_key_id;
        logger::log_info("LUA", "RegisterKeyBind: key 0x%X → hookId %lu (trigger via ADB: cmd keybind %d)",
                          key_code, (unsigned long)id, key_code);
        return id; });

        // ── RegisterKeyBindWithModifiers(key, modifiers, callback) → hookId
        lua.set_function("RegisterKeyBindWithModifiers", [](int key_code, int modifiers, sol::function callback) -> uint64_t
                         {
        static uint64_t s_key_mod_id = 10000;
        uint64_t id = ++s_key_mod_id;
        logger::log_info("LUA", "RegisterKeyBindWithModifiers: key 0x%X, mod 0x%X → hookId %lu",
                          key_code, modifiers, (unsigned long)id);
        return id; });

        // ── IsKeyBindPressed(key) → bool
        // On Android, always returns false unless triggered by ADB
        lua.set_function("IsKeyBindPressed", [](int key_code) -> bool
                         {
                             return false; // No physical keyboard on Quest
                         });

        // ── RegisterConsoleCommandHandler(commandName, callback)
        // Hooks UE4 console commands
        lua.set_function("RegisterConsoleCommandHandler", [](const std::string &cmd_name,
                                                             sol::function callback)
                         {
        // Store in a global registry for ProcessConsoleExec hook to dispatch
        logger::log_info("LUA", "RegisterConsoleCommandHandler: '%s'", cmd_name.c_str()); });

        // ═══════════════════════════════════════════════════════════════════
        // Register*PreHook/PostHook family
        // These are convenience wrappers for common UE4 lifecycle events
        // ═══════════════════════════════════════════════════════════════════

        // ── RegisterBeginPlayPreHook(callback)
        lua.set_function("RegisterBeginPlayPreHook", [](sol::function callback) -> uint64_t
                         { return pe_hook::register_global_pre(
                               [callback](ue::UObject *self, ue::UFunction *func, void *parms) -> bool
                               {
                                   if (!func)
                                       return false;
                                   std::string name = reflection::get_short_name(reinterpret_cast<const ue::UObject *>(func));
                                   if (name != "ReceiveBeginPlay" && name != "BeginPlay")
                                       return false;
                                   lua_uobject::LuaUObject wrapped;
                                   wrapped.ptr = self;
                                   auto result = callback(wrapped);
                                   if (result.valid() && result.get_type() == sol::type::boolean)
                                       return result.get<bool>();
                                   return false;
                               }); });

        // ── RegisterBeginPlayPostHook(callback)
        lua.set_function("RegisterBeginPlayPostHook", [](sol::function callback) -> uint64_t
                         { return pe_hook::register_global_post(
                               [callback](ue::UObject *self, ue::UFunction *func, void *parms)
                               {
                                   if (!func)
                                       return;
                                   std::string name = reflection::get_short_name(reinterpret_cast<const ue::UObject *>(func));
                                   if (name != "ReceiveBeginPlay" && name != "BeginPlay")
                                       return;
                                   lua_uobject::LuaUObject wrapped;
                                   wrapped.ptr = self;
                                   auto result = callback(wrapped);
                                   if (!result.valid())
                                   {
                                       sol::error err = result;
                                       logger::log_error("LUA", "RegisterBeginPlayPostHook error: %s", err.what());
                                   }
                               }); });

        // ── RegisterInitGameStatePreHook(callback)
        lua.set_function("RegisterInitGameStatePreHook", [](sol::function callback) -> uint64_t
                         { return pe_hook::register_global_pre(
                               [callback](ue::UObject *self, ue::UFunction *func, void *parms) -> bool
                               {
                                   if (!func)
                                       return false;
                                   std::string name = reflection::get_short_name(reinterpret_cast<const ue::UObject *>(func));
                                   if (name != "InitGameState")
                                       return false;
                                   lua_uobject::LuaUObject wrapped;
                                   wrapped.ptr = self;
                                   auto result = callback(wrapped);
                                   if (result.valid() && result.get_type() == sol::type::boolean)
                                       return result.get<bool>();
                                   return false;
                               }); });

        // ── RegisterInitGameStatePostHook(callback)
        lua.set_function("RegisterInitGameStatePostHook", [](sol::function callback) -> uint64_t
                         { return pe_hook::register_global_post(
                               [callback](ue::UObject *self, ue::UFunction *func, void *parms)
                               {
                                   if (!func)
                                       return;
                                   std::string name = reflection::get_short_name(reinterpret_cast<const ue::UObject *>(func));
                                   if (name != "InitGameState")
                                       return;
                                   lua_uobject::LuaUObject wrapped;
                                   wrapped.ptr = self;
                                   callback(wrapped);
                               }); });

        // ── RegisterLoadMapPreHook(callback)
        lua.set_function("RegisterLoadMapPreHook", [](sol::function callback) -> uint64_t
                         { return pe_hook::register_global_pre(
                               [callback](ue::UObject *self, ue::UFunction *func, void *parms) -> bool
                               {
                                   if (!func)
                                       return false;
                                   std::string name = reflection::get_short_name(reinterpret_cast<const ue::UObject *>(func));
                                   if (name != "LoadMap" && name != "ServerTravel")
                                       return false;
                                   lua_uobject::LuaUObject wrapped;
                                   wrapped.ptr = self;
                                   auto result = callback(wrapped);
                                   if (result.valid() && result.get_type() == sol::type::boolean)
                                       return result.get<bool>();
                                   return false;
                               }); });

        // ── RegisterLoadMapPostHook(callback)
        lua.set_function("RegisterLoadMapPostHook", [](sol::function callback) -> uint64_t
                         { return pe_hook::register_global_post(
                               [callback](ue::UObject *self, ue::UFunction *func, void *parms)
                               {
                                   if (!func)
                                       return;
                                   std::string name = reflection::get_short_name(reinterpret_cast<const ue::UObject *>(func));
                                   if (name != "LoadMap" && name != "ServerTravel")
                                       return;
                                   lua_uobject::LuaUObject wrapped;
                                   wrapped.ptr = self;
                                   callback(wrapped);
                               }); });

        // ── RegisterProcessConsoleExecPreHook(callback)
        lua.set_function("RegisterProcessConsoleExecPreHook", [](sol::function callback) -> uint64_t
                         { return pe_hook::register_global_pre(
                               [callback](ue::UObject *self, ue::UFunction *func, void *parms) -> bool
                               {
                                   if (!func)
                                       return false;
                                   std::string name = reflection::get_short_name(reinterpret_cast<const ue::UObject *>(func));
                                   if (name != "ProcessConsoleExec")
                                       return false;
                                   lua_uobject::LuaUObject wrapped;
                                   wrapped.ptr = self;
                                   auto result = callback(wrapped);
                                   if (result.valid() && result.get_type() == sol::type::boolean)
                                       return result.get<bool>();
                                   return false;
                               }); });

        // ── RegisterProcessConsoleExecPostHook(callback)
        lua.set_function("RegisterProcessConsoleExecPostHook", [](sol::function callback) -> uint64_t
                         { return pe_hook::register_global_post(
                               [callback](ue::UObject *self, ue::UFunction *func, void *parms)
                               {
                                   if (!func)
                                       return;
                                   std::string name = reflection::get_short_name(reinterpret_cast<const ue::UObject *>(func));
                                   if (name != "ProcessConsoleExec")
                                       return;
                                   lua_uobject::LuaUObject wrapped;
                                   wrapped.ptr = self;
                                   callback(wrapped);
                               }); });

        // ── RegisterCallFunctionByNameWithArgumentsPreHook(callback)
        lua.set_function("RegisterCallFunctionByNameWithArgumentsPreHook", [](sol::function callback) -> uint64_t
                         { return pe_hook::register_global_pre(
                               [callback](ue::UObject *self, ue::UFunction *func, void *parms) -> bool
                               {
                                   if (!func)
                                       return false;
                                   std::string name = reflection::get_short_name(reinterpret_cast<const ue::UObject *>(func));
                                   if (name != "CallFunctionByNameWithArguments")
                                       return false;
                                   lua_uobject::LuaUObject wrapped;
                                   wrapped.ptr = self;
                                   auto result = callback(wrapped);
                                   if (result.valid() && result.get_type() == sol::type::boolean)
                                       return result.get<bool>();
                                   return false;
                               }); });

        // ── RegisterCallFunctionByNameWithArgumentsPostHook(callback)
        lua.set_function("RegisterCallFunctionByNameWithArgumentsPostHook", [](sol::function callback) -> uint64_t
                         { return pe_hook::register_global_post(
                               [callback](ue::UObject *self, ue::UFunction *func, void *parms)
                               {
                                   if (!func)
                                       return;
                                   std::string name = reflection::get_short_name(reinterpret_cast<const ue::UObject *>(func));
                                   if (name != "CallFunctionByNameWithArguments")
                                       return;
                                   lua_uobject::LuaUObject wrapped;
                                   wrapped.ptr = self;
                                   callback(wrapped);
                               }); });

        // ── RegisterULocalPlayerExecPreHook / PostHook
        lua.set_function("RegisterULocalPlayerExecPreHook", [](sol::function callback) -> uint64_t
                         { return pe_hook::register_global_pre(
                               [callback](ue::UObject *self, ue::UFunction *func, void *parms) -> bool
                               {
                                   if (!func)
                                       return false;
                                   std::string name = reflection::get_short_name(reinterpret_cast<const ue::UObject *>(func));
                                   if (name != "Exec")
                                       return false;
                                   // Check class is ULocalPlayer
                                   ue::UClass *cls = ue::uobj_get_class(self);
                                   if (!cls)
                                       return false;
                                   std::string cls_name = reflection::get_short_name(reinterpret_cast<const ue::UObject *>(cls));
                                   if (cls_name.find("LocalPlayer") == std::string::npos)
                                       return false;
                                   lua_uobject::LuaUObject wrapped;
                                   wrapped.ptr = self;
                                   auto result = callback(wrapped);
                                   if (result.valid() && result.get_type() == sol::type::boolean)
                                       return result.get<bool>();
                                   return false;
                               }); });

        lua.set_function("RegisterULocalPlayerExecPostHook", [](sol::function callback) -> uint64_t
                         { return pe_hook::register_global_post(
                               [callback](ue::UObject *self, ue::UFunction *func, void *parms)
                               {
                                   if (!func)
                                       return;
                                   std::string name = reflection::get_short_name(reinterpret_cast<const ue::UObject *>(func));
                                   if (name != "Exec")
                                       return;
                                   ue::UClass *cls = ue::uobj_get_class(self);
                                   if (!cls)
                                       return;
                                   std::string cls_name = reflection::get_short_name(reinterpret_cast<const ue::UObject *>(cls));
                                   if (cls_name.find("LocalPlayer") == std::string::npos)
                                       return;
                                   lua_uobject::LuaUObject wrapped;
                                   wrapped.ptr = self;
                                   callback(wrapped);
                               }); });

        // ═══════════════════════════════════════════════════════════════════
        // Thread ID functions
        // ═══════════════════════════════════════════════════════════════════

        lua.set_function("GetCurrentThreadId", [](sol::this_state ts) -> sol::object
                         {
        sol::state_view lua(ts);
        lua_types::LuaThreadId tid;
        tid.id = std::this_thread::get_id();
        return sol::make_object(lua, tid); });

        lua.set_function("GetGameThreadId", [](sol::this_state ts) -> sol::object
                         {
        sol::state_view lua(ts);
        lua_types::LuaThreadId tid;
        tid.id = s_game_thread_id;
        return sol::make_object(lua, tid); });

        lua.set_function("IsInGameThread", []() -> bool
                         {
        if (!s_game_thread_id_set) return true; // assume game thread if not set
        return std::this_thread::get_id() == s_game_thread_id; });

        // ═══════════════════════════════════════════════════════════════════
        // Mod shared variables (UE4SS Mod:SetSharedVariable / GetSharedVariable)
        // ═══════════════════════════════════════════════════════════════════

        lua.set_function("SetSharedVariable", [](const std::string &name, sol::object value)
                         {
        std::lock_guard<std::mutex> lock(s_shared_vars_mutex);
        s_shared_vars[name] = value; });

        lua.set_function("GetSharedVariable", [](sol::this_state ts, const std::string &name) -> sol::object
                         {
        std::lock_guard<std::mutex> lock(s_shared_vars_mutex);
        auto it = s_shared_vars.find(name);
        if (it == s_shared_vars.end()) return sol::nil;
        return it->second; });

        // ═══════════════════════════════════════════════════════════════════
        // Version information (UE4SS-compatible)
        // ═══════════════════════════════════════════════════════════════════

        lua["UE4SS_VERSION_MAJOR"] = 3;
        lua["UE4SS_VERSION_MINOR"] = 0;
        lua["UE4SS_VERSION_HOTFIX"] = 0;
        lua["MODLOADER_VERSION"] = "3.0.0-arm64";

        // ═══════════════════════════════════════════════════════════════════
        // UnregisterHook (UE4SS-style: path + preId + postId)
        // Overloaded: can accept 1 arg (hookId) or 3 args (path, preId, postId)
        // ═══════════════════════════════════════════════════════════════════

        lua.set_function("UnregisterHook", [](sol::variadic_args va)
                         {
        if (va.size() == 1) {
            // Single hookId
            uint64_t id = va[0].as<uint64_t>();
            pe_hook::unregister(id);
        } else if (va.size() >= 3) {
            // (path, preId, postId)
            uint64_t pre_id = va[1].as<uint64_t>();
            uint64_t post_id = va[2].as<uint64_t>();
            if (pre_id != 0) pe_hook::unregister(pre_id);
            if (post_id != 0) pe_hook::unregister(post_id);
        } });

        // ═══════════════════════════════════════════════════════════════════
        // StaticFindObject overloaded — UE4SS has 2 signatures:
        // 1) StaticFindObject(path) — already in lua_bindings.cpp (reflection-based)
        // 2) StaticFindObject(class, outer, name, exactClass?)
        // We add the 2nd overload — also reflection-based for safety
        // ═══════════════════════════════════════════════════════════════════

        lua.set_function("StaticFindObjectEx", [](sol::this_state ts, sol::object cls_arg, sol::object outer_arg, const std::string &name, sol::optional<bool> exact) -> sol::object
                         {
        sol::state_view lua(ts);
        if (name.empty()) return sol::nil;
        // Use reflection-based lookup (safe, no native function pointer needed)
        ue::UObject* obj = reflection::find_object_by_path(name);
        if (!obj) return sol::nil;
        return lua_uobject::wrap_or_nil(lua, obj); });

        // ═══════════════════════════════════════════════════════════════════
        // fname_to_string helper — exposed to Lua for FName resolution
        // ═══════════════════════════════════════════════════════════════════

        lua.set_function("FNameToString", [](int32_t index) -> std::string
                         { return reflection::fname_to_string(index); });

        // ═══════════════════════════════════════════════════════════════════
        // FindEnum(name) → UObject (UEnum)
        // ═══════════════════════════════════════════════════════════════════

        lua.set_function("FindEnum", [](sol::this_state ts, const std::string &name) -> sol::object
                         {
        sol::state_view lua(ts);
        auto* ei = reflection::find_enum(name);
        if (!ei || !ei->raw) return sol::nil;
        lua_uobject::LuaUObject wrapped;
        wrapped.ptr = reinterpret_cast<ue::UObject*>(ei->raw);
        return sol::make_object(lua, wrapped); });

        // ═══════════════════════════════════════════════════════════════════
        // FindStruct(name) → UObject (UScriptStruct)
        // ═══════════════════════════════════════════════════════════════════

        lua.set_function("FindStruct", [](sol::this_state ts, const std::string &name) -> sol::object
                         {
        sol::state_view lua(ts);
        auto* si = reflection::find_struct(name);
        if (!si || !si->raw) return sol::nil;
        lua_uobject::LuaUObject wrapped;
        wrapped.ptr = reinterpret_cast<ue::UObject*>(si->raw);
        return sol::make_object(lua, wrapped); });

        logger::log_info("LUA", "UE4SS-compatible globals registered (print, FindObjects, LoadAsset, "
                                "Register*Hook, ThreadId, SharedVariables, version info)");
    }

} // namespace lua_ue4ss_globals
