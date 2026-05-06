#pragma once
// modloader/include/modloader/symbols.h
// Dynamic symbol resolution: dlsym → phdr scan → pattern scan → fallback offset
// All attempts logged at every priority level

#include <string>
#include <cstdint>
#include <unordered_map>
#include "modloader/types.h"

namespace symbols
{

    // Initialize — caches libUE4.so handle and base address
    void init();

    // Core resolution function — tries all priority levels in order:
    // 1. dlsym by exact name
    // 2. dl_iterate_phdr scan across all loaded libraries
    // 3. Pattern/AOB scan against libUE4.so mapped memory
    // 4. UE4Dumper fallback offset (if registered)
    // Returns nullptr if all fail — never a wrong address.
    void *resolve(const std::string &name);

    // Resolve with a fallback offset relative to libUE4.so base
    // Used only for UE4Dumper-derived constants as Priority 5
    void *resolve_with_fallback(const std::string &name, uintptr_t fallback_offset);

    // Register a fallback offset for a symbol (from UE4Dumper data)
    void register_fallback(const std::string &name, uintptr_t offset);

    // Register a pattern for a symbol (for Priority 3 AOB scan)
    // If rip_offset >= 0, resolution uses pattern::scan_rip() to decode
    // ADRP/ADD/LDR style PC-relative targets (for globals in .data/.bss).
    void register_pattern(const std::string &name,
                          const std::string &pattern,
                          int rip_offset = -1,
                          int instr_size = 0);

    // Register a relative offset for a symbol (for fallback resolution)
    // When `target_name` cannot be found directly, resolve `anchor_name` first
    // then add `delta` bytes to get the target address.
    // Used for functions lacking unique AOB patterns, resolved via a nearby
    // confirmed function in the same compilation unit.
    void register_relative_offset(const std::string &target_name,
                                  const std::string &anchor_name,
                                  int64_t delta);

    // Get the base address of libUE4.so
    uintptr_t lib_base();

    // Get the base address of libUE4.so as void* (convenience)
    void *get_lib_base();

    // Get the libUE4.so handle (for dlsym)
    void *lib_handle();

    // Check if a symbol was successfully resolved
    bool is_resolved(const std::string &name);

    // Get a previously resolved symbol without re-resolving
    void *get_cached(const std::string &name);

    // ═══ Pre-resolved core symbols — set during init ════════════════════════
    // These are resolved once on boot and cached globally

    // UObject::ProcessEvent
    extern ue::ProcessEventFn ProcessEvent;

    // Static helpers
    extern ue::StaticFindObjectFn StaticFindObject;
    extern ue::StaticLoadObjectFn StaticLoadObject;
    extern ue::StaticLoadClassFn StaticLoadClass;
    extern ue::StaticConstructObjectFn StaticConstructObject;
    extern ue::PakMountFn PakMount;

    // Globals (pointers to the global variables in libUE4.so)
    extern void *GUObjectArray;
    extern void *GNames;
    extern void *GEngine;
    extern void *GWorld;

    // GetTransientPackage
    using GetTransientPackageFn = ue::UObject *(*)();
    extern GetTransientPackageFn GetTransientPackage;

    // Game-specific
    extern void *GetEtcModelClass;

    // FName constructor — FName::Init(FName* out, const TCHAR* name, int32_t Number)
    // ARM64 Android: TCHAR = char16_t (2 bytes), NOT wchar_t (4 bytes)
    using FName_InitFn = void (*)(ue::FName *out, const char16_t *name, int32_t number);
    extern FName_InitFn FName_Init;

    // FOutputDevice::Log — void FOutputDevice::Log(const TCHAR* msg)
    using FOutputDevice_LogFn = void (*)(void *device, const char16_t *msg);
    extern FOutputDevice_LogFn FOutputDevice_Log;

    // FText::ToString() const — ARM64 ABI: X0=this, X8=sret(FString)
    // Called via arm64_call_ftext_tostring() wrapper (inline asm for correct X8 sret)
    using FText_ToStringFn = void *; // raw function pointer — never call directly
    extern FText_ToStringFn FText_ToString;

    // FText::FromString(FString&&) — static, ARM64 ABI: X0=FString*, X8=sret(FText)
    // Called via arm64_call_ftext_fromstring() wrapper (inline asm for correct X8 sret)
    using FText_FromStringFn = void *; // raw function pointer — never call directly
    extern FText_FromStringFn FText_FromString;

    // FText::~FText() — destructor, decrements refcount
    using FText_DtorFn = void (*)(void *ftext);
    extern FText_DtorFn FText_Dtor;

    // FText::FText() — default constructor
    using FText_CtorFn = void (*)(void *ftext);
    extern FText_CtorFn FText_Ctor;

    // Resolve all core symbols — called during boot
    void resolve_core_symbols();

    // Dump all exported symbols from libUE4.so to a file
    // Reads ELF .dynsym and .symtab sections directly
    // Returns the number of symbols dumped
    int dump_symbols(const std::string &output_path);

} // namespace symbols
