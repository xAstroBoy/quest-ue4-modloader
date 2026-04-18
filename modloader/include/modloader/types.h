#pragma once
// modloader/include/modloader/types.h
// UE4 runtime type definitions for RE4 VR (com.Armature.VR4)
// ARM64 / UE4.27 — all offsets from UE4Dumper verified against libUE4.so
// These are used for in-process reads (direct pointer dereference), not
// process_vm_readv — we are injected into the target process.

#include <cstdint>
#include <cstring>
#include <atomic>
#include <sys/mman.h>

namespace ue
{

    // ═══ Memory safety — msync page probe ═══════════════════════════════════
    // Uses msync(MS_ASYNC) to check if a page is mapped without risking a crash.
    // Returns false for null, low addresses, and unmapped/freed memory.
    // Cost: 1 syscall (~500ns on ARM64). Use at Lua API boundaries, not in hot loops.
    inline bool is_mapped_ptr(const void *p)
    {
        if (!p)
            return false;
        auto addr = reinterpret_cast<uintptr_t>(p);
        if (addr < 0x10000)
            return false;
        void *page = reinterpret_cast<void *>(addr & ~0xFFFULL);
        return msync(page, 1, MS_ASYNC) == 0;
    }

    // ═══ Forward declarations ═══════════════════════════════════════════════
    struct UObject;
    struct UClass;
    struct UStruct;
    struct UFunction;
    struct UField;
    struct UEnum;
    struct FField;
    struct FFieldClass;
    struct FProperty;

    // ═══ FName ══════════════════════════════════════════════════════════════
    // RE4 VR uses FNamePool (UE4.23+). GNames + 0x30 → FNamePool.
    // FNamePool + 0x10 → Blocks[] (8192 max), stride=2.
    // FNameEntry: +0x00 = int16 header (bit0=wide, bits 6..15=len), +0x02 = chars

    struct FName
    {
        int32_t ComparisonIndex;
        int32_t Number; // instance number (0 = none)
    };

    // ═══ FNamePool constants (RE4 VR / UE4.27 ARM64) ════════════════════════
    constexpr uint32_t GNNAMES_TO_FNAMEPOOL = 0x30;
    constexpr uint32_t FNAMEPOOL_TO_BLOCKS = 0x10;
    constexpr uint32_t FNAMEPOOL_CURRENT_BLOCK = 0x08;
    constexpr uint32_t FNAMEPOOL_CURRENT_BYTE = 0x0C;
    constexpr uint32_t FNAMENTRY_HEADER_SIZE = 2;
    constexpr uint32_t FNAMENTRY_LEN_SHIFT = 6;
    constexpr uint32_t FNAME_STRIDE = 2;
    constexpr uint32_t FNAME_MAX_BLOCKS = 8192;
    constexpr uint32_t FNAME_BLOCK_BITS = 16;

    // ═══ GUObjectArray constants ════════════════════════════════════════════
    // GUObjectArray + 0x10 → TUObjectArray (chunk pointer array)
    // TUObjectArray + 0x14 → NumElements (int32)
    // Chunk[i] = *(TUObjectArray + i*8)
    // UObject* = *(Chunk + index_in_chunk * FUOBJECTITEM_SIZE)
    constexpr uint32_t GUOBJECTARRAY_TO_OBJECTS = 0x10;
    constexpr uint32_t TUOBJECTARRAY_NUM_ELEMENTS = 0x14;
    constexpr uint32_t FUOBJECTITEM_PADDING = 0x00;
    constexpr uint32_t FUOBJECTITEM_CHUNK_SIZE = 0x10000; // 65536

    // FUObjectItem size — varies per game (RE4 VR: 0x18, Pinball FX VR: 0x14)
    // Initialized at boot from game_profile. Default to 0x18 (most common).
    inline uint32_t &fuobjectitem_size()
    {
        static uint32_t s_size = 0x18;
        return s_size;
    }
    // Legacy constexpr alias for code that hasn't been updated
    constexpr uint32_t FUOBJECTITEM_SIZE_DEFAULT = 0x18;
// Macro for code that needs the runtime value
#define FUOBJECTITEM_SIZE (ue::fuobjectitem_size())

    // ═══ UObjectBase field offsets (ARM64, RE4 VR) ══════════════════════════
    // +0x00: vtable*
    // +0x08: EObjectFlags (int32) + padding
    // +0x0C: InternalIndex (int32)
    // +0x10: ClassPrivate (UClass*)
    // +0x18: FNameIndex (int32, ComparisonIndex of NamePrivate)
    // +0x1C: Number part of FName
    // +0x20: OuterPrivate (UObject*)
    namespace uobj
    {
        constexpr uint32_t VTABLE = 0x00;
        constexpr uint32_t OBJECT_FLAGS = 0x08;
        constexpr uint32_t INTERNAL_INDEX = 0x0C;
        constexpr uint32_t CLASS_PRIVATE = 0x10;
        constexpr uint32_t FNAME_INDEX = 0x18;
        constexpr uint32_t FNAME_NUMBER = 0x1C;
        constexpr uint32_t OUTER_PRIVATE = 0x20;
    }

    // ═══ UField offsets ═════════════════════════════════════════════════════
    namespace ufield
    {
        constexpr uint32_t NEXT = 0x28;
    }

    // ═══ UStruct offsets (runtime-configurable, defaults = RE4 VR) ═════════
    namespace ustruct
    {
        inline uint32_t &SUPER_STRUCT_OFF()
        {
            static uint32_t v = 0x40;
            return v;
        }
        inline uint32_t &CHILDREN_OFF()
        {
            static uint32_t v = 0x48;
            return v;
        }
        inline uint32_t &CHILD_PROPERTIES_OFF()
        {
            static uint32_t v = 0x50;
            return v;
        }
        inline uint32_t &PROPERTIES_SIZE_OFF()
        {
            static uint32_t v = 0x58;
            return v;
        }
        inline uint32_t &DESTRUCTOR_LINK_OFF()
        {
            static uint32_t v = 0x80; // FProperty* — linked list of props needing destruction
            return v;
        }
    }

    // ═══ UFunction offsets (runtime-configurable, defaults = RE4 VR) ════════
    namespace ufunc
    {
        inline uint32_t &FUNCTION_FLAGS_OFF()
        {
            static uint32_t v = 0xB0;
            return v;
        }
        inline uint32_t &NUM_PARMS_OFF()
        {
            static uint32_t v = 0xB4;
            return v;
        }
        inline uint32_t &PARMS_SIZE_OFF()
        {
            static uint32_t v = 0xB6;
            return v;
        }
        inline uint32_t &RETURN_VALUE_OFFSET_OFF()
        {
            static uint32_t v = 0xB8;
            return v;
        }
        inline uint32_t &FUNC_PTR_OFF()
        {
            static uint32_t v = 0xD8;
            return v;
        }
    }

    // EFunctionFlags
    enum EFunctionFlags : uint32_t
    {
        FUNC_None = 0x00000000,
        FUNC_Final = 0x00000001,
        FUNC_RequiredAPI = 0x00000002,
        FUNC_BlueprintAuthorityOnly = 0x00000004,
        FUNC_BlueprintCosmetic = 0x00000008,
        FUNC_Net = 0x00000040,
        FUNC_NetReliable = 0x00000080,
        FUNC_NetRequest = 0x00000100,
        FUNC_Exec = 0x00000200,
        FUNC_Native = 0x00000400,
        FUNC_Event = 0x00000800,
        FUNC_NetResponse = 0x00001000,
        FUNC_Static = 0x00002000,
        FUNC_NetMulticast = 0x00004000,
        FUNC_UbergraphFunction = 0x00008000,
        FUNC_MulticastDelegate = 0x00010000,
        FUNC_Public = 0x00020000,
        FUNC_Private = 0x00040000,
        FUNC_Protected = 0x00080000,
        FUNC_Delegate = 0x00100000,
        FUNC_NetServer = 0x00200000,
        FUNC_NetClient = 0x00400000,
        FUNC_DLLImport = 0x00800000,
        FUNC_BlueprintCallable = 0x04000000,
        FUNC_BlueprintEvent = 0x08000000,
        FUNC_BlueprintPure = 0x10000000,
        FUNC_EditorOnly = 0x20000000,
        FUNC_Const = 0x40000000,
        FUNC_NetValidate = 0x80000000,
        FUNC_AllFlags = 0xFFFFFFFF,
        FUNC_HasOutParms = FUNC_Native | FUNC_Event,
    };

    // ═══ FField offsets (UE4.23+ / RE4 VR defaults, overridden at boot for UE5) ═══
    // UE5 uses compact 8-byte FFieldVariant, shifting all subsequent fields by -8.
    // These are runtime-configurable to support both UE4 and UE5 layouts.
    namespace ffield
    {
        inline uint32_t &CLASS_PRIVATE_OFF()
        {
            static uint32_t v = 0x08;
            return v;
        }
        inline uint32_t &OWNER_OFF()
        {
            static uint32_t v = 0x10;
            return v;
        }
        inline uint32_t &NEXT_OFF()
        {
            static uint32_t v = 0x20;
            return v;
        }
        inline uint32_t &NAME_PRIVATE_OFF()
        {
            static uint32_t v = 0x28;
            return v;
        }
        // Keep legacy constexpr names as references to runtime values for backward compat
        // (accessor functions below use the _OFF() versions)
    }

    // ═══ FProperty offsets (RE4 VR defaults, overridden at boot for UE5) ════
    namespace fprop
    {
        inline uint32_t &ELEMENT_SIZE_OFF()
        {
            static uint32_t v = 0x38;
            return v;
        }
        inline uint32_t &PROPERTY_FLAGS_OFF()
        {
            static uint32_t v = 0x40;
            return v;
        }
        inline uint32_t &OFFSET_INTERNAL_OFF()
        {
            static uint32_t v = 0x4C;
            return v;
        }
        inline uint32_t &SIZE_OFF()
        {
            static uint32_t v = 0x78;
            return v;
        }

        // UBoolProperty extras
        inline uint32_t &BOOL_FIELD_SIZE_OFF()
        {
            static uint32_t v = 0x78;
            return v;
        }
        inline uint32_t &BOOL_BYTE_OFFSET_OFF()
        {
            static uint32_t v = 0x79;
            return v;
        }
        inline uint32_t &BOOL_BYTE_MASK_OFF()
        {
            static uint32_t v = 0x7A;
            return v;
        }
        inline uint32_t &BOOL_FIELD_MASK_OFF()
        {
            static uint32_t v = 0x7B;
            return v;
        }

        // Typed property inner pointers
        inline uint32_t &OBJ_PROPERTY_CLASS_OFF()
        {
            static uint32_t v = 0x78;
            return v;
        }
        inline uint32_t &CLASS_META_CLASS_OFF()
        {
            static uint32_t v = 0x80;
            return v;
        }
        inline uint32_t &INTERFACE_CLASS_OFF()
        {
            static uint32_t v = 0x80;
            return v;
        }
        inline uint32_t &ARRAY_INNER_OFF()
        {
            static uint32_t v = 0x78;
            return v;
        }
        inline uint32_t &MAP_KEY_PROP_OFF()
        {
            static uint32_t v = 0x78;
            return v;
        }
        inline uint32_t &MAP_VALUE_PROP_OFF()
        {
            static uint32_t v = 0x80;
            return v;
        }
        inline uint32_t &SET_ELEMENT_PROP_OFF()
        {
            static uint32_t v = 0x78;
            return v;
        }
        inline uint32_t &STRUCT_INNER_STRUCT_OFF()
        {
            static uint32_t v = 0x78;
            return v;
        }
        inline uint32_t &ENUM_PROP_ENUM_OFF()
        {
            static uint32_t v = 0x80;
            return v;
        }
        inline uint32_t &BYTE_PROP_ENUM_OFF()
        {
            static uint32_t v = 0x78;
            return v;
        }
    }

    // EPropertyFlags
    enum EPropertyFlags : uint64_t
    {
        CPF_None = 0,
        CPF_Edit = 0x0000000000000001,
        CPF_ConstParm = 0x0000000000000002,
        CPF_BlueprintVisible = 0x0000000000000004,
        CPF_ExportObject = 0x0000000000000008,
        CPF_BlueprintReadOnly = 0x0000000000000010,
        CPF_Net = 0x0000000000000020,
        CPF_EditFixedSize = 0x0000000000000040,
        CPF_Parm = 0x0000000000000080,
        CPF_OutParm = 0x0000000000000100,
        CPF_ZeroConstructor = 0x0000000000000200,
        CPF_ReturnParm = 0x0000000000000400,
        CPF_DisableEditOnTemplate = 0x0000000000000800,
        CPF_Transient = 0x0000000000002000,
        CPF_Config = 0x0000000000004000,
        CPF_DisableEditOnInstance = 0x0000000000010000,
        CPF_EditConst = 0x0000000000020000,
        CPF_GlobalConfig = 0x0000000000040000,
        CPF_InstancedReference = 0x0000000000080000,
        CPF_DuplicateTransient = 0x0000000000200000,
        CPF_SubobjectReference = 0x0000000000400000,
        CPF_SaveGame = 0x0000000001000000,
        CPF_NoClear = 0x0000000002000000,
        CPF_ReferenceParm = 0x0000000008000000,
        CPF_BlueprintAssignable = 0x0000000010000000,
        CPF_Deprecated = 0x0000000020000000,
        CPF_IsPlainOldData = 0x0000000040000000,
        CPF_RepSkip = 0x0000000080000000,
        CPF_RepNotify = 0x0000000100000000,
        CPF_Interp = 0x0000000200000000,
        CPF_NonTransactional = 0x0000000400000000,
        CPF_EditorOnly = 0x0000000800000000,
        CPF_NoDestructor = 0x0000001000000000,
        CPF_AutoWeak = 0x0000004000000000,
        CPF_ContainsInstancedReference = 0x0000008000000000,
        CPF_AssetRegistrySearchable = 0x0000010000000000,
        CPF_SimpleDisplay = 0x0000020000000000,
        CPF_AdvancedDisplay = 0x0000040000000000,
        CPF_Protected = 0x0000080000000000,
        CPF_BlueprintCallable = 0x0000100000000000,
        CPF_BlueprintAuthorityOnly = 0x0000200000000000,
        CPF_TextExportTransient = 0x0000400000000000,
        CPF_NonPIEDuplicateTransient = 0x0000800000000000,
        CPF_ExposeOnSpawn = 0x0001000000000000,
        CPF_PersistentInstance = 0x0002000000000000,
        CPF_UObjectWrapper = 0x0004000000000000,
        CPF_HasGetValueTypeHash = 0x0008000000000000,
        CPF_NativeAccessSpecifierPublic = 0x0010000000000000,
        CPF_NativeAccessSpecifierProtected = 0x0020000000000000,
        CPF_NativeAccessSpecifierPrivate = 0x0040000000000000,
        CPF_SkipSerialization = 0x0080000000000000,
    };

    // ═══ UEnum offsets ══════════════════════════════════════════════════════
    namespace uenum
    {
        constexpr uint32_t NAMES_DATA = 0x40; // TArray<TPair<FName,int64>> data ptr
        constexpr uint32_t NAMES_NUM = 0x48;  // int32 count
        constexpr uint32_t NAMES_MAX = 0x4C;  // int32 capacity (TArray::Max)
        // Each entry is 16 bytes: FName (8 bytes) + int64 value (8 bytes)
        constexpr uint32_t ENUM_ENTRY_SIZE = 16;
    }

    // ═══ EObjectFlags ═══════════════════════════════════════════════════════
    enum EObjectFlags : int32_t
    {
        RF_NoFlags = 0x00000000,
        RF_Public = 0x00000001,
        RF_Standalone = 0x00000002,
        RF_MarkAsNative = 0x00000004,
        RF_Transactional = 0x00000008,
        RF_ClassDefaultObject = 0x00000010,
        RF_ArchetypeObject = 0x00000020,
        RF_Transient = 0x00000040,
        RF_MarkAsRootSet = 0x00000080,
        RF_TagGarbageTemp = 0x00000100,
        RF_NeedInitialization = 0x00000200,
        RF_NeedLoad = 0x00000400,
        RF_KeepForCooker = 0x00000800,
        RF_NeedPostLoad = 0x00001000,
        RF_NeedPostLoadSubobjects = 0x00002000,
        RF_NewerVersionExists = 0x00004000,
        RF_BeginDestroyed = 0x00008000,
        RF_FinishDestroyed = 0x00010000,
        RF_BeingRegenerated = 0x00020000,
        RF_DefaultSubObject = 0x00040000,
        RF_WasLoaded = 0x00080000,
        RF_TextExportTransient = 0x00100000,
        RF_LoadCompleted = 0x00200000,
        RF_InheritableComponentTemplate = 0x00400000,
        RF_DuplicateTransient = 0x00800000,
        RF_StrongRefOnFrame = 0x01000000,
        RF_NonPIEDuplicateTransient = 0x02000000,
        RF_Dynamic = 0x04000000,
        RF_WillBeLoaded = 0x08000000,
    };

    // ═══ UE4 ProcessEvent signature ═════════════════════════════════════════
    // void UObject::ProcessEvent(UFunction* Function, void* Parms)
    using ProcessEventFn = void (*)(UObject *obj, UFunction *func, void *parms);

    // ═══ StaticFindObject signature ═════════════════════════════════════════
    // TCHAR = char16_t on Android ARM64 (NOT wchar_t which is 4 bytes)
    using StaticFindObjectFn = UObject *(*)(UClass * cls, UObject *outer, const char16_t *name, bool exact);

    // ═══ StaticLoadObject signature ═════════════════════════════════════════
    using StaticLoadObjectFn = UObject *(*)(UClass * cls, UObject *outer, const char16_t *name,
                                            const char16_t *filename, uint32_t load_flags,
                                            void *sandbox, bool allow_obj_redirect);

    // ═══ StaticLoadClass signature ══════════════════════════════════════════
    using StaticLoadClassFn = UClass *(*)(UClass * base_cls, UObject *outer, const char16_t *name,
                                          const char16_t *filename, uint32_t load_flags, void *sandbox);

    // ═══ StaticConstructObject_Internal signature ═══════════════════════════
    // RE4 VR uses the multi-arg form (not the params-struct form)
    using StaticConstructObjectFn = UObject *(*)(UClass * cls, UObject *outer, FName name,
                                                 int32_t flags, int32_t internal_flags,
                                                 UObject *tmpl, bool copy_transients,
                                                 void *instance_graph, bool assume_archetype);

    // ═══ FPakPlatformFile::Mount ════════════════════════════════════════════
    // Mount(const TCHAR* InPakFilename, uint32 PakOrder, const TCHAR* InPath, bool bLoadIndex)
    // On Android, TCHAR = char16_t (NOT wchar_t which is 4 bytes on ARM64)
    using PakMountFn = bool (*)(void *this_ptr, const char16_t *pak_filename,
                                uint32_t pak_order, const char16_t *mount_point,
                                bool load_index);

    // ═══ FPlatformFileManager ═══════════════════════════════════════════════
    // FPlatformFileManager::Get() → returns singleton reference
    using FPlatformFileManager_GetFn = void *(*)();
    // FPlatformFileManager::FindPlatformFile(const TCHAR* Name) → returns IPlatformFile*
    using FPlatformFileManager_FindPlatformFileFn = void *(*)(void *this_ptr, const char16_t *name);

    // ═══ Inline helper to read a field at a known offset ════════════════════
    template <typename T>
    inline T read_field(const void *base, uint32_t offset)
    {
        T result;
        std::memcpy(&result, reinterpret_cast<const uint8_t *>(base) + offset, sizeof(T));
        return result;
    }

    template <typename T>
    inline void write_field(void *base, uint32_t offset, const T &value)
    {
        std::memcpy(reinterpret_cast<uint8_t *>(base) + offset, &value, sizeof(T));
    }

    template <typename T>
    inline T *ptr_field(void *base, uint32_t offset)
    {
        return reinterpret_cast<T *>(reinterpret_cast<uint8_t *>(base) + offset);
    }

    // ═══ Quick accessors for UObject fields ═════════════════════════════════
    inline UClass *uobj_get_class(const UObject *obj)
    {
        return read_field<UClass *>(obj, uobj::CLASS_PRIVATE);
    }

    inline int32_t uobj_get_name_index(const UObject *obj)
    {
        return read_field<int32_t>(obj, uobj::FNAME_INDEX);
    }

    inline int32_t uobj_get_name_number(const UObject *obj)
    {
        return read_field<int32_t>(obj, uobj::FNAME_NUMBER);
    }

    inline UObject *uobj_get_outer(const UObject *obj)
    {
        return read_field<UObject *>(obj, uobj::OUTER_PRIVATE);
    }

    inline int32_t uobj_get_internal_index(const UObject *obj)
    {
        return read_field<int32_t>(obj, uobj::INTERNAL_INDEX);
    }

    inline int32_t uobj_get_flags(const UObject *obj)
    {
        return read_field<int32_t>(obj, uobj::OBJECT_FLAGS);
    }

    // UStruct accessors
    inline UStruct *ustruct_get_super(const UStruct *s)
    {
        return read_field<UStruct *>(s, ustruct::SUPER_STRUCT_OFF());
    }

    inline UField *ustruct_get_children(const UStruct *s)
    {
        return read_field<UField *>(s, ustruct::CHILDREN_OFF());
    }

    inline FField *ustruct_get_child_properties(const UStruct *s)
    {
        return read_field<FField *>(s, ustruct::CHILD_PROPERTIES_OFF());
    }

    inline int32_t ustruct_get_properties_size(const UStruct *s)
    {
        return read_field<int32_t>(s, ustruct::PROPERTIES_SIZE_OFF());
    }

    // UField accessor
    inline UField *ufield_get_next(const UField *f)
    {
        return read_field<UField *>(f, ufield::NEXT);
    }

    // FField accessors (use runtime-configurable offsets)
    inline FFieldClass *ffield_get_class(const FField *f)
    {
        return read_field<FFieldClass *>(f, ffield::CLASS_PRIVATE_OFF());
    }

    inline FField *ffield_get_next(const FField *f)
    {
        return read_field<FField *>(f, ffield::NEXT_OFF());
    }

    inline int32_t ffield_get_name_index(const FField *f)
    {
        return read_field<int32_t>(f, ffield::NAME_PRIVATE_OFF());
    }

    // FProperty accessors (use runtime-configurable offsets)
    inline int32_t fprop_get_offset(const FProperty *p)
    {
        return read_field<int32_t>(p, fprop::OFFSET_INTERNAL_OFF());
    }

    inline int32_t fprop_get_element_size(const FProperty *p)
    {
        return read_field<int32_t>(p, fprop::ELEMENT_SIZE_OFF());
    }

    inline uint64_t fprop_get_flags(const FProperty *p)
    {
        return read_field<uint64_t>(p, fprop::PROPERTY_FLAGS_OFF());
    }

    // UFunction accessors
    inline uint32_t ufunc_get_flags(const UFunction *f)
    {
        return read_field<uint32_t>(f, ufunc::FUNCTION_FLAGS_OFF());
    }

    inline void *ufunc_get_func_ptr(const UFunction *f)
    {
        return read_field<void *>(f, ufunc::FUNC_PTR_OFF());
    }

    inline uint16_t ufunc_get_parms_size(const UFunction *f)
    {
        return read_field<uint16_t>(f, ufunc::PARMS_SIZE_OFF());
    }

    inline uint16_t ufunc_get_return_value_offset(const UFunction *f)
    {
        return read_field<uint16_t>(f, ufunc::RETURN_VALUE_OFFSET_OFF());
    }

    inline uint8_t ufunc_get_num_parms(const UFunction *f)
    {
        return read_field<uint8_t>(f, ufunc::NUM_PARMS_OFF());
    }

    // ═══ Validity checks ════════════════════════════════════════════════════
    inline bool is_valid_ptr(const void *p)
    {
        auto addr = reinterpret_cast<uintptr_t>(p);
        // ARM64 TBI (Top Byte Ignore): mask top byte before range check
        // Android on ARM64 may use tagged pointers (MTE, HWASan, etc.)
        auto untagged = addr & 0x00FFFFFFFFFFFFFFULL;
        return untagged > 0x10000 && untagged < 0x0001000000000000ULL;
    }

    inline bool is_valid_uobject(const UObject *obj)
    {
        if (!is_valid_ptr(obj))
            return false;
        auto cls = uobj_get_class(obj);
        if (!is_valid_ptr(cls))
            return false;
        auto idx = uobj_get_internal_index(obj);
        return idx >= 0 && idx < 0x200000;
    }

    inline bool is_default_object(const char *name)
    {
        if (!name)
            return false;
        if (strncmp(name, "Default__", 9) == 0)
            return true;
        if (strncmp(name, "REINST_", 7) == 0)
            return true;
        if (strncmp(name, "SKEL_", 5) == 0)
            return true;
        return false;
    }

} // namespace ue

// ═══ Apply game-profile offsets to runtime type system ══════════════════
// Must be called after game_profile::init() to set UE5-specific offsets.
#include "modloader/game_profile.h"
namespace ue
{
    inline void apply_type_offsets(const game_profile::TypeOffsets &o)
    {
        fuobjectitem_size() = o.FUObjectItem_size;

        // UStruct
        ustruct::SUPER_STRUCT_OFF() = o.UStruct_super;
        ustruct::CHILDREN_OFF() = o.UStruct_children;
        ustruct::CHILD_PROPERTIES_OFF() = o.UStruct_child_properties;
        ustruct::PROPERTIES_SIZE_OFF() = o.UStruct_properties_size;

        // UFunction
        ufunc::FUNCTION_FLAGS_OFF() = o.UFunction_flags;
        ufunc::NUM_PARMS_OFF() = o.UFunction_num_parms;
        ufunc::PARMS_SIZE_OFF() = o.UFunction_parms_size;
        ufunc::RETURN_VALUE_OFFSET_OFF() = o.UFunction_return_value_offset;
        ufunc::FUNC_PTR_OFF() = o.UFunction_func_ptr;

        // FField
        ffield::CLASS_PRIVATE_OFF() = o.FField_class;
        ffield::OWNER_OFF() = o.FField_owner;
        ffield::NEXT_OFF() = o.FField_next;
        ffield::NAME_PRIVATE_OFF() = o.FField_name;

        // FProperty
        fprop::ELEMENT_SIZE_OFF() = o.FProp_element_size;
        fprop::PROPERTY_FLAGS_OFF() = o.FProp_property_flags;
        fprop::OFFSET_INTERNAL_OFF() = o.FProp_offset_internal;
        fprop::SIZE_OFF() = o.FProp_size;
        fprop::BOOL_FIELD_SIZE_OFF() = o.FProp_bool_field_size;
        fprop::BOOL_BYTE_OFFSET_OFF() = o.FProp_bool_byte_offset;
        fprop::BOOL_BYTE_MASK_OFF() = o.FProp_bool_byte_mask;
        fprop::BOOL_FIELD_MASK_OFF() = o.FProp_bool_field_mask;
        fprop::OBJ_PROPERTY_CLASS_OFF() = o.FProp_obj_property_class;
        fprop::CLASS_META_CLASS_OFF() = o.FProp_class_meta_class;
        fprop::INTERFACE_CLASS_OFF() = o.FProp_interface_class;
        fprop::ARRAY_INNER_OFF() = o.FProp_array_inner;
        fprop::MAP_KEY_PROP_OFF() = o.FProp_map_key_prop;
        fprop::MAP_VALUE_PROP_OFF() = o.FProp_map_value_prop;
        fprop::SET_ELEMENT_PROP_OFF() = o.FProp_set_element_prop;
        fprop::STRUCT_INNER_STRUCT_OFF() = o.FProp_struct_inner_struct;
        fprop::ENUM_PROP_ENUM_OFF() = o.FProp_enum_prop_enum;
        fprop::BYTE_PROP_ENUM_OFF() = o.FProp_byte_prop_enum;
    }
} // namespace ue
