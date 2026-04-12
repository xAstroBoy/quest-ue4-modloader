// modloader/include/modloader/lua_tarray.h
// UE4SS-compatible TArray, TMap, TSet Lua usertypes
// Provides typed element access through UE4 reflection

#pragma once

#include <sol/sol.hpp>
#include <cstdint>
#include <string>

namespace ue
{
    struct FProperty;
}

namespace lua_tarray
{

    // ═══════════════════════════════════════════════════════════════════════
    // TArray Lua wrapper — typed array with inner FProperty dispatch
    // ═══════════════════════════════════════════════════════════════════════

    struct LuaTArray
    {
        void *array_ptr;           // Pointer to the live TArray<> struct in UE memory
        ue::FProperty *inner_prop; // The inner element FProperty (for type dispatch)
        int32_t element_size;      // Cached element size

        int32_t num() const;
        int32_t max() const;
        void *data() const;
        bool is_valid() const;
    };

    // ═══════════════════════════════════════════════════════════════════════
    // TMap Lua wrapper
    // ═══════════════════════════════════════════════════════════════════════

    struct LuaTMap
    {
        void *map_ptr;             // Pointer to the live TMap<> in UE memory
        ue::FProperty *key_prop;   // Key FProperty
        ue::FProperty *value_prop; // Value FProperty
        int32_t key_size;
        int32_t value_size;

        // Layout info from FMapProperty (read once at creation time)
        int32_t key_offset;   // Offset of key within set element (from SetLayout.ElementOffset + MapLayout.KeyOffset)
        int32_t value_offset; // Offset of value within set element (from SetLayout.ElementOffset + MapLayout.ValueOffset)
        int32_t entry_stride; // Total size of one set element slot (from SetLayout.Size)

        bool is_valid() const;
    };

    // ═══════════════════════════════════════════════════════════════════════
    // Registration
    // ═══════════════════════════════════════════════════════════════════════

    void register_all(sol::state &lua);

    // Read a single element from raw memory using an FProperty for type dispatch
    sol::object read_element(sol::state_view lua, const void *element_ptr, ue::FProperty *prop);

    // Write a single element to raw memory using an FProperty for type dispatch
    void write_element(void *element_ptr, ue::FProperty *prop, const sol::object &value);

} // namespace lua_tarray
