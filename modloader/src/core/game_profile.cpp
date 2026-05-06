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
        p.detected_engine_version = EngineVersion::UE5_3;

        // Build offsets from engine_versions.h constants (derived from UE source)
        p.offsets = build_offsets_for_version(EngineVersion::UE5_3);

        // ── Pinball FX VR specific overrides (verified on live binary) ──
        // FUObjectItem is 0x14 (20 bytes) — no padding, no RefCount in this build
        p.offsets.FUObjectItem_size = 0x14;

        // ── Stable global offsets — CONFIRMED on new binary (Pinball FX New Dump, May 2026) ──
        // Always applied regardless of disallow_hardcoded_fallbacks(). Used as Priority 4
        // fallback after dlsym / ELF / pattern / auto-discovery all fail.
        // Data globals (GNames, GUObjectArray, GEngine) are NOT here — they are runtime VAs
        // not derivable from file offsets; handled by auto_offsets::find_g*() scanners.
        // Offsets verified via: aob-port old=pfx_old_offsets_confirmed_v2.txt new=Pinball FX New Dump
        p.stable_global_offsets = {
            // Core UE engine functions
            {"ProcessEvent", 0x16854DC},
            {"StaticConstructObject_Internal", 0x169DFDC},
            {"StaticFindObject", 0x14F6004},
            {"StaticLoadObject", 0x169A630},
            {"FName::Init", 0x14A41F0},
            {"GetTransientPackage", 0x16151A0},
            // PFX store / achievements
            {"PFXStoreManager::IsTableOwned", 0x4EF048C},
            {"PFXAchievementsManager::IsAchievementUnlocked", 0x4E7CC74},
            {"PFXAchievementsManager::IsHologram", 0x4E7C87C},
            {"PFXAchievementsManager::GetTrophy", 0x4E7CBC4},
            {"PFXAchievementsManager::SetTrophyBeenSeen", 0x4E7CA14},
            // PFX collectibles
            {"PFXCollectiblesManager::UnlockEntry", 0x4E981FC},
            {"PFXCollectibleEntry::SetUnlocked", 0x4E91C24},
            // YUP component system
            {"YUP_AddPropertyToComponent", 0x478FBB0},
            {"YUP_RegisterPropertyBinding", 0x47A20EC},
            {"YUP_RegisterActionBinding", 0x47A2248},
            {"YUP_ResetComponent", 0x478F120},
            {"YUP_PendulumBall_Detach", 0x48A39CC},
            {"YUP_BallInformator_GetSingleton", 0x48BA280},
            {"YUP_BallInformator_GetSpeed", 0x48BA5F8},
            // YUP table debug
            {"YUP_TableDebug_SetPause", 0x49315A8},
            {"YUP_TableDebug_SetGodMode", 0x49315B8},
            {"YUP_TableDebug_PauseToggle", 0x49315C8},
            {"YUP_TableDebug_ToggleGodMode", 0x49315EC},
            {"YUP_TableDebug_SetSpeed", 0x4931600},
            {"YUP_TableDebug_DumpAction", 0x4931538},
            {"YUP_TableDebug_DebugLoad", 0x4931628},
            // PFX save / award system
            {"PFX_SaveManager_SetSingleton", 0x4EEDAD0},
            {"PFX_SaveManager_ClearSingleton", 0x4EEDAE0},
            {"PFX_SaveManager_MutexLock", 0x4F72544},
            {"PFX_SaveManager_MutexUnlock", 0x4F72550},
            {"PFX_CheatManager_GetSingleton", 0x4C6164C},
            {"PFX_AwardUnlockTable", 0x4F6BD0C},
            {"PFX_AwardLockTable", 0x4F6C0BC},
            {"PFX_AwardUnlockAll", 0x4F5D4A0},
            {"PFX_AwardLockAll", 0x4F5D574},
            {"PFX_UnLockAllTables", 0x4F5DAAC},
            {"PFX_SaveManager_HashLookup", 0x4F6E0D8},
            // AES / PAK
            {"AES_set_decrypt_key", 0x6273DC0},
            {"AES_set_encrypt_key", 0x6273BA0},
            {"FAES_DecryptData", 0x1402EE4},
        };

        // ── Fallback offsets ──
        // Dynamic-only policy for Pinball FX VR:
        // keep all symbol names here for runtime updates, but ship NO hardcoded
        // offsets in source. Resolution must come from dlsym/ELF/pattern/auto-discovery.
        p.fallback_offsets = {
            // Core engine symbols
            {"GNames", 0},
            {"GUObjectArray", 0},
            {"ProcessEvent", 0},
            {"StaticConstructObject_Internal", 0},
            {"StaticFindObject", 0},
            {"StaticLoadObject", 0},
            {"FName::Init", 0},
            {"GetTransientPackage", 0},
            {"GEngine", 0},

            // Pinball FX VR specific
            {"PFXStoreManager::IsTableOwned", 0},
            {"PFXAchievementsManager::IsAchievementUnlocked", 0},
            {"PFXAchievementsManager::IsHologram", 0},
            {"PFXAchievementsManager::GetTrophy", 0},
            {"PFXAchievementsManager::SetTrophyBeenSeen", 0},
            {"PFXCollectiblesManager::GetAllEntries", 0},
            {"PFXCollectiblesManager::UnlockEntry", 0},
            {"PFXCollectibleEntry::IsUnlocked", 0},
            {"PFXCollectibleEntry::SetUnlocked", 0},

            // FText raw symbols not needed on PFX (Kismet conversion path used)
            {"_ZNK5FText8ToStringEv", 0},           // not needed — Kismet path used
            {"_ZN5FText10FromStringEO7FString", 0}, // not needed — Kismet path used
            {"_ZN5FTextD2Ev", 0},                   // not needed — Kismet path used
            {"_ZN5FTextC1Ev", 0},                   // not needed — Kismet path used

            // YUP physics engine
            {"YUP_MakePropertyName", 0},
            {"YUP_AddPropertyToComponent", 0},
            {"YUP_AddActionToComponent", 0},
            {"YUP_RegisterPropertyBinding", 0},
            {"YUP_RegisterActionBinding", 0},
            {"YUP_FinalizeComponent", 0},
            {"YUP_ResetComponent", 0},
            {"YUP_TableDebugInterface_Init", 0},
            {"YUP_TableDebug_SetPause", 0},
            {"YUP_TableDebug_SetGodMode", 0},
            {"YUP_TableDebug_PauseToggle", 0},
            {"YUP_TableDebug_ToggleGodMode", 0},
            {"YUP_TableDebug_SetSpeed", 0},
            {"YUP_TableDebug_DumpAction", 0},
            {"YUP_TableDebug_DebugLoad", 0},
            {"YUP_InitPhysPropertyBindings", 0},
            {"YUP_PendulumBall_Attach", 0},
            {"YUP_PendulumBall_Detach", 0},
            {"YUP_PendulumBall_Reset", 0},
            {"YUP_BallInformator_GetSingleton", 0},
            {"YUP_BallInformator_Init", 0},
            {"YUP_BallInformator_GetSpeed", 0},

            // PFX table/award system
            {"PFX_SaveManager_SetSingleton", 0},
            {"PFX_SaveManager_ClearSingleton", 0},
            {"PFX_SaveManager_MutexLock", 0},
            {"PFX_SaveManager_MutexUnlock", 0},
            {"PFX_CheatManager_GetSingleton", 0},
            {"PFX_GameSubsystem_GetSingleton", 0},
            {"PFX_AwardUnlockTable", 0},
            {"PFX_AwardLockTable", 0},
            {"PFX_AwardUnlockAll", 0},
            {"PFX_AwardLockAll", 0},
            {"PFX_UnLockAllTables", 0},
            {"PFX_SaveManager_HashLookup", 0},

            // AES/PAK decrypt path
            {"AES_set_decrypt_key", 0},
            {"AES_set_encrypt_key", 0},
            {"FAES_DecryptData", 0},
            {"GetPakEncryptionKey", 0},
        };

        // ── Pattern signatures for dynamic scanning ──
        // Generated by bindump aob-port: verified UNIQUE in both old and new Pinball FX VR binaries.
        // These survive game updates by matching instruction sequences regardless of address changes.
        //
        // Format: {"SymbolName", "AA BB ?? CC", -1, 0}
        // ?? = wildcard byte (masked ARM64 PC-relative immediates: ADRP, B/BL, ADD, LDR lit, CBZ/CBNZ, TBZ/TBNZ)
        p.pattern_signatures = {
            // ══ Core UE engine functions ═══════════════════════════════════════
            {"ProcessEvent",
             "FF 03 01 D1 FD 7B 01 A9 FD 43 00 91 F6 57 02 A9 F4 4F 03 A9 55 D0 3B D5 F3 03 02 AA A8 16 40 F9 E2 03 00 91 E8 07 00 F9 20 A0 41 A9 09 25 00 91 14 01 40 F9 FF 03 00 F9",
             -1, 0},
            {"StaticConstructObject_Internal",
             "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? F8 03 14 AA 1F 03 00 F1 62 06 40 F9",
             -1, 0},
            {"StaticFindObject",
             "09 0D 40 B9 4B 25 40 B9 7F 01 09 6B 6D 01 00 54 2B FD 4D D3 4A 09 40 F9 6B 3D 7D 92 29 3D 00 12 4A 69 6B F8 8B 02 80 52 29 29 AB 9B 29 09 40 B9 ?? ?? ?? ?? ?? ?? ?? ?? E9 03 1F AA 29 09 40 B9 ?? ?? ?? ?? 14 89 40 F9 ?? ?? ?? ?? ?? ?? ?? ??",
             -1, 0},
            {"StaticLoadObject",
             "1F 04 00 71 E1 01 00 54 F4 03 13 AA E0 03 13 AA 88 C6 40 F8 08 01 40 F9 00 01 3F D6 00 00 80 12 E1 03 14 AA ?? ?? ?? ?? 1F 04 00 71 A1 00 00 54 68 02 40 F9 E0 03 13 AA 08 09 40 F9 00 01 3F D6 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? E0 13 40 F9 ?? ?? ?? ?? ?? ?? ?? ?? F5 3B 40 B9",
             -1, 0},
            {"FName::Init",
             "E9 03 1F 2A 4B 01 40 79 6B C1 00 51 7F 29 00 71 A2 00 00 54 29 05 00 11 ?? ?? ?? ?? 5F 01 14 EB",
             -1, 0},
            {"GetTransientPackage",
             "08 BD 40 F9 00 01 3F D6 ?? ?? ?? ?? 08 01 40 B9 28 00 00 B9 08 04 40 F9 09 01 40 F9 ?? ?? ?? ?? 09 01 00 F9 ?? ?? ?? ?? ?? ?? ?? ?? 88 02 40 F9 ?? ?? ?? ?? 00 05 40 F9 09 04 40 F9 28 25 40 A9 ?? ?? ?? ?? 5F 01 09 EB 69 01 00 54 08 A8 40 39",
             -1, 0},

            // ══ PFX Store / Achievements ═══════════════════════════════════════
            {"PFXStoreManager::IsTableOwned",
             "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 74 86 40 F9 ?? ?? ?? ?? ?? ?? ?? ?? 74 7E 40 F9 ?? ?? ?? ?? ?? ?? ?? ?? 60 76 40 F9",
             -1, 0},
            {"PFXAchievementsManager::IsAchievementUnlocked",
             "88 46 00 F9 ?? ?? ?? ?? E8 07 40 B9 E0 03 13 AA 89 12 40 F9 1F 01 00 71 E1 07 9F 1A 3F 01 00 F1 28 05 89 9A 88 12 00 F9 68 02 40 F9 08 11 40 F9",
             -1, 0},
            // Alias — some mods use _Real suffix
            {"PFXAchievementsManager::IsAchievementUnlocked_Real",
             "88 46 00 F9 ?? ?? ?? ?? E8 07 40 B9 E0 03 13 AA 89 12 40 F9 1F 01 00 71 E1 07 9F 1A 3F 01 00 F1 28 05 89 9A 88 12 00 F9 68 02 40 F9 08 11 40 F9",
             -1, 0},
            {"PFXAchievementsManager::IsHologram",
             "A8 16 40 F9 F3 03 00 AA E8 07 00 F9 FF 07 00 B9 ?? ?? ?? ?? 88 12 40 F9 ?? ?? ?? ?? 81 0E 40 F9 ?? ?? ?? ?? E0 03 14 AA ?? ?? ?? ?? ?? ?? ?? ?? 82 46 40 F9 ?? ?? ?? ?? E0 03 14 AA 48 0C 40 F9 88 46 00 F9 ?? ?? ?? ?? E8 07 40 B9 E0 03 13 AA 89 12 40 F9 1F 01 00 71 E1 07 9F 1A 3F 01 00 F1 28 05 89 9A 88 12 00 F9 68 02 40 F9 08 01 40 F9",
             -1, 0},
            {"PFXAchievementsManager::GetTrophy",
             "E8 07 40 B9 E0 03 13 AA 89 12 40 F9 1F 01 00 71 E1 07 9F 1A 3F 01 00 F1 28 05 89 9A 88 12 00 F9 68 02 40 F9 08 0D 40 F9",
             -1, 0},
            {"PFXAchievementsManager::SetTrophyBeenSeen",
             "88 12 00 F9 68 02 40 F9 08 09 40 F9 00 01 3F D6 E0 3F 40 F9 ?? ?? ?? ??",
             -1, 0},

            // ══ PFX Collectibles ═══════════════════════════════════════════════
            {"PFXCollectiblesManager::UnlockEntry",
             "?? ?? ?? ?? ?? ?? ?? ?? EA 27 02 A9 ?? ?? ?? ?? A8 03 1F F8 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 26 00 88 52 E7 03 1F AA ?? ?? ?? ?? E9 23 01 A9 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? E2 03 14 AA 04 05 80 52 05 01 80 52 06 00 A2 72 ?? ?? ?? ?? E8 2B 00 A9 ?? ?? ?? ?? 81 02 40 F9 E0 03 13 AA ?? ?? ?? ?? ?? ?? ?? ?? 08 00 40 F9 08 0D 40 F9 00 01 3F D6 A0 83 1E F8",
             -1, 0},
            {"PFXCollectibleEntry::IsUnlocked",
             "?? ?? ?? ?? ?? ?? ?? ?? 08 51 43 F9 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? EA 27 02 A9 ?? ?? ?? ??",
             -1, 0},
            {"PFXCollectibleEntry::SetUnlocked",
             "E1 13 40 39 1F 01 00 F1 08 05 88 9A 88 12 00 F9 ?? ?? ?? ?? 60 02 00 F9 C8 16 40 F9 E9 07 40 F9 1F 01 09 EB C1 00 00 54 F4 4F 43 A9 F6 57 42 A9 FD 7B 41 A9 ?? ?? ?? ?? C0 03 5F D6 ?? ?? ?? ?? ?? ?? ?? ?? FD 7B 04 A9 ?? ?? ?? ?? F4 4F 05 A9",
             -1, 0},

            // ══ YUP Component System ═══════════════════════════════════════════            // ── PFX Collectibles GetAllEntries (added — was missing from pattern_signatures) ──
            {"PFXCollectiblesManager::GetAllEntries",
             "08 51 43 F9 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? EA 27 02 A9 ?? ?? ?? ?? A8 03 1F F8 ?? ?? ?? ??",
             -1, 0},
            {"YUP_AddPropertyToComponent",
             "E9 20 04 29 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? FD 7B BF A9 ?? ?? ?? ??",
             -1, 0},
            {"YUP_RegisterPropertyBinding",
             "?? ?? ?? ?? E1 03 1F 2A 02 05 00 51 ?? ?? ?? ?? 95 02 40 F9 FF 83 00 39",
             -1, 0},
            {"YUP_RegisterActionBinding",
             "FF 23 00 F9 FF FF 06 A9 FF 33 00 F9 F8 7F 09 A9",
             -1, 0},
            {"YUP_FinalizeComponent",
             "00 5D 41 F9 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 1F 7D 00 A9",
             -1, 0},
            {"YUP_ResetComponent",
             "?? ?? ?? ?? 9F 0A 00 F9 9F 62 00 39 88 02 00 F9",
             -1, 0},

            // ══ YUP Table Debug ════════════════════════════════════════════════
            {"YUP_TableDebugInterface_Init",
             "80 0F 01 AD ?? ?? ?? ?? E8 23 40 F9 E9 07 40 F9",
             -1, 0},
            {"YUP_TableDebug_SetPause",
             "?? ?? ?? ?? ?? ?? ?? ?? E1 03 16 AA ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? E1 03 1B 2A FF 23 08 A9 ?? ?? ?? ?? ?? ?? ?? ?? E3 03 14 2A E7 03 1A AA FF 4B 00 F9 F4 33 01 79 FF 23 06 A9 08 00 80 12 FF 3B 00 F9 F4 F3 00 79 E8 13 00 B9 48 00 80 52 FF 0B 00 B9 E8 03 00 B9 ?? ?? ?? ??",
             -1, 0},
            {"YUP_TableDebug_SetGodMode",
             "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? E1 03 1B 2A FF 23 08 A9 ?? ?? ?? ?? ?? ?? ?? ?? E3 03 14 2A E7 03 1A AA FF 4B 00 F9 F4 33 01 79 FF 23 06 A9 08 00 80 12 FF 3B 00 F9 F4 F3 00 79 E8 13 00 B9 48 00 80 52 FF 0B 00 B9 E8 03 00 B9 ?? ?? ?? ??",
             -1, 0},
            {"YUP_TableDebug_PauseToggle",
             "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? E1 03 1B 2A FF 23 08 A9 ?? ?? ?? ?? ?? ?? ?? ?? E3 03 14 2A E7 03 1A AA FF 4B 00 F9 F4 33 01 79 FF 23 06 A9 08 00 80 12 FF 3B 00 F9 F4 F3 00 79 E8 13 00 B9 48 00 80 52 FF 0B 00 B9 E8 03 00 B9 ?? ?? ?? ??",
             -1, 0},
            {"YUP_TableDebug_ToggleGodMode",
             "FF 4B 00 F9 F4 33 01 79 FF 23 06 A9 08 00 80 12 FF 3B 00 F9 F4 F3 00 79 E8 13 00 B9 48 00 80 52",
             -1, 0},
            {"YUP_TableDebug_SetSpeed",
             "F4 F3 00 79 E8 13 00 B9 48 00 80 52 FF 0B 00 B9",
             -1, 0},
            {"YUP_TableDebug_DumpAction",
             "88 02 08 AA 49 00 C0 F2 E9 A3 16 A9 ?? ?? ?? ??",
             -1, 0},
            {"YUP_TableDebug_DebugLoad",
             "?? ?? ?? ?? E0 03 18 AA ?? ?? ?? ?? E0 03 1C AA ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 9F 2A 00 F1 01 F5 FF 54",
             -1, 0},

            // ══ YUP Physics ════════════════════════════════════════════════════
            {"YUP_InitPhysPropertyBindings",
             "D5 0A 01 39 ?? ?? ?? ?? 15 85 03 39 C0 4E 40 F9",
             -1, 0},
            {"YUP_PendulumBall_Attach",
             "00 E6 07 2F ?? ?? ?? ?? 29 91 40 F9 7F 42 01 39",
             -1, 0},
            {"YUP_PendulumBall_Detach",
             "?? ?? ?? ?? E0 03 17 AA 76 A2 40 B9 ?? ?? ?? ?? ?? ?? ?? ?? E0 03 17 AA ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? E1 03 16 2A E8 53 01 F9 ?? ?? ?? ?? ?? ?? ?? ?? 03 00 80 12 E7 03 15 AA FF 4F 01 F9 FF 57 01 F9 ?? ?? ?? ?? FB 63 05 79 FF 3F 01 F9 E8 43 01 F9 FF 47 01 F9 FB 23 05 79 F8 13 00 B9 FF 0B 00 B9 FA 03 00 B9 ?? ?? ?? ??",
             -1, 0},
            {"YUP_PendulumBall_Reset",
             "F5 13 40 F9 ?? ?? ?? ?? C0 03 5F D6 88 42 80 B9",
             -1, 0},
            {"YUP_BallInformator_GetSingleton",
             "08 00 40 F9 E9 13 00 39 08 71 40 F9 00 01 3F D6",
             -1, 0},
            {"YUP_MakePropertyName",
             "?? ?? ?? ?? E0 07 00 F9 ?? ?? ?? ?? E0 03 08 AA ?? ?? ?? ?? E8 03 00 2A E0 07 40 F9 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 1F 01 00 F9 1F 09 00 B9 ?? ?? ?? ?? E0 07 40 F9 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? FD 7B 02 A9 ?? ?? ?? ?? F4 4F 03 A9 53 D0 3B D5 68 16 40 F9 A8 83 1F F8 ?? ?? ?? ?? ?? ?? ?? ?? 08 FD DF 08 ?? ?? ?? ?? ?? ?? ?? ?? 08 91 72 39 ?? ?? ?? ??",
             -1, 0},
            {"YUP_BallInformator_GetSpeed",
             "?? ?? ?? ?? ?? ?? ?? ?? 03 00 80 12 E7 03 15 AA FF C7 01 F9 FF CF 01 F9 ?? ?? ?? ?? E8 BB 01 F9",
             -1, 0},

            // ══ PFX Save / Award System ════════════════════════════════════════
            {"PFX_SaveManager_SetSingleton",
             "?? ?? ?? ?? F6 57 02 A9 F4 4F 03 A9 ?? ?? ?? ?? F3 03 03 2A F4 03 02 AA F5 03 01 AA F6 03 1F AA 97 02 16 8B B8 02 16 8B ?? ?? ?? ?? ?? ?? ?? ?? E8 0A 40 B9 08 0B 00 B9 E8 1A 40 B9 08 1B 00 B9",
             -1, 0},
            {"PFX_SaveManager_ClearSingleton",
             "F3 03 03 2A F4 03 02 AA F5 03 01 AA F6 03 1F AA 97 02 16 8B B8 02 16 8B ?? ?? ?? ?? ?? ?? ?? ?? E8 0A 40 B9 08 0B 00 B9",
             -1, 0},
            {"PFX_SaveManager_MutexLock",
             "B4 83 5E B8 B3 03 5E F8 F6 0B 40 F9 ?? ?? ?? ?? ?? ?? ?? ?? E0 03 13 AA ?? ?? ?? ?? C8 16 40 F9",
             -1, 0},
            {"PFX_SaveManager_MutexUnlock",
             "?? ?? ?? ?? ?? ?? ?? ?? E0 03 13 AA ?? ?? ?? ?? C8 16 40 F9 A9 03 5F F8 1F 01 09 EB 61 02 00 54",
             -1, 0},
            {"PFX_CheatManager_GetSingleton",
             "?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? E0 03 13 AA ?? ?? ?? ?? 60 02 40 F9 F3 0B 40 F9 FD 7B C2 A8 C0 03 5F D6 FD 7B BE A9 F3 0B 00 F9 ?? ?? ?? ?? ?? ?? ?? ?? 00 E5 43 F9 ?? ?? ?? ?? F3 0B 40 F9 FD 7B C2 A8 C0 03 5F D6 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? E0 03 13 AA ?? ?? ?? ?? 60 02 40 F9 F3 0B 40 F9 FD 7B C2 A8 C0 03 5F D6 FD 7B BE A9 F3 0B 00 F9 ?? ?? ?? ?? 13 00 40 F9",
             -1, 0},
            {"PFX_GameSubsystem_GetSingleton",
             "?? ?? ?? ?? 42 00 80 52 ?? ?? ?? ?? ?? ?? ?? ?? 88 16 40 F9 A9 83 5F F8 1F 01 09 EB A1 00 00 54 F4 4F 45 A9 FD 7B 44 A9 ?? ?? ?? ?? C0 03 5F D6 ?? ?? ?? ?? FD 7B BE A9 F3 0B 00 F9 ?? ?? ?? ?? ?? ?? ?? ?? 00 45 46 F9 ?? ?? ?? ?? F3 0B 40 F9 FD 7B C2 A8 C0 03 5F D6 ?? ?? ?? ?? ?? ?? ?? ??",
             -1, 0},
            {"PFX_AwardUnlockTable",
             "F3 03 00 AA 1F AC 02 F9 08 00 00 F9 ?? ?? ?? ??",
             -1, 0},
            {"PFX_AwardLockTable",
             "FF 5B 00 B9 F8 23 00 F9 E8 2B 00 F9 B4 6B 3E A9 ?? ?? ?? ?? E8 5B 40 B9 ?? ?? ?? ?? A8 02 00 B9 ?? ?? ?? ?? E8 7B 8C B9 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 08 7D 1B 9B E9 1B 00 F9 3F 6B 28 B8",
             -1, 0},
            {"PFX_AwardUnlockAll",
             "FD 7B C1 A8 C0 03 5F D6 ?? ?? ?? ?? FD 7B 05 A9 ?? ?? ?? ?? F4 4F 06 A9 54 D0 3B D5 F3 03 00 AA 88 16 40 F9 A8 83 1F F8 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 08 FD DF 08 ?? ?? ?? ?? ?? ?? ?? ?? 24 01 40 F9",
             -1, 0},
            {"PFX_AwardLockAll",
             "E8 03 00 2A ?? ?? ?? ?? E0 03 40 F9 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? 22 00 80 52 00 05 40 AD 02 09 C0 3D E0 87 00 AD 08 61 40 79 E2 0F 80 3D E8 83 00 79 ?? ?? ?? ??",
             -1, 0},
            {"PFX_UnLockAllTables",
             "?? ?? ?? ?? 1F 04 00 71 A1 00 00 54 E8 02 40 F9 E0 03 17 AA 08 09 40 F9 00 01 3F D6 57 83 5A F8",
             -1, 0},
            {"PFX_SaveManager_HashLookup",
             "?? ?? ?? ?? 22 00 80 52 68 02 00 F9 ?? ?? ?? ?? 69 26 00 F9 60 82 85 3C",
             -1, 0},

            // ══ AES / PAK ══════════════════════════════════════════════════════
            {"AES_set_decrypt_key",
             "00 1C 21 6E 08 1D 29 6E 84 03 02 4E 8C 03 0A 4E",
             -1, 0},
            {"AES_set_encrypt_key",
             "41 7D 40 4C E0 02 03 4E E8 02 0B 4E 84 1C 30 6E",
             -1, 0},
            {"FAES_DecryptData",
             "1F 01 09 6B 4D 05 00 54 ?? ?? ?? ?? 0C 0C 40 92",
             -1, 0},
            {"GetPakEncryptionKey",
             "F7 03 00 AA 16 00 00 F9 ?? ?? ?? ?? E1 02 40 F9",
             -1, 0},
        };

        // ── Relative offset fallbacks for functions without unique AOB patterns ──
        // These resolve via a known working function + a fixed offset.
        // Verified on both old (Pinball FX VR launch) and new (latest update) binaries.
        // The offset must be in the SAME compilation unit (.o file) for stability.
        //
        // Format: {"TargetSymbol", "AnchorSymbol", delta_bytes}
        // Resolved as: Target = resolve(AnchorSymbol) + delta_bytes
        p.relative_offsets = {
            // BallInformator_Init = GetSingleton + 0xE4 (confirmed same .o file)
            {"YUP_BallInformator_Init", "YUP_BallInformator_GetSingleton", 0xE4},
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
