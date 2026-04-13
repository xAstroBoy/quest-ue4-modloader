#pragma once
// modloader/include/modloader/auto_offsets.h
// ═══════════════════════════════════════════════════════════════════════════
// DYNAMIC OFFSET FINDER — Auto-discovers critical UE engine offsets for
// ANY Unreal Engine version (UE4.25 → UE5.6+) on Android ARM64.
//
// This module works by:
//   1. String scanning — finds version markers in .rodata
//   2. String cross-references — finds functions that reference known strings
//   3. Heuristic probing — validates structures by reading and checking patterns
//   4. Pattern matching — AOB scans for known instruction sequences
//   5. Statistical validation — cross-checks multiple independent discoveries
//
// DISCOVERS:
//   - Engine version (UE4.25, UE4.27, UE5.0, ..., UE5.6)
//   - GNames / FNamePool pointer
//   - GUObjectArray pointer
//   - ProcessEvent function
//   - FUObjectItem size (0x14 for UE4, 0x18 for UE5)
//   - FField owner type (8 bytes UE5, 16 bytes UE4 FFieldVariant)
//   - UObject::ProcessEvent vtable index
//   - StaticFindObject / StaticLoadObject / StaticLoadClass
//   - FName::Init function
//   - GEngine pointer
//
// USAGE:
//   auto_offsets::init();  // call after pattern::init()
//   auto result = auto_offsets::discover_all();
//   if (result.gnames) { ... }
//   auto_offsets::apply_to_profile(profile);  // merge into game_profile
// ═══════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include "modloader/engine_versions.h"

namespace auto_offsets
{

    // ═══ Discovery result ═══════════════════════════════════════════════════
    struct DiscoveryResult
    {
        // Engine version detection
        engine_versions::EngineVersion detected_version;
        std::string version_string; // raw version string found in binary (e.g. "+UE4+Release-4.27")
        bool is_ue5;                // true if UE5.x detected

        // Global pointers (0 = not found)
        uintptr_t gnames;        // GNames or FNamePool
        uintptr_t guobjectarray; // GUObjectArray
        uintptr_t gengine;       // GEngine
        uintptr_t gworld;        // GWorld

        // Function addresses (0 = not found)
        uintptr_t process_event; // UObject::ProcessEvent
        uintptr_t static_find_object;
        uintptr_t static_load_object;
        uintptr_t static_load_class;
        uintptr_t static_construct_object;
        uintptr_t fname_init;       // FName::Init
        uintptr_t ftext_tostring;   // FText::ToString
        uintptr_t ftext_fromstring; // FText::FromString
        uintptr_t pak_mount;        // FPakPlatformFile::Mount

        // Structural offsets (0 = unknown / use defaults)
        uint32_t fuobjectitem_size;    // 0x14 (UE4) or 0x18 (UE5)
        uint32_t ffield_owner_size;    // 8 (UE5 raw ptr) or 16 (UE4 FFieldVariant)
        uint32_t uobject_name_offset;  // typically 0x18
        uint32_t uobject_class_offset; // typically 0x10
        uint32_t uobject_outer_offset; // typically 0x20

        // Confidence scores (0.0 = no data, 1.0 = high confidence)
        float version_confidence;
        float gnames_confidence;
        float guobjectarray_confidence;
        float process_event_confidence;

        // Diagnostics
        int total_discoveries;
        int failed_discoveries;
        std::vector<std::string> discovery_log;
    };

    // ═══ Initialization ═════════════════════════════════════════════════════
    // Must be called after pattern::init() since we use the pattern scanner.
    void init();

    // ═══ Full discovery — runs ALL heuristics ═══════════════════════════════
    // This is the main entry point. Runs all finders in order:
    //   1. detect_engine_version()
    //   2. find_gnames()
    //   3. find_guobjectarray()
    //   4. find_process_event()
    //   5. find_global_pointers()
    //   6. probe_struct_sizes()
    //   7. validate_discoveries()
    DiscoveryResult discover_all();

    // ═══ Individual finders (can be called separately) ═══════════════════════

    // Detect UE version from binary strings (+UE4+Release-X.Y, +UE5+Release-X.Y)
    engine_versions::EngineVersion detect_engine_version(std::string *out_version_string = nullptr);

    // Find GNames / FNamePool pointer via string xrefs
    uintptr_t find_gnames();

    // Find GUObjectArray pointer via string xrefs
    uintptr_t find_guobjectarray();

    // Find UObject::ProcessEvent via string xrefs or vtable scan
    uintptr_t find_process_event();

    // Find GEngine via string xrefs
    uintptr_t find_gengine();

    // Find GWorld via string xrefs
    uintptr_t find_gworld();

    // Find FName::Init via string xrefs
    uintptr_t find_fname_init();

    // Find StaticFindObject via string xrefs
    uintptr_t find_static_find_object();

    // Find StaticConstructObject_Internal via string xrefs
    uintptr_t find_static_construct_object();

    // Probe FUObjectItem size by validating object chains
    // Returns 0x14 (UE4) or 0x18 (UE5) or 0 if unknown
    uint32_t probe_fuobjectitem_size(uintptr_t guobjectarray);

    // Probe FField owner size (8 vs 16) by checking field chain integrity
    uint32_t probe_ffield_owner_size();

    // ═══ Apply discoveries to game profile ══════════════════════════════════
    // Merges discovered offsets into the active game profile.
    // Only overwrites values that were NOT already set by the game profile
    // (game profile takes priority over auto-discovery for known games).
    void apply_to_profile(const DiscoveryResult &result);

    // ═══ Utility — ARM64 instruction helpers ════════════════════════════════

    // Given an ADRP+ADD pair at `addr`, decode the target address.
    // Returns the absolute address, or 0 on failure.
    uintptr_t decode_adrp_add(uintptr_t addr);

    // Given an ADRP+LDR pair at `addr`, decode the target address.
    uintptr_t decode_adrp_ldr(uintptr_t addr);

    // Find all ADRP instructions in .text that reference a given page.
    // Useful for finding all code that references a particular .data address.
    std::vector<uintptr_t> find_adrp_refs_to(uintptr_t target_addr);

    // Scan backwards from an address to find the function prologue (STP X29,X30)
    uintptr_t find_function_start(uintptr_t addr, int max_search = 4096);

} // namespace auto_offsets
