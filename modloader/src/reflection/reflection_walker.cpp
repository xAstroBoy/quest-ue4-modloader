// modloader/src/reflection/reflection_walker.cpp
// Walks the live UE4 reflection graph — GUObjectArray + GNames
// Extracts all UClass, UStruct, UEnum, UFunction, FProperty
// Ported from UE4Dumper resolution logic with RE4 VR ARM64 offsets

#include "modloader/reflection_walker.h"
#include "modloader/symbols.h"
#include "modloader/logger.h"
#include "modloader/types.h"
#include <cstring>
#include <algorithm>
#include <mutex>
#include <cstdio>

namespace reflection
{

    // ═══ Internal state ═════════════════════════════════════════════════════
    static std::vector<ClassInfo> s_classes;
    static std::vector<StructInfo> s_structs;
    static std::vector<EnumInfo> s_enums;

    static std::unordered_map<std::string, size_t> s_class_map; // name → index in s_classes
    static std::unordered_map<std::string, size_t> s_struct_map;
    static std::unordered_map<std::string, size_t> s_enum_map;

    static int32_t s_object_count = 0;

    // FNamePool pointer (GNames + 0x30 → FNamePool base)
    static uintptr_t s_fname_pool = 0;

    // GUObjectArray resolved pointer
    static uintptr_t s_guobject_array = 0;

    // ═══ Memory map snapshot for safe pointer validation ════════════════════
    // Snapshots /proc/self/maps once at the start of walk_all() to determine
    // which memory ranges are actually readable. This prevents SIGSEGV from
    // dereferencing pointers that pass is_valid_ptr() but point to unmapped pages.
    struct MemRange
    {
        uintptr_t start;
        uintptr_t end;
    };
    static std::vector<MemRange> s_readable_ranges;

    static void build_memory_map()
    {
        s_readable_ranges.clear();
        FILE *f = fopen("/proc/self/maps", "r");
        if (!f)
        {
            logger::log_warn("REFLECT", "Cannot open /proc/self/maps — falling back to is_valid_ptr()");
            return;
        }
        char line[512];
        while (fgets(line, sizeof(line), f))
        {
            uintptr_t start = 0, end = 0;
            char perms[5] = {};
            if (sscanf(line, "%lx-%lx %4s", &start, &end, perms) == 3)
            {
                if (perms[0] == 'r')
                { // readable
                    s_readable_ranges.push_back({start, end});
                }
            }
        }
        fclose(f);
        // Sort for binary search
        std::sort(s_readable_ranges.begin(), s_readable_ranges.end(),
                  [](const MemRange &a, const MemRange &b)
                  { return a.start < b.start; });
        logger::log_info("REFLECT", "Memory map snapshot: %zu readable ranges", s_readable_ranges.size());
    }

    // Public API: refresh /proc/self/maps snapshot so that on-demand rebuilds
    // can validate pointers in memory regions allocated after boot.
    void refresh_memory_map()
    {
        build_memory_map();
    }

    // Check if a pointer is in a readable memory range (from /proc/self/maps snapshot)
    static bool is_readable_ptr(const void *p)
    {
        if (!p)
            return false;
        uintptr_t addr = reinterpret_cast<uintptr_t>(p);
        if (addr < 0x10000)
            return false;

        // If memory map wasn't built, fall back to basic range check
        if (s_readable_ranges.empty())
            return ue::is_valid_ptr(p);

        // Binary search: find the first range whose start > addr, then check the one before it
        auto it = std::upper_bound(s_readable_ranges.begin(), s_readable_ranges.end(), addr,
                                   [](uintptr_t val, const MemRange &r)
                                   { return val < r.start; });
        if (it != s_readable_ranges.begin())
        {
            --it;
            return addr >= it->start && addr < it->end;
        }
        return false;
    }

    // ═══ FName resolution ═══════════════════════════════════════════════════
    // RE4 VR FNamePool layout:
    //   FNamePool + 0x08 = CurrentBlock (int32)
    //   FNamePool + 0x0C = CurrentByte  (int32)
    //   FNamePool + 0x10 = Blocks[8192] (void* array)
    // Each block is a contiguous allocation of FNameEntry records.
    // FNameEntry: +0x00 = uint16 header, +0x02 = chars
    //   header bit 0 = bIsWide, bits 6..15 = length
    // Stride = 2 (entries aligned to 2 bytes)

    std::string fname_to_string(int32_t index)
    {
        if (index < 0 || s_fname_pool == 0)
            return "";

        // Split index into block index and offset within block
        // FName ComparisonIndex encoding: upper bits = block, lower 16 bits = offset/stride
        int32_t block_idx = index >> ue::FNAME_BLOCK_BITS;
        int32_t offset_in_block = index & ((1 << ue::FNAME_BLOCK_BITS) - 1);

        if (block_idx < 0 || block_idx >= static_cast<int32_t>(ue::FNAME_MAX_BLOCKS))
            return "";

        // Read block pointer from FNamePool.Blocks[block_idx]
        uintptr_t blocks_base = s_fname_pool + ue::FNAMEPOOL_TO_BLOCKS;
        uintptr_t block_ptr = ue::read_field<uintptr_t>(
            reinterpret_cast<const void *>(blocks_base), block_idx * 8);

        if (!ue::is_valid_ptr(reinterpret_cast<const void *>(block_ptr)))
            return "";

        // Compute entry address within block
        uintptr_t entry_addr = block_ptr + static_cast<uintptr_t>(offset_in_block) * ue::FNAME_STRIDE;
        if (!ue::is_valid_ptr(reinterpret_cast<const void *>(entry_addr)))
            return "";

        // Read header
        uint16_t header = *reinterpret_cast<const uint16_t *>(entry_addr);
        bool is_wide = (header & 1) != 0;
        int length = (header >> ue::FNAMENTRY_LEN_SHIFT);

        if (length <= 0 || length > 1024)
            return "";

        const char *chars = reinterpret_cast<const char *>(entry_addr + ue::FNAMENTRY_HEADER_SIZE);

        if (is_wide)
        {
            // Wide chars — convert to narrow (ASCII only in game names)
            std::string result;
            result.reserve(length);
            const uint16_t *wchars = reinterpret_cast<const uint16_t *>(chars);
            for (int i = 0; i < length; i++)
            {
                result.push_back(static_cast<char>(wchars[i] & 0xFF));
            }
            return result;
        }

        return std::string(chars, length);
    }

    // ═══ Walk ALL FNamePool entries by traversing blocks sequentially ════════
    // This is the CORRECT way to enumerate all FNames.
    // Sequential index iteration (0,1,2,3...) is WRONG because FName
    // ComparisonIndex = (block_idx << BLOCK_BITS) | (byte_offset / STRIDE).
    // Index 1 would point 2 bytes into block 0 = middle of first entry = garbage.
    //
    // Instead, we walk each block from byte 0, reading entry headers to determine
    // actual entry size, and compute the correct FName index for each entry.
    void walk_all_fnames(const std::function<void(int32_t index, const std::string &name)> &callback)
    {
        if (s_fname_pool == 0)
            return;

        int32_t current_block = ue::read_field<int32_t>(
            reinterpret_cast<const void *>(s_fname_pool), ue::FNAMEPOOL_CURRENT_BLOCK);
        int32_t current_byte = ue::read_field<int32_t>(
            reinterpret_cast<const void *>(s_fname_pool), ue::FNAMEPOOL_CURRENT_BYTE);

        if (current_block < 0 || current_block >= static_cast<int32_t>(ue::FNAME_MAX_BLOCKS))
        {
            logger::log_error("REFLECT", "walk_all_fnames: invalid CurrentBlock=%d", current_block);
            return;
        }

        uintptr_t blocks_base = s_fname_pool + ue::FNAMEPOOL_TO_BLOCKS;

        for (int32_t blk = 0; blk <= current_block; blk++)
        {
            // Read block pointer
            uintptr_t block_ptr = ue::read_field<uintptr_t>(
                reinterpret_cast<const void *>(blocks_base), blk * 8);

            if (!ue::is_valid_ptr(reinterpret_cast<const void *>(block_ptr)))
                continue;

            // Determine the byte limit for this block
            // For the last (current) block, use CurrentByte
            // For earlier blocks, we walk until we hit an invalid entry or a
            // reasonable upper bound. FNamePool blocks are typically 64KB or 128KB.
            // We use a safe cap of 256KB per block.
            int32_t byte_limit;
            if (blk == current_block)
            {
                byte_limit = current_byte;
            }
            else
            {
                // For non-current blocks, walk until we find entries with 0 length
                // Use a generous cap — FNamePool blocks can be up to 256KB
                byte_limit = 256 * 1024;
            }

            int32_t byte_pos = 0;
            int consecutive_empty = 0;
            const int MAX_CONSECUTIVE_EMPTY = 4; // 4 consecutive zero-length entries = end of block

            while (byte_pos < byte_limit)
            {
                uintptr_t entry_addr = block_ptr + byte_pos;
                if (!is_readable_ptr(reinterpret_cast<const void *>(entry_addr)))
                    break;

                // Read header
                uint16_t header = *reinterpret_cast<const uint16_t *>(entry_addr);
                bool is_wide = (header & 1) != 0;
                int length = (header >> ue::FNAMENTRY_LEN_SHIFT);

                if (length <= 0 || length > 1024)
                {
                    // Zero or invalid length — could be padding or end of block
                    // Advance by minimum stride and track empties
                    byte_pos += ue::FNAME_STRIDE;
                    consecutive_empty++;
                    if (consecutive_empty >= MAX_CONSECUTIVE_EMPTY && blk < current_block)
                        break; // End of this non-current block
                    continue;
                }
                consecutive_empty = 0;

                // Compute the FName ComparisonIndex for this entry
                int32_t fname_index = (blk << ue::FNAME_BLOCK_BITS) | (byte_pos / ue::FNAME_STRIDE);

                // Read the string
                const char *chars = reinterpret_cast<const char *>(entry_addr + ue::FNAMENTRY_HEADER_SIZE);
                std::string name;

                if (is_wide)
                {
                    const uint16_t *wchars = reinterpret_cast<const uint16_t *>(chars);
                    name.reserve(length);
                    for (int i = 0; i < length; i++)
                    {
                        name.push_back(static_cast<char>(wchars[i] & 0xFF));
                    }
                }
                else
                {
                    name = std::string(chars, length);
                }

                if (!name.empty())
                {
                    callback(fname_index, name);
                }

                // Compute entry size: header + string data, aligned up to stride
                int char_size = is_wide ? 2 : 1;
                int raw_size = ue::FNAMENTRY_HEADER_SIZE + length * char_size;
                // Align up to FNAME_STRIDE
                int entry_size = ((raw_size + ue::FNAME_STRIDE - 1) / ue::FNAME_STRIDE) * ue::FNAME_STRIDE;
                if (entry_size < static_cast<int>(ue::FNAME_STRIDE))
                    entry_size = ue::FNAME_STRIDE;

                byte_pos += entry_size;
            }
        }
    }

    // ═══ FName reverse lookup (string → comparison index) ═══════════════════
    // Uses the block walker to build a full cache on first miss, then O(1) lookups.
    static std::unordered_map<std::string, int32_t> s_fname_reverse_cache;
    static std::mutex s_fname_reverse_mutex;
    static bool s_fname_reverse_built = false;

    // Build the full reverse cache by walking all FNamePool entries
    static void build_fname_reverse_cache()
    {
        if (s_fname_reverse_built)
            return;

        logger::log_info("REFLECT", "Building FName reverse cache...");
        int count = 0;
        walk_all_fnames([&](int32_t index, const std::string &name)
                        {
            s_fname_reverse_cache[name] = index;
            count++; });
        s_fname_reverse_built = true;
        logger::log_info("REFLECT", "FName reverse cache built: %d entries", count);
    }

    int32_t fname_string_to_index(const std::string &name)
    {
        std::lock_guard<std::mutex> lock(s_fname_reverse_mutex);

        // Check cache first
        auto it = s_fname_reverse_cache.find(name);
        if (it != s_fname_reverse_cache.end())
            return it->second;

        // Build the full cache on first miss
        if (!s_fname_reverse_built)
        {
            build_fname_reverse_cache();
            auto it2 = s_fname_reverse_cache.find(name);
            if (it2 != s_fname_reverse_cache.end())
                return it2->second;
        }

        return 0; // Not found
    }

    // ═══ Object name helpers ════════════════════════════════════════════════
    std::string get_short_name(const ue::UObject *obj)
    {
        if (!obj)
            return "";
        int32_t name_idx = ue::uobj_get_name_index(obj);
        std::string name = fname_to_string(name_idx);
        int32_t number = ue::uobj_get_name_number(obj);
        if (number > 0)
        {
            name += "_" + std::to_string(number - 1);
        }
        return name;
    }

    std::string get_full_name(const ue::UObject *obj)
    {
        if (!obj)
            return "";

        std::string result = get_short_name(obj);
        ue::UObject *outer = ue::uobj_get_outer(obj);

        while (outer && ue::is_valid_ptr(outer))
        {
            std::string outer_name = get_short_name(outer);
            if (outer_name.empty())
                break;
            result = outer_name + "." + result;
            outer = ue::uobj_get_outer(outer);
        }

        return result;
    }

    std::string get_package_name(const ue::UObject *obj)
    {
        if (!obj)
            return "UnknownPackage";
        // Walk the outer chain to the top-level UPackage
        const ue::UObject *current = obj;
        const ue::UObject *top = obj;
        while (current && ue::is_valid_ptr(current))
        {
            top = current;
            ue::UObject *outer = ue::uobj_get_outer(current);
            if (!outer || !ue::is_valid_ptr(outer))
                break;
            current = outer;
        }
        std::string name = get_short_name(top);
        return name.empty() ? "UnknownPackage" : name;
    }

    // ═══ Get FFieldClass name (for property type classification) ════════════
    static std::string get_ffield_class_name(const ue::FField *field)
    {
        if (!field || !is_readable_ptr(field))
            return "";
        ue::FFieldClass *fc = ue::ffield_get_class(field);
        if (!is_readable_ptr(fc))
            return "";
        // FFieldClass has Name (FName) at offset 0x00
        int32_t name_idx = ue::read_field<int32_t>(fc, 0x00);
        return fname_to_string(name_idx);
    }

    // ═══ Property type classification ═══════════════════════════════════════
    PropType classify_property(const ue::FField *field)
    {
        std::string cn = get_ffield_class_name(field);
        if (cn.empty())
            return PropType::Unknown;

        if (cn == "BoolProperty")
            return PropType::BoolProperty;
        if (cn == "ByteProperty")
            return PropType::ByteProperty;
        if (cn == "Int8Property")
            return PropType::Int8Property;
        if (cn == "Int16Property")
            return PropType::Int16Property;
        if (cn == "IntProperty")
            return PropType::IntProperty;
        if (cn == "Int64Property")
            return PropType::Int64Property;
        if (cn == "UInt16Property")
            return PropType::UInt16Property;
        if (cn == "UInt32Property")
            return PropType::UInt32Property;
        if (cn == "UInt64Property")
            return PropType::UInt64Property;
        if (cn == "FloatProperty")
            return PropType::FloatProperty;
        if (cn == "DoubleProperty")
            return PropType::DoubleProperty;
        if (cn == "NameProperty")
            return PropType::NameProperty;
        if (cn == "StrProperty")
            return PropType::StrProperty;
        if (cn == "TextProperty")
            return PropType::TextProperty;
        if (cn == "ObjectProperty")
            return PropType::ObjectProperty;
        if (cn == "WeakObjectProperty")
            return PropType::WeakObjectProperty;
        if (cn == "LazyObjectProperty")
            return PropType::LazyObjectProperty;
        if (cn == "SoftObjectProperty")
            return PropType::SoftObjectProperty;
        if (cn == "ClassProperty")
            return PropType::ClassProperty;
        if (cn == "SoftClassProperty")
            return PropType::SoftClassProperty;
        if (cn == "InterfaceProperty")
            return PropType::InterfaceProperty;
        if (cn == "StructProperty")
            return PropType::StructProperty;
        if (cn == "ArrayProperty")
            return PropType::ArrayProperty;
        if (cn == "MapProperty")
            return PropType::MapProperty;
        if (cn == "SetProperty")
            return PropType::SetProperty;
        if (cn == "EnumProperty")
            return PropType::EnumProperty;
        if (cn == "DelegateProperty")
            return PropType::DelegateProperty;
        if (cn == "MulticastDelegateProperty")
            return PropType::MulticastDelegateProperty;
        if (cn == "MulticastInlineDelegateProperty")
            return PropType::MulticastInlineDelegateProperty;
        if (cn == "MulticastSparseDelegateProperty")
            return PropType::MulticastSparseDelegateProperty;
        if (cn == "FieldPathProperty")
            return PropType::FieldPathProperty;

        return PropType::Unknown;
    }

    static std::string prop_type_to_string(PropType pt)
    {
        switch (pt)
        {
        case PropType::BoolProperty:
            return "bool";
        case PropType::ByteProperty:
            return "uint8";
        case PropType::Int8Property:
            return "int8";
        case PropType::Int16Property:
            return "int16";
        case PropType::IntProperty:
            return "int32";
        case PropType::Int64Property:
            return "int64";
        case PropType::UInt16Property:
            return "uint16";
        case PropType::UInt32Property:
            return "uint32";
        case PropType::UInt64Property:
            return "uint64";
        case PropType::FloatProperty:
            return "float";
        case PropType::DoubleProperty:
            return "double";
        case PropType::NameProperty:
            return "FName";
        case PropType::StrProperty:
            return "FString";
        case PropType::TextProperty:
            return "FText";
        case PropType::ObjectProperty:
            return "UObject*";
        case PropType::WeakObjectProperty:
            return "TWeakObjectPtr";
        case PropType::LazyObjectProperty:
            return "TLazyObjectPtr";
        case PropType::SoftObjectProperty:
            return "TSoftObjectPtr";
        case PropType::ClassProperty:
            return "UClass*";
        case PropType::SoftClassProperty:
            return "TSoftClassPtr";
        case PropType::InterfaceProperty:
            return "FScriptInterface";
        case PropType::StructProperty:
            return "struct";
        case PropType::ArrayProperty:
            return "TArray";
        case PropType::MapProperty:
            return "TMap";
        case PropType::SetProperty:
            return "TSet";
        case PropType::EnumProperty:
            return "enum";
        case PropType::DelegateProperty:
            return "FDelegate";
        case PropType::MulticastDelegateProperty:
            return "FMulticastDelegate";
        case PropType::MulticastInlineDelegateProperty:
            return "FMulticastInlineDelegate";
        case PropType::MulticastSparseDelegateProperty:
            return "FMulticastSparseDelegate";
        case PropType::FieldPathProperty:
            return "FFieldPath";
        default:
            return "unknown";
        }
    }

    // ═══ Get inner type name for typed properties ═══════════════════════════
    static std::string get_inner_type_name(const ue::FProperty *prop, PropType pt)
    {
        switch (pt)
        {
        case PropType::ObjectProperty:
        case PropType::WeakObjectProperty:
        case PropType::LazyObjectProperty:
        case PropType::SoftObjectProperty:
        {
            ue::UClass *inner = ue::read_field<ue::UClass *>(prop, ue::fprop::OBJ_PROPERTY_CLASS_OFF());
            if (is_readable_ptr(inner))
            {
                return get_short_name(reinterpret_cast<const ue::UObject *>(inner));
            }
            return "";
        }
        case PropType::ClassProperty:
        {
            ue::UClass *meta = ue::read_field<ue::UClass *>(prop, ue::fprop::CLASS_META_CLASS_OFF());
            if (is_readable_ptr(meta))
            {
                return get_short_name(reinterpret_cast<const ue::UObject *>(meta));
            }
            return "";
        }
        case PropType::StructProperty:
        {
            ue::UStruct *inner = ue::read_field<ue::UStruct *>(prop, ue::fprop::STRUCT_INNER_STRUCT_OFF());
            if (is_readable_ptr(inner))
            {
                return get_short_name(reinterpret_cast<const ue::UObject *>(inner));
            }
            return "";
        }
        case PropType::EnumProperty:
        {
            ue::UEnum *inner = ue::read_field<ue::UEnum *>(prop, ue::fprop::ENUM_PROP_ENUM_OFF());
            if (is_readable_ptr(inner))
            {
                return get_short_name(reinterpret_cast<const ue::UObject *>(inner));
            }
            return "";
        }
        case PropType::ArrayProperty:
        {
            ue::FProperty *inner = ue::read_field<ue::FProperty *>(prop, ue::fprop::ARRAY_INNER_OFF());
            if (is_readable_ptr(inner))
            {
                PropType inner_pt = classify_property(reinterpret_cast<const ue::FField *>(inner));
                return prop_type_to_string(inner_pt);
            }
            return "";
        }
        default:
            return "";
        }
    }

    // ═══ Walk properties of a UStruct ═══════════════════════════════════════
    std::vector<PropertyInfo> walk_properties(const ue::UStruct *ustruct, bool include_inherited)
    {
        std::vector<PropertyInfo> result;
        if (!ustruct)
            return result;

        const ue::UStruct *current = ustruct;
        while (current)
        {
            ue::FField *field = ue::ustruct_get_child_properties(current);
            // Safety: log and skip if ChildProperties points to unmapped memory
            if (field && !is_readable_ptr(field))
            {
                logger::log_warn("REFLECT", "  walk_properties: UStruct 0x%lX has unmapped ChildProperties 0x%lX — skipping",
                                 (unsigned long)current, (unsigned long)field);
                break;
            }
            while (field && is_readable_ptr(field))
            {
                PropType pt = classify_property(field);
                if (pt != PropType::Unknown)
                {
                    PropertyInfo pi;
                    int32_t name_idx = ue::ffield_get_name_index(field);
                    pi.name = fname_to_string(name_idx);
                    pi.type = pt;
                    pi.raw = reinterpret_cast<ue::FProperty *>(const_cast<ue::FField *>(field));
                    pi.offset = ue::fprop_get_offset(pi.raw);
                    pi.element_size = ue::fprop_get_element_size(pi.raw);
                    pi.flags = ue::fprop_get_flags(pi.raw);

                    // Bool specifics
                    if (pt == PropType::BoolProperty)
                    {
                        pi.bool_field_size = ue::read_field<uint8_t>(field, ue::fprop::BOOL_FIELD_SIZE_OFF());
                        pi.bool_byte_offset = ue::read_field<uint8_t>(field, ue::fprop::BOOL_BYTE_OFFSET_OFF());
                        pi.bool_byte_mask = ue::read_field<uint8_t>(field, ue::fprop::BOOL_BYTE_MASK_OFF());
                        pi.bool_field_mask = ue::read_field<uint8_t>(field, ue::fprop::BOOL_FIELD_MASK_OFF());
                    }
                    else
                    {
                        pi.bool_field_size = 0;
                        pi.bool_byte_offset = 0;
                        pi.bool_byte_mask = 0;
                        pi.bool_field_mask = 0;
                    }

                    // Inner type name
                    pi.inner_type_name = get_inner_type_name(pi.raw, pt);
                    pi.inner_type_name2 = "";

                    if (pt == PropType::MapProperty)
                    {
                        ue::FProperty *val_prop = ue::read_field<ue::FProperty *>(field, ue::fprop::MAP_VALUE_PROP_OFF());
                        if (is_readable_ptr(val_prop))
                        {
                            PropType val_pt = classify_property(reinterpret_cast<const ue::FField *>(val_prop));
                            pi.inner_type_name2 = prop_type_to_string(val_pt);
                        }
                    }

                    result.push_back(pi);
                }

                field = ue::ffield_get_next(field);
            }

            if (include_inherited)
            {
                ue::UStruct *sup = ue::ustruct_get_super(current);
                current = (sup && is_readable_ptr(sup)) ? sup : nullptr;
            }
            else
            {
                break;
            }
        }

        return result;
    }

    // ═══ Walk functions of a UStruct ════════════════════════════════════════
    std::vector<FunctionInfo> walk_functions(const ue::UStruct *ustruct, bool include_inherited)
    {
        std::vector<FunctionInfo> result;
        if (!ustruct)
            return result;

        const ue::UStruct *current = ustruct;
        while (current)
        {
            ue::UField *child = ue::ustruct_get_children(current);
            // Safety: log and skip if Children points to unmapped memory
            if (child && !is_readable_ptr(child))
            {
                logger::log_warn("REFLECT", "  walk_functions: UStruct 0x%lX has unmapped Children 0x%lX — skipping",
                                 (unsigned long)current, (unsigned long)child);
                break;
            }
            while (child && is_readable_ptr(child))
            {
                // Check if this UField is a UFunction by examining its class
                ue::UClass *field_class = ue::uobj_get_class(reinterpret_cast<const ue::UObject *>(child));
                if (field_class && is_readable_ptr(field_class))
                {
                    std::string class_name = get_short_name(reinterpret_cast<const ue::UObject *>(field_class));
                    if (class_name == "Function" || class_name == "DelegateFunction")
                    {
                        ue::UFunction *func = reinterpret_cast<ue::UFunction *>(child);
                        FunctionInfo fi;
                        fi.name = get_short_name(reinterpret_cast<const ue::UObject *>(func));
                        fi.flags = ue::ufunc_get_flags(func);
                        fi.parms_size = ue::ufunc_get_parms_size(func);
                        fi.num_parms = ue::ufunc_get_num_parms(func);
                        fi.return_value_offset = ue::ufunc_get_return_value_offset(func);
                        fi.raw = func;
                        fi.return_prop = nullptr;

                        // Walk function parameters (properties of the UFunction itself)
                        fi.params = walk_properties(reinterpret_cast<const ue::UStruct *>(func), false);

                        // Find return property
                        for (auto &p : fi.params)
                        {
                            if (p.flags & ue::CPF_ReturnParm)
                            {
                                fi.return_prop = &p;
                                break;
                            }
                        }

                        result.push_back(fi);
                    }
                }

                child = ue::ufield_get_next(child);
            }

            if (include_inherited)
            {
                ue::UStruct *sup = ue::ustruct_get_super(current);
                current = (sup && is_readable_ptr(sup)) ? sup : nullptr;
            }
            else
            {
                break;
            }
        }

        return result;
    }

    // ═══ UObject iteration via GUObjectArray ════════════════════════════════
    static ue::UObject *get_object_at_index(int32_t index)
    {
        if (s_guobject_array == 0)
            return nullptr;

        // FChunkedFixedUObjectArray is EMBEDDED at GUObjectArray + 0x10
        // First field (offset 0x00 within embedded struct) = Objects** (pointer to chunk array)
        uintptr_t embedded_start = s_guobject_array + ue::GUOBJECTARRAY_TO_OBJECTS;
        uintptr_t obj_objects = ue::read_field<uintptr_t>(
            reinterpret_cast<const void *>(embedded_start), 0);

        if (!ue::is_valid_ptr(reinterpret_cast<const void *>(obj_objects)))
            return nullptr;

        // Chunked array: each chunk holds FUOBJECTITEM_CHUNK_SIZE items
        int32_t chunk_idx = index / static_cast<int32_t>(ue::FUOBJECTITEM_CHUNK_SIZE);
        int32_t within_chunk = index % static_cast<int32_t>(ue::FUOBJECTITEM_CHUNK_SIZE);

        // Read chunk pointer: ObjObjects.Objects + chunk_idx * 8
        uintptr_t chunk_ptr = ue::read_field<uintptr_t>(
            reinterpret_cast<const void *>(obj_objects), chunk_idx * 8);

        if (!ue::is_valid_ptr(reinterpret_cast<const void *>(chunk_ptr)))
            return nullptr;

        // Read UObject* from within chunk: chunk_ptr + within_chunk * FUOBJECTITEM_SIZE
        uintptr_t item_addr = chunk_ptr + static_cast<uintptr_t>(within_chunk) * FUOBJECTITEM_SIZE;
        ue::UObject *obj = ue::read_field<ue::UObject *>(
            reinterpret_cast<const void *>(item_addr), 0);

        return obj;
    }

    static int32_t get_num_elements()
    {
        if (s_guobject_array == 0)
            return 0;
        // FChunkedFixedUObjectArray is EMBEDDED at GUObjectArray + 0x10
        // NumElements is at offset 0x14 within the embedded struct
        uintptr_t embedded_start = s_guobject_array + ue::GUOBJECTARRAY_TO_OBJECTS;
        return ue::read_field<int32_t>(
            reinterpret_cast<const void *>(embedded_start), ue::TUOBJECTARRAY_NUM_ELEMENTS);
    }

    // ═══ Check if a UObject is a UClass ═════════════════════════════════════
    static bool is_uclass(const ue::UObject *obj)
    {
        if (!obj || !ue::is_valid_ptr(obj))
            return false;
        ue::UClass *cls = ue::uobj_get_class(obj);
        if (!cls || !ue::is_valid_ptr(cls))
            return false;
        std::string class_name = get_short_name(reinterpret_cast<const ue::UObject *>(cls));
        return (class_name == "Class" || class_name == "BlueprintGeneratedClass" || class_name == "WidgetBlueprintGeneratedClass" || class_name == "AnimBlueprintGeneratedClass");
    }

    static bool is_ustruct(const ue::UObject *obj)
    {
        if (!obj || !ue::is_valid_ptr(obj))
            return false;
        ue::UClass *cls = ue::uobj_get_class(obj);
        if (!cls || !ue::is_valid_ptr(cls))
            return false;
        std::string class_name = get_short_name(reinterpret_cast<const ue::UObject *>(cls));
        return (class_name == "ScriptStruct");
    }

    static bool is_uenum(const ue::UObject *obj)
    {
        if (!obj || !ue::is_valid_ptr(obj))
            return false;
        ue::UClass *cls = ue::uobj_get_class(obj);
        if (!cls || !ue::is_valid_ptr(cls))
            return false;
        std::string class_name = get_short_name(reinterpret_cast<const ue::UObject *>(cls));
        return (class_name == "Enum" || class_name == "UserDefinedEnum");
    }

    // ═══ Init ═══════════════════════════════════════════════════════════════
    void init()
    {
        // Resolve GUObjectArray pointer
        if (symbols::GUObjectArray)
        {
            s_guobject_array = reinterpret_cast<uintptr_t>(symbols::GUObjectArray);
            logger::log_info("REFLECT", "GUObjectArray at 0x%lX", (unsigned long)s_guobject_array);

            // Diagnostic: read and log the embedded FChunkedFixedUObjectArray fields
            uintptr_t embedded = s_guobject_array + ue::GUOBJECTARRAY_TO_OBJECTS;
            uintptr_t obj_ptrs = ue::read_field<uintptr_t>(reinterpret_cast<const void *>(embedded), 0);
            int32_t num_elements = ue::read_field<int32_t>(
                reinterpret_cast<const void *>(embedded), ue::TUOBJECTARRAY_NUM_ELEMENTS);
            int32_t max_elements = ue::read_field<int32_t>(
                reinterpret_cast<const void *>(embedded), ue::TUOBJECTARRAY_NUM_ELEMENTS + 4);

            logger::log_info("REFLECT", "  Embedded FChunkedFixedUObjectArray at 0x%lX", (unsigned long)embedded);
            // Check if Objects** looks like a real heap pointer (arm64 Android: 0x7a+ range)
            bool obj_ptrs_valid = obj_ptrs > 0x700000000ULL && obj_ptrs < 0x800000000000ULL;
            logger::log_info("REFLECT", "  Objects**: 0x%lX (%s)",
                             (unsigned long)obj_ptrs,
                             obj_ptrs_valid ? "valid" : "INVALID/uninitialized");
            logger::log_info("REFLECT", "  NumElements: %d", num_elements);
            logger::log_info("REFLECT", "  MaxElements(+4): %d", max_elements);

            // Only probe further if Objects** is a real heap pointer
            // GUObjectArray may not be populated yet — deferred thread handles the wait
            if (obj_ptrs_valid)
            {
                uintptr_t chunk0 = ue::read_field<uintptr_t>(reinterpret_cast<const void *>(obj_ptrs), 0);
                logger::log_info("REFLECT", "  Chunk[0]: 0x%lX (%s)",
                                 (unsigned long)chunk0,
                                 ue::is_valid_ptr(reinterpret_cast<const void *>(chunk0)) ? "valid" : "INVALID");

                if (ue::is_valid_ptr(reinterpret_cast<const void *>(chunk0)))
                {
                    ue::UObject *first_obj = ue::read_field<ue::UObject *>(
                        reinterpret_cast<const void *>(chunk0), 0);
                    logger::log_info("REFLECT", "  Object[0]: 0x%lX (%s)",
                                     (unsigned long)first_obj,
                                     (first_obj && ue::is_valid_ptr(first_obj)) ? "valid" : "INVALID/null");
                }
            }
            else
            {
                logger::log_warn("REFLECT", "  Objects** not populated yet — deferred thread will wait for engine init");
            }
        }
        else
        {
            logger::log_error("REFLECT", "GUObjectArray is null — cannot walk objects");
            return;
        }

        // Resolve GNames → FNamePool
        if (symbols::GNames)
        {
            uintptr_t gnames_addr = reinterpret_cast<uintptr_t>(symbols::GNames);
            s_fname_pool = gnames_addr + ue::GNNAMES_TO_FNAMEPOOL;
            logger::log_info("REFLECT", "FNamePool at 0x%lX (GNames + 0x%X)",
                             (unsigned long)s_fname_pool, ue::GNNAMES_TO_FNAMEPOOL);

            // Verify by reading a known name (index 0 should be "None")
            std::string test = fname_to_string(0);
            if (test == "None" || test == "none")
            {
                logger::log_info("REFLECT", "FName[0] = \"%s\" — name resolution working", test.c_str());
            }
            else
            {
                logger::log_warn("REFLECT", "FName[0] = \"%s\" — unexpected, name resolution may be misconfigured", test.c_str());
            }

            // Walk the first few REAL FNamePool entries (matching UE4Dumper's DumpBlocks423 logic)
            // FName indices are NOT sequential — they encode byte position in block.
            // Must iterate packed entries by reading each header to compute the next valid index.
            {
                uintptr_t blocks_base = s_fname_pool + ue::FNAMEPOOL_TO_BLOCKS;
                uintptr_t block0_ptr = ue::read_field<uintptr_t>(
                    reinterpret_cast<const void *>(blocks_base), 0);

                if (block0_ptr && ue::is_valid_ptr(reinterpret_cast<const void *>(block0_ptr)))
                {
                    // Read CurrentByteCursor to know block 0 size
                    uint32_t current_block = ue::read_field<uint32_t>(
                        reinterpret_cast<const void *>(s_fname_pool), ue::FNAMEPOOL_CURRENT_BLOCK);
                    uint32_t current_byte = ue::read_field<uint32_t>(
                        reinterpret_cast<const void *>(s_fname_pool), ue::FNAMEPOOL_CURRENT_BYTE);
                    uint32_t block0_size = (current_block == 0) ? current_byte : (ue::FNAME_STRIDE * 65536);

                    logger::log_info("REFLECT", "FNamePool: CurrentBlock=%u CurrentByteCursor=%u Block0Size=%u",
                                     current_block, current_byte, block0_size);

                    uintptr_t it = block0_ptr;
                    uintptr_t end = block0_ptr + block0_size - ue::FNAMENTRY_HEADER_SIZE;
                    int entry_count = 0;
                    int max_test = 8; // show first 8 real entries

                    while (it < end && entry_count < max_test)
                    {
                        uint16_t header = *reinterpret_cast<const uint16_t *>(it);
                        int str_len = header >> ue::FNAMENTRY_LEN_SHIFT;
                        if (str_len <= 0)
                            break; // null terminator entry
                        if (str_len >= 250)
                            break; // sanity

                        bool wide = (header & 1) != 0;

                        // Compute the FName ComparisonIndex for this entry
                        uint32_t byte_offset = static_cast<uint32_t>(it - block0_ptr);
                        uint32_t fname_index = byte_offset / ue::FNAME_STRIDE;

                        std::string name = fname_to_string(static_cast<int32_t>(fname_index));
                        logger::log_info("REFLECT", "FName[0x%X] = \"%s\" (len=%d%s)",
                                         fname_index, name.c_str(), str_len, wide ? " wide" : "");
                        entry_count++;

                        // Advance to next entry: header_size + string_bytes, aligned to stride
                        uint16_t total_bytes = ue::FNAMENTRY_HEADER_SIZE +
                                               str_len * (wide ? sizeof(uint16_t) : sizeof(char));
                        it += (total_bytes + ue::FNAME_STRIDE - 1u) & ~(ue::FNAME_STRIDE - 1u);
                    }

                    logger::log_info("REFLECT", "FNamePool walk: %d entries verified OK", entry_count);
                }
                else
                {
                    logger::log_error("REFLECT", "FNamePool Blocks[0] is null/invalid — name resolution broken");
                }
            }
        }
        else
        {
            logger::log_error("REFLECT", "GNames is null — cannot resolve FNames");
        }
    }

    // ═══ Walk all objects ═══════════════════════════════════════════════════
    void walk_all()
    {
        // Snapshot process memory map for safe pointer validation
        build_memory_map();

        s_classes.clear();
        s_structs.clear();
        s_enums.clear();
        s_class_map.clear();
        s_struct_map.clear();
        s_enum_map.clear();

        s_object_count = get_num_elements();
        logger::log_info("REFLECT", "Walking GUObjectArray — %d objects found", s_object_count);

        if (s_object_count <= 0)
        {
            // Extra diagnostics: try reading memory at a few different offsets
            uintptr_t guoa = s_guobject_array;
            logger::log_error("REFLECT", "NumElements is %d — dumping GUObjectArray memory for diagnostics", s_object_count);
            for (int off = 0; off < 0x40; off += 4)
            {
                int32_t val = ue::read_field<int32_t>(reinterpret_cast<const void *>(guoa), off);
                uintptr_t val64 = ue::read_field<uintptr_t>(reinterpret_cast<const void *>(guoa), off);
                logger::log_info("REFLECT", "  GUObjectArray+0x%02X: int32=%d (0x%08X) / ptr=0x%lX",
                                 off, val, (unsigned)val, (unsigned long)val64);
            }
            return;
        }

        int class_count = 0;
        int struct_count = 0;
        int enum_count = 0;
        int null_count = 0;
        int invalid_count = 0;
        (void)0; // unmapped_count removed — was unused

        for (int32_t i = 0; i < s_object_count; i++)
        {
            ue::UObject *obj = get_object_at_index(i);
            if (!obj)
            {
                null_count++;
                continue;
            }
            if (!is_readable_ptr(obj))
            {
                invalid_count++;
                continue;
            }

            // Log first 10 objects for diagnostics
            if (i < 10)
            {
                std::string dbg_name = get_short_name(obj);
                ue::UClass *dbg_cls = ue::uobj_get_class(obj);
                std::string dbg_cls_name = dbg_cls ? get_short_name(reinterpret_cast<const ue::UObject *>(dbg_cls)) : "???";
                logger::log_info("REFLECT", "  Object[%d]: 0x%lX name=\"%s\" class=\"%s\"",
                                 i, (unsigned long)obj, dbg_name.c_str(), dbg_cls_name.c_str());
            }

            // Progress logging every 50000 objects
            if (i > 0 && (i % 50000) == 0)
            {
                logger::log_info("REFLECT", "  ... processed %d/%d objects (%d classes, %d structs, %d enums, %d null, %d invalid)",
                                 i, s_object_count, class_count, struct_count, enum_count, null_count, invalid_count);
            }

            // Skip Default__ objects
            std::string name = get_short_name(obj);
            if (name.empty())
                continue;
            if (ue::is_default_object(name.c_str()))
                continue;

            if (is_uclass(obj))
            {
                // This is a UClass — extract class info
                ue::UClass *cls = reinterpret_cast<ue::UClass *>(obj);
                ClassInfo ci;
                ci.name = name;
                ci.full_name = get_full_name(obj);
                ci.package_name = get_package_name(obj);
                ci.raw = cls;
                ci.properties_size = ue::ustruct_get_properties_size(reinterpret_cast<const ue::UStruct *>(cls));

                // Parent class
                ue::UStruct *super = ue::ustruct_get_super(reinterpret_cast<const ue::UStruct *>(cls));
                if (super && is_readable_ptr(super))
                {
                    ci.parent_name = get_short_name(reinterpret_cast<const ue::UObject *>(super));
                }

                // Check if blueprint
                ue::UClass *meta_class = ue::uobj_get_class(obj);
                std::string meta_name = get_short_name(reinterpret_cast<const ue::UObject *>(meta_class));
                ci.is_blueprint = (meta_name == "BlueprintGeneratedClass" || meta_name == "WidgetBlueprintGeneratedClass" || meta_name == "AnimBlueprintGeneratedClass");

                // Walk own properties and functions
                ci.properties = walk_properties(reinterpret_cast<const ue::UStruct *>(cls), false);
                ci.functions = walk_functions(reinterpret_cast<const ue::UStruct *>(cls), false);

                s_class_map[ci.name] = s_classes.size();
                s_classes.push_back(ci);
                class_count++;
            }
            else if (is_ustruct(obj))
            {
                ue::UStruct *st = reinterpret_cast<ue::UStruct *>(obj);
                StructInfo si;
                si.name = name;
                si.package_name = get_package_name(obj);
                si.raw = st;
                si.properties_size = ue::ustruct_get_properties_size(st);

                ue::UStruct *super = ue::ustruct_get_super(st);
                if (super && is_readable_ptr(super))
                {
                    si.parent_name = get_short_name(reinterpret_cast<const ue::UObject *>(super));
                }

                si.properties = walk_properties(st, false);

                s_struct_map[si.name] = s_structs.size();
                s_structs.push_back(si);
                struct_count++;
            }
            else if (is_uenum(obj))
            {
                ue::UEnum *ue_enum = reinterpret_cast<ue::UEnum *>(obj);
                EnumInfo ei;
                ei.name = name;
                ei.package_name = get_package_name(obj);
                ei.raw = ue_enum;

                // Read enum values: TArray<TPair<FName,int64>> at offset 0x40
                uintptr_t data_ptr = ue::read_field<uintptr_t>(ue_enum, ue::uenum::NAMES_DATA);
                int32_t num_values = ue::read_field<int32_t>(ue_enum, ue::uenum::NAMES_NUM);

                if (is_readable_ptr(reinterpret_cast<const void *>(data_ptr)) && num_values > 0 && num_values < 10000)
                {
                    for (int32_t j = 0; j < num_values; j++)
                    {
                        uintptr_t entry = data_ptr + j * ue::uenum::ENUM_ENTRY_SIZE;
                        int32_t value_name_idx = ue::read_field<int32_t>(reinterpret_cast<const void *>(entry), 0);
                        int64_t value = ue::read_field<int64_t>(reinterpret_cast<const void *>(entry), 8);
                        std::string value_name = fname_to_string(value_name_idx);

                        // Strip the enum class prefix (e.g. "ECollisionChannel::ECC_WorldStatic" → "ECC_WorldStatic")
                        size_t sep = value_name.find("::");
                        if (sep != std::string::npos)
                        {
                            value_name = value_name.substr(sep + 2);
                        }

                        // Skip the _MAX sentinel value
                        if (value_name.size() >= 4 && value_name.substr(value_name.size() - 4) == "_MAX")
                            continue;

                        ei.values.push_back({value_name, value});
                    }
                }

                s_enum_map[ei.name] = s_enums.size();
                s_enums.push_back(ei);
                enum_count++;
            }
        }

        logger::log_info("REFLECT", "Walk complete: %d classes, %d structs, %d enums (null=%d, invalid=%d)",
                         class_count, struct_count, enum_count, null_count, invalid_count);
    }

    // ═══ Getters ════════════════════════════════════════════════════════════
    const std::vector<ClassInfo> &get_classes() { return s_classes; }
    const std::vector<StructInfo> &get_structs() { return s_structs; }
    const std::vector<EnumInfo> &get_enums() { return s_enums; }

    ClassInfo *find_class(const std::string &name)
    {
        auto it = s_class_map.find(name);
        if (it != s_class_map.end())
            return &s_classes[it->second];
        return nullptr;
    }

    StructInfo *find_struct(const std::string &name)
    {
        auto it = s_struct_map.find(name);
        if (it != s_struct_map.end())
            return &s_structs[it->second];
        return nullptr;
    }

    EnumInfo *find_enum(const std::string &name)
    {
        auto it = s_enum_map.find(name);
        if (it != s_enum_map.end())
            return &s_enums[it->second];
        return nullptr;
    }

    ue::UObject *find_object_by_name(const std::string &name)
    {
        int32_t count = get_num_elements();
        for (int32_t i = 0; i < count; i++)
        {
            ue::UObject *obj = get_object_at_index(i);
            if (!obj || !ue::is_valid_ptr(obj))
                continue;
            std::string obj_name = get_short_name(obj);
            if (obj_name == name)
                return obj;
        }
        return nullptr;
    }

    ue::UObject *find_object_by_path(const std::string &path)
    {
        if (path.empty())
            return nullptr;

        // Parse the path to extract the short name (last component after '.' or '/')
        // Examples:
        //   "/Script/Engine.PlayerController" → short="PlayerController", pkg="Engine"
        //   "/Script/Engine.Default__PlayerController" → short="Default__PlayerController"
        //   "/Game/Maps/MainLevel.MainLevel" → short="MainLevel"
        //   "PlayerController" → short="PlayerController" (no package)
        std::string short_name;
        std::string package_name;

        // Strip /Script/ or /Game/ prefix
        std::string work = path;
        if (work.compare(0, 8, "/Script/") == 0)
            work = work.substr(8);
        else if (work.compare(0, 6, "/Game/") == 0)
            work = work.substr(6);
        else if (work[0] == '/')
            work = work.substr(1);

        // Split on last '.' to get package.shortname
        size_t dot = work.rfind('.');
        if (dot != std::string::npos)
        {
            package_name = work.substr(0, dot);
            short_name = work.substr(dot + 1);
        }
        else
        {
            short_name = work;
        }

        if (short_name.empty())
            return nullptr;

        // First try: exact match via find_class_ptr (fast, uses cached class list)
        if (!package_name.empty())
        {
            ue::UClass *cls = find_class_ptr(short_name);
            if (cls)
            {
                // Verify package matches if specified
                std::string obj_pkg = get_package_name(reinterpret_cast<const ue::UObject *>(cls));
                // Package names: "/Script/Engine" outer chain gives us "Engine"
                if (obj_pkg == package_name || package_name.empty())
                    return reinterpret_cast<ue::UObject *>(cls);
            }
        }

        // Second try: scan GUObjectArray for exact short name match
        // For full path matching, also verify the outer chain
        int32_t count = get_num_elements();
        ue::UObject *best_match = nullptr;

        for (int32_t i = 0; i < count; i++)
        {
            ue::UObject *obj = get_object_at_index(i);
            if (!obj || !ue::is_valid_ptr(obj))
                continue;
            std::string obj_name = get_short_name(obj);
            if (obj_name != short_name)
                continue;

            // Short name matches — if no package filter, return first match
            if (package_name.empty())
                return obj;

            // Verify package/outer chain matches
            std::string obj_pkg = get_package_name(obj);
            if (obj_pkg == package_name)
                return obj;

            // Also check outer name (for sub-objects)
            ue::UObject *outer = ue::uobj_get_outer(obj);
            if (outer && ue::is_valid_ptr(outer))
            {
                std::string outer_name = get_short_name(outer);
                if (outer_name == package_name)
                    return obj;
            }

            // Keep as fallback in case nothing else matches
            if (!best_match)
                best_match = obj;
        }

        return best_match;
    }

    ue::UClass *find_class_ptr(const std::string &name)
    {
        auto *ci = find_class(name);
        if (ci)
            return ci->raw;
        // Fallback: scan GUObjectArray
        ue::UObject *obj = find_object_by_name(name);
        if (obj && is_uclass(obj))
            return reinterpret_cast<ue::UClass *>(obj);
        return nullptr;
    }

    ue::UObject *find_first_instance(const std::string &class_name)
    {
        int32_t count = get_num_elements();
        for (int32_t i = 0; i < count; i++)
        {
            ue::UObject *obj = get_object_at_index(i);
            if (!obj || !ue::is_valid_ptr(obj))
                continue;
            // Skip CDOs
            std::string obj_name = get_short_name(obj);
            if (ue::is_default_object(obj_name.c_str()))
                continue;
            // Check class name
            ue::UClass *cls = ue::uobj_get_class(obj);
            if (!cls || !ue::is_valid_ptr(cls))
                continue;
            std::string cls_name = get_short_name(reinterpret_cast<const ue::UObject *>(cls));
            if (cls_name == class_name)
                return obj;
        }
        return nullptr;
    }

    std::vector<ue::UObject *> find_all_instances(const std::string &class_name)
    {
        std::vector<ue::UObject *> result;
        int32_t count = get_num_elements();
        for (int32_t i = 0; i < count; i++)
        {
            ue::UObject *obj = get_object_at_index(i);
            if (!obj || !ue::is_valid_ptr(obj))
                continue;
            std::string obj_name = get_short_name(obj);
            if (ue::is_default_object(obj_name.c_str()))
                continue;
            ue::UClass *cls = ue::uobj_get_class(obj);
            if (!cls || !ue::is_valid_ptr(cls))
                continue;
            std::string cls_name = get_short_name(reinterpret_cast<const ue::UObject *>(cls));
            if (cls_name == class_name)
                result.push_back(obj);
        }
        return result;
    }

    int32_t get_object_count() { return s_object_count; }

    int32_t get_live_object_count() { return get_num_elements(); }

    ue::UObject *get_object_by_index(int32_t index)
    {
        return get_object_at_index(index);
    }

} // namespace reflection
