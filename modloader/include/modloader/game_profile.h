#pragma once
// modloader/include/modloader/game_profile.h
// Multi-game support — runtime detection of game package + engine library.
// Provides per-game UE type offsets, fallback symbol addresses, and engine
// library name. Detected once at boot from /proc/self/cmdline.
//
// Engine version auto-detection: If the game is UNKNOWN, the modloader
// probes the loaded binary for version strings and structural signatures
// to determine UE4 vs UE5 and the exact minor version.

#include <string>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include "modloader/engine_versions.h"

namespace game_profile
{

    // ═══ Supported games ════════════════════════════════════════════════════
    enum class GameID
    {
        RE4_VR,        // com.Armature.VR4 — UE4.27 ARM64 (libUE4.so)
        PINBALL_FX_VR, // com.zenstudios.PFXVRQuest — UE5 ARM64 (libUnreal.so)
        UNKNOWN        // fallback — auto-detects engine version at boot
    };

    // ═══ Per-game UE type layout offsets ════════════════════════════════════
    // These vary between UE4/UE5 builds and even game-specific custom builds.
    // The game profile sets them once at boot and the rest of the modloader reads them.
    struct TypeOffsets
    {
        // ── GNames / FNamePool ──
        uint32_t GNames_to_FNamePool;    // offset from GNames global to FNamePool start
        uint32_t FNamePool_to_Blocks;    // offset within FNamePool to Blocks[] array
        uint32_t FNameEntry_header_size; // bytes per FNameEntry header (usually 2)
        uint32_t FNameEntry_len_shift;   // bit shift to extract length from header
        uint32_t FName_stride;           // stride multiplier for block offset
        uint32_t FName_max_blocks;       // max blocks in FNamePool
        uint32_t FName_block_bits;       // bits for block index

        // ── GUObjectArray ──
        uint32_t GUObjectArray_to_objects;   // offset to embedded TUObjectArray
        uint32_t TUObjectArray_num_elements; // offset to NumElements within TUObjectArray
        uint32_t FUObjectItem_size;          // size of each FUObjectItem (0x18 RE4, 0x14 PFXVR)
        uint32_t FUObjectItem_padding;       // padding in FUObjectItem
        uint32_t FUObjectItem_chunk_size;    // elements per chunk (65536)

        // ── UObjectBase field offsets ──
        uint32_t UObj_vtable;
        uint32_t UObj_flags;
        uint32_t UObj_internal_index;
        uint32_t UObj_class;
        uint32_t UObj_fname_index;
        uint32_t UObj_fname_number;
        uint32_t UObj_outer;

        // ── UField ──
        uint32_t UField_next;

        // ── UStruct ──
        uint32_t UStruct_super;
        uint32_t UStruct_children;
        uint32_t UStruct_child_properties;
        uint32_t UStruct_properties_size;

        // ── UFunction ──
        uint32_t UFunction_flags;
        uint32_t UFunction_num_parms;
        uint32_t UFunction_parms_size;
        uint32_t UFunction_return_value_offset;
        uint32_t UFunction_func_ptr;

        // ── FField ──
        uint32_t FField_class;
        uint32_t FField_owner;
        uint32_t FField_next;
        uint32_t FField_name;

        // ── FProperty ──
        uint32_t FProp_element_size;
        uint32_t FProp_property_flags;
        uint32_t FProp_offset_internal;
        uint32_t FProp_size;

        // FProperty typed inner pointers
        uint32_t FProp_bool_field_size;
        uint32_t FProp_bool_byte_offset;
        uint32_t FProp_bool_byte_mask;
        uint32_t FProp_bool_field_mask;
        uint32_t FProp_obj_property_class;
        uint32_t FProp_class_meta_class;
        uint32_t FProp_interface_class;
        uint32_t FProp_array_inner;
        uint32_t FProp_map_key_prop;
        uint32_t FProp_map_value_prop;
        uint32_t FProp_set_element_prop;
        uint32_t FProp_struct_inner_struct;
        uint32_t FProp_enum_prop_enum;
        uint32_t FProp_byte_prop_enum;

        // ── UEnum ──
        uint32_t UEnum_names_data;
        uint32_t UEnum_names_num;
        uint32_t UEnum_names_max;
        uint32_t UEnum_entry_size;
    };

    // ═══ Fallback offset entry ══════════════════════════════════════════════
    struct FallbackOffset
    {
        std::string symbol_name;
        uintptr_t offset; // offset from lib base
    };

    // ═══ Pattern signature entry ════════════════════════════════════════════
    struct PatternSignature
    {
        std::string symbol_name;
        std::string pattern; // AOB pattern "AA BB ?? CC DD"
        int rip_offset;      // -1 = no rip resolution
        int instr_size;      // used with rip_offset
    };

    // ═══ Game profile ═══════════════════════════════════════════════════════
    struct GameProfile
    {
        GameID id;
        std::string package_name;    // e.g. "com.Armature.VR4"
        std::string display_name;    // e.g. "RE4 VR"
        std::string engine_lib_name; // e.g. "libUE4.so"
        std::string engine_version;  // e.g. "UE4.27", "UE5"

        // Detected engine version enum (set during init or auto-detection)
        engine_versions::EngineVersion detected_engine_version =
            engine_versions::EngineVersion::UNKNOWN;

        TypeOffsets offsets;

        std::vector<FallbackOffset> fallback_offsets;
        std::vector<PatternSignature> pattern_signatures;
    };

    // ═══ API ════════════════════════════════════════════════════════════════

    // Detect the current game from /proc/self/cmdline and initialize the profile.
    // Must be called FIRST, before symbols::init() or pattern::init().
    void init();

    // Get the detected game ID
    GameID detected_game();

    // Get the full game profile
    const GameProfile &profile();

    // Get a mutable reference to the game profile (for auto_offsets to update)
    GameProfile &mutable_profile();

    // Get the type offsets for the current game
    const TypeOffsets &offsets();

    // Get the engine library name (e.g. "libUE4.so" or "libUnreal.so")
    const std::string &engine_lib_name();

    // Get the display name of the game
    const std::string &display_name();

    // Get the package name
    const std::string &package_name();

    // Helper: is this Pinball FX VR?
    bool is_pinball_fx();

    // Helper: is this RE4 VR?
    bool is_re4_vr();

    // Get the detected engine version enum
    engine_versions::EngineVersion engine_version_enum();

    // Helper: is this a UE5 game?
    bool is_ue5();

    // Helper: is this a UE4 game?
    bool is_ue4();

    // ═══ Engine Version Auto-Detection ═════════════════════════════════════
    // For unknown games, probe the engine binary to determine version.
    // This runs automatically during init() if GameID is UNKNOWN.

    // Detect engine version by scanning for version strings in mapped memory.
    // Requires pattern::init() to have been called (needs library memory region).
    // Returns EngineVersion::UNKNOWN if no version string found.
    engine_versions::EngineVersion detect_engine_version_from_binary();

    // Build type offsets for a given engine version using compile-time constants
    // from engine_versions.h. Falls back to UE4.25 offsets for UNKNOWN.
    TypeOffsets build_offsets_for_version(engine_versions::EngineVersion version);

} // namespace game_profile
