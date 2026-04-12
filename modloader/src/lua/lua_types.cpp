// modloader/src/lua/lua_types.cpp
// UE4SS-compatible Lua type wrappers: FName, FText, FString, FAnsiString,
// RemoteUnrealParam, LocalUnrealParam, FWeakObjectPtr, FOutputDevice,
// ThreadId, UE4SS version, UnrealVersion
// ARM64 implementation — Sol2 bindings

#include "modloader/lua_types.h"
#include "modloader/lua_uobject.h"
#include "modloader/reflection_walker.h"
#include "modloader/symbols.h"
#include "modloader/logger.h"
#include "modloader/types.h"

#include <cstring>
#include <thread>
#include <sstream>

namespace lua_types
{

    // ═══════════════════════════════════════════════════════════════════════
    // FName — UE4SS-compatible
    // ═══════════════════════════════════════════════════════════════════════

    void register_fname(sol::state &lua)
    {
        lua.new_usertype<LuaFName>("FName",
                                   // Constructors
                                   sol::constructors<
                                       LuaFName(),
                                       LuaFName(const std::string &),
                                       LuaFName(const std::string &, int),
                                       LuaFName(int32_t),
                                       LuaFName(int32_t, int)>(),

                                   // Methods
                                   "ToString", &LuaFName::to_string,
                                   "GetComparisonIndex", &LuaFName::get_comparison_index,
                                   "IsValid", &LuaFName::is_valid,

                                   // Metamethods
                                   sol::meta_function::to_string, &LuaFName::to_string,
                                   sol::meta_function::equal_to, [](const LuaFName &a, const LuaFName &b) -> bool
                                   { return a.comparison_index == b.comparison_index; });

        // Global FName constructor function (UE4SS-style)
        lua.set_function("FName", [](sol::this_state ts, sol::variadic_args va) -> LuaFName
                         {
        if (va.size() == 0) return LuaFName();
        sol::object arg0 = va[0];
        int find_type = 1; // FNAME_Add by default
        if (va.size() > 1 && va[1].is<int>()) {
            find_type = va[1].as<int>();
        }
        if (arg0.is<std::string>()) {
            return LuaFName(arg0.as<std::string>(), find_type);
        } else if (arg0.is<int32_t>()) {
            return LuaFName(arg0.as<int32_t>(), find_type);
        }
        return LuaFName(); });
    }

    LuaFName::LuaFName() : comparison_index(0), number(0) {}

    LuaFName::LuaFName(const std::string &name)
    {
        // Resolve via FName pool — FNAME_Add
        if (symbols::FName_Init)
        {
            ue::FName fname;
            // Android ARM64: TCHAR = char16_t (2 bytes), NOT wchar_t (4 bytes)
            std::u16string u16name(name.begin(), name.end());
            symbols::FName_Init(&fname, u16name.c_str(), 0);
            comparison_index = fname.ComparisonIndex;
            number = fname.Number;
        }
        else
        {
            // Fallback: search reflection cache
            comparison_index = reflection::fname_string_to_index(name);
            number = 0;
        }
        cached_string = name;
    }

    LuaFName::LuaFName(const std::string &name, int find_type)
    {
        if (find_type == 0)
        { // FNAME_Find
            comparison_index = reflection::fname_string_to_index(name);
            number = 0;
        }
        else
        { // FNAME_Add
            if (symbols::FName_Init)
            {
                ue::FName fname;
                // Android ARM64: TCHAR = char16_t (2 bytes), NOT wchar_t (4 bytes)
                std::u16string u16name(name.begin(), name.end());
                symbols::FName_Init(&fname, u16name.c_str(), 0);
                comparison_index = fname.ComparisonIndex;
                number = fname.Number;
            }
            else
            {
                comparison_index = reflection::fname_string_to_index(name);
                number = 0;
            }
        }
        cached_string = name;
    }

    LuaFName::LuaFName(int32_t index) : comparison_index(index), number(0)
    {
        cached_string = reflection::fname_to_string(index);
    }

    LuaFName::LuaFName(int32_t index, int) : comparison_index(index), number(0)
    {
        cached_string = reflection::fname_to_string(index);
    }

    std::string LuaFName::to_string() const
    {
        if (!cached_string.empty())
            return cached_string;
        return reflection::fname_to_string(comparison_index);
    }

    int32_t LuaFName::get_comparison_index() const
    {
        return comparison_index;
    }

    bool LuaFName::is_valid() const
    {
        return comparison_index != 0 || number != 0;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // FText — UE4SS-compatible
    // ═══════════════════════════════════════════════════════════════════════

    void register_ftext(sol::state &lua)
    {
        lua.new_usertype<LuaFText>("FText",
                                   sol::constructors<LuaFText(), LuaFText(const std::string &)>(),
                                   "ToString", &LuaFText::to_string,
                                   sol::meta_function::to_string, &LuaFText::to_string);

        // Global FText constructor
        lua.set_function("FText", [](const std::string &str) -> LuaFText
                         { return LuaFText(str); });
    }

    LuaFText::LuaFText() {}
    LuaFText::LuaFText(const std::string &str) : text(str) {}
    std::string LuaFText::to_string() const { return text; }

    // ═══════════════════════════════════════════════════════════════════════
    // FString — UE4SS-compatible with full methods
    // ═══════════════════════════════════════════════════════════════════════

    void register_fstring(sol::state &lua)
    {
        lua.new_usertype<LuaFString>("FString", sol::constructors<LuaFString(), LuaFString(const std::string &)>(),

                                     "ToString", &LuaFString::to_string, "Empty", &LuaFString::empty, "Clear", &LuaFString::empty, // alias
                                     "Len", &LuaFString::len, "IsEmpty", &LuaFString::is_empty, "Append", &LuaFString::append, "Find", [](const LuaFString &self, const std::string &search, sol::this_state ts) -> sol::object
                                     {
                                         sol::state_view lua(ts);
                                         int idx = self.find(search);
                                         if (idx < 0)
                                             return sol::nil;
                                         return sol::make_object(lua, idx + 1); // 1-based
                                     },
                                     "StartsWith", &LuaFString::starts_with, "EndsWith", &LuaFString::ends_with, "ToUpper", &LuaFString::to_upper, "ToLower", &LuaFString::to_lower,

                                     sol::meta_function::to_string, &LuaFString::to_string, sol::meta_function::length, &LuaFString::len, sol::meta_function::concatenation, [](const LuaFString &a, const LuaFString &b) -> LuaFString
                                     {
            LuaFString result(a.data);
            result.append(b.data);
            return result; }, sol::meta_function::equal_to, [](const LuaFString &a, const LuaFString &b) -> bool
                                     { return a.data == b.data; });
    }

    LuaFString::LuaFString() {}
    LuaFString::LuaFString(const std::string &str) : data(str) {}
    std::string LuaFString::to_string() const { return data; }
    void LuaFString::empty() { data.clear(); }
    int LuaFString::len() const { return static_cast<int>(data.size()); }
    bool LuaFString::is_empty() const { return data.empty(); }
    void LuaFString::append(const std::string &str) { data += str; }
    int LuaFString::find(const std::string &search) const
    {
        auto pos = data.find(search);
        return pos == std::string::npos ? -1 : static_cast<int>(pos);
    }
    bool LuaFString::starts_with(const std::string &prefix) const
    {
        return data.size() >= prefix.size() && data.compare(0, prefix.size(), prefix) == 0;
    }
    bool LuaFString::ends_with(const std::string &suffix) const
    {
        return data.size() >= suffix.size() && data.compare(data.size() - suffix.size(), suffix.size(), suffix) == 0;
    }
    LuaFString LuaFString::to_upper() const
    {
        LuaFString result;
        result.data.reserve(data.size());
        for (char c : data)
            result.data += static_cast<char>(toupper(static_cast<unsigned char>(c)));
        return result;
    }
    LuaFString LuaFString::to_lower() const
    {
        LuaFString result;
        result.data.reserve(data.size());
        for (char c : data)
            result.data += static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return result;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // RemoteUnrealParam — wraps hook callback arguments (UE4SS-compatible)
    // ═══════════════════════════════════════════════════════════════════════

    void register_remote_unreal_param(sol::state &lua)
    {
        lua.new_usertype<LuaRemoteUnrealParam>("RemoteUnrealParam",
                                               sol::no_constructor,
                                               "get", &LuaRemoteUnrealParam::get,
                                               "Get", &LuaRemoteUnrealParam::get,
                                               "set", &LuaRemoteUnrealParam::set,
                                               "type", &LuaRemoteUnrealParam::type_name,
                                               sol::meta_function::to_string, [](const LuaRemoteUnrealParam &self) -> std::string
                                               { return "RemoteUnrealParam(" + self.type_name() + ")"; });
    }

    sol::object LuaRemoteUnrealParam::get(sol::this_state ts) const
    {
        sol::state_view lua(ts);
        switch (param_type)
        {
        case ParamType::UObjectPtr:
        {
            ue::UObject *obj = reinterpret_cast<ue::UObject *>(ptr);
            if (!obj || !ue::is_valid_ptr(obj))
                return sol::nil;
            lua_uobject::LuaUObject wrapped;
            wrapped.ptr = obj;
            return sol::make_object(lua, wrapped);
        }
        case ParamType::Bool:
            return sol::make_object(lua, *reinterpret_cast<bool *>(ptr));
        case ParamType::Int32:
            return sol::make_object(lua, *reinterpret_cast<int32_t *>(ptr));
        case ParamType::Int64:
            return sol::make_object(lua, static_cast<double>(*reinterpret_cast<int64_t *>(ptr)));
        case ParamType::Float:
            return sol::make_object(lua, *reinterpret_cast<float *>(ptr));
        case ParamType::Double:
            return sol::make_object(lua, *reinterpret_cast<double *>(ptr));
        case ParamType::FNameVal:
        {
            int32_t idx = *reinterpret_cast<int32_t *>(ptr);
            LuaFName fn(idx);
            return sol::make_object(lua, fn);
        }
        case ParamType::FStringVal:
        {
            // FString: { char16_t* data, int32 num, int32 max }
            struct FStr
            {
                char16_t *data;
                int32_t num;
                int32_t max;
            };
            FStr *fstr = reinterpret_cast<FStr *>(ptr);
            if (fstr->data && fstr->num > 0)
            {
                std::string utf8;
                for (int i = 0; i < fstr->num - 1; i++)
                {
                    char16_t c = fstr->data[i];
                    if (c < 0x80)
                        utf8 += static_cast<char>(c);
                    else if (c < 0x800)
                    {
                        utf8 += static_cast<char>(0xC0 | (c >> 6));
                        utf8 += static_cast<char>(0x80 | (c & 0x3F));
                    }
                    else
                    {
                        utf8 += static_cast<char>(0xE0 | (c >> 12));
                        utf8 += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                        utf8 += static_cast<char>(0x80 | (c & 0x3F));
                    }
                }
                return sol::make_object(lua, LuaFString(utf8));
            }
            return sol::make_object(lua, LuaFString(""));
        }
        case ParamType::Byte:
            return sol::make_object(lua, static_cast<int>(*reinterpret_cast<uint8_t *>(ptr)));
        case ParamType::RawPtr:
            return sol::make_object(lua, sol::lightuserdata_value(ptr));
        default:
            return sol::make_object(lua, sol::lightuserdata_value(ptr));
        }
    }

    void LuaRemoteUnrealParam::set(sol::object value)
    {
        if (!ptr)
            return;
        switch (param_type)
        {
        case ParamType::UObjectPtr:
            if (value.is<lua_uobject::LuaUObject>())
            {
                *reinterpret_cast<ue::UObject **>(ptr) = value.as<lua_uobject::LuaUObject &>().ptr;
            }
            break;
        case ParamType::Bool:
            *reinterpret_cast<bool *>(ptr) = value.as<bool>();
            break;
        case ParamType::Int32:
            *reinterpret_cast<int32_t *>(ptr) = value.as<int32_t>();
            break;
        case ParamType::Float:
            *reinterpret_cast<float *>(ptr) = value.as<float>();
            break;
        case ParamType::Double:
            *reinterpret_cast<double *>(ptr) = value.as<double>();
            break;
        case ParamType::Byte:
            *reinterpret_cast<uint8_t *>(ptr) = static_cast<uint8_t>(value.as<int>());
            break;
        default:
            logger::log_warn("PARAM", "Cannot set RemoteUnrealParam of type %d", static_cast<int>(param_type));
            break;
        }
    }

    std::string LuaRemoteUnrealParam::type_name() const
    {
        switch (param_type)
        {
        case ParamType::UObjectPtr:
            return "UObject";
        case ParamType::Bool:
            return "bool";
        case ParamType::Int32:
            return "int32";
        case ParamType::Int64:
            return "int64";
        case ParamType::Float:
            return "float";
        case ParamType::Double:
            return "double";
        case ParamType::FNameVal:
            return "FName";
        case ParamType::FStringVal:
            return "FString";
        case ParamType::Byte:
            return "byte";
        case ParamType::RawPtr:
            return "pointer";
        default:
            return "unknown";
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // FWeakObjectPtr
    // ═══════════════════════════════════════════════════════════════════════

    void register_fweakobjectptr(sol::state &lua)
    {
        lua.new_usertype<LuaFWeakObjectPtr>("FWeakObjectPtr",
                                            sol::no_constructor,
                                            "get", &LuaFWeakObjectPtr::get,
                                            "Get", &LuaFWeakObjectPtr::get,
                                            "IsValid", &LuaFWeakObjectPtr::is_valid,
                                            sol::meta_function::to_string, [](const LuaFWeakObjectPtr &self) -> std::string
                                            { return "FWeakObjectPtr(" + std::string(self.is_valid() ? "valid" : "invalid") + ")"; });
    }

    sol::object LuaFWeakObjectPtr::get(sol::this_state ts) const
    {
        sol::state_view lua(ts);
        if (!ptr || !ue::is_valid_ptr(ptr))
            return sol::nil;
        lua_uobject::LuaUObject wrapped;
        wrapped.ptr = ptr;
        return sol::make_object(lua, wrapped);
    }

    bool LuaFWeakObjectPtr::is_valid() const
    {
        return ptr && ue::is_valid_ptr(ptr) && ue::is_valid_uobject(ptr);
    }

    // ═══════════════════════════════════════════════════════════════════════
    // FOutputDevice
    // ═══════════════════════════════════════════════════════════════════════

    void register_foutputdevice(sol::state &lua)
    {
        lua.new_usertype<LuaFOutputDevice>("FOutputDevice",
                                           sol::no_constructor,
                                           "Log", &LuaFOutputDevice::log,
                                           sol::meta_function::to_string, [](const LuaFOutputDevice &) -> std::string
                                           { return "FOutputDevice"; });
    }

    void LuaFOutputDevice::log(const std::string &msg)
    {
        if (!device)
        {
            logger::log_info("LUA", "%s", msg.c_str());
            return;
        }
        // UE4's FOutputDevice::Log(TEXT("..."))
        if (symbols::FOutputDevice_Log)
        {
            std::u16string wmsg(msg.begin(), msg.end());
            symbols::FOutputDevice_Log(device, wmsg.c_str());
        }
        else
        {
            logger::log_info("LUA", "%s", msg.c_str());
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // ThreadId
    // ═══════════════════════════════════════════════════════════════════════

    void register_thread_id(sol::state &lua)
    {
        lua.new_usertype<LuaThreadId>("ThreadId", sol::no_constructor, "ToString", [](const LuaThreadId &self) -> std::string
                                      {
            std::ostringstream oss;
            oss << self.id;
            return oss.str(); }, sol::meta_function::to_string, [](const LuaThreadId &self) -> std::string
                                      {
            std::ostringstream oss;
            oss << self.id;
            return oss.str(); }, sol::meta_function::equal_to, [](const LuaThreadId &a, const LuaThreadId &b) -> bool
                                      { return a.id == b.id; });
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Register ALL types
    // ═══════════════════════════════════════════════════════════════════════

    void register_all(sol::state &lua)
    {
        register_fname(lua);
        register_ftext(lua);
        register_fstring(lua);
        register_remote_unreal_param(lua);
        register_fweakobjectptr(lua);
        register_foutputdevice(lua);
        register_thread_id(lua);

        logger::log_info("LUA", "UE4SS-compatible types registered: FName, FText, FString, "
                                "RemoteUnrealParam, FWeakObjectPtr, FOutputDevice, ThreadId");
    }

} // namespace lua_types
