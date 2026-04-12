#pragma once
// modloader/include/modloader/reflection_walker.h
// Walks the live UE4 reflection graph (GUObjectArray, GNames)
// Extracts all UClass, UStruct, UEnum, UFunction, FProperty
// Used for SDK generation and class rebuilding

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <cstdint>
#include "modloader/types.h"

namespace reflection
{

    // Property type tag — derived from FFieldClass name
    enum class PropType
    {
        Unknown,
        BoolProperty,
        ByteProperty,
        Int8Property,
        Int16Property,
        IntProperty,
        Int64Property,
        UInt16Property,
        UInt32Property,
        UInt64Property,
        FloatProperty,
        DoubleProperty,
        NameProperty,
        StrProperty,
        TextProperty,
        ObjectProperty,
        WeakObjectProperty,
        LazyObjectProperty,
        SoftObjectProperty,
        ClassProperty,
        SoftClassProperty,
        InterfaceProperty,
        StructProperty,
        ArrayProperty,
        MapProperty,
        SetProperty,
        EnumProperty,
        DelegateProperty,
        MulticastDelegateProperty,
        MulticastInlineDelegateProperty,
        MulticastSparseDelegateProperty,
        FieldPathProperty,
    };

    // Resolved property info
    struct PropertyInfo
    {
        std::string name;
        PropType type;
        int32_t offset; // from FProperty->Offset_Internal
        int32_t element_size;
        uint64_t flags;     // EPropertyFlags
        ue::FProperty *raw; // live pointer to FProperty

        // Bool-specific
        uint8_t bool_field_size;
        uint8_t bool_byte_offset;
        uint8_t bool_byte_mask;
        uint8_t bool_field_mask;

        // Type reference name (for Object/Struct/Enum properties)
        std::string inner_type_name;
        std::string inner_type_name2; // second inner (MapProperty value, etc.)
    };

    // Resolved function info
    struct FunctionInfo
    {
        std::string name;
        uint32_t flags; // EFunctionFlags
        uint16_t parms_size;
        uint8_t num_parms;
        uint16_t return_value_offset;
        ue::UFunction *raw; // live pointer to UFunction

        // Parameters (subset of properties with CPF_Parm)
        std::vector<PropertyInfo> params;

        // Return value property (if any — CPF_ReturnParm)
        PropertyInfo *return_prop; // points into params vector, or nullptr
    };

    // Full resolved class info
    struct ClassInfo
    {
        std::string name;
        std::string full_name; // with package path
        std::string parent_name;
        std::string package_name; // top-level UPackage name (e.g. "Engine")
        int32_t properties_size;
        ue::UClass *raw; // live pointer to UClass

        std::vector<PropertyInfo> properties; // own properties only (not inherited)
        std::vector<FunctionInfo> functions;  // own functions only (not inherited)

        bool is_blueprint;
    };

    // Resolved struct info (not a UClass)
    struct StructInfo
    {
        std::string name;
        std::string parent_name;
        std::string package_name; // top-level UPackage name
        int32_t properties_size;
        ue::UStruct *raw;

        std::vector<PropertyInfo> properties;
    };

    // Resolved enum info
    struct EnumInfo
    {
        std::string name;
        std::string package_name; // top-level UPackage name
        std::vector<std::pair<std::string, int64_t>> values;
        ue::UEnum *raw;
    };

    // ═══ Main walker API ════════════════════════════════════════════════════

    // Initialize — resolves GUObjectArray, GNames pointers
    void init();

    // Resolve an FName index to a string
    std::string fname_to_string(int32_t index);

    // Resolve a string to an FName comparison index (reverse lookup, cached)
    int32_t fname_string_to_index(const std::string &name);

    // Walk ALL entries in FNamePool by traversing blocks sequentially.
    // Calls callback(index, name) for each valid entry.
    // This is the CORRECT way to enumerate FNames — sequential index iteration is WRONG
    // because FName ComparisonIndex encodes (block << 16 | byte_offset / stride).
    void walk_all_fnames(const std::function<void(int32_t index, const std::string &name)> &callback);

    // Get the full path name of a UObject (Package.Outer.Name)
    std::string get_full_name(const ue::UObject *obj);

    // Get just the short name of a UObject
    std::string get_short_name(const ue::UObject *obj);

    // Get the top-level UPackage name for a UObject (outermost outer)
    std::string get_package_name(const ue::UObject *obj);

    // Refresh the /proc/self/maps snapshot used by is_readable_ptr().
    // Must be called before on-demand rebuilds that happen after boot,
    // because the game may have allocated new memory regions since the
    // initial snapshot taken during walk_all().
    void refresh_memory_map();

    // Walk all objects in GUObjectArray and classify them
    // Populates the internal caches — call once on boot
    void walk_all();

    // Get all discovered classes, structs, enums
    const std::vector<ClassInfo> &get_classes();
    const std::vector<StructInfo> &get_structs();
    const std::vector<EnumInfo> &get_enums();

    // Find a class by name
    ClassInfo *find_class(const std::string &name);

    // Find a struct by name
    StructInfo *find_struct(const std::string &name);

    // Find an enum by name
    EnumInfo *find_enum(const std::string &name);

    // Find a UObject* by name via GUObjectArray scan
    ue::UObject *find_object_by_name(const std::string &name);

    // Find a UObject* by full UE path (e.g. "/Script/Engine.PlayerController")
    // Parses the path, extracts the short name, and matches against GUObjectArray.
    // Supports package paths ("/Script/Pkg.Class"), short names, and full outer chains.
    ue::UObject *find_object_by_path(const std::string &path);

    // Find a UClass* by name
    ue::UClass *find_class_ptr(const std::string &name);

    // Find the first live instance of a class by class name (walks GUObjectArray)
    ue::UObject *find_first_instance(const std::string &class_name);

    // Find ALL live instances of a class by class name (walks GUObjectArray)
    std::vector<ue::UObject *> find_all_instances(const std::string &class_name);

    // Walk properties of a UStruct (including inherited via SuperStruct chain)
    std::vector<PropertyInfo> walk_properties(const ue::UStruct *ustruct, bool include_inherited = false);

    // Walk functions of a UStruct (via Children UField chain)
    std::vector<FunctionInfo> walk_functions(const ue::UStruct *ustruct, bool include_inherited = false);

    // Get the property type tag from an FField's class name
    PropType classify_property(const ue::FField *field);

    // Get total object count in GUObjectArray (from last walk)
    int32_t get_object_count();

    // Get LIVE object count from GUObjectArray (reads memory directly, no walk needed)
    int32_t get_live_object_count();

    // Get a UObject by its GUObjectArray index (public wrapper)
    ue::UObject *get_object_by_index(int32_t index);

} // namespace reflection
