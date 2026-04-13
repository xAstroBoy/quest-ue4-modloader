// modloader/src/lua/lua_tarray.cpp
// UE4SS-compatible TArray, TMap, TSet Lua usertypes
// Element type dispatch via FProperty reflection — reads/writes individual
// elements from raw UE4 memory using the same type classification system
// as the rest of the modloader.

#include "modloader/lua_tarray.h"
#include "modloader/lua_uobject.h"
#include "modloader/lua_ustruct.h"
#include "modloader/lua_types.h"
#include "modloader/reflection_walker.h"
#include "modloader/symbols.h"
#include "modloader/logger.h"
#include "modloader/types.h"

#include <cstring>
#include <vector>

namespace lua_tarray
{

    // ═══════════════════════════════════════════════════════════════════════
    // UE4 TArray memory layout: { void* Data; int32 ArrayNum; int32 ArrayMax; }
    // Total: 16 bytes on ARM64
    // ═══════════════════════════════════════════════════════════════════════

    struct RawTArray
    {
        void *data;
        int32_t num;
        int32_t max;
    };

    // ═══════════════════════════════════════════════════════════════════════
    // UE4 TMap/TSet memory layout (FScriptMap/FScriptSet):
    // TSet: { FScriptSparseArray Elements; FHashAllocator Hash; }
    // FScriptSparseArray: { void* Data; int32 MaxElements; int32 NumElements;
    //                       FSparseArrayAllocationInfo FirstFreeIndex; }
    // Simplified — we iterate valid elements via sparse array metadata
    // ═══════════════════════════════════════════════════════════════════════

    // ═══════════════════════════════════════════════════════════════════════
    // Element type dispatch — read from raw memory via FProperty
    // ═══════════════════════════════════════════════════════════════════════

    sol::object read_element(sol::state_view lua, const void *element_ptr, ue::FProperty *prop)
    {
        if (!element_ptr || !prop)
            return sol::nil;
        // Validate prop pointer is actually mapped before dereferencing
        if (!ue::is_mapped_ptr(prop))
            return sol::nil;

        const uint8_t *base = reinterpret_cast<const uint8_t *>(element_ptr);

        // Classify the property
        reflection::PropType pt = reflection::classify_property(reinterpret_cast<const ue::FField *>(prop));
        int32_t elem_size = ue::fprop_get_element_size(prop);

        switch (pt)
        {
        case reflection::PropType::BoolProperty:
        {
            uint8_t field_mask = ue::read_field<uint8_t>(prop, ue::fprop::BOOL_FIELD_MASK_OFF());
            if (field_mask != 0xFF)
            {
                return sol::make_object(lua, (base[0] & field_mask) != 0);
            }
            return sol::make_object(lua, *reinterpret_cast<const bool *>(base));
        }

        case reflection::PropType::ByteProperty:
            return sol::make_object(lua, static_cast<int>(base[0]));

        case reflection::PropType::Int8Property:
            return sol::make_object(lua, static_cast<int>(*reinterpret_cast<const int8_t *>(base)));

        case reflection::PropType::Int16Property:
            return sol::make_object(lua, static_cast<int>(*reinterpret_cast<const int16_t *>(base)));

        case reflection::PropType::UInt16Property:
            return sol::make_object(lua, static_cast<int>(*reinterpret_cast<const uint16_t *>(base)));

        case reflection::PropType::IntProperty:
            return sol::make_object(lua, *reinterpret_cast<const int32_t *>(base));

        case reflection::PropType::UInt32Property:
            return sol::make_object(lua, static_cast<double>(*reinterpret_cast<const uint32_t *>(base)));

        case reflection::PropType::Int64Property:
            return sol::make_object(lua, static_cast<double>(*reinterpret_cast<const int64_t *>(base)));

        case reflection::PropType::UInt64Property:
            return sol::make_object(lua, static_cast<double>(*reinterpret_cast<const uint64_t *>(base)));

        case reflection::PropType::FloatProperty:
            return sol::make_object(lua, *reinterpret_cast<const float *>(base));

        case reflection::PropType::DoubleProperty:
            return sol::make_object(lua, *reinterpret_cast<const double *>(base));

        case reflection::PropType::NameProperty:
        {
            int32_t fname_idx = *reinterpret_cast<const int32_t *>(base);
            // Return plain string for consistency with UObject/UStruct access
            return sol::make_object(lua, reflection::fname_to_string(fname_idx));
        }

        case reflection::PropType::StrProperty:
        {
            // Return plain UTF-8 string using shared utility
            return sol::make_object(lua, lua_uobject::fstring_to_utf8(base));
        }

        case reflection::PropType::TextProperty:
        {
            // FText is 24 bytes — read via KismetTextLibrary::Conv_TextToString
            std::string utf8 = lua_uobject::ftext_to_string(base);
            return sol::make_object(lua, utf8);
        }

        case reflection::PropType::ObjectProperty:
        case reflection::PropType::WeakObjectProperty:
        case reflection::PropType::SoftObjectProperty:
        case reflection::PropType::LazyObjectProperty:
        case reflection::PropType::InterfaceProperty:
        case reflection::PropType::ClassProperty:
        {
            ue::UObject *obj = *reinterpret_cast<ue::UObject *const *>(base);
            if (!obj || !ue::is_valid_ptr(obj))
                return sol::nil;
            lua_uobject::LuaUObject wrapped;
            wrapped.ptr = obj;
            return sol::make_object(lua, wrapped);
        }

        case reflection::PropType::StructProperty:
        {
            // Return as LuaUStruct with typed field access via reflection
            ue::UStruct *inner_struct = ue::read_field<ue::UStruct *>(prop, ue::fprop::STRUCT_INNER_STRUCT_OFF());
            if (inner_struct)
            {
                lua_ustruct::LuaUStruct s;
                s.data = const_cast<uint8_t *>(base);
                s.ustruct = inner_struct;
                s.size = elem_size;
                s.owns_data = false;
                return sol::make_object(lua, s);
            }
            return sol::make_object(lua, sol::lightuserdata_value(const_cast<uint8_t *>(base)));
        }

        case reflection::PropType::EnumProperty:
        {
            if (elem_size == 1)
                return sol::make_object(lua, static_cast<int>(base[0]));
            if (elem_size == 2)
                return sol::make_object(lua, static_cast<int>(*reinterpret_cast<const int16_t *>(base)));
            if (elem_size == 4)
                return sol::make_object(lua, *reinterpret_cast<const int32_t *>(base));
            return sol::make_object(lua, static_cast<int>(base[0]));
        }

        case reflection::PropType::ArrayProperty:
        {
            // Nested TArray — return as LuaTArray
            ue::FProperty *nested_inner = ue::read_field<ue::FProperty *>(prop, ue::fprop::ARRAY_INNER_OFF());
            // Validate nested inner prop is actually mapped
            if (nested_inner && !ue::is_mapped_ptr(nested_inner))
                nested_inner = nullptr;
            LuaTArray nested;
            nested.array_ptr = const_cast<uint8_t *>(base);
            nested.inner_prop = nested_inner;
            nested.element_size = nested_inner ? ue::fprop_get_element_size(nested_inner) : 0;
            return sol::make_object(lua, nested);
        }

        default:
            return sol::make_object(lua, sol::lightuserdata_value(const_cast<uint8_t *>(base)));
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Element type dispatch — write to raw memory via FProperty
    // ═══════════════════════════════════════════════════════════════════════

    void write_element(void *element_ptr, ue::FProperty *prop, const sol::object &value)
    {
        if (!element_ptr || !prop)
            return;
        // Validate prop pointer is actually mapped before dereferencing
        if (!ue::is_mapped_ptr(prop))
            return;

        uint8_t *base = reinterpret_cast<uint8_t *>(element_ptr);
        reflection::PropType pt = reflection::classify_property(reinterpret_cast<const ue::FField *>(prop));
        int32_t elem_size = ue::fprop_get_element_size(prop);

        switch (pt)
        {
        case reflection::PropType::BoolProperty:
        {
            uint8_t field_mask = ue::read_field<uint8_t>(prop, ue::fprop::BOOL_FIELD_MASK_OFF());
            bool val = value.as<bool>();
            if (field_mask != 0xFF)
            {
                if (val)
                    base[0] |= field_mask;
                else
                    base[0] &= ~field_mask;
            }
            else
            {
                *reinterpret_cast<bool *>(base) = val;
            }
            break;
        }
        case reflection::PropType::ByteProperty:
        case reflection::PropType::Int8Property:
            base[0] = static_cast<uint8_t>(value.as<int>());
            break;
        case reflection::PropType::Int16Property:
        case reflection::PropType::UInt16Property:
            *reinterpret_cast<int16_t *>(base) = static_cast<int16_t>(value.as<int>());
            break;
        case reflection::PropType::IntProperty:
            *reinterpret_cast<int32_t *>(base) = value.as<int32_t>();
            break;
        case reflection::PropType::UInt32Property:
            *reinterpret_cast<uint32_t *>(base) = static_cast<uint32_t>(value.as<double>());
            break;
        case reflection::PropType::Int64Property:
            *reinterpret_cast<int64_t *>(base) = static_cast<int64_t>(value.as<double>());
            break;
        case reflection::PropType::UInt64Property:
            *reinterpret_cast<uint64_t *>(base) = static_cast<uint64_t>(value.as<double>());
            break;
        case reflection::PropType::FloatProperty:
            *reinterpret_cast<float *>(base) = value.as<float>();
            break;
        case reflection::PropType::DoubleProperty:
            *reinterpret_cast<double *>(base) = value.as<double>();
            break;
        case reflection::PropType::ObjectProperty:
        case reflection::PropType::WeakObjectProperty:
        case reflection::PropType::ClassProperty:
        {
            if (value.is<lua_uobject::LuaUObject>())
            {
                *reinterpret_cast<ue::UObject **>(base) = value.as<lua_uobject::LuaUObject &>().ptr;
            }
            else if (value == sol::nil)
            {
                *reinterpret_cast<ue::UObject **>(base) = nullptr;
            }
            break;
        }
        case reflection::PropType::EnumProperty:
        {
            if (elem_size == 1)
                base[0] = static_cast<uint8_t>(value.as<int>());
            else if (elem_size == 2)
                *reinterpret_cast<int16_t *>(base) = static_cast<int16_t>(value.as<int>());
            else
                *reinterpret_cast<int32_t *>(base) = value.as<int32_t>();
            break;
        }
        case reflection::PropType::NameProperty:
        {
            // FName is { int32 ComparisonIndex, int32 Number } = 8 bytes
            int32_t comp_idx = 0;
            int32_t number = 0;
            if (value.is<lua_types::LuaFName>())
            {
                auto &fn = value.as<lua_types::LuaFName &>();
                comp_idx = fn.comparison_index;
                number = fn.number;
            }
            else if (value.is<std::string>())
            {
                std::string s = value.as<std::string>();
                comp_idx = reflection::fname_string_to_index(s);
                if (comp_idx == 0 && symbols::FName_Init)
                {
                    ue::FName fname;
                    // On Android ARM64, TCHAR = char16_t (2 bytes), NOT wchar_t (4 bytes)
                    std::u16string u16name(s.begin(), s.end());
                    symbols::FName_Init(&fname, u16name.c_str(), 0);
                    comp_idx = fname.ComparisonIndex;
                }
            }
            else if (value.is<int32_t>())
            {
                comp_idx = value.as<int32_t>();
            }
            *reinterpret_cast<int32_t *>(base) = comp_idx;
            if (elem_size >= 8)
            {
                *reinterpret_cast<int32_t *>(base + 4) = number;
            }
            break;
        }
        case reflection::PropType::StrProperty:
        {
            // FString write using shared utility (safe allocator)
            if (value.is<std::string>())
            {
                lua_uobject::fstring_from_utf8(base, value.as<std::string>());
            }
            else if (value.is<lua_types::LuaFString>())
            {
                lua_uobject::fstring_from_utf8(base, value.as<lua_types::LuaFString &>().to_string());
            }
            break;
        }
        case reflection::PropType::TextProperty:
        {
            // FText write using shared utility (via Kismet ProcessEvent)
            std::string str;
            if (value.is<std::string>())
                str = value.as<std::string>();
            else if (value.is<lua_types::LuaFText>())
                str = value.as<lua_types::LuaFText &>().to_string();
            else if (value.is<lua_types::LuaFString>())
                str = value.as<lua_types::LuaFString &>().to_string();
            else
                break;
            lua_uobject::ftext_from_string(base, str);
            break;
        }
        case reflection::PropType::StructProperty:
        {
            // For struct writes, support table → struct via reflection (same as Set())
            // or LuaUStruct → memcpy
            if (value.is<lua_ustruct::LuaUStruct>())
            {
                auto &src = value.as<lua_ustruct::LuaUStruct &>();
                int copy_size = (src.size < elem_size) ? src.size : elem_size;
                std::memcpy(base, src.data, copy_size);
            }
            else if (value.is<sol::table>())
            {
                // Fill struct from table using reflection
                ue::UStruct *inner_struct = ue::read_field<ue::UStruct *>(prop, ue::fprop::STRUCT_INNER_STRUCT_OFF());
                if (inner_struct)
                {
                    sol::table tbl = value.as<sol::table>();
                    ue::FField *child = ue::read_field<ue::FField *>(inner_struct, ue::ustruct::CHILDREN_OFF());
                    while (child)
                    {
                        int32_t name_idx = ue::ffield_get_name_index(child);
                        std::string field_name_str = reflection::fname_to_string(name_idx);
                        const char *field_name = field_name_str.c_str();
                        if (field_name && field_name_str.length() > 0)
                        {
                            sol::object field_val = tbl[field_name];
                            if (field_val.valid() && field_val != sol::nil)
                            {
                                int32_t field_offset = ue::read_field<int32_t>(child, ue::fprop::OFFSET_INTERNAL_OFF());
                                write_element(base + field_offset, reinterpret_cast<ue::FProperty *>(child), field_val);
                            }
                        }
                        child = ue::read_field<ue::FField *>(child, ue::ffield::NEXT_OFF());
                    }
                }
            }
            break;
        }
        default:
            logger::log_warn("TARRAY", "Cannot write to element of type %d", static_cast<int>(pt));
            break;
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // LuaTArray methods
    // ═══════════════════════════════════════════════════════════════════════

    int32_t LuaTArray::num() const
    {
        if (!array_ptr)
            return 0;
        return reinterpret_cast<const RawTArray *>(array_ptr)->num;
    }

    int32_t LuaTArray::max() const
    {
        if (!array_ptr)
            return 0;
        return reinterpret_cast<const RawTArray *>(array_ptr)->max;
    }

    void *LuaTArray::data() const
    {
        if (!array_ptr)
            return nullptr;
        return reinterpret_cast<const RawTArray *>(array_ptr)->data;
    }

    bool LuaTArray::is_valid() const
    {
        return array_ptr != nullptr;
    }

    bool LuaTMap::is_valid() const
    {
        return map_ptr != nullptr;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Register TArray usertype
    // ═══════════════════════════════════════════════════════════════════════

    static void register_tarray(sol::state &lua)
    {
        lua.new_usertype<LuaTArray>("TArray", sol::no_constructor,

                                    // ── __index (1-indexed, like UE4SS)
                                    // Use sol::object key to handle BOTH integer indices AND string method names
                                    sol::meta_function::index, [](sol::this_state ts, const LuaTArray &self, sol::object key) -> sol::object
                                    {
            sol::state_view lua(ts);

            // If key is a string, look up named methods
            if (key.get_type() == sol::type::string) {
                std::string name = key.as<std::string>();
                // Expose named methods via string key lookup
                if (name == "GetArrayNum") return sol::make_object(lua, self.num());
                if (name == "GetArrayMax") return sol::make_object(lua, self.max());
                if (name == "GetArrayAddress") return sol::make_object(lua, reinterpret_cast<uintptr_t>(self.array_ptr));
                if (name == "GetArrayDataAddress") return sol::make_object(lua, reinterpret_cast<uintptr_t>(self.data()));
                if (name == "IsEmpty") return sol::make_object(lua, self.num() == 0);
                if (name == "IsValid") return sol::make_object(lua, self.is_valid());
                if (name == "Num") return sol::make_object(lua, self.num());
                // ForEach and other function methods will be registered separately
                return sol::nil;
            }

            // Must be a number for array indexing
            if (key.get_type() != sol::type::number) return sol::nil;

            int index = key.as<int>();
            if (!self.array_ptr || !self.inner_prop) {
                logger::log_warn("TARRAY", "[%d] nil: array_ptr=%p inner_prop=%p",
                    index, self.array_ptr, (void*)self.inner_prop);
                return sol::nil;
            }
            int32_t arr_num = self.num();
            // 1-indexed → 0-indexed
            int idx = index - 1;
            if (idx < 0 || idx >= arr_num) {
                logger::log_warn("TARRAY", "[%d] nil: out of bounds (num=%d)", index, arr_num);
                return sol::nil;
            }
            if (self.element_size <= 0) {
                logger::log_warn("TARRAY", "[%d] nil: element_size=%d", index, self.element_size);
                return sol::nil;
            }
            void* data_ptr = self.data();
            if (!data_ptr) {
                logger::log_warn("TARRAY", "[%d] nil: data() returned null", index);
                return sol::nil;
            }
            const uint8_t* elem = reinterpret_cast<const uint8_t*>(data_ptr) + idx * self.element_size;

            // Diagnostic: log first access details
            static int diag_count = 0;
            if (diag_count < 5) {
                diag_count++;
                reflection::PropType pt = reflection::classify_property(reinterpret_cast<const ue::FField *>(self.inner_prop));
                logger::log_info("TARRAY", "DIAG[%d]: idx=%d elem_ptr=%p elem_sz=%d proptype=%d data=%p num=%d",
                    index, idx, elem, self.element_size, (int)pt, data_ptr, arr_num);
                // For ObjectProperty, log the raw pointer value
                if (pt == reflection::PropType::ObjectProperty ||
                    pt == reflection::PropType::WeakObjectProperty ||
                    pt == reflection::PropType::SoftObjectProperty) {
                    uintptr_t raw_val = *reinterpret_cast<const uintptr_t*>(elem);
                    logger::log_info("TARRAY", "DIAG[%d]: raw UObject* = 0x%lx is_valid=%d is_mapped=%d",
                        index, raw_val, ue::is_valid_ptr((void*)raw_val), ue::is_mapped_ptr((void*)raw_val));
                }
            }

            return read_element(lua, elem, self.inner_prop); },

                                    // ── __newindex (1-indexed)
                                    sol::meta_function::new_index, [](LuaTArray &self, int index, sol::object value)
                                    {
            if (!self.array_ptr || !self.inner_prop) return;
            int32_t arr_num = self.num();
            int idx = index - 1;
            if (idx < 0 || idx >= arr_num) {
                logger::log_warn("TARRAY", "Write out of bounds: index %d, num %d", index, arr_num);
                return;
            }
            if (self.element_size <= 0) return;
            uint8_t* elem = reinterpret_cast<uint8_t*>(self.data()) + idx * self.element_size;
            write_element(elem, self.inner_prop, value); },

                                    // ── __len
                                    sol::meta_function::length, [](const LuaTArray &self) -> int
                                    { return self.num(); },

                                    // ── __tostring
                                    sol::meta_function::to_string, [](const LuaTArray &self) -> std::string
                                    {
            char buf[64];
            snprintf(buf, sizeof(buf), "TArray(Num=%d, Max=%d)", self.num(), self.max());
            return std::string(buf); },

                                    // ── Methods
                                    "GetArrayNum", &LuaTArray::num, "GetArrayMax", &LuaTArray::max,

                                    "GetArrayAddress", [](const LuaTArray &self) -> uintptr_t
                                    { return reinterpret_cast<uintptr_t>(self.array_ptr); },

                                    "GetArrayDataAddress", [](const LuaTArray &self) -> uintptr_t
                                    { return reinterpret_cast<uintptr_t>(self.data()); },

                                    "IsEmpty", [](const LuaTArray &self) -> bool
                                    { return self.num() == 0; },

                                    "IsValid", &LuaTArray::is_valid,

                                    // ForEach — iterate all elements, callback(index, element)
                                    // 1-indexed. Return true from callback to break early.
                                    "ForEach", [](sol::this_state ts, const LuaTArray &self, sol::function callback)
                                    {
            sol::state_view lua(ts);
            if (!self.array_ptr || !self.inner_prop) return;
            int32_t n = self.num();
            if (n <= 0 || self.element_size <= 0) return;
            const uint8_t* base = reinterpret_cast<const uint8_t*>(self.data());
            if (!base) return;

            for (int32_t i = 0; i < n; i++) {
                sol::object elem = read_element(lua, base + i * self.element_size, self.inner_prop);
                auto result = callback(i + 1, elem); // 1-indexed
                if (result.valid() && result.get_type() == sol::type::boolean && result.get<bool>()) {
                    break;
                }
            } },

                                    // Empty / Clear — zero out the array count (does NOT free memory)
                                    "Empty", [](LuaTArray &self)
                                    {
            if (!self.array_ptr) return;
            RawTArray* raw = reinterpret_cast<RawTArray*>(self.array_ptr);
            raw->num = 0; },

                                    "Clear", [](LuaTArray &self)
                                    {
            if (!self.array_ptr) return;
            RawTArray* raw = reinterpret_cast<RawTArray*>(self.array_ptr);
            raw->num = 0; },

                                    // Add — appends an element to the end of the array.
                                    // REQUIRES that Num < Max (there must be pre-allocated capacity).
                                    // For safety, does NOT realloc — UE4 FMemory is not available from Lua.
                                    // Returns the new 1-based index, or 0 on failure.
                                    "Add", [](LuaTArray &self, sol::object value) -> int
                                    {
                                        if (!self.array_ptr || !self.inner_prop)
                                            return 0;
                                        RawTArray *raw = reinterpret_cast<RawTArray *>(self.array_ptr);
                                        if (raw->num >= raw->max)
                                        {
                                            logger::log_warn("TARRAY", "Add: array full (Num=%d, Max=%d) — cannot append",
                                                             raw->num, raw->max);
                                            return 0;
                                        }
                                        if (self.element_size <= 0)
                                            return 0;
                                        uint8_t *elem = reinterpret_cast<uint8_t *>(raw->data) + raw->num * self.element_size;
                                        // Zero the element slot first
                                        std::memset(elem, 0, self.element_size);
                                        write_element(elem, self.inner_prop, value);
                                        raw->num++;
                                        return raw->num; // 1-based index of the new element
                                    },

                                    // AddFName — specialized: appends an FName to an FName[] array.
                                    // Takes a string or LuaFName. Returns new 1-based index or 0 on failure.
                                    "AddFName", [](LuaTArray &self, sol::object value) -> int
                                    {
            if (!self.array_ptr) return 0;
            RawTArray* raw = reinterpret_cast<RawTArray*>(self.array_ptr);
            if (raw->num >= raw->max) {
                logger::log_warn("TARRAY", "AddFName: array full (Num=%d, Max=%d)", raw->num, raw->max);
                return 0;
            }
            // FName is 8 bytes: { int32 ComparisonIndex, int32 Number }
            if (self.element_size < 8 && self.element_size != 4) {
                logger::log_warn("TARRAY", "AddFName: element_size=%d (expected 4 or 8)", self.element_size);
                return 0;
            }
            uint8_t* elem = reinterpret_cast<uint8_t*>(raw->data) + raw->num * self.element_size;
            int32_t comp_idx = 0;
            int32_t number = 0;
            if (value.is<lua_types::LuaFName>()) {
                auto& fn = value.as<lua_types::LuaFName&>();
                comp_idx = fn.comparison_index;
                number = fn.number;
            } else if (value.is<std::string>()) {
                std::string s = value.as<std::string>();
                comp_idx = reflection::fname_string_to_index(s);
                if (comp_idx == 0 && symbols::FName_Init) {
                    ue::FName fname;
                    std::u16string u16name(s.begin(), s.end());
                    symbols::FName_Init(&fname, u16name.c_str(), 0);
                    comp_idx = fname.ComparisonIndex;
                }
            } else if (value.is<int32_t>()) {
                comp_idx = value.as<int32_t>();
            }
            *reinterpret_cast<int32_t*>(elem) = comp_idx;
            if (self.element_size >= 8) {
                *reinterpret_cast<int32_t*>(elem + 4) = number;
            }
            raw->num++;
            return raw->num; },

                                    // RemoveAt — remove element at 1-based index, shifts remaining elements down.
                                    // Returns true if removed successfully.
                                    "RemoveAt", [](LuaTArray &self, int index) -> bool
                                    {
            if (!self.array_ptr || self.element_size <= 0) return false;
            RawTArray* raw = reinterpret_cast<RawTArray*>(self.array_ptr);
            int idx = index - 1;
            if (idx < 0 || idx >= raw->num) {
                logger::log_warn("TARRAY", "RemoveAt: index %d out of bounds (num=%d)", index, raw->num);
                return false;
            }
            uint8_t* data = reinterpret_cast<uint8_t*>(raw->data);
            // Shift elements after idx down by one position
            int remaining = raw->num - idx - 1;
            if (remaining > 0) {
                std::memmove(data + idx * self.element_size,
                           data + (idx + 1) * self.element_size,
                           remaining * self.element_size);
            }
            // Zero out the last element slot (now unused)
            std::memset(data + (raw->num - 1) * self.element_size, 0, self.element_size);
            raw->num--;
            return true; },

                                    // Insert — insert value at 1-based index, shifts existing elements up.
                                    // Requires Num < Max (pre-allocated capacity).
                                    // Returns true if inserted.
                                    "Insert", [](LuaTArray &self, int index, sol::object value) -> bool
                                    {
            if (!self.array_ptr || !self.inner_prop || self.element_size <= 0) return false;
            RawTArray* raw = reinterpret_cast<RawTArray*>(self.array_ptr);
            if (raw->num >= raw->max) {
                logger::log_warn("TARRAY", "Insert: array full (Num=%d, Max=%d)", raw->num, raw->max);
                return false;
            }
            int idx = index - 1;
            if (idx < 0 || idx > raw->num) {
                logger::log_warn("TARRAY", "Insert: index %d out of bounds (num=%d)", index, raw->num);
                return false;
            }
            uint8_t* data = reinterpret_cast<uint8_t*>(raw->data);
            // Shift elements from idx..end up by one position
            int to_shift = raw->num - idx;
            if (to_shift > 0) {
                std::memmove(data + (idx + 1) * self.element_size,
                           data + idx * self.element_size,
                           to_shift * self.element_size);
            }
            // Zero and write the new element
            uint8_t* elem = data + idx * self.element_size;
            std::memset(elem, 0, self.element_size);
            write_element(elem, self.inner_prop, value);
            raw->num++;
            return true; },

                                    // Swap — swap elements at two 1-based indices.
                                    "Swap", [](LuaTArray &self, int idx1, int idx2) -> bool
                                    {
            if (!self.array_ptr || self.element_size <= 0) return false;
            RawTArray* raw = reinterpret_cast<RawTArray*>(self.array_ptr);
            int i1 = idx1 - 1, i2 = idx2 - 1;
            if (i1 < 0 || i1 >= raw->num || i2 < 0 || i2 >= raw->num) return false;
            if (i1 == i2) return true;
            uint8_t* data = reinterpret_cast<uint8_t*>(raw->data);
            // Use stack-allocated temp buffer for small elements, heap for large
            if (self.element_size <= 256) {
                uint8_t tmp[256];
                std::memcpy(tmp, data + i1 * self.element_size, self.element_size);
                std::memcpy(data + i1 * self.element_size, data + i2 * self.element_size, self.element_size);
                std::memcpy(data + i2 * self.element_size, tmp, self.element_size);
            } else {
                std::vector<uint8_t> tmp(self.element_size);
                std::memcpy(tmp.data(), data + i1 * self.element_size, self.element_size);
                std::memcpy(data + i1 * self.element_size, data + i2 * self.element_size, self.element_size);
                std::memcpy(data + i2 * self.element_size, tmp.data(), self.element_size);
            }
            return true; },

                                    // Contains — check if array contains a value (string comparison).
                                    // Works for FName, FString, int, float, bool.
                                    "Contains", [](sol::this_state ts, const LuaTArray &self, sol::object target) -> bool
                                    {
            sol::state_view lua(ts);
            if (!self.array_ptr || !self.inner_prop || self.element_size <= 0) return false;
            int32_t n = self.num();
            if (n <= 0) return false;
            const uint8_t* data = reinterpret_cast<const uint8_t*>(self.data());
            if (!data) return false;

            // Convert target to string for universal comparison
            std::string target_str;
            if (target.is<std::string>()) target_str = target.as<std::string>();
            else if (target.is<int>()) target_str = std::to_string(target.as<int>());
            else if (target.is<double>()) target_str = std::to_string(target.as<double>());
            else if (target.is<bool>()) target_str = target.as<bool>() ? "true" : "false";
            else if (target.is<lua_types::LuaFName>()) target_str = target.as<lua_types::LuaFName&>().to_string();
            else if (target.is<lua_types::LuaFString>()) target_str = target.as<lua_types::LuaFString&>().to_string();
            else return false;

            for (int32_t i = 0; i < n; i++) {
                sol::object elem = read_element(lua, data + i * self.element_size, self.inner_prop);
                std::string elem_str;
                if (elem.is<std::string>()) elem_str = elem.as<std::string>();
                else if (elem.is<int>()) elem_str = std::to_string(elem.as<int>());
                else if (elem.is<double>()) elem_str = std::to_string(elem.as<double>());
                else if (elem.is<bool>()) elem_str = elem.as<bool>() ? "true" : "false";
                else if (elem.is<lua_types::LuaFName>()) elem_str = elem.as<lua_types::LuaFName&>().to_string();
                else if (elem.is<lua_types::LuaFString>()) elem_str = elem.as<lua_types::LuaFString&>().to_string();
                else continue;
                if (elem_str == target_str) return true;
            }
            return false; },

                                    // IndexOf — find 1-based index of first matching value, or 0 if not found.
                                    "IndexOf", [](sol::this_state ts, const LuaTArray &self, sol::object target) -> int
                                    {
            sol::state_view lua(ts);
            if (!self.array_ptr || !self.inner_prop || self.element_size <= 0) return 0;
            int32_t n = self.num();
            if (n <= 0) return 0;
            const uint8_t* data = reinterpret_cast<const uint8_t*>(self.data());
            if (!data) return 0;

            std::string target_str;
            if (target.is<std::string>()) target_str = target.as<std::string>();
            else if (target.is<int>()) target_str = std::to_string(target.as<int>());
            else if (target.is<double>()) target_str = std::to_string(target.as<double>());
            else if (target.is<bool>()) target_str = target.as<bool>() ? "true" : "false";
            else if (target.is<lua_types::LuaFName>()) target_str = target.as<lua_types::LuaFName&>().to_string();
            else if (target.is<lua_types::LuaFString>()) target_str = target.as<lua_types::LuaFString&>().to_string();
            else return 0;

            for (int32_t i = 0; i < n; i++) {
                sol::object elem = read_element(lua, data + i * self.element_size, self.inner_prop);
                std::string elem_str;
                if (elem.is<std::string>()) elem_str = elem.as<std::string>();
                else if (elem.is<int>()) elem_str = std::to_string(elem.as<int>());
                else if (elem.is<double>()) elem_str = std::to_string(elem.as<double>());
                else if (elem.is<bool>()) elem_str = elem.as<bool>() ? "true" : "false";
                else if (elem.is<lua_types::LuaFName>()) elem_str = elem.as<lua_types::LuaFName&>().to_string();
                else if (elem.is<lua_types::LuaFString>()) elem_str = elem.as<lua_types::LuaFString&>().to_string();
                else continue;
                if (elem_str == target_str) return i + 1; // 1-indexed
            }
            return 0; },

                                    // ToTable — convert entire array to a Lua table
                                    "ToTable", [](sol::this_state ts, const LuaTArray &self) -> sol::table
                                    {
            sol::state_view lua(ts);
            sol::table result = lua.create_table();
            if (!self.array_ptr || !self.inner_prop || self.element_size <= 0) return result;
            int32_t n = self.num();
            if (n <= 0) return result;
            const uint8_t* data = reinterpret_cast<const uint8_t*>(self.data());
            if (!data) return result;
            for (int32_t i = 0; i < n; i++) {
                result[i + 1] = read_element(lua, data + i * self.element_size, self.inner_prop);
            }
            return result; });
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Register TMap usertype (basic read-only iteration)
    // ═══════════════════════════════════════════════════════════════════════

    static void register_tmap(sol::state &lua)
    {
        // TMap in UE4 uses FScriptMap internally:
        // struct FScriptMap {
        //   FScriptSet Pairs; // Actually stores TPair<Key,Value> as elements
        // };
        // FScriptSet uses a sparse array + hash:
        // struct FScriptSet {
        //   FScriptSparseArray Elements;
        //   FHashAllocator Hash;
        // };
        //
        // FScriptSparseArray on ARM64 (64-bit pointers):
        //   offset 0:  void* Data          (8 bytes)
        //   offset 8:  int32 MaxElements   (4 bytes)
        //   offset 12: int32 NumElements   (4 bytes)
        //   offset 16: int32 FirstFreeIndex(4 bytes)
        //
        // Each element in the sparse array is a UNION of:
        //   Occupied: { TPair<Key,Value>; int32 HashNextId; int32 HashIndex; }
        //   Free:     { int32 PrevFreeIndex; int32 NextFreeIndex; ... padding }
        //
        // The element stride (entry_stride) and key/value offsets within each
        // element are read from FMapProperty's FScriptSetLayout at creation time.

        lua.new_usertype<LuaTMap>("TMap", sol::no_constructor,

                                  "IsValid", &LuaTMap::is_valid,

                                  // ── __len returns count of valid entries
                                  sol::meta_function::length, [](const LuaTMap &self) -> int
                                  {
            if (!self.map_ptr) return 0;
            // ARM64 TSparseArray: TArray{Data(8), ArrayNum(4), ArrayMax(4)}, TBitArray(32), FirstFreeIndex(4), NumFreeIndices(4)
            // ArrayNum at offset 8 = total allocated slots (valid + free)
            // NumFreeIndices at offset 52 = number of free slots
            // Valid count = ArrayNum - NumFreeIndices
            const uint8_t* b = reinterpret_cast<const uint8_t*>(self.map_ptr);
            int32_t array_num = *reinterpret_cast<const int32_t*>(b + 8);
            int32_t num_free  = *reinterpret_cast<const int32_t*>(b + 52);
            if (num_free < 0 || num_free > array_num) num_free = 0;
            return array_num - num_free; },

                                  sol::meta_function::to_string, [](const LuaTMap &self) -> std::string
                                  {
            if (!self.map_ptr) return "TMap(invalid)";
            const uint8_t* b = reinterpret_cast<const uint8_t*>(self.map_ptr);
            int32_t array_num = *reinterpret_cast<const int32_t*>(b + 8);
            int32_t num_free  = *reinterpret_cast<const int32_t*>(b + 52);
            if (num_free < 0 || num_free > array_num) num_free = 0;
            char buf[64];
            snprintf(buf, sizeof(buf), "TMap(Num=%d)", array_num - num_free);
            return std::string(buf); },

                                  // GetDebugInfo — returns string with all layout details
                                  "GetDebugInfo", [](const LuaTMap &self) -> std::string
                                  {
            if (!self.map_ptr) return "invalid";
            const uint8_t* b = reinterpret_cast<const uint8_t*>(self.map_ptr);
            void* data = *reinterpret_cast<void* const*>(b);
            char buf[512];
            snprintf(buf, sizeof(buf),
                "map_ptr=%p data=%p "
                "int@4=%d int@8=%d int@12=%d int@16=%d int@20=%d "
                "key_size=%d val_size=%d "
                "key_off=%d val_off=%d stride=%d",
                self.map_ptr, data,
                *reinterpret_cast<const int32_t*>(b + 4),
                *reinterpret_cast<const int32_t*>(b + 8),
                *reinterpret_cast<const int32_t*>(b + 12),
                *reinterpret_cast<const int32_t*>(b + 16),
                *reinterpret_cast<const int32_t*>(b + 20),
                self.key_size, self.value_size,
                self.key_offset, self.value_offset, self.entry_stride);
            return std::string(buf); },

                                  // PeekElement — read raw int32 values from element i at various offsets
                                  // Returns string with hex dump of first 32 bytes of element
                                  "PeekElement", [](const LuaTMap &self, int index) -> std::string
                                  {
            if (!self.map_ptr || self.entry_stride <= 0) return "invalid";
            const uint8_t* b = reinterpret_cast<const uint8_t*>(self.map_ptr);
            void* data = *reinterpret_cast<void* const*>(b);
            if (!data) return "null data";
            int idx = index - 1; // 1-indexed
            if (idx < 0) return "bad index";
            const uint8_t* entry = reinterpret_cast<const uint8_t*>(data) + idx * self.entry_stride;
            char buf[512];
            int pos = 0;
            pos += snprintf(buf + pos, sizeof(buf) - pos, "elem[%d] @%p stride=%d: ", index, entry, self.entry_stride);
            for (int i = 0; i < 64 && i < self.entry_stride; i += 4) {
                int32_t val = *reinterpret_cast<const int32_t*>(entry + i);
                pos += snprintf(buf + pos, sizeof(buf) - pos, "[%d]=%d ", i, val);
            }
            return std::string(buf); },

                                  // ForEach(callback) — iterates valid entries, callback(key, value)
                                  // Return true from callback to break.
                                  "ForEach", [](sol::this_state ts, const LuaTMap &self, sol::function callback)
                                  {
            sol::state_view lua(ts);
            if (!self.map_ptr || !self.key_prop || !self.value_prop) return;
            if (self.key_size <= 0 || self.value_size <= 0) return;
            if (self.entry_stride <= 0) return;

            // ARM64 TSparseArray layout:
            //   offset 0:   void* Data              (8 bytes)
            //   offset 8:   int32 ArrayNum           (4 bytes) — allocated slots
            //   offset 12:  int32 ArrayMax           (4 bytes) — capacity
            //   offset 16:  TBitArray (32 bytes)     — allocation flags
            //     16-31: InlineData[16]
            //     32-39: SecondaryData ptr
            //     40-43: NumBits
            //     44-47: MaxBits
            //   offset 48:  int32 FirstFreeIndex     (4 bytes)
            //   offset 52:  int32 NumFreeIndices     (4 bytes)
            const uint8_t* sparse_base = reinterpret_cast<const uint8_t*>(self.map_ptr);
            void* data = *reinterpret_cast<void* const*>(sparse_base);
            int32_t array_num    = *reinterpret_cast<const int32_t*>(sparse_base + 8);
            int32_t first_free   = *reinterpret_cast<const int32_t*>(sparse_base + 48);
            int32_t num_free_idx = *reinterpret_cast<const int32_t*>(sparse_base + 52);

            logger::log_info("TMAP", "ForEach: data=%p array_num=%d first_free=%d num_free=%d stride=%d key_off=%d val_off=%d key_sz=%d val_sz=%d",
                             data, array_num, first_free, num_free_idx,
                             self.entry_stride, self.key_offset, self.value_offset,
                             self.key_size, self.value_size);

            if (!data || array_num <= 0) return;

            int32_t num_valid = array_num - num_free_idx;
            if (num_valid <= 0) return;
            if (num_free_idx < 0) num_free_idx = 0;

            // Build free-index set by walking the free list chain.
            // Free elements reuse bytes 0..7 as { int32 PrevFreeIndex; int32 NextFreeIndex }.
            // NextFreeIndex is at offset 4 within the element.
            std::vector<bool> is_free(array_num, false);
            if (first_free >= 0 && num_free_idx > 0) {
                int32_t idx = first_free;
                int32_t safety = num_free_idx + 1;
                while (idx >= 0 && idx < array_num && safety-- > 0) {
                    is_free[idx] = true;
                    const uint8_t* slot = reinterpret_cast<const uint8_t*>(data) + idx * self.entry_stride;
                    idx = *reinterpret_cast<const int32_t*>(slot + 4); // NextFreeIndex
                }
            }

            int32_t visited = 0;
            for (int32_t i = 0; i < array_num && visited < num_valid; i++) {
                if (is_free[i]) continue;

                const uint8_t* entry = reinterpret_cast<const uint8_t*>(data) + i * self.entry_stride;

                // Key and value at their respective offsets within the element
                const uint8_t* key_ptr = entry + self.key_offset;
                const uint8_t* value_ptr = entry + self.value_offset;

                // Safety: validate entry pointer before reading
                if (!ue::is_mapped_ptr(key_ptr) || !ue::is_mapped_ptr(value_ptr)) {
                    visited++;
                    continue;
                }

                sol::object key_obj = read_element(lua, key_ptr, self.key_prop);
                sol::object val_obj = read_element(lua, value_ptr, self.value_prop);

                auto result = callback(key_obj, val_obj);
                visited++;
                if (result.valid() && result.get_type() == sol::type::boolean && result.get<bool>()) {
                    break;
                }
            } },

                                  // SetByKey(key, newValue) — finds existing entry by key and overwrites value.
                                  // Does NOT add new entries (requires hash table rebuild).
                                  // Key comparison: converts both key and candidate to string for matching.
                                  // Returns true if key was found and value written, false otherwise.
                                  "SetByKey", [](sol::this_state ts, LuaTMap &self, sol::object key_lua, sol::object value_lua) -> bool
                                  {
            sol::state_view lua(ts);
            if (!self.map_ptr || !self.key_prop || !self.value_prop) return false;
            if (self.key_size <= 0 || self.value_size <= 0) return false;
            if (self.entry_stride <= 0) return false;

            const uint8_t* sparse_base = reinterpret_cast<const uint8_t*>(self.map_ptr);
            void* data = *reinterpret_cast<void* const*>(sparse_base);
            int32_t array_num    = *reinterpret_cast<const int32_t*>(sparse_base + 8);
            int32_t first_free   = *reinterpret_cast<const int32_t*>(sparse_base + 48);
            int32_t num_free_idx = *reinterpret_cast<const int32_t*>(sparse_base + 52);

            if (!data || array_num <= 0) return false;
            int32_t num_valid = array_num - num_free_idx;
            if (num_valid <= 0) return false;
            if (num_free_idx < 0) num_free_idx = 0;

            // Build free-index set
            std::vector<bool> is_free(array_num, false);
            if (first_free >= 0 && num_free_idx > 0) {
                int32_t idx = first_free;
                int32_t safety = num_free_idx + 1;
                while (idx >= 0 && idx < array_num && safety-- > 0) {
                    is_free[idx] = true;
                    const uint8_t* slot = reinterpret_cast<const uint8_t*>(data) + idx * self.entry_stride;
                    idx = *reinterpret_cast<const int32_t*>(slot + 4);
                }
            }

            // Convert target key to string for comparison
            std::string target_key;
            if (key_lua.is<lua_types::LuaFName>()) {
                target_key = key_lua.as<lua_types::LuaFName&>().to_string();
            } else if (key_lua.is<std::string>()) {
                target_key = key_lua.as<std::string>();
            } else if (key_lua.is<int>()) {
                target_key = std::to_string(key_lua.as<int>());
            } else if (key_lua.is<double>()) {
                target_key = std::to_string(static_cast<int64_t>(key_lua.as<double>()));
            } else {
                return false;
            }

            // Search for matching key
            int32_t visited = 0;
            for (int32_t i = 0; i < array_num && visited < num_valid; i++) {
                if (is_free[i]) continue;

                uint8_t* entry = reinterpret_cast<uint8_t*>(data) + i * self.entry_stride;
                const uint8_t* key_ptr = entry + self.key_offset;
                uint8_t* value_ptr = entry + self.value_offset;

                if (!ue::is_mapped_ptr(key_ptr) || !ue::is_mapped_ptr(value_ptr)) {
                    visited++;
                    continue;
                }

                // Read key and convert to string for comparison
                sol::object key_obj = read_element(lua, key_ptr, self.key_prop);
                std::string key_str;
                if (key_obj.is<lua_types::LuaFName>()) {
                    key_str = key_obj.as<lua_types::LuaFName&>().to_string();
                } else if (key_obj.is<std::string>()) {
                    key_str = key_obj.as<std::string>();
                } else if (key_obj.is<lua_types::LuaFString>()) {
                    key_str = key_obj.as<lua_types::LuaFString&>().to_string();
                } else if (key_obj.is<int>()) {
                    key_str = std::to_string(key_obj.as<int>());
                } else if (key_obj.is<double>()) {
                    key_str = std::to_string(static_cast<int64_t>(key_obj.as<double>()));
                }

                if (key_str == target_key) {
                    write_element(value_ptr, self.value_prop, value_lua);
                    logger::log_info("TMAP", "SetByKey: '%s' updated", target_key.c_str());
                    return true;
                }
                visited++;
            }

            logger::log_warn("TMAP", "SetByKey: key '%s' not found (searched %d entries)", target_key.c_str(), visited);
            return false; },

                                  // ── GetByKey — find value by key ──────────────────────────
                                  // Usage: local value = myMap:GetByKey("SomeKey")
                                  // Searches for a matching key by string comparison.
                                  // Returns the value object, or nil if not found.
                                  "GetByKey", [](sol::this_state ts, const LuaTMap &self, sol::object key_lua) -> sol::object
                                  {
            sol::state_view lua(ts);
            if (!self.map_ptr || !self.key_prop || !self.value_prop) return sol::nil;
            if (self.key_size <= 0 || self.value_size <= 0) return sol::nil;
            if (self.entry_stride <= 0) return sol::nil;

            const uint8_t* sparse_base = reinterpret_cast<const uint8_t*>(self.map_ptr);
            void* data = *reinterpret_cast<void* const*>(sparse_base);
            int32_t array_num    = *reinterpret_cast<const int32_t*>(sparse_base + 8);
            int32_t first_free   = *reinterpret_cast<const int32_t*>(sparse_base + 48);
            int32_t num_free_idx = *reinterpret_cast<const int32_t*>(sparse_base + 52);

            if (!data || array_num <= 0) return sol::nil;
            int32_t num_valid = array_num - num_free_idx;
            if (num_valid <= 0) return sol::nil;
            if (num_free_idx < 0) num_free_idx = 0;

            // Build free-index set
            std::vector<bool> is_free(array_num, false);
            if (first_free >= 0 && num_free_idx > 0) {
                int32_t idx = first_free;
                int32_t safety = num_free_idx + 1;
                while (idx >= 0 && idx < array_num && safety-- > 0) {
                    is_free[idx] = true;
                    const uint8_t* slot = reinterpret_cast<const uint8_t*>(data) + idx * self.entry_stride;
                    idx = *reinterpret_cast<const int32_t*>(slot + 4);
                }
            }

            // Convert target key to string for comparison
            std::string target_key;
            if (key_lua.is<lua_types::LuaFName>()) {
                target_key = key_lua.as<lua_types::LuaFName&>().to_string();
            } else if (key_lua.is<std::string>()) {
                target_key = key_lua.as<std::string>();
            } else if (key_lua.is<int>()) {
                target_key = std::to_string(key_lua.as<int>());
            } else if (key_lua.is<double>()) {
                target_key = std::to_string(static_cast<int64_t>(key_lua.as<double>()));
            } else {
                return sol::nil;
            }

            // Search for matching key
            int32_t visited = 0;
            for (int32_t i = 0; i < array_num && visited < num_valid; i++) {
                if (is_free[i]) continue;

                const uint8_t* entry = reinterpret_cast<const uint8_t*>(data) + i * self.entry_stride;
                const uint8_t* key_ptr = entry + self.key_offset;
                const uint8_t* value_ptr = entry + self.value_offset;

                if (!ue::is_mapped_ptr(key_ptr) || !ue::is_mapped_ptr(value_ptr)) {
                    visited++;
                    continue;
                }

                sol::object key_obj = read_element(lua, key_ptr, self.key_prop);
                std::string key_str;
                if (key_obj.is<lua_types::LuaFName>()) {
                    key_str = key_obj.as<lua_types::LuaFName&>().to_string();
                } else if (key_obj.is<std::string>()) {
                    key_str = key_obj.as<std::string>();
                } else if (key_obj.is<lua_types::LuaFString>()) {
                    key_str = key_obj.as<lua_types::LuaFString&>().to_string();
                } else if (key_obj.is<int>()) {
                    key_str = std::to_string(key_obj.as<int>());
                } else if (key_obj.is<double>()) {
                    key_str = std::to_string(static_cast<int64_t>(key_obj.as<double>()));
                }

                if (key_str == target_key) {
                    return read_element(lua, value_ptr, self.value_prop);
                }
                visited++;
            }

            return sol::nil; },

                                  // ── Keys — return table of all keys ───────────────────────
                                  "Keys", [](sol::this_state ts, const LuaTMap &self) -> sol::table
                                  {
            sol::state_view lua(ts);
            sol::table result = lua.create_table();
            if (!self.map_ptr || !self.key_prop) return result;
            if (self.key_size <= 0 || self.entry_stride <= 0) return result;

            const uint8_t* sparse_base = reinterpret_cast<const uint8_t*>(self.map_ptr);
            void* data = *reinterpret_cast<void* const*>(sparse_base);
            int32_t array_num    = *reinterpret_cast<const int32_t*>(sparse_base + 8);
            int32_t first_free   = *reinterpret_cast<const int32_t*>(sparse_base + 48);
            int32_t num_free_idx = *reinterpret_cast<const int32_t*>(sparse_base + 52);

            if (!data || array_num <= 0) return result;
            if (num_free_idx < 0) num_free_idx = 0;
            int32_t num_valid = array_num - num_free_idx;
            if (num_valid <= 0) return result;

            std::vector<bool> is_free(array_num, false);
            if (first_free >= 0 && num_free_idx > 0) {
                int32_t idx = first_free;
                int32_t safety = num_free_idx + 1;
                while (idx >= 0 && idx < array_num && safety-- > 0) {
                    is_free[idx] = true;
                    const uint8_t* slot = reinterpret_cast<const uint8_t*>(data) + idx * self.entry_stride;
                    idx = *reinterpret_cast<const int32_t*>(slot + 4);
                }
            }

            int lua_idx = 1;
            int32_t visited = 0;
            for (int32_t i = 0; i < array_num && visited < num_valid; i++) {
                if (is_free[i]) continue;
                const uint8_t* entry = reinterpret_cast<const uint8_t*>(data) + i * self.entry_stride;
                const uint8_t* key_ptr = entry + self.key_offset;
                if (!ue::is_mapped_ptr(key_ptr)) { visited++; continue; }
                result[lua_idx++] = read_element(lua, key_ptr, self.key_prop);
                visited++;
            }
            return result; },

                                  // ── Values — return table of all values ───────────────────
                                  "Values", [](sol::this_state ts, const LuaTMap &self) -> sol::table
                                  {
            sol::state_view lua(ts);
            sol::table result = lua.create_table();
            if (!self.map_ptr || !self.value_prop) return result;
            if (self.value_size <= 0 || self.entry_stride <= 0) return result;

            const uint8_t* sparse_base = reinterpret_cast<const uint8_t*>(self.map_ptr);
            void* data = *reinterpret_cast<void* const*>(sparse_base);
            int32_t array_num    = *reinterpret_cast<const int32_t*>(sparse_base + 8);
            int32_t first_free   = *reinterpret_cast<const int32_t*>(sparse_base + 48);
            int32_t num_free_idx = *reinterpret_cast<const int32_t*>(sparse_base + 52);

            if (!data || array_num <= 0) return result;
            if (num_free_idx < 0) num_free_idx = 0;
            int32_t num_valid = array_num - num_free_idx;
            if (num_valid <= 0) return result;

            std::vector<bool> is_free(array_num, false);
            if (first_free >= 0 && num_free_idx > 0) {
                int32_t idx = first_free;
                int32_t safety = num_free_idx + 1;
                while (idx >= 0 && idx < array_num && safety-- > 0) {
                    is_free[idx] = true;
                    const uint8_t* slot = reinterpret_cast<const uint8_t*>(data) + idx * self.entry_stride;
                    idx = *reinterpret_cast<const int32_t*>(slot + 4);
                }
            }

            int lua_idx = 1;
            int32_t visited = 0;
            for (int32_t i = 0; i < array_num && visited < num_valid; i++) {
                if (is_free[i]) continue;
                const uint8_t* entry = reinterpret_cast<const uint8_t*>(data) + i * self.entry_stride;
                const uint8_t* value_ptr = entry + self.value_offset;
                if (!ue::is_mapped_ptr(value_ptr)) { visited++; continue; }
                result[lua_idx++] = read_element(lua, value_ptr, self.value_prop);
                visited++;
            }
            return result; },

                                  // ── ToTable — return all entries as {key, value} pairs ────
                                  "ToTable", [](sol::this_state ts, const LuaTMap &self) -> sol::table
                                  {
            sol::state_view lua(ts);
            sol::table result = lua.create_table();
            if (!self.map_ptr || !self.key_prop || !self.value_prop) return result;
            if (self.key_size <= 0 || self.value_size <= 0 || self.entry_stride <= 0) return result;

            const uint8_t* sparse_base = reinterpret_cast<const uint8_t*>(self.map_ptr);
            void* data = *reinterpret_cast<void* const*>(sparse_base);
            int32_t array_num    = *reinterpret_cast<const int32_t*>(sparse_base + 8);
            int32_t first_free   = *reinterpret_cast<const int32_t*>(sparse_base + 48);
            int32_t num_free_idx = *reinterpret_cast<const int32_t*>(sparse_base + 52);

            if (!data || array_num <= 0) return result;
            if (num_free_idx < 0) num_free_idx = 0;
            int32_t num_valid = array_num - num_free_idx;
            if (num_valid <= 0) return result;

            std::vector<bool> is_free(array_num, false);
            if (first_free >= 0 && num_free_idx > 0) {
                int32_t idx = first_free;
                int32_t safety = num_free_idx + 1;
                while (idx >= 0 && idx < array_num && safety-- > 0) {
                    is_free[idx] = true;
                    const uint8_t* slot = reinterpret_cast<const uint8_t*>(data) + idx * self.entry_stride;
                    idx = *reinterpret_cast<const int32_t*>(slot + 4);
                }
            }

            int lua_idx = 1;
            int32_t visited = 0;
            for (int32_t i = 0; i < array_num && visited < num_valid; i++) {
                if (is_free[i]) continue;
                const uint8_t* entry = reinterpret_cast<const uint8_t*>(data) + i * self.entry_stride;
                const uint8_t* key_ptr = entry + self.key_offset;
                const uint8_t* value_ptr = entry + self.value_offset;
                if (!ue::is_mapped_ptr(key_ptr) || !ue::is_mapped_ptr(value_ptr)) { visited++; continue; }

                sol::table pair = lua.create_table();
                pair["key"] = read_element(lua, key_ptr, self.key_prop);
                pair["value"] = read_element(lua, value_ptr, self.value_prop);
                result[lua_idx++] = pair;
                visited++;
            }
            return result; },

                                  // ── ContainsKey — check if a key exists in the map ───────
                                  "ContainsKey", [](sol::this_state ts, const LuaTMap &self, sol::object key_lua) -> bool
                                  {
            sol::state_view lua(ts);
            if (!self.map_ptr || !self.key_prop) return false;
            if (self.key_size <= 0 || self.entry_stride <= 0) return false;

            const uint8_t* sparse_base = reinterpret_cast<const uint8_t*>(self.map_ptr);
            void* data = *reinterpret_cast<void* const*>(sparse_base);
            int32_t array_num    = *reinterpret_cast<const int32_t*>(sparse_base + 8);
            int32_t first_free   = *reinterpret_cast<const int32_t*>(sparse_base + 48);
            int32_t num_free_idx = *reinterpret_cast<const int32_t*>(sparse_base + 52);

            if (!data || array_num <= 0) return false;
            if (num_free_idx < 0) num_free_idx = 0;
            int32_t num_valid = array_num - num_free_idx;
            if (num_valid <= 0) return false;

            std::vector<bool> is_free(array_num, false);
            if (first_free >= 0 && num_free_idx > 0) {
                int32_t idx = first_free;
                int32_t safety = num_free_idx + 1;
                while (idx >= 0 && idx < array_num && safety-- > 0) {
                    is_free[idx] = true;
                    const uint8_t* slot = reinterpret_cast<const uint8_t*>(data) + idx * self.entry_stride;
                    idx = *reinterpret_cast<const int32_t*>(slot + 4);
                }
            }

            std::string target_key;
            if (key_lua.is<lua_types::LuaFName>()) target_key = key_lua.as<lua_types::LuaFName&>().to_string();
            else if (key_lua.is<std::string>()) target_key = key_lua.as<std::string>();
            else if (key_lua.is<int>()) target_key = std::to_string(key_lua.as<int>());
            else if (key_lua.is<double>()) target_key = std::to_string(static_cast<int64_t>(key_lua.as<double>()));
            else return false;

            int32_t visited = 0;
            for (int32_t i = 0; i < array_num && visited < num_valid; i++) {
                if (is_free[i]) continue;
                const uint8_t* entry = reinterpret_cast<const uint8_t*>(data) + i * self.entry_stride;
                const uint8_t* key_ptr = entry + self.key_offset;
                if (!ue::is_mapped_ptr(key_ptr)) { visited++; continue; }

                sol::object key_obj = read_element(lua, key_ptr, self.key_prop);
                std::string key_str;
                if (key_obj.is<lua_types::LuaFName>()) key_str = key_obj.as<lua_types::LuaFName&>().to_string();
                else if (key_obj.is<std::string>()) key_str = key_obj.as<std::string>();
                else if (key_obj.is<lua_types::LuaFString>()) key_str = key_obj.as<lua_types::LuaFString&>().to_string();
                else if (key_obj.is<int>()) key_str = std::to_string(key_obj.as<int>());
                else if (key_obj.is<double>()) key_str = std::to_string(static_cast<int64_t>(key_obj.as<double>()));

                if (key_str == target_key) return true;
                visited++;
            }
            return false; },

                                  // ── Num — return count of valid entries (alias for #map) ─
                                  "Num", [](const LuaTMap &self) -> int
                                  {
            if (!self.map_ptr) return 0;
            const uint8_t* b = reinterpret_cast<const uint8_t*>(self.map_ptr);
            int32_t array_num = *reinterpret_cast<const int32_t*>(b + 8);
            int32_t num_free  = *reinterpret_cast<const int32_t*>(b + 52);
            if (num_free < 0 || num_free > array_num) num_free = 0;
            return array_num - num_free; },

                                  // ── IsEmpty — check if map has no valid entries ────────
                                  "IsEmpty", [](const LuaTMap &self) -> bool
                                  {
            if (!self.map_ptr) return true;
            const uint8_t* b = reinterpret_cast<const uint8_t*>(self.map_ptr);
            int32_t array_num = *reinterpret_cast<const int32_t*>(b + 8);
            int32_t num_free  = *reinterpret_cast<const int32_t*>(b + 52);
            if (num_free < 0 || num_free > array_num) num_free = 0;
            return (array_num - num_free) <= 0; });
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Register all
    // ═══════════════════════════════════════════════════════════════════════

    void register_all(sol::state &lua)
    {
        register_tarray(lua);
        register_tmap(lua);

        logger::log_info("LUA", "TArray + TMap usertypes registered (UE4SS-compatible)");
    }

} // namespace lua_tarray
