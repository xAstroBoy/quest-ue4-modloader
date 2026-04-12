// modloader/src/lua/lua_enum_ext.cpp
// Enum extension API — FindEnum, GetEnumTable, AppendEnumValue, lazy Enums table
//
// AppendEnumValue memory-edits the live UEnum's TArray<TPair<FName,int64>>
// to add new named values.  This updates reflection metadata so:
//   • UEnumMethods.ForEachName sees the new entry
//   • UEnumMethods.GetNameByValue returns the new name
//   • Enums.X table includes the new entry (after cache invalidation)
//
// NOTE: Appending to UEnum does NOT create new Blueprint logic.
// The game's switch/branch per enum value is compiled into bytecode.
// New values need explicit handling in hook code (e.g. DebugMenuAPI).

#include "modloader/lua_enum_ext.h"
#include "modloader/reflection_walker.h"
#include "modloader/types.h"
#include "modloader/symbols.h"
#include "modloader/logger.h"

#include <cstring>
#include <string>
#include <vector>

namespace lua_enum_ext
{

    // ═══════════════════════════════════════════════════════════════════════
    // FindEnum("DebugMenuType") → lightuserdata (UEnum*) or nil
    // ═══════════════════════════════════════════════════════════════════════
    static sol::object find_enum_lua(sol::state &lua, const std::string &name)
    {
        auto *info = reflection::find_enum(name);
        if (info && info->raw)
        {
            return sol::make_object(lua, sol::lightuserdata_value(info->raw));
        }
        return sol::make_object(lua, sol::nil);
    }

    // ═══════════════════════════════════════════════════════════════════════
    // GetEnumTable("DebugMenuType") → {ValueName = intValue, ...} or nil
    // ═══════════════════════════════════════════════════════════════════════
    static sol::object get_enum_table(sol::state &lua, const std::string &name)
    {
        auto *info = reflection::find_enum(name);
        if (!info)
            return sol::make_object(lua, sol::nil);

        sol::table t = lua.create_table(0, static_cast<int>(info->values.size()));
        for (const auto &[vname, vval] : info->values)
        {
            t[vname] = vval;
        }
        return sol::make_object(lua, t);
    }

    // ═══════════════════════════════════════════════════════════════════════
    // GetEnumNames() → {"DebugMenuType", "ECollisionChannel", ...}
    // ═══════════════════════════════════════════════════════════════════════
    static sol::table get_enum_names(sol::state &lua)
    {
        const auto &all = reflection::get_enums();
        sol::table t = lua.create_table(static_cast<int>(all.size()), 0);
        for (size_t i = 0; i < all.size(); i++)
        {
            t[static_cast<int>(i + 1)] = all[i].name; // 1-indexed Lua array
        }
        return t;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // AppendEnumValue("DebugMenuType", "ModsPage", 99) → true/false
    //
    // Memory layout of UEnum's names TArray:
    //   base + 0x40 : void*  Data   (ptr to TPair<FName,int64>[])
    //   base + 0x48 : int32  Num    (element count)
    //   base + 0x4C : int32  Max    (capacity)
    //   Each entry  : 16 bytes = FName(8) + int64(8)
    // ═══════════════════════════════════════════════════════════════════════
    static bool append_enum_value(const std::string &enum_name,
                                  const std::string &value_name,
                                  int64_t value)
    {
        // --- find the enum ---
        auto *info = reflection::find_enum(enum_name);
        if (!info || !info->raw)
        {
            logger::log_warn("ENUM", "AppendEnumValue: enum '%s' not found",
                             enum_name.c_str());
            return false;
        }

        if (!symbols::FName_Init)
        {
            logger::log_warn("ENUM", "AppendEnumValue: FName_Init not resolved");
            return false;
        }

        // --- check for duplicate value ---
        for (const auto &[vn, vv] : info->values)
        {
            if (vn == value_name || vv == value)
            {
                logger::log_warn("ENUM",
                                 "AppendEnumValue: '%s'=%lld already exists in %s (as '%s'=%lld)",
                                 value_name.c_str(), (long long)value,
                                 enum_name.c_str(), vn.c_str(), (long long)vv);
                return false;
            }
        }

        ue::UEnum *ue_enum = info->raw;
        uintptr_t base = reinterpret_cast<uintptr_t>(ue_enum);

        // --- read TArray state ---
        uintptr_t data_ptr = ue::read_field<uintptr_t>(ue_enum, ue::uenum::NAMES_DATA);
        int32_t num = ue::read_field<int32_t>(ue_enum, ue::uenum::NAMES_NUM);
        int32_t max_cap = ue::read_field<int32_t>(ue_enum, ue::uenum::NAMES_MAX);

        // --- create FName for "EnumName::ValueName" ---
        std::string full_name = enum_name + "::" + value_name;
        ue::FName fname;
        // Android ARM64: TCHAR = char16_t (2 bytes), NOT wchar_t (4 bytes)
        std::u16string u16full(full_name.begin(), full_name.end());
        symbols::FName_Init(&fname, u16full.c_str(), 0);

        if (num < max_cap && ue::is_valid_ptr(reinterpret_cast<void *>(data_ptr)))
        {
            // ── capacity available → write in-place ──
            uintptr_t entry = data_ptr + static_cast<uintptr_t>(num) * ue::uenum::ENUM_ENTRY_SIZE;
            std::memcpy(reinterpret_cast<void *>(entry), &fname, sizeof(ue::FName));
            *reinterpret_cast<int64_t *>(entry + sizeof(ue::FName)) = value;

            // bump count
            *reinterpret_cast<int32_t *>(base + ue::uenum::NAMES_NUM) = num + 1;
        }
        else
        {
            // ── need to reallocate ──
            int32_t new_max = (max_cap < 4) ? 8 : max_cap * 2;
            size_t new_bytes = static_cast<size_t>(new_max) * ue::uenum::ENUM_ENTRY_SIZE;

            uint8_t *new_data = static_cast<uint8_t *>(malloc(new_bytes));
            if (!new_data)
            {
                logger::log_error("ENUM", "AppendEnumValue: malloc(%zu) failed", new_bytes);
                return false;
            }
            std::memset(new_data, 0, new_bytes);

            // copy old entries
            if (ue::is_valid_ptr(reinterpret_cast<void *>(data_ptr)) && num > 0)
            {
                std::memcpy(new_data, reinterpret_cast<void *>(data_ptr),
                            static_cast<size_t>(num) * ue::uenum::ENUM_ENTRY_SIZE);
            }

            // write new entry at end
            uintptr_t entry = reinterpret_cast<uintptr_t>(new_data) + static_cast<uintptr_t>(num) * ue::uenum::ENUM_ENTRY_SIZE;
            std::memcpy(reinterpret_cast<void *>(entry), &fname, sizeof(ue::FName));
            *reinterpret_cast<int64_t *>(entry + sizeof(ue::FName)) = value;

            // update TArray header in the UEnum object
            *reinterpret_cast<uintptr_t *>(base + ue::uenum::NAMES_DATA) =
                reinterpret_cast<uintptr_t>(new_data);
            *reinterpret_cast<int32_t *>(base + ue::uenum::NAMES_NUM) = num + 1;
            *reinterpret_cast<int32_t *>(base + ue::uenum::NAMES_MAX) = new_max;

            // NOTE: old Data buffer is intentionally leaked.
            // We cannot safely call FMemory::Free on UE4 allocations.
        }

        // --- update our walker cache so find_enum/get_enums stays current ---
        info->values.push_back({value_name, value});

        logger::log_info("ENUM", "Appended %s::%s = %lld  (total: %d entries)",
                         enum_name.c_str(), value_name.c_str(),
                         (long long)value, num + 1);
        return true;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // register_all — wire everything into Lua
    // ═══════════════════════════════════════════════════════════════════════
    void register_all(sol::state &lua)
    {

        // -- global functions --
        lua.set_function("FindEnum", [&lua](const std::string &name) -> sol::object
                         { return find_enum_lua(lua, name); });

        lua.set_function("GetEnumTable", [&lua](const std::string &name) -> sol::object
                         { return get_enum_table(lua, name); });

        lua.set_function("GetEnumNames", [&lua]() -> sol::table
                         { return get_enum_names(lua); });

        lua.set_function("AppendEnumValue",
                         [&lua](const std::string &enum_name,
                                const std::string &value_name,
                                int64_t value) -> bool
                         {
                             bool ok = append_enum_value(enum_name, value_name, value);
                             if (ok)
                             {
                                 // Invalidate the Lua-side cache for this enum
                                 // so next Enums.X access re-fetches from walker cache
                                 sol::object enums_obj = lua["Enums"];
                                 if (enums_obj.is<sol::table>())
                                 {
                                     sol::table enums = enums_obj;
                                     enums[enum_name] = sol::nil;
                                 }
                             }
                             return ok;
                         });

        // -- lazy Enums global table --
        // Enums.DebugMenuType → calls GetEnumTable("DebugMenuType") on first access,
        // then caches the result.  After AppendEnumValue, the cache slot is cleared
        // so the next read picks up the new entry.
        const char *enums_init = R"lua(
Enums = setmetatable({}, {
    __index = function(self, name)
        local t = GetEnumTable(name)
        if t then
            rawset(self, name, t)
            return t
        end
        return nil
    end
})
)lua";
        auto result = lua.safe_script(enums_init, sol::script_pass_on_error, "Enums_init");
        if (!result.valid())
        {
            sol::error err = result;
            logger::log_error("ENUM", "Enums table init failed: %s", err.what());
        }

        logger::log_info("ENUM", "Enum extension API registered "
                                 "(FindEnum, GetEnumTable, GetEnumNames, AppendEnumValue, Enums.*)");
    }

} // namespace lua_enum_ext
