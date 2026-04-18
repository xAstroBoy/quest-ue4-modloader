// modloader/src/lua/lua_ustruct.cpp
// UE4 UStruct Lua userdata wrapper — typed field access via reflection
// Enables struct.X, struct.Y, struct.Z syntax for FVector, FRotator, IntPoint, etc.

#include "modloader/lua_ustruct.h"
#include "modloader/reflection_walker.h"
#include "modloader/lua_uobject.h"
#include "modloader/lua_tarray.h"
#include "modloader/lua_types.h"
#include "modloader/symbols.h"
#include "modloader/logger.h"
#include "modloader/types.h"

#include <cstring>
#include <cstdlib>

namespace lua_ustruct
{

    // ═══════════════════════════════════════════════════════════════════════
    // LuaUStruct lifecycle
    // ═══════════════════════════════════════════════════════════════════════

    LuaUStruct::~LuaUStruct()
    {
        if (owns_data && data)
        {
            delete[] data;
            data = nullptr;
        }
    }

    LuaUStruct::LuaUStruct(const LuaUStruct &other)
        : ustruct(other.ustruct), size(other.size), owns_data(other.owns_data)
    {
        if (other.owns_data && other.data && other.size > 0)
        {
            data = new uint8_t[other.size];
            std::memcpy(data, other.data, other.size);
        }
        else
        {
            data = other.data;
        }
    }

    LuaUStruct &LuaUStruct::operator=(const LuaUStruct &other)
    {
        if (this == &other)
            return *this;
        if (owns_data && data)
            delete[] data;
        ustruct = other.ustruct;
        size = other.size;
        owns_data = other.owns_data;
        if (other.owns_data && other.data && other.size > 0)
        {
            data = new uint8_t[other.size];
            std::memcpy(data, other.data, other.size);
        }
        else
        {
            data = other.data;
        }
        return *this;
    }

    LuaUStruct::LuaUStruct(LuaUStruct &&other) noexcept
        : data(other.data), ustruct(other.ustruct), size(other.size), owns_data(other.owns_data)
    {
        other.data = nullptr;
        other.owns_data = false;
    }

    LuaUStruct &LuaUStruct::operator=(LuaUStruct &&other) noexcept
    {
        if (this == &other)
            return *this;
        if (owns_data && data)
            delete[] data;
        data = other.data;
        ustruct = other.ustruct;
        size = other.size;
        owns_data = other.owns_data;
        other.data = nullptr;
        other.owns_data = false;
        return *this;
    }

    bool LuaUStruct::is_valid() const
    {
        return data != nullptr && ustruct != nullptr && size > 0;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Field read helper — reads a single field from struct memory
    // offset is relative to the struct base (data pointer)
    // ═══════════════════════════════════════════════════════════════════════

    static sol::object read_field_value(sol::state_view lua, const uint8_t *base,
                                        const reflection::PropertyInfo &fi)
    {
        const uint8_t *ptr = base + fi.offset;
        if (!ue::is_mapped_ptr(ptr))
            return sol::nil;

        switch (fi.type)
        {
        case reflection::PropType::BoolProperty:
        {
            if (fi.bool_byte_mask)
            {
                return sol::make_object(lua, (ptr[fi.bool_byte_offset] & fi.bool_byte_mask) != 0);
            }
            return sol::make_object(lua, *reinterpret_cast<const bool *>(ptr));
        }

        case reflection::PropType::ByteProperty:
            return sol::make_object(lua, static_cast<int>(ptr[0]));

        case reflection::PropType::Int8Property:
            return sol::make_object(lua, static_cast<int>(*reinterpret_cast<const int8_t *>(ptr)));

        case reflection::PropType::Int16Property:
            return sol::make_object(lua, static_cast<int>(*reinterpret_cast<const int16_t *>(ptr)));

        case reflection::PropType::UInt16Property:
            return sol::make_object(lua, static_cast<int>(*reinterpret_cast<const uint16_t *>(ptr)));

        case reflection::PropType::IntProperty:
            return sol::make_object(lua, *reinterpret_cast<const int32_t *>(ptr));

        case reflection::PropType::UInt32Property:
            return sol::make_object(lua, static_cast<double>(*reinterpret_cast<const uint32_t *>(ptr)));

        case reflection::PropType::Int64Property:
            return sol::make_object(lua, static_cast<double>(*reinterpret_cast<const int64_t *>(ptr)));

        case reflection::PropType::UInt64Property:
            return sol::make_object(lua, static_cast<double>(*reinterpret_cast<const uint64_t *>(ptr)));

        case reflection::PropType::FloatProperty:
            return sol::make_object(lua, *reinterpret_cast<const float *>(ptr));

        case reflection::PropType::DoubleProperty:
            return sol::make_object(lua, *reinterpret_cast<const double *>(ptr));

        case reflection::PropType::NameProperty:
        {
            int32_t fname_idx = *reinterpret_cast<const int32_t *>(ptr);
            return sol::make_object(lua, reflection::fname_to_string(fname_idx));
        }

        case reflection::PropType::StrProperty:
        {
            // FString — use shared utility for consistency
            return sol::make_object(lua, lua_uobject::fstring_to_utf8(ptr));
        }

        case reflection::PropType::TextProperty:
        {
            // FText (24 bytes) — read via KismetTextLibrary::Conv_TextToString
            std::string utf8 = lua_uobject::ftext_to_string(ptr);
            return sol::make_object(lua, utf8);
        }

        case reflection::PropType::ObjectProperty:
        case reflection::PropType::WeakObjectProperty:
        case reflection::PropType::SoftObjectProperty:
        case reflection::PropType::LazyObjectProperty:
        case reflection::PropType::InterfaceProperty:
        case reflection::PropType::ClassProperty:
        {
            ue::UObject *obj = *reinterpret_cast<ue::UObject *const *>(ptr);
            if (!obj || !ue::is_valid_ptr(obj))
                return sol::nil;
            lua_uobject::LuaUObject wrapped;
            wrapped.ptr = obj;
            return sol::make_object(lua, wrapped);
        }

        case reflection::PropType::StructProperty:
        {
            // Nested struct — recurse with LuaUStruct
            ue::UStruct *inner = nullptr;
            if (fi.raw && ue::is_valid_ptr(fi.raw))
            {
                ue::UStruct *candidate = ue::read_field<ue::UStruct *>(fi.raw, ue::fprop::STRUCT_INNER_STRUCT_OFF());
                if (candidate && ue::is_mapped_ptr(candidate))
                    inner = candidate;
            }
            if (inner)
            {
                LuaUStruct nested;
                nested.data = const_cast<uint8_t *>(ptr);
                nested.ustruct = inner;
                nested.size = fi.element_size;
                nested.owns_data = false;
                return sol::make_object(lua, nested);
            }
            return sol::make_object(lua, sol::lightuserdata_value(const_cast<uint8_t *>(ptr)));
        }

        case reflection::PropType::ArrayProperty:
        {
            ue::FProperty *inner_prop = nullptr;
            if (fi.raw && ue::is_valid_ptr(fi.raw))
            {
                inner_prop = ue::read_field<ue::FProperty *>(fi.raw, ue::fprop::ARRAY_INNER_OFF());
            }
            // Auto-detect correct offset if inner_prop is null (same as lua_uobject.cpp)
            if (!inner_prop)
            {
                for (uint32_t scan_off = 0x58; scan_off <= 0x98; scan_off += 8)
                {
                    void *candidate = ue::read_field<void *>(fi.raw, scan_off);
                    if (candidate && ue::is_mapped_ptr(candidate))
                    {
                        void *field_cls = ue::read_field<void *>(candidate, ue::ffield::CLASS_PRIVATE_OFF());
                        if (field_cls && ue::is_mapped_ptr(field_cls))
                        {
                            int32_t cls_name_idx = ue::read_field<int32_t>(field_cls, 0);
                            std::string cls_name = reflection::fname_to_string(cls_name_idx);
                            if (!cls_name.empty() && cls_name.find("Property") != std::string::npos)
                            {
                                inner_prop = reinterpret_cast<ue::FProperty *>(candidate);
                                ue::fprop::ARRAY_INNER_OFF() = scan_off;
                                logger::log_warn("TARRAY", "Struct ArrayProperty: auto-corrected ARRAY_INNER_OFF to 0x%x (%s)", scan_off, cls_name.c_str());
                                break;
                            }
                        }
                    }
                }
            }
            // Validate inner_prop is actually mapped (not just non-null)
            if (inner_prop && !ue::is_mapped_ptr(inner_prop))
            {
                logger::log_warn("TARRAY", "Struct ArrayProperty '%s': inner_prop=%p fails is_mapped_ptr",
                                 fi.name.c_str(), inner_prop);
                inner_prop = nullptr;
            }
            lua_tarray::LuaTArray arr;
            arr.array_ptr = const_cast<uint8_t *>(ptr);
            arr.inner_prop = inner_prop;
            arr.element_size = inner_prop ? ue::fprop_get_element_size(inner_prop) : 0;
            return sol::make_object(lua, arr);
        }

        case reflection::PropType::EnumProperty:
        {
            if (fi.element_size == 1)
                return sol::make_object(lua, static_cast<int>(ptr[0]));
            if (fi.element_size == 2)
                return sol::make_object(lua, static_cast<int>(*reinterpret_cast<const int16_t *>(ptr)));
            if (fi.element_size == 4)
                return sol::make_object(lua, *reinterpret_cast<const int32_t *>(ptr));
            return sol::make_object(lua, static_cast<int>(ptr[0]));
        }

        case reflection::PropType::MapProperty:
        {
            // TMap field within a struct — return as LuaTMap with key/value dispatch
            ue::FProperty *key_prop = nullptr;
            ue::FProperty *val_prop = nullptr;
            if (fi.raw && ue::is_valid_ptr(fi.raw))
            {
                key_prop = ue::read_field<ue::FProperty *>(fi.raw, ue::fprop::MAP_KEY_PROP_OFF());
                val_prop = ue::read_field<ue::FProperty *>(fi.raw, ue::fprop::MAP_VALUE_PROP_OFF());
            }
            // Validate key/val pointers are actually mapped
            if (key_prop && !ue::is_mapped_ptr(key_prop))
            {
                key_prop = nullptr;
            }
            if (val_prop && !ue::is_mapped_ptr(val_prop))
            {
                val_prop = nullptr;
            }

            // Auto-detect: scan FMapProperty for valid FProperty* pointers if key or val missing
            if (fi.raw && ue::is_valid_ptr(fi.raw) && (!key_prop || !val_prop))
            {
                uintptr_t parent_addr = reinterpret_cast<uintptr_t>(fi.raw);
                std::vector<std::pair<uint32_t, ue::FProperty *>> found_props;
                for (uint32_t scan_off = 0x58; scan_off <= 0xB0; scan_off += 8)
                {
                    void *candidate = ue::read_field<void *>(fi.raw, scan_off);
                    if (!candidate || !ue::is_mapped_ptr(candidate))
                        continue;
                    void *field_cls = ue::read_field<void *>(candidate, ue::ffield::CLASS_PRIVATE_OFF());
                    if (!field_cls || !ue::is_mapped_ptr(field_cls))
                        continue;
                    int32_t cls_name_idx = ue::read_field<int32_t>(field_cls, 0);
                    std::string cls_name = reflection::fname_to_string(cls_name_idx);
                    if (!cls_name.empty() && cls_name.find("Property") != std::string::npos)
                    {
                        // Filter: only child properties (Owner == this MapProperty)
                        void *owner = ue::read_field<void *>(candidate, 0x10);
                        uintptr_t owner_addr = reinterpret_cast<uintptr_t>(owner);
                        if (owner_addr != parent_addr && (owner_addr & ~1ULL) != parent_addr)
                            continue;
                        found_props.push_back({scan_off, reinterpret_cast<ue::FProperty *>(candidate)});
                    }
                }
                // ALWAYS override both from scanner — initial read may have wrong types
                if (found_props.size() >= 2)
                {
                    key_prop = found_props[0].second;
                    val_prop = found_props[1].second;
                    ue::fprop::MAP_KEY_PROP_OFF() = found_props[0].first;
                    ue::fprop::MAP_VALUE_PROP_OFF() = found_props[1].first;
                }
                else if (found_props.size() == 1 && !key_prop)
                {
                    key_prop = found_props[0].second;
                }
            }

            lua_tarray::LuaTMap map;
            map.map_ptr = const_cast<uint8_t *>(ptr);
            map.key_prop = key_prop;
            map.value_prop = val_prop;
            map.key_size = key_prop ? ue::fprop_get_element_size(key_prop) : 0;
            map.value_size = val_prop ? ue::fprop_get_element_size(val_prop) : 0;

            // Read FScriptMapLayout from the FMapProperty (same as lua_uobject.cpp)
            uint32_t layout_off = ue::fprop::MAP_VALUE_PROP_OFF() + 8;
            int32_t ml_value_offset = ue::read_field<int32_t>(fi.raw, layout_off + 0);
            int32_t ml_entry_stride = ue::read_field<int32_t>(fi.raw, layout_off + 12);

            if (ml_entry_stride > 0 && ml_entry_stride <= 4096 &&
                ml_value_offset >= 0 && ml_value_offset < ml_entry_stride)
            {
                map.entry_stride = ml_entry_stride;
                map.key_offset = 0;
                map.value_offset = ml_value_offset;
            }
            else
            {
                map.entry_stride = ((map.key_size + map.value_size + 8) + 7) & ~7;
                map.key_offset = 0;
                map.value_offset = map.entry_stride - map.value_size - 8;
                if (map.value_offset < map.key_size)
                    map.value_offset = map.key_size;
            }

            return sol::make_object(lua, map);
        }

        default:
            return sol::make_object(lua, sol::lightuserdata_value(const_cast<uint8_t *>(ptr)));
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Field write helper — writes a single field in struct memory
    // ═══════════════════════════════════════════════════════════════════════

    static bool write_field_value(uint8_t *base, const reflection::PropertyInfo &fi,
                                  const sol::object &value)
    {
        uint8_t *ptr = base + fi.offset;
        if (!ue::is_mapped_ptr(ptr))
            return false;

        switch (fi.type)
        {
        case reflection::PropType::BoolProperty:
        {
            bool val = value.as<bool>();
            if (fi.bool_byte_mask)
            {
                if (val)
                    ptr[fi.bool_byte_offset] |= fi.bool_byte_mask;
                else
                    ptr[fi.bool_byte_offset] &= ~fi.bool_byte_mask;
            }
            else
            {
                *reinterpret_cast<bool *>(ptr) = val;
            }
            return true;
        }

        case reflection::PropType::ByteProperty:
        case reflection::PropType::Int8Property:
            ptr[0] = static_cast<uint8_t>(value.as<int>());
            return true;

        case reflection::PropType::Int16Property:
        case reflection::PropType::UInt16Property:
            *reinterpret_cast<int16_t *>(ptr) = static_cast<int16_t>(value.as<int>());
            return true;

        case reflection::PropType::IntProperty:
            *reinterpret_cast<int32_t *>(ptr) = value.as<int32_t>();
            return true;

        case reflection::PropType::UInt32Property:
            *reinterpret_cast<uint32_t *>(ptr) = static_cast<uint32_t>(value.as<double>());
            return true;

        case reflection::PropType::Int64Property:
            *reinterpret_cast<int64_t *>(ptr) = static_cast<int64_t>(value.as<double>());
            return true;

        case reflection::PropType::UInt64Property:
            *reinterpret_cast<uint64_t *>(ptr) = static_cast<uint64_t>(value.as<double>());
            return true;

        case reflection::PropType::FloatProperty:
            *reinterpret_cast<float *>(ptr) = value.as<float>();
            return true;

        case reflection::PropType::DoubleProperty:
            *reinterpret_cast<double *>(ptr) = value.as<double>();
            return true;

        case reflection::PropType::ObjectProperty:
        case reflection::PropType::WeakObjectProperty:
        case reflection::PropType::ClassProperty:
        {
            if (value.is<lua_uobject::LuaUObject>())
            {
                *reinterpret_cast<ue::UObject **>(ptr) = value.as<lua_uobject::LuaUObject &>().ptr;
                return true;
            }
            else if (value == sol::nil)
            {
                *reinterpret_cast<ue::UObject **>(ptr) = nullptr;
                return true;
            }
            return false;
        }

        case reflection::PropType::StructProperty:
        {
            // Accept LuaUStruct (memcpy) or table (fill fields)
            if (value.is<LuaUStruct>())
            {
                const LuaUStruct &src = value.as<const LuaUStruct &>();
                if (src.data && src.size > 0)
                {
                    int32_t copy_size = (src.size < fi.element_size) ? src.size : fi.element_size;
                    std::memcpy(ptr, src.data, copy_size);
                }
                return true;
            }
            else if (value.get_type() == sol::type::table)
            {
                ue::UStruct *inner = ue::read_field<ue::UStruct *>(fi.raw, ue::fprop::STRUCT_INNER_STRUCT_OFF());
                if (inner)
                {
                    fill_from_table(ptr, inner, value.as<sol::table>());
                    return true;
                }
            }
            return false;
        }

        case reflection::PropType::EnumProperty:
        {
            int val = value.as<int>();
            if (fi.element_size == 1)
                ptr[0] = static_cast<uint8_t>(val);
            else if (fi.element_size == 2)
                *reinterpret_cast<int16_t *>(ptr) = static_cast<int16_t>(val);
            else
                *reinterpret_cast<int32_t *>(ptr) = val;
            return true;
        }

        case reflection::PropType::NameProperty:
        {
            // FName = { int32 ComparisonIndex, int32 Number }
            if (value.is<lua_types::LuaFName>())
            {
                auto &fn = value.as<lua_types::LuaFName &>();
                *reinterpret_cast<int32_t *>(ptr) = fn.comparison_index;
                *reinterpret_cast<int32_t *>(ptr + 4) = fn.number;
            }
            else if (value.is<std::string>())
            {
                std::string s = value.as<std::string>();
                int32_t idx = reflection::fname_string_to_index(s);
                if (idx == 0 && symbols::FName_Init)
                {
                    ue::FName fname;
                    std::u16string u16name(s.begin(), s.end());
                    symbols::FName_Init(&fname, u16name.c_str(), 0);
                    idx = fname.ComparisonIndex;
                }
                *reinterpret_cast<int32_t *>(ptr) = idx;
                *reinterpret_cast<int32_t *>(ptr + 4) = 0;
            }
            else if (value.is<int32_t>())
            {
                *reinterpret_cast<int32_t *>(ptr) = value.as<int32_t>();
                *reinterpret_cast<int32_t *>(ptr + 4) = 0;
            }
            else
            {
                return false;
            }
            return true;
        }

        case reflection::PropType::StrProperty:
        {
            // FString write using shared utility (safe allocator)
            if (value.is<std::string>())
            {
                return lua_uobject::fstring_from_utf8(ptr, value.as<std::string>());
            }
            else if (value.is<lua_types::LuaFString>())
            {
                return lua_uobject::fstring_from_utf8(ptr, value.as<lua_types::LuaFString &>().to_string());
            }
            return false;
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
                return false;
            return lua_uobject::ftext_from_string(ptr, str);
        }

        case reflection::PropType::ArrayProperty:
        {
            // TArray field write — accept LuaTArray userdata
            if (value.is<lua_tarray::LuaTArray>())
            {
                const auto &src = value.as<const lua_tarray::LuaTArray &>();
                if (src.array_ptr && src.is_valid())
                {
                    // Get destination's inner FProperty and element size
                    ue::FProperty *dest_inner = nullptr;
                    int32_t dest_elem_size = 0;
                    if (fi.raw && ue::is_valid_ptr(fi.raw))
                    {
                        dest_inner = ue::read_field<ue::FProperty *>(fi.raw, ue::fprop::ARRAY_INNER_OFF());
                        if (dest_inner && ue::is_mapped_ptr(dest_inner))
                            dest_elem_size = ue::fprop_get_element_size(dest_inner);
                        else
                            dest_inner = nullptr;
                    }

                    int32_t src_elem_size = src.element_size;
                    int32_t num = src.num();

                    // If element sizes match or we can't determine dest size, fast path
                    if (dest_elem_size <= 0 || src_elem_size <= 0 ||
                        dest_elem_size == src_elem_size || num <= 0)
                    {
                        std::memcpy(ptr, src.array_ptr, 16);
                        return true;
                    }

                    // Element size MISMATCH — need conversion
                    // Common case: source TArray<UObject*> (8 bytes) → dest TArray<TSoftObjectPtr> (40 bytes)
                    auto dest_type = dest_inner ? reflection::classify_property(
                                                      reinterpret_cast<const ue::FField *>(dest_inner))
                                                : reflection::PropType::Unknown;
                    auto src_type = src.inner_prop ? reflection::classify_property(
                                                         reinterpret_cast<const ue::FField *>(src.inner_prop))
                                                   : reflection::PropType::Unknown;

                    bool src_is_obj = (src_type == reflection::PropType::ObjectProperty ||
                                       src_type == reflection::PropType::WeakObjectProperty);
                    bool dest_is_soft = (dest_type == reflection::PropType::SoftObjectProperty ||
                                         dest_type == reflection::PropType::SoftClassProperty);

                    if (src_is_obj && dest_is_soft && num > 0 && num <= 256 && symbols::FName_Init)
                    {
                        // Convert UObject* → FSoftObjectPtr (proper FSoftObjectPath construction)
                        // Layout of FSoftObjectPtr on ARM64 UE5:
                        //   +0x00: FTopLevelAssetPath { FName PackageName(8), FName AssetName(8) } = 16 bytes
                        //   +0x10: FString SubPathString { TCHAR* Data(8), int32 Num(4), int32 Max(4) } = 16 bytes
                        //   +0x20: FWeakObjectPtr { int32 ObjectIndex(4), int32 ObjectSerialNumber(4) } = 8 bytes
                        //   Total: 40 bytes
                        // Note: dest_elem_size from reflection tells us the actual size on this build.

                        size_t buf_size = (size_t)num * (size_t)dest_elem_size;
                        uint8_t *new_data = static_cast<uint8_t *>(std::calloc(1, buf_size));
                        if (!new_data)
                        {
                            std::memcpy(ptr, src.array_ptr, 16);
                            return true;
                        }

                        const uint8_t *src_data = static_cast<const uint8_t *>(src.data());
                        for (int32_t i = 0; i < num; i++)
                        {
                            ue::UObject *obj = *reinterpret_cast<ue::UObject *const *>(
                                src_data + i * src_elem_size);
                            uint8_t *elem = new_data + i * dest_elem_size;

                            if (!obj || !ue::is_valid_ptr(obj))
                                continue; // Leave element zeroed (null soft ref)

                            // Build FSoftObjectPath from UObject's path
                            // GetFullName returns "PackageName.AssetName" (outer chain)
                            std::string full_path = reflection::get_full_name(obj);
                            if (full_path.empty())
                                continue;

                            // Split at last '.' → package path + asset name
                            size_t dot_pos = full_path.rfind('.');
                            std::string pkg_path, asset_name;
                            if (dot_pos != std::string::npos)
                            {
                                pkg_path = full_path.substr(0, dot_pos);
                                asset_name = full_path.substr(dot_pos + 1);
                            }
                            else
                            {
                                pkg_path = full_path;
                                asset_name = full_path;
                            }

                            // Convert to FNames using FName_Init
                            ue::FName pkg_fname = {0, 0};
                            ue::FName asset_fname = {0, 0};
                            {
                                std::u16string u16pkg(pkg_path.begin(), pkg_path.end());
                                symbols::FName_Init(&pkg_fname, u16pkg.c_str(), 0);
                            }
                            {
                                std::u16string u16asset(asset_name.begin(), asset_name.end());
                                symbols::FName_Init(&asset_fname, u16asset.c_str(), 0);
                            }

                            // Write FTopLevelAssetPath at element start
                            // PackageName FName at +0x00
                            std::memcpy(elem + 0, &pkg_fname, 8);
                            // AssetName FName at +0x08
                            std::memcpy(elem + 8, &asset_fname, 8);
                            // SubPathString (FString) at +0x10 = {nullptr, 0, 0} — already zeroed
                            // FWeakObjectPtr at +0x20 = {0, 0} — already zeroed (will be resolved lazily)
                        }

                        // Write TArray header: {Data*, Num, Max}
                        *reinterpret_cast<void **>(ptr) = new_data;
                        *reinterpret_cast<int32_t *>(ptr + 8) = num;
                        *reinterpret_cast<int32_t *>(ptr + 12) = num;

                        logger::log_info("TARRAY", "Converted TArray<UObject*>(%d) → TArray<SoftObj>(%d) "
                                                   "for field '%s': %d elems, src_sz=%d dest_sz=%d",
                                         src_elem_size, dest_elem_size, fi.name.c_str(),
                                         num, src_elem_size, dest_elem_size);
                        return true;
                    }

                    // Generic element size mismatch — zero-padded conversion (best effort)
                    if (num > 0 && num <= 256)
                    {
                        size_t buf_size = (size_t)num * (size_t)dest_elem_size;
                        uint8_t *new_data = static_cast<uint8_t *>(std::calloc(1, buf_size));
                        if (new_data)
                        {
                            const uint8_t *src_data = static_cast<const uint8_t *>(src.data());
                            int32_t copy_per_elem = (src_elem_size < dest_elem_size)
                                                        ? src_elem_size
                                                        : dest_elem_size;
                            for (int32_t i = 0; i < num; i++)
                            {
                                std::memcpy(new_data + i * dest_elem_size,
                                            src_data + i * src_elem_size,
                                            copy_per_elem);
                            }
                            *reinterpret_cast<void **>(ptr) = new_data;
                            *reinterpret_cast<int32_t *>(ptr + 8) = num;
                            *reinterpret_cast<int32_t *>(ptr + 12) = num;

                            logger::log_warn("TARRAY", "TArray element size mismatch for '%s': "
                                                       "src=%d dest=%d, zero-padded %d elems",
                                             fi.name.c_str(), src_elem_size, dest_elem_size, num);
                            return true;
                        }
                    }

                    // Fallback: raw memcpy (original behavior)
                    std::memcpy(ptr, src.array_ptr, 16);
                    return true;
                }
            }
            return false;
        }

        case reflection::PropType::SoftObjectProperty:
        case reflection::PropType::SoftClassProperty:
        {
            // TSoftObjectPtr / TSoftClassPtr — shallow copy from another struct field
            // These are typically FSoftObjectPath (FName + FString) — just memcpy the raw bytes
            if (value.is<LuaUStruct>())
            {
                const LuaUStruct &src = value.as<const LuaUStruct &>();
                if (src.data && src.size > 0)
                {
                    int32_t copy_size = (src.size < fi.element_size) ? src.size : fi.element_size;
                    std::memcpy(ptr, src.data, copy_size);
                    return true;
                }
            }
            // Also accept nil → zero out
            if (value == sol::nil)
            {
                std::memset(ptr, 0, fi.element_size);
                return true;
            }
            return false;
        }

        default:
            logger::log_warn("USTRUCT", "Cannot write field '%s' — unsupported type %d",
                             fi.name.c_str(), static_cast<int>(fi.type));
            return false;
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Property lookup cache — walk_properties is expensive, cache results
    // ═══════════════════════════════════════════════════════════════════════

    static std::unordered_map<ue::UStruct *, std::vector<reflection::PropertyInfo>> s_prop_cache;

    static const std::vector<reflection::PropertyInfo> &get_cached_props(ue::UStruct *ustruct)
    {
        auto it = s_prop_cache.find(ustruct);
        if (it != s_prop_cache.end())
            return it->second;
        auto props = reflection::walk_properties(ustruct, true);
        auto [inserted, _] = s_prop_cache.emplace(ustruct, std::move(props));
        return inserted->second;
    }

    static const reflection::PropertyInfo *find_field(ue::UStruct *ustruct, const std::string &name)
    {
        const auto &props = get_cached_props(ustruct);
        for (const auto &p : props)
        {
            if (p.name == name)
                return &p;
        }
        return nullptr;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Registration — UStruct usertype for sol2
    // ═══════════════════════════════════════════════════════════════════════

    void register_all(sol::state &lua)
    {
        lua.new_usertype<LuaUStruct>("UStruct", sol::no_constructor,

                                     "IsValid", [](const LuaUStruct &self) -> bool
                                     { return self.is_valid(); },

                                     "GetTypeName", [](const LuaUStruct &self) -> std::string
                                     {
            if (!self.ustruct) return "UStruct(unknown)";
            return reflection::get_short_name(reinterpret_cast<const ue::UObject*>(self.ustruct)); },

                                     "GetSize", [](const LuaUStruct &self) -> int32_t
                                     { return self.size; },

                                     "IsOwning", [](const LuaUStruct &self) -> bool
                                     { return self.owns_data; },

                                     // Clone — create an owning copy of this struct
                                     "Clone", [](const LuaUStruct &self) -> LuaUStruct
                                     {
            if (!self.data || self.size <= 0) return LuaUStruct{};
            return lua_ustruct::copy(self.data, self.ustruct, self.size); },

                                     // CopyFrom — copy data from another struct or table
                                     "CopyFrom", [](LuaUStruct &self, sol::object source) -> bool
                                     {
            if (!self.data || self.size <= 0) return false;
            if (source.is<LuaUStruct>()) {
                const LuaUStruct& src = source.as<const LuaUStruct&>();
                if (src.data && src.size > 0) {
                    int32_t copy_size = (src.size < self.size) ? src.size : self.size;
                    std::memcpy(self.data, src.data, copy_size);
                    return true;
                }
            } else if (source.get_type() == sol::type::table) {
                if (self.ustruct) {
                    fill_from_table(self.data, self.ustruct, source.as<sol::table>());
                    return true;
                }
            }
            return false; },

                                     // GetFields — return a table of {name=type_string} for all fields
                                     "GetFields", [](sol::this_state ts, const LuaUStruct &self) -> sol::object
                                     {
            sol::state_view lua(ts);
            if (!self.ustruct) return sol::nil;
            sol::table result = lua.create_table();
            const auto& props = get_cached_props(self.ustruct);
            for (const auto& p : props) {
                result[p.name] = p.inner_type_name.empty() ? "unknown" : p.inner_type_name;
            }
            return result; },

                                     // ── __index for dynamic field access: struct.X, struct.Y, etc. ──
                                     sol::meta_function::index, [](sol::this_state ts, LuaUStruct &self, const std::string &key) -> sol::object
                                     {
            sol::state_view lua(ts);
            if (!self.data || !self.ustruct) return sol::nil;

            const auto* fi = find_field(self.ustruct, key);
            if (!fi) {
                // Not a field — check if it's a known method
                // Sol2 checks usertype methods BEFORE __index, so we only get here
                // for keys that are NOT built-in methods (IsValid, GetTypeName, etc.)
                return sol::nil;
            }

            return read_field_value(lua, self.data, *fi); },

                                     // ── __newindex for dynamic field writing: struct.X = 100 ──
                                     sol::meta_function::new_index, [](LuaUStruct &self, const std::string &key, sol::object value)
                                     {
            if (!self.data || !self.ustruct) return;

            const auto* fi = find_field(self.ustruct, key);
            if (!fi) {
                logger::log_warn("USTRUCT", "Field '%s' not found on struct '%s'",
                               key.c_str(),
                               reflection::get_short_name(reinterpret_cast<const ue::UObject*>(self.ustruct)).c_str());
                return;
            }

            write_field_value(self.data, *fi, value); },

                                     // ── __tostring ──
                                     sol::meta_function::to_string, [](sol::this_state ts, const LuaUStruct &self) -> std::string
                                     {
            if (!self.ustruct || !self.data) return "UStruct(nil)";

            std::string type_name = reflection::get_short_name(
                reinterpret_cast<const ue::UObject*>(self.ustruct));

            // For common types, show field values inline
            const auto& props = get_cached_props(self.ustruct);
            if (props.empty()) {
                char buf[128];
                snprintf(buf, sizeof(buf), "UStruct(%s, %d bytes)", type_name.c_str(), self.size);
                return std::string(buf);
            }

            std::string fields;
            int count = 0;
            for (const auto& p : props) {
                if (count > 0) fields += ", ";
                if (count >= 6) { fields += "..."; break; }  // Limit output

                fields += p.name + "=";

                // Read the value and append a simple representation
                const uint8_t* ptr = self.data + p.offset;
                switch (p.type) {
                    case reflection::PropType::FloatProperty:
                        fields += std::to_string(*reinterpret_cast<const float*>(ptr));
                        break;
                    case reflection::PropType::DoubleProperty:
                        fields += std::to_string(*reinterpret_cast<const double*>(ptr));
                        break;
                    case reflection::PropType::IntProperty:
                        fields += std::to_string(*reinterpret_cast<const int32_t*>(ptr));
                        break;
                    case reflection::PropType::ByteProperty:
                        fields += std::to_string(static_cast<int>(ptr[0]));
                        break;
                    case reflection::PropType::BoolProperty:
                        if (p.bool_byte_mask)
                            fields += (ptr[p.bool_byte_offset] & p.bool_byte_mask) ? "true" : "false";
                        else
                            fields += *reinterpret_cast<const bool*>(ptr) ? "true" : "false";
                        break;
                    default:
                        fields += "?";
                        break;
                }
                count++;
            }

            return "UStruct(" + type_name + ": " + fields + ")"; },

                                     // ── Equality — same data pointer AND same struct type ──
                                     sol::meta_function::equal_to, [](const LuaUStruct &a, const LuaUStruct &b) -> bool
                                     { return a.data == b.data && a.ustruct == b.ustruct; });

        logger::log_info("LUA", "UStruct userdata type registered");
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Factory helpers
    // ═══════════════════════════════════════════════════════════════════════

    LuaUStruct wrap(uint8_t *data, ue::UStruct *ustruct, int32_t size)
    {
        LuaUStruct s;
        s.data = data;
        s.ustruct = ustruct;
        s.size = size;
        s.owns_data = false;
        return s;
    }

    LuaUStruct copy(const uint8_t *data, ue::UStruct *ustruct, int32_t size)
    {
        LuaUStruct s;
        if (data && size > 0)
        {
            s.data = new uint8_t[size];
            std::memcpy(s.data, data, size);
        }
        s.ustruct = ustruct;
        s.size = size;
        s.owns_data = true;
        return s;
    }

    LuaUStruct from_table(sol::state_view lua, const sol::table &tbl,
                          ue::UStruct *ustruct, int32_t size)
    {
        LuaUStruct s;
        if (size > 0)
        {
            s.data = new uint8_t[size];
            std::memset(s.data, 0, size);
        }
        s.ustruct = ustruct;
        s.size = size;
        s.owns_data = true;

        if (ustruct)
        {
            fill_from_table(s.data, ustruct, tbl);
        }

        return s;
    }

    void fill_from_table(uint8_t *data, ue::UStruct *ustruct, const sol::table &tbl)
    {
        if (!data || !ustruct)
            return;

        const auto &props = get_cached_props(ustruct);

        for (const auto &p : props)
        {
            sol::optional<sol::object> val = tbl[p.name];
            if (val && val->valid() && val->get_type() != sol::type::lua_nil)
            {
                write_field_value(data, p, *val);
            }
        }
    }

} // namespace lua_ustruct
