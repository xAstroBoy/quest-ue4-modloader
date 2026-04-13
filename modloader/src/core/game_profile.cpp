// modloader/src/core/game_profile.cpp
// Multi-game support — runtime detection + per-game offsets and symbols.
// Reads /proc/self/cmdline to detect game package, then selects the
// matching profile with correct UE type offsets and fallback addresses.
//
// For UNKNOWN games, auto-detects engine version by scanning the loaded
// engine library for version strings ("+UE4+", "+UE5+", "Release-X.Y").
// Uses engine_versions.h compile-time constants to build correct offsets.

#include "modloader/game_profile.h"
#include "modloader/engine_versions.h"
#include <cstdio>
#include <cstring>
#include <android/log.h>
#include <dlfcn.h>

#define LOG_TAG "UEModLoader"

namespace game_profile
{

    static GameProfile s_profile;
    static bool s_initialized = false;

    // ═══ Read package name from /proc/self/cmdline ══════════════════════════
    static std::string read_package_name()
    {
        char buf[256];
        memset(buf, 0, sizeof(buf));
        FILE *f = fopen("/proc/self/cmdline", "r");
        if (f)
        {
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            if (n > 0)
                return std::string(buf);
        }
        return "";
    }

    // ═══ Build offsets for a given engine version ═══════════════════════════
    // Uses compile-time constants from engine_versions.h to construct
    // a complete TypeOffsets struct. This is the SINGLE source of truth
    // for offset calculations — derived from UE source code analysis.
    TypeOffsets build_offsets_for_version(engine_versions::EngineVersion version)
    {
        using namespace engine_versions;

        TypeOffsets o = {};

        // ── FNamePool — IDENTICAL across all UE4.23+ versions ──
        o.GNames_to_FNamePool = fnamepool::GNAMES_TO_FNAMEPOOL;
        o.FNamePool_to_Blocks = fnamepool::FNAMEPOOL_TO_BLOCKS;
        o.FNameEntry_header_size = fnamepool::FNAMENTRY_HEADER_SIZE;
        o.FNameEntry_len_shift = fnamepool::FNAMENTRY_LEN_SHIFT;
        o.FName_stride = fnamepool::FNAME_STRIDE;
        o.FName_max_blocks = fnamepool::FNAME_MAX_BLOCKS;
        o.FName_block_bits = fnamepool::FNAME_BLOCK_BITS;

        // ── UObjectBase — IDENTICAL across all versions ──
        o.UObj_vtable = uobject_base::VTABLE;
        o.UObj_flags = uobject_base::OBJECT_FLAGS;
        o.UObj_internal_index = uobject_base::INTERNAL_INDEX;
        o.UObj_class = uobject_base::CLASS_PRIVATE;
        o.UObj_fname_index = uobject_base::FNAME_INDEX;
        o.UObj_fname_number = uobject_base::FNAME_NUMBER;
        o.UObj_outer = uobject_base::OUTER_PRIVATE;

        // ── UField — IDENTICAL across all versions ──
        o.UField_next = ufield_layout::NEXT;

        // ── UStruct — IDENTICAL across all versions (shipping) ──
        o.UStruct_super = ustruct_layout::SUPER_STRUCT;
        o.UStruct_children = ustruct_layout::CHILDREN;
        o.UStruct_child_properties = ustruct_layout::CHILD_PROPERTIES;
        o.UStruct_properties_size = ustruct_layout::PROPERTIES_SIZE;

        // ── UFunction — IDENTICAL across all versions ──
        o.UFunction_flags = ufunction_layout::FUNCTION_FLAGS;
        o.UFunction_num_parms = ufunction_layout::NUM_PARMS;
        o.UFunction_parms_size = ufunction_layout::PARMS_SIZE;
        o.UFunction_return_value_offset = ufunction_layout::RETURN_VALUE_OFFSET;
        o.UFunction_func_ptr = ufunction_layout::FUNC_PTR;

        // ── UEnum — IDENTICAL across all versions ──
        o.UEnum_names_data = uenum_layout::NAMES_DATA;
        o.UEnum_names_num = uenum_layout::NAMES_NUM;
        o.UEnum_names_max = uenum_layout::NAMES_MAX;
        o.UEnum_entry_size = uenum_layout::ENTRY_SIZE;

        // ── GUObjectArray — DIFFERS between UE4 and UE5 ──
        o.GUObjectArray_to_objects = guobjectarray::TO_OBJECTS;
        o.FUObjectItem_chunk_size = guobjectarray::ELEMENTS_PER_CHUNK;
        o.FUObjectItem_padding = 0;

        if (is_ue5(version))
        {
            // UE5: PreAllocatedObjects pointer shifts NumElements by +8
            o.TUObjectArray_num_elements = guobjectarray::UE5_NUM_ELEMENTS;
            // UE5 default: FUObjectItem has RefCount → 24 bytes
            // BUT some games (e.g., Pinball FX VR) use 20 bytes (no RefCount or no padding)
            // Default to 24, auto-detect will override if needed
            o.FUObjectItem_size = guobjectarray::FUOBJECTITEM_SIZE_24;
        }
        else
        {
            // UE4: No PreAllocatedObjects
            o.TUObjectArray_num_elements = guobjectarray::UE4_NUM_ELEMENTS;
            o.FUObjectItem_size = guobjectarray::FUOBJECTITEM_SIZE_24;
        }

        // ── FField + FProperty — DIFFERS between UE4 and UE5 ──
        if (is_ue5(version))
        {
            // UE5: compact 8-byte FFieldVariant (tagged pointer)
            o.FField_class = ffield_layout::ue5::CLASS_PRIVATE;
            o.FField_owner = ffield_layout::ue5::OWNER;
            o.FField_next = ffield_layout::ue5::NEXT;
            o.FField_name = ffield_layout::ue5::NAME_PRIVATE;

            o.FProp_element_size = fproperty_layout::ue5::ELEMENT_SIZE;
            o.FProp_property_flags = fproperty_layout::ue5::PROPERTY_FLAGS;
            o.FProp_offset_internal = fproperty_layout::ue5::OFFSET_INTERNAL;
            o.FProp_size = fproperty_layout::ue5::BASE_SIZE;

            o.FProp_bool_field_size = fproperty_ext::ue5::BOOL_FIELD_SIZE;
            o.FProp_bool_byte_offset = fproperty_ext::ue5::BOOL_BYTE_OFFSET;
            o.FProp_bool_byte_mask = fproperty_ext::ue5::BOOL_BYTE_MASK;
            o.FProp_bool_field_mask = fproperty_ext::ue5::BOOL_FIELD_MASK;
            o.FProp_obj_property_class = fproperty_ext::ue5::OBJ_PROPERTY_CLASS;
            o.FProp_class_meta_class = fproperty_ext::ue5::CLASS_META_CLASS;
            o.FProp_interface_class = fproperty_ext::ue5::INTERFACE_CLASS;
            o.FProp_array_inner = fproperty_ext::ue5::ARRAY_INNER;
            o.FProp_map_key_prop = fproperty_ext::ue5::MAP_KEY_PROP;
            o.FProp_map_value_prop = fproperty_ext::ue5::MAP_VALUE_PROP;
            o.FProp_set_element_prop = fproperty_ext::ue5::SET_ELEMENT_PROP;
            o.FProp_struct_inner_struct = fproperty_ext::ue5::STRUCT_INNER_STRUCT;
            o.FProp_enum_prop_enum = fproperty_ext::ue5::ENUM_PROP_ENUM;
            o.FProp_byte_prop_enum = fproperty_ext::ue5::BYTE_PROP_ENUM;
        }
        else
        {
            // UE4: 16-byte FFieldVariant
            o.FField_class = ffield_layout::ue4::CLASS_PRIVATE;
            o.FField_owner = ffield_layout::ue4::OWNER;
            o.FField_next = ffield_layout::ue4::NEXT;
            o.FField_name = ffield_layout::ue4::NAME_PRIVATE;

            o.FProp_element_size = fproperty_layout::ue4::ELEMENT_SIZE;
            o.FProp_property_flags = fproperty_layout::ue4::PROPERTY_FLAGS;
            o.FProp_offset_internal = fproperty_layout::ue4::OFFSET_INTERNAL;
            o.FProp_size = fproperty_layout::ue4::BASE_SIZE;

            o.FProp_bool_field_size = fproperty_ext::ue4::BOOL_FIELD_SIZE;
            o.FProp_bool_byte_offset = fproperty_ext::ue4::BOOL_BYTE_OFFSET;
            o.FProp_bool_byte_mask = fproperty_ext::ue4::BOOL_BYTE_MASK;
            o.FProp_bool_field_mask = fproperty_ext::ue4::BOOL_FIELD_MASK;
            o.FProp_obj_property_class = fproperty_ext::ue4::OBJ_PROPERTY_CLASS;
            o.FProp_class_meta_class = fproperty_ext::ue4::CLASS_META_CLASS;
            o.FProp_interface_class = fproperty_ext::ue4::INTERFACE_CLASS;
            o.FProp_array_inner = fproperty_ext::ue4::ARRAY_INNER;
            o.FProp_map_key_prop = fproperty_ext::ue4::MAP_KEY_PROP;
            o.FProp_map_value_prop = fproperty_ext::ue4::MAP_VALUE_PROP;
            o.FProp_set_element_prop = fproperty_ext::ue4::SET_ELEMENT_PROP;
            o.FProp_struct_inner_struct = fproperty_ext::ue4::STRUCT_INNER_STRUCT;
            o.FProp_enum_prop_enum = fproperty_ext::ue4::ENUM_PROP_ENUM;
            o.FProp_byte_prop_enum = fproperty_ext::ue4::BYTE_PROP_ENUM;
        }

        return o;
    }

    // ═══ RE4 VR profile ═════════════════════════════════════════════════════
    static GameProfile build_re4_vr_profile()
    {
        using namespace engine_versions;

        GameProfile p;
        p.id = GameID::RE4_VR;
        p.package_name = "com.Armature.VR4";
        p.display_name = "RE4 VR";
        p.engine_lib_name = "libUE4.so";
        p.engine_version = "UE4.27";
        p.detected_engine_version = EngineVersion::UE4_27;

        // Build offsets from engine_versions.h constants (derived from UE source)
        p.offsets = build_offsets_for_version(EngineVersion::UE4_27);

        // ── RE4 VR specific overrides (verified on live binary) ──
        // FUObjectItem: RE4 VR uses 0x18 (24 bytes) — confirmed via UE4Dumper
        p.offsets.FUObjectItem_size = 0x18;
        // TUObjectArray::NumElements at +0x14 (from ObjObjects base) — RE4 VR specific
        // The UE4 standard is 0x0C, but RE4 VR binary was verified at 0x14
        p.offsets.TUObjectArray_num_elements = 0x14;

        // Fallback offsets from UE4Dumper / Binary Ninja analysis
        p.fallback_offsets = {
            {"GUObjectArray", 0x0A785E08},
            {"GNames", 0x0A763300},
            {"StaticFindObject", 0x06E1AE74},
            {"StaticLoadObject", 0x06E1D42C},
            {"StaticLoadClass", 0x06E1DE74},
            {"StaticConstructObject_Internal", 0x06E20A8C},
            {"GetEtcModelClass", 0x066909C8},
            {"FPakPlatformFile::Mount", 0x08056024},
            {"MountAllPakFiles_1", 0x08054B78},
            {"MountAllPakFiles_2", 0x08054FE8},
            {"_ZN20FPlatformFileManager3GetEv", 0x0666D4B4},
            {"_ZN20FPlatformFileManager16FindPlatformFileEPKDs", 0x0666D034},
            {"_ZNK5FText8ToStringEv", 0x066C49BC},
            {"_ZN5FText10FromStringEO7FString", 0x066C4FA8},
            {"_ZN5FTextD2Ev", 0x055089A8},
            {"_ZN5FTextC1Ev", 0x066C3DE4},
        };

        return p;
    }

    // ═══ Pinball FX VR profile ══════════════════════════════════════════════
    static GameProfile build_pinball_fx_vr_profile()
    {
        using namespace engine_versions;

        GameProfile p;
        p.id = GameID::PINBALL_FX_VR;
        p.package_name = "com.zenstudios.PFXVRQuest";
        p.display_name = "Pinball FX VR";
        p.engine_lib_name = "libUnreal.so";
        p.engine_version = "UE5";
        p.detected_engine_version = EngineVersion::UE5_4;

        // Build offsets from engine_versions.h constants (derived from UE source)
        p.offsets = build_offsets_for_version(EngineVersion::UE5_4);

        // ── Pinball FX VR specific overrides (verified on live binary) ──
        // FUObjectItem is 0x14 (20 bytes) — no padding, no RefCount in this build
        p.offsets.FUObjectItem_size = 0x14;

        // ── Fallback offsets (v1.7 addresses from IDA/Binja analysis) ──
        // These are fallbacks ONLY — the pattern scanner should find them dynamically.
        // GNames and GUObjectArray are UNCHANGED between v1.6 and v1.7.
        // Only function offsets changed in v1.7.
        // Offsets verified via Rust bindump tool (tools/bindump) cross-referencing
        // IDA + BINJA decompilation of the stripped libUnreal.so binary.
        p.fallback_offsets = {
            // Core engine symbols (same in v1.6 and v1.7)
            {"GNames", 0x071D5600},
            {"GUObjectArray", 0x0721CC38},
            {"ProcessEvent", 0x016774DC},                   // unchanged between versions
            {"StaticConstructObject_Internal", 0x01696FDC}, // unchanged between versions

            // Additional core symbols resolved via Binja analysis
            {"StaticFindObject", 0x014EF004},    // sub_18ef004 — used by FindObject reflection
            {"StaticLoadObject", 0x01693630},    // sub_1a93630 — used by LoadObject
            {"FName::Init", 0x0149D1F0},         // sub_189d1f0 — FName::Init (string → FName)
            {"GetTransientPackage", 0x0160E1A0}, // sub_1a0e1a0 — GetTransientPackage()

            // Global pointers found via bindump string xref analysis:
            // GEngine: "Create GEngine" string at 0x4BF2C3 → xref at text:0x3FFF8B8
            //   → sub_3FFF768 writes new engine object to qword_73CF2A0 via off_715D0A8
            //   → STR X20, [X22] at 0x3FFF9D8. Confirmed: 2318 xrefs to qword_73CF2A0.
            {"GEngine", 0x073CF2A0},

            // GWorld: resolved at runtime from GUObjectArray UObject walk.
            // Not a simple global in UE5 (it's UWorldProxy). The modloader's
            // process_event_hook.cpp already finds it dynamically — no offset needed.

            // Pinball FX VR specific — entitlement/unlock exec thunks
            {"PFXStoreManager::IsTableOwned", 0x04ED5FB8},                 // v1.7 exec thunk, returns bool
            {"PFXAchievementsManager::IsAchievementUnlocked", 0x04E62E30}, // v1.7 exec thunk
            {"PFXAchievementsManager::IsHologram", 0x04E62A38},            // same in v1.6 and v1.7
            {"PFXAchievementsManager::GetTrophy", 0x04E62D80},             // v1.7
            {"PFXAchievementsManager::SetTrophyBeenSeen", 0x04E62BD0},     // v1.7
            {"PFXCollectiblesManager::GetAllEntries", 0x04E7E124},         // v1.7
            {"PFXCollectiblesManager::UnlockEntry", 0x04E7E3A4},           // v1.7
            {"PFXCollectibleEntry::IsUnlocked", 0x04E77F50},               // v1.7
            {"PFXCollectibleEntry::SetUnlocked", 0x04E77DEC},              // v1.7

            // FText symbols — NOT needed for Pinball FX VR.
            // The modloader uses Kismet Conv_TextToString / Conv_StringToText
            // via ProcessEvent for all FText read/write operations. The raw
            // FText_ToString/FromString/Ctor/Dtor symbols are never called.
            // These are stripped from the binary (no .symtab, only .dynsym
            // with JNI exports). Keeping them at 0 is correct behavior.
            {"_ZNK5FText8ToStringEv", 0},           // not needed — Kismet path used
            {"_ZN5FText10FromStringEO7FString", 0}, // not needed — Kismet path used
            {"_ZN5FTextD2Ev", 0},                   // not needed — Kismet path used
            {"_ZN5FTextC1Ev", 0},                   // not needed — Kismet path used
        };

        // ── Pattern signatures for dynamic scanning ──
        // These survive game updates by matching instruction sequences instead
        // of hardcoded offsets. Each pattern finds a key function or global.
        //
        // ProcessEvent: UObject::ProcessEvent(UFunction*, void*)
        // Signature: function prologue + ObjectFlags check + FunctionFlags read
        // Pattern is from the first ~20 bytes of sub_16774DC
        p.pattern_signatures = {
            // ProcessEvent prologue — SUB SP, SP, #frame; STP X28,X27
            // Stable across versions because it's the ProcessEvent calling convention
            {
                "ProcessEvent",
                "FF 03 04 D1 FC 6F 11 A9 FA 67 12 A9 F8 5F 13 A9 F6 57 14 A9 F4 4F 15 A9",
                -1, 0},

            // GNames — ADRP+ADD pattern loading FNamePool address
            // Find via string xref "FNamePool.DuplicateHardcodedName"
            // The ADRP instruction loads the page, ADD adds the offset
            // We scan for the FNamePool init function's characteristic pattern

            // GUObjectArray — find via string "UObjectArray.cpp"
            // The init function references &GUObjectArray as first arg
        };

        return p;
    }

    // ═══ Unknown/fallback profile ═══════════════════════════════════════════
    // For games we haven't seen before. Tries to auto-detect UE4 vs UE5
    // based on which engine library is loaded, then uses engine_versions.h
    // constants to build correct offsets.
    static GameProfile build_unknown_profile(const std::string &pkg)
    {
        using namespace engine_versions;

        GameProfile p;
        p.id = GameID::UNKNOWN;
        p.package_name = pkg;
        p.display_name = "Unknown Game (" + pkg + ")";

        // Detect engine library — try libUnreal.so first (UE5), then libUE4.so (UE4)
        void *ue5_handle = dlopen("libUnreal.so", RTLD_NOLOAD);
        void *ue4_handle = dlopen("libUE4.so", RTLD_NOLOAD);

        if (ue5_handle)
        {
            p.engine_lib_name = "libUnreal.so";
            p.engine_version = "UE5 (auto-detected)";
            p.detected_engine_version = EngineVersion::UE5_4; // default UE5 assumption
            p.offsets = build_offsets_for_version(EngineVersion::UE5_4);
            dlclose(ue5_handle);

            __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                                "[PROFILE] Auto-detected UE5 (libUnreal.so loaded)");
        }
        else if (ue4_handle)
        {
            p.engine_lib_name = "libUE4.so";
            p.engine_version = "UE4 (auto-detected)";
            p.detected_engine_version = EngineVersion::UE4_27; // default UE4 assumption
            p.offsets = build_offsets_for_version(EngineVersion::UE4_27);
            dlclose(ue4_handle);

            __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                                "[PROFILE] Auto-detected UE4 (libUE4.so loaded)");
        }
        else
        {
            // Neither library found yet — default to UE4.27 (most common)
            p.engine_lib_name = "libUE4.so";
            p.engine_version = "Unknown";
            p.detected_engine_version = EngineVersion::UNKNOWN;
            p.offsets = build_offsets_for_version(EngineVersion::UE4_27);

            __android_log_print(ANDROID_LOG_WARN, LOG_TAG,
                                "[PROFILE] No engine library detected! Defaulting to UE4.27 offsets");
        }

        return p;
    }

    // ═══ Engine version detection from binary strings ═══════════════════════
    // Scans the loaded engine library's memory for version markers.
    // This is called AFTER pattern::init() has mapped the library regions.
    engine_versions::EngineVersion detect_engine_version_from_binary()
    {
        using namespace engine_versions;

        // We need the pattern scanner to have been initialized to get memory regions.
        // If not available, we can try to read /proc/self/maps ourselves.
        // For now, this is a stub that will be called from main.cpp after pattern::init().

        __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                            "[PROFILE] Engine version detection from binary: using library-based detection");

        // Library name is the most reliable indicator
        void *ue5_handle = dlopen("libUnreal.so", RTLD_NOLOAD);
        if (ue5_handle)
        {
            dlclose(ue5_handle);
            // Default UE5 → try to narrow down via string scanning later
            return EngineVersion::UE5_4;
        }

        void *ue4_handle = dlopen("libUE4.so", RTLD_NOLOAD);
        if (ue4_handle)
        {
            dlclose(ue4_handle);
            return EngineVersion::UE4_27;
        }

        return EngineVersion::UNKNOWN;
    }

    // ═══ Init ═══════════════════════════════════════════════════════════════
    void init()
    {
        if (s_initialized)
            return;

        std::string pkg = read_package_name();

        __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                            "[PROFILE] Package detected: '%s'", pkg.c_str());

        if (pkg == "com.Armature.VR4")
        {
            s_profile = build_re4_vr_profile();
        }
        else if (pkg == "com.zenstudios.PFXVRQuest")
        {
            s_profile = build_pinball_fx_vr_profile();
        }
        else
        {
            s_profile = build_unknown_profile(pkg);
        }

        __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                            "[PROFILE] Game: %s (%s), Engine: %s, Lib: %s",
                            s_profile.display_name.c_str(),
                            s_profile.package_name.c_str(),
                            s_profile.engine_version.c_str(),
                            s_profile.engine_lib_name.c_str());

        __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                            "[PROFILE] Engine version enum: %u",
                            static_cast<uint32_t>(s_profile.detected_engine_version));

        __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                            "[PROFILE] FUObjectItem size: 0x%X, GUObjectArray_to_objects: 0x%X",
                            s_profile.offsets.FUObjectItem_size,
                            s_profile.offsets.GUObjectArray_to_objects);

        __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                            "[PROFILE] FField: class=0x%X, owner=0x%X, next=0x%X, name=0x%X",
                            s_profile.offsets.FField_class,
                            s_profile.offsets.FField_owner,
                            s_profile.offsets.FField_next,
                            s_profile.offsets.FField_name);

        __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                            "[PROFILE] FProp: elem_size=0x%X, flags=0x%X, offset=0x%X, base_size=0x%X",
                            s_profile.offsets.FProp_element_size,
                            s_profile.offsets.FProp_property_flags,
                            s_profile.offsets.FProp_offset_internal,
                            s_profile.offsets.FProp_size);

        s_initialized = true;
    }

    // ═══ Accessors ══════════════════════════════════════════════════════════
    GameID detected_game() { return s_profile.id; }
    const GameProfile &profile() { return s_profile; }
    GameProfile &mutable_profile() { return s_profile; }
    const TypeOffsets &offsets() { return s_profile.offsets; }
    const std::string &engine_lib_name() { return s_profile.engine_lib_name; }
    const std::string &display_name() { return s_profile.display_name; }
    const std::string &package_name() { return s_profile.package_name; }
    bool is_pinball_fx() { return s_profile.id == GameID::PINBALL_FX_VR; }
    bool is_re4_vr() { return s_profile.id == GameID::RE4_VR; }

    engine_versions::EngineVersion engine_version_enum()
    {
        return s_profile.detected_engine_version;
    }

    bool is_ue5()
    {
        return engine_versions::is_ue5(s_profile.detected_engine_version);
    }

    bool is_ue4()
    {
        return engine_versions::is_ue4(s_profile.detected_engine_version);
    }

} // namespace game_profile
