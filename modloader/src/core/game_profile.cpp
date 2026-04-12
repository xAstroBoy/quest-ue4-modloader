// modloader/src/core/game_profile.cpp
// Multi-game support — runtime detection + per-game offsets and symbols.
// Reads /proc/self/cmdline to detect game package, then selects the
// matching profile with correct UE type offsets and fallback addresses.

#include "modloader/game_profile.h"
#include <cstdio>
#include <cstring>
#include <android/log.h>

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

    // ═══ RE4 VR profile ═════════════════════════════════════════════════════
    static GameProfile build_re4_vr_profile()
    {
        GameProfile p;
        p.id = GameID::RE4_VR;
        p.package_name = "com.Armature.VR4";
        p.display_name = "RE4 VR";
        p.engine_lib_name = "libUE4.so";
        p.engine_version = "UE4.27";

        // Type offsets — verified from UE4Dumper against live RE4 VR binary
        auto &o = p.offsets;

        // FNamePool
        o.GNames_to_FNamePool = 0x30;
        o.FNamePool_to_Blocks = 0x10;
        o.FNameEntry_header_size = 2;
        o.FNameEntry_len_shift = 6;
        o.FName_stride = 2;
        o.FName_max_blocks = 8192;
        o.FName_block_bits = 16;

        // GUObjectArray
        o.GUObjectArray_to_objects = 0x10;
        o.TUObjectArray_num_elements = 0x14;
        o.FUObjectItem_size = 0x18;
        o.FUObjectItem_padding = 0x00;
        o.FUObjectItem_chunk_size = 0x10000;

        // UObjectBase
        o.UObj_vtable = 0x00;
        o.UObj_flags = 0x08;
        o.UObj_internal_index = 0x0C;
        o.UObj_class = 0x10;
        o.UObj_fname_index = 0x18;
        o.UObj_fname_number = 0x1C;
        o.UObj_outer = 0x20;

        // UField
        o.UField_next = 0x28;

        // UStruct
        o.UStruct_super = 0x40;
        o.UStruct_children = 0x48;
        o.UStruct_child_properties = 0x50;
        o.UStruct_properties_size = 0x58;

        // UFunction
        o.UFunction_flags = 0xB0;
        o.UFunction_num_parms = 0xB4;
        o.UFunction_parms_size = 0xB6;
        o.UFunction_return_value_offset = 0xB8;
        o.UFunction_func_ptr = 0xD8;

        // FField
        o.FField_class = 0x08;
        o.FField_owner = 0x10;
        o.FField_next = 0x20;
        o.FField_name = 0x28;

        // FProperty
        o.FProp_element_size = 0x38;
        o.FProp_property_flags = 0x40;
        o.FProp_offset_internal = 0x4C;
        o.FProp_size = 0x78;

        o.FProp_bool_field_size = 0x78;
        o.FProp_bool_byte_offset = 0x79;
        o.FProp_bool_byte_mask = 0x7A;
        o.FProp_bool_field_mask = 0x7B;
        o.FProp_obj_property_class = 0x78;
        o.FProp_class_meta_class = 0x80;
        o.FProp_interface_class = 0x80;
        o.FProp_array_inner = 0x78;
        o.FProp_map_key_prop = 0x70;
        o.FProp_map_value_prop = 0x78;
        o.FProp_set_element_prop = 0x78;
        o.FProp_struct_inner_struct = 0x78;
        o.FProp_enum_prop_enum = 0x80;
        o.FProp_byte_prop_enum = 0x78;

        // UEnum
        o.UEnum_names_data = 0x40;
        o.UEnum_names_num = 0x48;
        o.UEnum_names_max = 0x4C;
        o.UEnum_entry_size = 16;

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
        GameProfile p;
        p.id = GameID::PINBALL_FX_VR;
        p.package_name = "com.zenstudios.PFXVRQuest";
        p.display_name = "Pinball FX VR";
        p.engine_lib_name = "libUnreal.so";
        p.engine_version = "UE5";

        // Type offsets — verified from IDA/Binja analysis + UE4Dumper config
        // Pinball FX VR uses UE5 with a non-standard FUObjectItem size (0x14 vs 0x18)
        auto &o = p.offsets;

        // FNamePool — same as RE4 VR (standard UE4.23+ layout)
        o.GNames_to_FNamePool = 0x30;
        o.FNamePool_to_Blocks = 0x10;
        o.FNameEntry_header_size = 2;
        o.FNameEntry_len_shift = 6;
        o.FName_stride = 2;
        o.FName_max_blocks = 8192;
        o.FName_block_bits = 16;

        // GUObjectArray — FUObjectItem is 0x14 (no padding), not 0x18
        o.GUObjectArray_to_objects = 0x10;
        o.TUObjectArray_num_elements = 0x14;
        o.FUObjectItem_size = 0x14; // KEY DIFFERENCE from RE4 VR
        o.FUObjectItem_padding = 0x00;
        o.FUObjectItem_chunk_size = 0x10000;

        // UObjectBase — same layout as RE4 VR
        o.UObj_vtable = 0x00;
        o.UObj_flags = 0x08;
        o.UObj_internal_index = 0x0C;
        o.UObj_class = 0x10;
        o.UObj_fname_index = 0x18;
        o.UObj_fname_number = 0x1C;
        o.UObj_outer = 0x20;

        // UField
        o.UField_next = 0x28;

        // UStruct — same as RE4 VR (UE5 uses same layout as UE4.23+)
        o.UStruct_super = 0x40;
        o.UStruct_children = 0x48;
        o.UStruct_child_properties = 0x50;
        o.UStruct_properties_size = 0x58;

        // UFunction — same as RE4 VR
        o.UFunction_flags = 0xB0;
        o.UFunction_num_parms = 0xB4;
        o.UFunction_parms_size = 0xB6;
        o.UFunction_return_value_offset = 0xB8;
        o.UFunction_func_ptr = 0xD8;

        // FField — UE5 uses compact 8-byte FFieldVariant (tagged pointer), NOT 16-byte
        // This shifts Next and Name by -8 compared to RE4 VR (UE4.27)
        o.FField_class = 0x08;
        o.FField_owner = 0x10; // FFieldVariant is 8 bytes (compact tagged pointer)
        o.FField_next = 0x18;  // was 0x20 in RE4 VR (UE4 has 16-byte FFieldVariant)
        o.FField_name = 0x20;  // was 0x28 in RE4 VR

        // FProperty — UE5 compact FFieldVariant shifts base FProperty fields by -8
        // BUT the FProperty base class size stays at 0x78 (UE5 added RepNotifyFunc field
        // or padding that compensates for the FField header shrinkage)
        o.FProp_element_size = 0x30;    // was 0x38 in UE4
        o.FProp_property_flags = 0x38;  // was 0x40 in UE4
        o.FProp_offset_internal = 0x44; // was 0x4C in UE4
        o.FProp_size = 0x70;            // was 0x78 in UE4

        // Typed FProperty inner offsets — FProperty base = 0x70 bytes in this UE5 build
        // Most extension members start at 0x70, EXCEPT:
        //   - FArrayProperty has EArrayPropertyFlags at 0x70, Inner at 0x78
        //   - FBoolProperty packs 4 uint8 fields starting at 0x78
        // Confirmed via runtime auto-detection on Pinball FX VR v1.7
        o.FProp_bool_field_size = 0x78;     // confirmed working
        o.FProp_bool_byte_offset = 0x79;    // confirmed working
        o.FProp_bool_byte_mask = 0x7A;      // confirmed working
        o.FProp_bool_field_mask = 0x7B;     // confirmed working
        o.FProp_obj_property_class = 0x70;  // auto-detected: FProperty base = 0x70
        o.FProp_class_meta_class = 0x78;    // obj_property_class + 8
        o.FProp_interface_class = 0x78;     // same as class_meta_class
        o.FProp_array_inner = 0x78;         // ArrayFlags at 0x70, Inner at 0x78
        o.FProp_map_key_prop = 0x70;        // auto-detected: key at 0x70
        o.FProp_map_value_prop = 0x78;      // auto-detected: value at 0x78
        o.FProp_set_element_prop = 0x70;    // same as map_key (first extension member)
        o.FProp_struct_inner_struct = 0x70; // auto-detected: inner UStruct at 0x70
        o.FProp_enum_prop_enum = 0x78;      // may need verification
        o.FProp_byte_prop_enum = 0x70;      // first extension member

        // UEnum — same layout as RE4 VR
        o.UEnum_names_data = 0x40;
        o.UEnum_names_num = 0x48;
        o.UEnum_names_max = 0x4C;
        o.UEnum_entry_size = 16;

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
    static GameProfile build_unknown_profile(const std::string &pkg)
    {
        GameProfile p;
        p.id = GameID::UNKNOWN;
        p.package_name = pkg;
        p.display_name = "Unknown Game (" + pkg + ")";
        p.engine_lib_name = "libUE4.so"; // try UE4 first, main.cpp tries libUnreal.so too
        p.engine_version = "Unknown";

        // Use RE4 VR offsets as defaults (most common UE4 layout)
        p.offsets = build_re4_vr_profile().offsets;

        return p;
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
                            "[PROFILE] FUObjectItem size: 0x%X, GUObjectArray_to_objects: 0x%X",
                            s_profile.offsets.FUObjectItem_size,
                            s_profile.offsets.GUObjectArray_to_objects);

        s_initialized = true;
    }

    // ═══ Accessors ══════════════════════════════════════════════════════════
    GameID detected_game() { return s_profile.id; }
    const GameProfile &profile() { return s_profile; }
    const TypeOffsets &offsets() { return s_profile.offsets; }
    const std::string &engine_lib_name() { return s_profile.engine_lib_name; }
    const std::string &display_name() { return s_profile.display_name; }
    const std::string &package_name() { return s_profile.package_name; }
    bool is_pinball_fx() { return s_profile.id == GameID::PINBALL_FX_VR; }
    bool is_re4_vr() { return s_profile.id == GameID::RE4_VR; }

} // namespace game_profile
