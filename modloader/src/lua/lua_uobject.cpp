// modloader/src/lua/lua_uobject.cpp
// UObject Lua userdata wrapper — property access, function calls, validity checks
// __index resolves properties via Offset_Internal
// __newindex writes properties
// Methods: Get, Set, Call, IsValid, GetName, GetFullName, GetClass, GetClassName

#include "modloader/lua_uobject.h"
#include "modloader/class_rebuilder.h"
#include "modloader/reflection_walker.h"
#include "modloader/process_event_hook.h"
#include "modloader/lua_tarray.h"
#include "modloader/lua_ustruct.h"
#include "modloader/lua_types.h"
#include "modloader/symbols.h"
#include "modloader/logger.h"
#include "modloader/types.h"

#include <cstring>
#include <atomic>
#include <setjmp.h>
#include <algorithm>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace lua_uobject
{
    // ═══ ProcessEvent Crash Guard ══════════════════════════════════════════
    // When Call()/CallBg() invokes ProcessEvent and the target UFunction
    // crashes (SIGSEGV/SIGBUS — e.g. dangling pointer inside the function,
    // null widget, uninitialized subsystem), the crash handler detects this
    // flag and siglongjmp's back instead of killing the entire process.
    // This is the same pattern used by native_hooks.cpp for hook originals.
    thread_local volatile int g_in_call_ufunction = 0;
    thread_local sigjmp_buf g_call_ufunction_jmp;
    thread_local volatile uintptr_t g_call_ufunction_fault_addr = 0;

    // GUObjectArray round-trip validation.
    // Reads InternalIndex via an UNTAGGED (MTE tag=0) pointer — ARM MTE excludes
    // tag-0 accesses from hardware tag checking, so this is safe even when the
    // Lua-held pointer has a stale or mismatched MTE tag in its top byte.
    // Returns false if the GUObjectArray slot no longer holds this exact object
    // (i.e., object was GC'd, freed, or reallocated since the pointer was captured).
    // EObjectFlags that indicate an object is NOT safe to call native methods on.
    // These cover objects that are: CDOs, archetypes, still loading (PrimaryDataAsset async),
    // not yet initialized, or in the process of being GC'd.
    // CRASH PATTERN: fault_addr=0x0/0x44/0xd4/0x118 etc. are NULL+field_offset derefs
    // caused by calling SetBrandNewUnlock on PFXCollectibleCategoryData objects that
    // extend PrimaryDataAsset and have RF_NeedPostLoad set — their C++ fields are garbage.
    static constexpr int32_t UNSAFE_OBJECT_FLAGS =
        ue::RF_ClassDefaultObject       |  // 0x0010 CDO — never has real data
        ue::RF_ArchetypeObject          |  // 0x0020 archetype — template, not instance
        ue::RF_NeedInitialization       |  // 0x0200 constructor not run yet
        ue::RF_NeedLoad                 |  // 0x0400 still being deserialized
        ue::RF_NeedPostLoad             |  // 0x1000 PostLoad() not called yet
        ue::RF_NeedPostLoadSubobjects   |  // 0x2000 sub-object post-load pending
        ue::RF_BeginDestroyed           |  // 0x8000 ConditionalBeginDestroy() called
        ue::RF_FinishDestroyed;            // 0x10000 FinishDestroy() called

    static bool is_live_in_guobjectarray(ue::UObject *obj)
    {
        if (!obj)
            return false;
        // Strip top byte → untagged pointer (tag=0 bypasses MTE check on ARM64)
        uintptr_t raw = reinterpret_cast<uintptr_t>(obj) & 0x00FFFFFFFFFFFFFFULL;
        if (raw < 0x10000)
            return false;
        // Sanity: page must still be mapped before we read anything from it
        if (!ue::is_mapped_ptr(reinterpret_cast<const void *>(raw)))
            return false;
        // Check EObjectFlags — reject objects in unsafe states (uninitialized,
        // still loading, CDO, archetype, or being destroyed). These objects exist
        // in GUObjectArray but their C++ fields are not ready for native function calls.
        int32_t obj_flags = 0;
        __builtin_memcpy(&obj_flags, reinterpret_cast<const void *>(raw + ue::uobj::OBJECT_FLAGS), sizeof(obj_flags));
        if (obj_flags & UNSAFE_OBJECT_FLAGS)
            return false;
        // Read InternalIndex at offset 0x0C via the untagged address
        int32_t idx = 0;
        __builtin_memcpy(&idx, reinterpret_cast<const void *>(raw + ue::uobj::INTERNAL_INDEX), sizeof(idx));
        // Sanity bound: valid indices are [0, NumElements).
        // Use a safe upper cap to avoid reading past the chunk pointer array.
        int32_t live_count = reflection::get_live_object_count();
        if (idx < 0 || live_count <= 0 || idx >= live_count)
            return false;
        // Look up the GUObjectArray slot — get_object_by_index is safe for validated idx
        ue::UObject *slot_obj = reflection::get_object_by_index(idx);
        if (!slot_obj)
            return false; // slot is empty (object was freed)
        // Compare raw addresses — strip tags for comparison so a freshly reallocated
        // object at the same VA but different MTE tag is detected as different
        uintptr_t slot_raw = reinterpret_cast<uintptr_t>(slot_obj) & 0x00FFFFFFFFFFFFFFULL;
        return slot_raw == raw;
    }

    // Diagnostic version: logs WHY the check failed (called from ObjectProperty reader)
    static bool is_live_in_guobjectarray_diag(ue::UObject *obj, const char *prop_name)
    {
        if (!obj)
            return false;
        uintptr_t raw = reinterpret_cast<uintptr_t>(obj) & 0x00FFFFFFFFFFFFFFULL;
        if (raw < 0x10000)
        {
            logger::log_warn("GUOA", "'%s' 0x%llx: raw<0x10000", prop_name, (unsigned long long)raw);
            return false;
        }
        if (!ue::is_mapped_ptr(reinterpret_cast<const void *>(raw)))
        {
            logger::log_warn("GUOA", "'%s' 0x%llx: not mapped", prop_name, (unsigned long long)raw);
            return false;
        }
        // Check EObjectFlags — object must be fully initialized and not being destroyed
        int32_t obj_flags = 0;
        __builtin_memcpy(&obj_flags, reinterpret_cast<const void *>(raw + ue::uobj::OBJECT_FLAGS), sizeof(obj_flags));
        if (obj_flags & UNSAFE_OBJECT_FLAGS)
        {
            logger::log_warn("GUOA", "'%s' 0x%llx: unsafe EObjectFlags=0x%x (loading/uninit/destroyed)",
                             prop_name, (unsigned long long)raw, (unsigned)obj_flags);
            return false;
        }
        int32_t idx = 0;
        __builtin_memcpy(&idx, reinterpret_cast<const void *>(raw + ue::uobj::INTERNAL_INDEX), sizeof(idx));
        int32_t live_count = reflection::get_live_object_count();
        if (idx < 0 || live_count <= 0 || idx >= live_count)
        {
            logger::log_warn("GUOA", "'%s' 0x%llx: idx=%d out of [0,%d)", prop_name, (unsigned long long)raw, idx, live_count);
            return false;
        }
        ue::UObject *slot_obj = reflection::get_object_by_index(idx);
        if (!slot_obj)
        {
            logger::log_warn("GUOA", "'%s' 0x%llx: idx=%d slot is null (freed)", prop_name, (unsigned long long)raw, idx);
            return false;
        }
        uintptr_t slot_raw = reinterpret_cast<uintptr_t>(slot_obj) & 0x00FFFFFFFFFFFFFFULL;
        if (slot_raw != raw)
        {
            logger::log_warn("GUOA", "'%s' 0x%llx: idx=%d slot=0x%llx MISMATCH", prop_name, (unsigned long long)raw, idx, (unsigned long long)slot_raw);
            return false;
        }
        return true;
    }

    // ═══ Game-thread ProcessEvent dispatch (throttled via pe_hook queue) ════
    // Instead of a background worker thread (which crashes because UE Blueprint
    // functions are NOT thread-safe), we dispatch CallBg items to the game thread.
    // The game-thread queue in process_event_hook.cpp is THROTTLED (max 10 items
    // per PE tick), so 5000+ items won't cause ANR — they process over ~8 seconds.
    //
    // Previous approach #1 (bg thread + Dobby trampoline) crashed after ~10 calls
    // because UE Blueprint functions access game-thread-only state (JNI, Slate, etc.)
    //
    // Previous approach #2 (game-thread dispatch draining ALL at once) froze the game.
    //
    // This approach works because: (1) ProcessEvent runs on game thread where it's safe,
    // (2) throttled drain ensures responsive game thread.
    struct GtCallItem
    {
        ue::UObject *obj;
        ue::UFunction *func;
        std::shared_ptr<std::vector<uint8_t>> params;
        std::shared_ptr<std::vector<char16_t *>> fstring_allocs;
        std::string class_name;
        std::string func_name;
        std::vector<size_t> array_param_offsets; // offsets of TArray params to zero after PE
    };

    static std::atomic<int> s_gt_call_total{0};
    static std::atomic<int> s_gt_call_queued{0};

    static std::string hex_dump(const uint8_t *data, size_t len, size_t max_len = 32)
    {
        std::string result;
        for (size_t i = 0; i < std::min(len, max_len); i++)
        {
            char buf[4];
            snprintf(buf, sizeof(buf), "%02X ", data[i]);
            result += buf;
        }
        return result;
    }

    static size_t marshaled_min_size(const reflection::PropertyInfo &pi)
    {
        auto clamp_elem = [&]() -> size_t
        {
            return pi.element_size > 0 ? static_cast<size_t>(pi.element_size) : static_cast<size_t>(1);
        };

        switch (pi.type)
        {
        case reflection::PropType::BoolProperty:
            if (pi.bool_byte_mask)
                return static_cast<size_t>(pi.bool_byte_offset) + 1;
            return std::max(clamp_elem(), static_cast<size_t>(1));

        case reflection::PropType::Int8Property:
        case reflection::PropType::ByteProperty:
            return std::max(clamp_elem(), static_cast<size_t>(1));

        case reflection::PropType::Int16Property:
        case reflection::PropType::UInt16Property:
            return std::max(clamp_elem(), static_cast<size_t>(2));

        case reflection::PropType::IntProperty:
        case reflection::PropType::UInt32Property:
        case reflection::PropType::FloatProperty:
        case reflection::PropType::EnumProperty:
            return std::max(clamp_elem(), static_cast<size_t>(4));

        case reflection::PropType::Int64Property:
        case reflection::PropType::UInt64Property:
        case reflection::PropType::DoubleProperty:
        case reflection::PropType::NameProperty:
        case reflection::PropType::ObjectProperty:
        case reflection::PropType::WeakObjectProperty:
        case reflection::PropType::SoftObjectProperty:
        case reflection::PropType::LazyObjectProperty:
        case reflection::PropType::ClassProperty:
        case reflection::PropType::SoftClassProperty:
        case reflection::PropType::InterfaceProperty:
            return std::max(clamp_elem(), static_cast<size_t>(8));

        case reflection::PropType::DelegateProperty:
            return std::max(clamp_elem(), static_cast<size_t>(16));

        case reflection::PropType::StrProperty:
            // FString raw layout: { char16_t* data; int32 num; int32 max } => 16 bytes on ARM64
            return std::max(clamp_elem(), static_cast<size_t>(16));

        case reflection::PropType::TextProperty:
            return std::max(clamp_elem(), static_cast<size_t>(24));

        case reflection::PropType::StructProperty:
        case reflection::PropType::ArrayProperty:
        case reflection::PropType::MapProperty:
        case reflection::PropType::SetProperty:
        case reflection::PropType::FieldPathProperty:
        case reflection::PropType::MulticastDelegateProperty:
        case reflection::PropType::MulticastInlineDelegateProperty:
        case reflection::PropType::MulticastSparseDelegateProperty:
        case reflection::PropType::Unknown:
        default:
            return clamp_elem();
        }
    }

    static bool marshaled_range_valid(uint16_t parms_size,
                                      const reflection::PropertyInfo &pi,
                                      const std::string &class_name,
                                      const std::string &func_name,
                                      const char *tag)
    {
        if (pi.offset < 0)
        {
            logger::log_error(tag, "%s::%s has negative property offset for '%s' (offset=%d)",
                              class_name.c_str(), func_name.c_str(), pi.name.c_str(), pi.offset);
            return false;
        }

        size_t min_sz = marshaled_min_size(pi);
        size_t start = static_cast<size_t>(pi.offset);
        size_t limit = static_cast<size_t>(parms_size);

        if (start > limit || min_sz > (limit - start))
        {
            logger::log_error(tag,
                              "%s::%s param '%s' out of bounds (offset=%d min_size=%zu parms_size=%u type=%d)",
                              class_name.c_str(), func_name.c_str(), pi.name.c_str(), pi.offset,
                              min_sz, parms_size, static_cast<int>(pi.type));
            return false;
        }

        return true;
    }

    static void dispatch_gt_call(GtCallItem item)
    {
        // This runs ON the game thread (called from pe_hook queue drain)
        // Use symbols::ProcessEvent (hooked entry) — this is safe because:
        // 1. We're on the game thread (thread-safe for UE)
        // 2. The queue drain has s_draining guard preventing recursive drains
        // 3. Going through the hook ensures normal UE hook processing
        auto pe_fn = pe_hook::get_original();
        if (!pe_fn)
            pe_fn = symbols::ProcessEvent;
        if (!pe_fn)
        {
            if (item.fstring_allocs)
                for (auto *p : *item.fstring_allocs)
                    delete[] p;
            return;
        }

        // Defensive validation — queued calls may execute later when object/function
        // is already destroyed or GC'd.
        if (!item.obj || !ue::is_mapped_ptr(item.obj) || !ue::is_valid_ptr(item.obj) || !ue::is_valid_uobject(item.obj) || !is_live_in_guobjectarray(item.obj))
        {
            logger::log_warn("CALLBG", "[GT] Dropping call %s::%s — invalid/stale object %p",
                             item.class_name.c_str(), item.func_name.c_str(), item.obj);
            if (item.fstring_allocs)
                for (auto *p : *item.fstring_allocs)
                    delete[] p;
            return;
        }
        if (!item.func || !ue::is_mapped_ptr(item.func) || !ue::is_valid_ptr(item.func))
        {
            logger::log_warn("CALLBG", "[GT] Dropping call %s::%s — invalid function %p",
                             item.class_name.c_str(), item.func_name.c_str(), item.func);
            if (item.fstring_allocs)
                for (auto *p : *item.fstring_allocs)
                    delete[] p;
            return;
        }

        void *params_ptr = item.params ? item.params->data() : nullptr;

        // Save FunctionFlags — ProcessEvent modifies them internally
        uint32_t saved_func_flags = ue::ufunc_get_flags(item.func);

        // Save DestructorLink — null it before PE to prevent DestroyValue
        // from freeing shallow-copied TArray Data pointers in struct params.
        void *saved_dtor_link = ue::read_field<void *>(item.func, ue::ustruct::DESTRUCTOR_LINK_OFF());

        // Null DestructorLink only for non-native Blueprint functions (same race guard as Call()).
        static constexpr uint32_t FUNC_Native_GT = 0x00000400;
        const bool is_native_gt = (saved_func_flags & FUNC_Native_GT) != 0;

        // Crash guard — if ProcessEvent crashes, recover instead of killing game
        g_in_call_ufunction = 1;
        int pe_crash_sig = sigsetjmp(g_call_ufunction_jmp, 1);
        if (pe_crash_sig != 0)
        {
            ue::write_field(item.func, ue::ufunc::FUNCTION_FLAGS_OFF(), saved_func_flags);
            // Only restore DestructorLink if we mutated it (non-native functions only)
            if (!is_native_gt)
                ue::write_field(item.func, ue::ustruct::DESTRUCTOR_LINK_OFF(), saved_dtor_link);
            uintptr_t fault = g_call_ufunction_fault_addr;
            logger::log_error("CALLBG", "ProcessEvent CRASHED (signal %d, fault_addr=0x%lx) calling %s::%s on %p — recovered, skipping",
                              pe_crash_sig, (unsigned long)fault,
                              item.class_name.c_str(), item.func_name.c_str(), item.obj);
            g_in_call_ufunction = 0;
            // Still clean up allocations
            if (item.fstring_allocs)
                for (auto *p : *item.fstring_allocs)
                    delete[] p;
            // Conversion buffers are tracked per-thread via ConvBufScope in call_ufunction.
            // dispatch_gt_call runs on game thread — no conversion buffers to clean up here.
            return;
        }

        // Null DestructorLink only for non-native Blueprint functions (race guard applied above)
        if (!is_native_gt)
        {
            ue::write_field(item.func, ue::ustruct::DESTRUCTOR_LINK_OFF(), static_cast<void *>(nullptr));
        }

        pe_fn(item.obj, item.func, params_ptr);

        // Restore DestructorLink + FunctionFlags (only if we mutated them)
        if (!is_native_gt)
        {
            ue::write_field(item.func, ue::ustruct::DESTRUCTOR_LINK_OFF(), saved_dtor_link);
        }
        ue::write_field(item.func, ue::ufunc::FUNCTION_FLAGS_OFF(), saved_func_flags);
        g_in_call_ufunction = 0;

        // Conversion buffers are tracked per-thread via ConvBufScope in call_ufunction.
        // dispatch_gt_call runs on game thread — no conversion buffers to clean up here.

        int total = s_gt_call_total.fetch_add(1, std::memory_order_relaxed) + 1;

        // Detailed logging for first 10 calls, then every 500
        if (total <= 10)
        {
            auto hex = params_ptr ? hex_dump(static_cast<uint8_t *>(params_ptr),
                                             item.params ? item.params->size() : 0)
                                  : "NULL";
            logger::log_info("CALLBG", "[GT] #%d %s::%s obj=%p func=%p params(%zu)=[%s]",
                             total, item.class_name.c_str(), item.func_name.c_str(),
                             item.obj, item.func,
                             item.params ? item.params->size() : 0,
                             hex.c_str());
        }
        else if (total % 500 == 0)
        {
            int remaining = s_gt_call_queued.load(std::memory_order_relaxed) - total;
            logger::log_info("CALLBG", "[GT] Progress: %d/%d calls processed (%d remaining)",
                             total, s_gt_call_queued.load(std::memory_order_relaxed),
                             remaining > 0 ? remaining : 0);
        }

        // Clean up FString heap allocs
        if (item.fstring_allocs)
        {
            for (auto *p : *item.fstring_allocs)
                delete[] p;
        }
    }

    static void enqueue_gt_call(GtCallItem &&item)
    {
        s_gt_call_queued.fetch_add(1, std::memory_order_relaxed);
        auto shared_item = std::make_shared<GtCallItem>(std::move(item));
        pe_hook::queue_game_thread([shared_item]()
                                   { dispatch_gt_call(std::move(*shared_item)); });
    }

    // ═══ ARM64 FText support ═════════════════════════════════════════════════
    // FText layout (24 bytes):
    //   [0x00] ITextData*       — pointer to the text data object
    //   [0x08] FSharedRefCount* — reference controller
    //   [0x10] uint32 Flags + padding
    //
    // FText::ToString() returns FString by value, which on ARM64 uses X8 sret
    // (FString has non-trivial destructor → indirect return per Itanium ABI).
    // This is problematic to call from inline asm.
    //
    // SOLUTION: Use UKismetTextLibrary::Conv_TextToString via ProcessEvent.
    // This is exactly what UE4SS does — no calling convention issues since
    // ProcessEvent handles all parameter marshaling through the reflection system.
    //
    // FText WRITE uses FText::FromString via inline asm (confirmed working).

    struct FStrRaw
    {
        char16_t *data;
        int32_t num;
        int32_t max;
    };

    // ═══ FText → string via UKismetTextLibrary::Conv_TextToString (ProcessEvent) ═══
    // This avoids all ARM64 calling convention issues by going through UE4's
    // reflection/ProcessEvent system — exactly what UE4SS does.
    static std::string ftext_to_string_via_kismet(const void *ftext_ptr)
    {
        // Cache the CDO, function, and param info on first call
        static ue::UObject *s_cdo = nullptr;
        static ue::UFunction *s_func = nullptr;
        static int32_t s_text_param_offset = -1;
        static int32_t s_ret_offset = -1;
        static uint16_t s_parms_size = 0;
        static bool s_init_failed = false;

        if (s_init_failed)
            return "";

        if (!s_func)
        {
            if (!symbols::ProcessEvent)
            {
                s_init_failed = true;
                logger::log_warn("FTEXT", "Conv_TextToString: ProcessEvent not available");
                return "";
            }

            // Strategy: rebuild KismetTextLibrary class to get the function,
            // then find the CDO as "Default__KismetTextLibrary" in GUObjectArray.
            auto *rc = rebuilder::rebuild("KismetTextLibrary");
            if (!rc)
            {
                s_init_failed = true;
                logger::log_warn("FTEXT", "Conv_TextToString: failed to rebuild KismetTextLibrary");
                return "";
            }

            auto *rf = rc->find_function("Conv_TextToString");
            if (!rf || !rf->raw)
            {
                s_init_failed = true;
                logger::log_warn("FTEXT", "Conv_TextToString: function not found on KismetTextLibrary");
                return "";
            }

            // Find the CDO — search GUObjectArray for "Default__KismetTextLibrary"
            s_cdo = reflection::find_object_by_name("Default__KismetTextLibrary");
            if (!s_cdo)
            {
                // Fallback: try to find any instance of KismetTextLibrary
                s_cdo = reflection::find_first_instance("KismetTextLibrary");
            }
            if (!s_cdo)
            {
                s_init_failed = true;
                logger::log_warn("FTEXT", "Conv_TextToString: KismetTextLibrary CDO not found");
                return "";
            }

            logger::log_info("FTEXT", "Conv_TextToString: CDO found @ %p (%s)",
                             s_cdo, reflection::get_short_name(s_cdo).c_str());

            s_func = static_cast<ue::UFunction *>(rf->raw);
            s_parms_size = ue::ufunc_get_parms_size(s_func);

            // Find param offsets from reflection data
            for (const auto &pi : rf->params)
            {
                if (pi.type == reflection::PropType::TextProperty && !(pi.flags & ue::CPF_ReturnParm))
                {
                    s_text_param_offset = pi.offset;
                }
            }
            // Accept return value of type StrProperty OR TextProperty
            if (rf->return_prop)
            {
                s_ret_offset = rf->return_offset;
            }

            logger::log_info("FTEXT", "Conv_TextToString ready: parms=%d text@%d ret@%d",
                             s_parms_size, s_text_param_offset, s_ret_offset);

            if (s_text_param_offset < 0 || s_ret_offset < 0 || s_parms_size == 0)
            {
                s_init_failed = true;
                logger::log_warn("FTEXT", "Conv_TextToString: bad param layout");
                return "";
            }
        }

        // Allocate and zero the params buffer
        std::vector<uint8_t> params_buf(s_parms_size, 0);

        // Copy the 24-byte FText into the input param position
        // We're just copying the raw bytes (pointers) — NOT incrementing refcount.
        // This is safe because ProcessEvent won't destroy the input (it's const FText&).
        std::memcpy(params_buf.data() + s_text_param_offset, ftext_ptr, 24);

        // Safety: check the FText has a valid ITextData pointer before calling
        void *text_data_check = *reinterpret_cast<void *const *>(
            params_buf.data() + s_text_param_offset);
        if (!text_data_check || !ue::is_mapped_ptr(text_data_check))
        {
            return ""; // empty/null FText — return empty string
        }

        // Call ProcessEvent — Conv_TextToString returns FString in the params buffer
        symbols::ProcessEvent(s_cdo, s_func, params_buf.data());

        // Read the return FString from the params buffer at offset 24
        // param[1] is ReturnValue: StrProperty, offset=24, size=16
        const uint8_t *ret_ptr = params_buf.data() + s_ret_offset;
        const FStrRaw *fstr = reinterpret_cast<const FStrRaw *>(ret_ptr);

        std::string result;
        if (fstr->data && fstr->num > 0 && ue::is_mapped_ptr(fstr->data))
        {
            // Convert UTF-16 to UTF-8
            int count = fstr->num - 1; // num includes null terminator
            for (int i = 0; i < count; i++)
            {
                char16_t c = fstr->data[i];
                if (c < 0x80)
                    result += static_cast<char>(c);
                else if (c < 0x800)
                {
                    result += static_cast<char>(0xC0 | (c >> 6));
                    result += static_cast<char>(0x80 | (c & 0x3F));
                }
                else
                {
                    result += static_cast<char>(0xE0 | (c >> 12));
                    result += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                    result += static_cast<char>(0x80 | (c & 0x3F));
                }
            }
            // NOTE: The FString data was allocated by UE4's allocator (FMemory::Malloc).
            // Do NOT call free() on it — that would corrupt the heap.
            // Intentional small leak per FText read (~100 bytes typical).
            // FMemory::Free is not exported on stripped ARM64 binaries and cannot
            // be reliably resolved. Calling libc free() on UE4-allocated memory
            // causes immediate heap corruption. The leak is acceptable — FText
            // reads are infrequent and the per-read cost is negligible.
        }

        return result;
    }

    // ═══ String → FText via UKismetTextLibrary::Conv_StringToText (ProcessEvent) ═══
    // Creates an FText from a UTF-8 string using UE4's own allocator.
    // This ensures the FText's internal data uses FMemory::Malloc, so
    // when UE4 later destroys it (e.g., via SetText), it calls FMemory::Free
    // on data allocated by FMemory::Malloc — no allocator mismatch.
    // Writes the resulting 24-byte FText directly to out_ftext.
    static bool ftext_from_string_via_kismet(void *out_ftext, const std::string &str)
    {
        static ue::UObject *s_cdo = nullptr;
        static ue::UFunction *s_func = nullptr;
        static int32_t s_str_param_offset = -1;
        static int32_t s_ret_offset = -1;
        static uint16_t s_parms_size = 0;
        static bool s_init_failed = false;

        if (s_init_failed)
            return false;

        if (!s_func)
        {
            if (!symbols::ProcessEvent)
            {
                s_init_failed = true;
                return false;
            }

            auto *rc = rebuilder::rebuild("KismetTextLibrary");
            if (!rc)
            {
                s_init_failed = true;
                return false;
            }

            auto *rf = rc->find_function("Conv_StringToText");
            if (!rf || !rf->raw)
            {
                s_init_failed = true;
                return false;
            }

            // Reuse the CDO we already cached for Conv_TextToString
            s_cdo = reflection::find_object_by_name("Default__KismetTextLibrary");
            if (!s_cdo)
            {
                s_init_failed = true;
                return false;
            }

            s_func = static_cast<ue::UFunction *>(rf->raw);
            s_parms_size = ue::ufunc_get_parms_size(s_func);

            for (const auto &pi : rf->params)
            {
                if (pi.type == reflection::PropType::StrProperty && !(pi.flags & ue::CPF_ReturnParm))
                {
                    s_str_param_offset = pi.offset;
                }
            }
            if (rf->return_prop)
            {
                s_ret_offset = rf->return_offset;
            }

            logger::log_info("FTEXT", "Conv_StringToText ready: parms=%d str@%d ret@%d",
                             s_parms_size, s_str_param_offset, s_ret_offset);

            if (s_str_param_offset < 0 || s_ret_offset < 0 || s_parms_size == 0)
            {
                s_init_failed = true;
                return false;
            }
        }

        // Allocate and zero the params buffer
        std::vector<uint8_t> params_buf(s_parms_size, 0);
        logger::log_info("FTEXT", "DIAG: params_buf allocated, size=%d, ptr=%p",
                         (int)s_parms_size, params_buf.data());

        // Fill the FString input param: { char16_t* Data, int32 Num, int32 Max }
        size_t wlen = str.size() + 1;
        char16_t *wbuf = new char16_t[wlen];
        for (size_t i = 0; i < str.size(); i++)
            wbuf[i] = static_cast<char16_t>(static_cast<unsigned char>(str[i]));
        wbuf[str.size()] = 0;

        logger::log_info("FTEXT", "DIAG: FString ready, wbuf=%p, wlen=%d, str='%s'",
                         wbuf, (int)wlen, str.c_str());

        FStrRaw *fstr = reinterpret_cast<FStrRaw *>(params_buf.data() + s_str_param_offset);
        fstr->data = wbuf;
        fstr->num = static_cast<int32_t>(wlen);
        fstr->max = static_cast<int32_t>(wlen);

        logger::log_info("FTEXT", "DIAG: FStrRaw set at offset %d: data=%p num=%d max=%d",
                         s_str_param_offset, fstr->data, fstr->num, fstr->max);
        logger::log_info("FTEXT", "DIAG: CDO=%p, func=%p, params=%p, about to call ProcessEvent",
                         s_cdo, s_func, params_buf.data());

        // Call Conv_StringToText via ProcessEvent
        auto original_pe = pe_hook::get_original();
        if (original_pe)
        {
            logger::log_info("FTEXT", "DIAG: Using ORIGINAL ProcessEvent (bypass hook) at %p", (void *)original_pe);
            original_pe(s_cdo, s_func, params_buf.data());
        }
        else
        {
            logger::log_info("FTEXT", "DIAG: Using HOOKED ProcessEvent at %p", (void *)symbols::ProcessEvent);
            symbols::ProcessEvent(s_cdo, s_func, params_buf.data());
        }

        logger::log_info("FTEXT", "DIAG: ProcessEvent RETURNED (no crash), copying FText from offset %d", s_ret_offset);

        // Copy the 24-byte FText return value to the output buffer
        std::memcpy(out_ftext, params_buf.data() + s_ret_offset, 24);

        logger::log_info("FTEXT", "DIAG: FText copied to out_ftext=%p, done!", out_ftext);

        // DO NOT delete[] wbuf — ProcessEvent may have already freed the FString
        // data via FMemory::Free. Accept a small leak for safety.
        // delete[] wbuf;

        return true;
    }

    // Convert FStrRaw (UTF-16) to std::string (UTF-8)
    // Used for FString property reading and call return values
    static std::string fstr_to_utf8(const FStrRaw &fstr, bool owns_data = false)
    {
        if (!fstr.data || fstr.num <= 0)
            return "";
        std::string utf8;
        int count = fstr.num - 1; // num includes null terminator
        for (int i = 0; i < count; i++)
        {
            char16_t c = fstr.data[i];
            if (c < 0x80)
                utf8 += static_cast<char>(c);
            else if (c < 0x800)
            {
                utf8 += static_cast<char>(0xC0 | (c >> 6));
                utf8 += static_cast<char>(0x80 | (c & 0x3F));
            }
            else
            {
                utf8 += static_cast<char>(0xE0 | (c >> 12));
                utf8 += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                utf8 += static_cast<char>(0x80 | (c & 0x3F));
            }
        }
        if (owns_data)
            free(fstr.data);
        return utf8;
    }

    // FText::FromString(FString&&) → FText  (static member function)
    // FText is 24 bytes > 16 → always indirect return via X8 on ARM64
    // X0 = FString* param, X8 = sret (FText buffer to write into)
    static inline void arm64_call_ftext_fromstring(void *out_ftext, void *fstring_param)
    {
        if (!symbols::FText_FromString)
            return;
        __asm__ __volatile__(
            "mov x8, %0\n\t" // sret buffer → X8
            "mov x0, %1\n\t" // FString&& param → X0
            "blr %2\n\t"     // call FText::FromString
            :
            : "r"(out_ftext), "r"(fstring_param), "r"(symbols::FText_FromString)
            : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8",
              "x9", "x10", "x11", "x12", "x13", "x14", "x15",
              "x16", "x17", "x30", "memory");
    }

    // ═══ Property read helper ═══════════════════════════════════════════════
    static sol::object read_property_value(sol::state_view lua, ue::UObject *obj,
                                           const rebuilder::RebuiltProperty &prop)
    {
        if (!obj)
            return sol::nil;
        // msync page probe — catches freed/dangling UObject pointers before dereference
        if (!ue::is_mapped_ptr(obj))
            return sol::nil;
        // Also probe the page at the target offset to catch cross-page overflows
        const uint8_t *target = reinterpret_cast<const uint8_t *>(obj) + prop.offset;
        if (!ue::is_mapped_ptr(target))
            return sol::nil;

        const uint8_t *base = reinterpret_cast<const uint8_t *>(obj);

        switch (prop.type)
        {
        case reflection::PropType::BoolProperty:
            return sol::make_object(lua, prop.read_bool(obj));

        case reflection::PropType::ByteProperty:
            return sol::make_object(lua, static_cast<int>(base[prop.offset]));

        case reflection::PropType::Int8Property:
            return sol::make_object(lua, static_cast<int>(*reinterpret_cast<const int8_t *>(base + prop.offset)));

        case reflection::PropType::Int16Property:
            return sol::make_object(lua, static_cast<int>(*reinterpret_cast<const int16_t *>(base + prop.offset)));

        case reflection::PropType::UInt16Property:
            return sol::make_object(lua, static_cast<int>(*reinterpret_cast<const uint16_t *>(base + prop.offset)));

        case reflection::PropType::IntProperty:
            return sol::make_object(lua, *reinterpret_cast<const int32_t *>(base + prop.offset));

        case reflection::PropType::UInt32Property:
            return sol::make_object(lua, static_cast<double>(*reinterpret_cast<const uint32_t *>(base + prop.offset)));

        case reflection::PropType::Int64Property:
            return sol::make_object(lua, static_cast<double>(*reinterpret_cast<const int64_t *>(base + prop.offset)));

        case reflection::PropType::UInt64Property:
            return sol::make_object(lua, static_cast<double>(*reinterpret_cast<const uint64_t *>(base + prop.offset)));

        case reflection::PropType::FloatProperty:
            return sol::make_object(lua, *reinterpret_cast<const float *>(base + prop.offset));

        case reflection::PropType::DoubleProperty:
            return sol::make_object(lua, *reinterpret_cast<const double *>(base + prop.offset));

        case reflection::PropType::NameProperty:
        {
            // FName is at offset — resolve it via the FNamePool
            int32_t fname_index = *reinterpret_cast<const int32_t *>(base + prop.offset);
            std::string name = reflection::fname_to_string(fname_index);
            return sol::make_object(lua, name);
        }

        case reflection::PropType::StrProperty:
        {
            // FString: { TCHAR* Data; int32 ArrayNum; int32 ArrayMax; }
            struct FString
            {
                char16_t *data;
                int32_t num;
                int32_t max;
            };
            const FString *fstr = reinterpret_cast<const FString *>(base + prop.offset);
            if (fstr->data && fstr->num > 0 && ue::is_mapped_ptr(fstr->data))
            {
                // Convert UTF-16 to UTF-8
                std::string utf8;
                for (int i = 0; i < fstr->num - 1; i++)
                {
                    char16_t c = fstr->data[i];
                    if (c < 0x80)
                    {
                        utf8 += static_cast<char>(c);
                    }
                    else if (c < 0x800)
                    {
                        utf8 += static_cast<char>(0xC0 | (c >> 6));
                        utf8 += static_cast<char>(0x80 | (c & 0x3F));
                    }
                    else
                    {
                        utf8 += static_cast<char>(0xE0 | (c >> 12));
                        utf8 += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                        utf8 += static_cast<char>(0x80 | (c & 0x3F));
                    }
                }
                return sol::make_object(lua, utf8);
            }
            return sol::make_object(lua, std::string(""));
        }

        case reflection::PropType::TextProperty:
        {
            // FText is 0x18 (24) bytes. Read via UKismetTextLibrary::Conv_TextToString.
            const void *ftext_ptr = base + prop.offset;
            std::string utf8 = ftext_to_string_via_kismet(ftext_ptr);
            return sol::make_object(lua, utf8);
        }

        case reflection::PropType::ObjectProperty:
        case reflection::PropType::WeakObjectProperty:
        case reflection::PropType::SoftObjectProperty:
        case reflection::PropType::LazyObjectProperty:
        case reflection::PropType::InterfaceProperty:
        {
            ue::UObject *ptr = *reinterpret_cast<ue::UObject *const *>(base + prop.offset);
            // is_valid_ptr: range check (strips MTE top byte)
            // is_mapped_ptr: msync to confirm the page is actually mapped
            // is_live_in_guobjectarray: validates through GUObjectArray so that MTE-tagged
            //   garbage pointers (e.g. UnlockData=0x8000000b300044) are rejected.
            //   is_mapped_ptr alone is insufficient — the Linux kernel strips the MTE top
            //   byte in syscalls (TBI), so msync(0x8000000b300000) succeeds if the untagged
            //   address is mapped. The GUObjectArray lookup strips tags before comparing,
            //   so a stale/garbage tagged pointer will fail the InternalIndex→slot check.
            if (!ptr)
                return sol::nil;
            if (!ue::is_valid_ptr(ptr))
            {
                logger::log_warn("OBJPROP", "'%s' raw=0x%llx failed is_valid_ptr", prop.name.c_str(), (unsigned long long)(uintptr_t)ptr);
                return sol::nil;
            }
            if (!is_live_in_guobjectarray_diag(ptr, prop.name.c_str()))
                return sol::nil;
            // Wrap as LuaUObject
            LuaUObject wrapped;
            wrapped.ptr = ptr;
            return sol::make_object(lua, wrapped);
        }

        case reflection::PropType::ClassProperty:
        {
            ue::UClass *cls = *reinterpret_cast<ue::UClass *const *>(base + prop.offset);
            if (!cls || !ue::is_valid_ptr(cls))
                return sol::nil;
            LuaUObject wrapped;
            wrapped.ptr = reinterpret_cast<ue::UObject *>(cls);
            return sol::make_object(lua, wrapped);
        }

        case reflection::PropType::StructProperty:
        {
            // Return as LuaUStruct with typed field access via reflection
            // Try to read inner UStruct from the FStructProperty.
            // We attempt this if prop.raw passes at least is_valid_ptr, since
            // is_mapped_ptr (msync) can give false negatives on some UE5 memory pages.
            ue::UStruct *inner_struct = nullptr;
            if (prop.raw && ue::is_valid_ptr(prop.raw))
            {
                ue::UStruct *candidate = ue::read_field<ue::UStruct *>(prop.raw, ue::fprop::STRUCT_INNER_STRUCT_OFF());
                if (candidate && ue::is_mapped_ptr(candidate))
                {
                    inner_struct = candidate;
                }
                // Auto-detect: if inner_struct not found at configured offset, scan nearby
                if (!inner_struct)
                {
                    static bool struct_scan_logged = false;
                    for (uint32_t scan_off = 0x68; scan_off <= 0x88; scan_off += 8)
                    {
                        if (scan_off == ue::fprop::STRUCT_INNER_STRUCT_OFF())
                            continue;
                        void *scan_candidate = ue::read_field<void *>(prop.raw, scan_off);
                        if (!scan_candidate || !ue::is_mapped_ptr(scan_candidate))
                            continue;
                        // Check if it looks like a UStruct: has a valid FName at UObject name offset
                        // UStruct inherits from UObject, so it has ObjIndex, ClassPrivate, etc.
                        // Try reading the FFieldClass of ChildProperties chain
                        // Simpler: check if it has a valid NamePrivate FName
                        void *obj_class = ue::read_field<void *>(scan_candidate, ue::uobj::CLASS_PRIVATE);
                        if (obj_class && ue::is_mapped_ptr(obj_class))
                        {
                            inner_struct = reinterpret_cast<ue::UStruct *>(scan_candidate);
                            if (!struct_scan_logged)
                            {
                                logger::log_warn("STRUCT", "Auto-detected StructProperty '%s': inner UStruct at +0x%x (was 0x%x)",
                                                 prop.name.c_str(), scan_off, ue::fprop::STRUCT_INNER_STRUCT_OFF());
                                ue::fprop::STRUCT_INNER_STRUCT_OFF() = scan_off;
                                struct_scan_logged = true;
                            }
                            break;
                        }
                    }
                }
            }
            if (inner_struct)
            {
                lua_ustruct::LuaUStruct s;
                s.data = const_cast<uint8_t *>(base + prop.offset);
                s.ustruct = inner_struct;
                s.size = prop.element_size;
                s.owns_data = false; // Points into live UObject memory
                return sol::make_object(lua, s);
            }
            // Fallback if inner struct not resolved
            logger::log_warn("STRUCT", "StructProperty '%s': inner UStruct not found (raw=%p valid=%d), returning lightuserdata",
                             prop.name.c_str(), prop.raw, prop.raw ? (int)ue::is_valid_ptr(prop.raw) : -1);
            return sol::make_object(lua, sol::lightuserdata_value(const_cast<uint8_t *>(base + prop.offset)));
        }

        case reflection::PropType::ArrayProperty:
        {
            // Return as proper LuaTArray usertype with typed element access
            ue::FProperty *inner_prop = nullptr;
            if (prop.raw && ue::is_valid_ptr(prop.raw))
            {
                inner_prop = ue::read_field<ue::FProperty *>(prop.raw, ue::fprop::ARRAY_INNER_OFF());
            }

            // DIAGNOSTIC: if inner_prop is null, scan for it
            if (!inner_prop && prop.raw && ue::is_valid_ptr(prop.raw))
            {
                logger::log_warn("TARRAY", "ArrayProperty inner_prop is NULL at ARRAY_INNER_OFF=0x%x for prop '%s'",
                                 ue::fprop::ARRAY_INNER_OFF(), prop.name.c_str());
                // Scan nearby offsets to find a valid FProperty/FField pointer
                for (uint32_t scan_off = 0x58; scan_off <= 0x98; scan_off += 8)
                {
                    void *candidate = ue::read_field<void *>(prop.raw, scan_off);
                    if (candidate && ue::is_mapped_ptr(candidate))
                    {
                        // Check if it looks like an FField (has a valid FFieldClass pointer at +0x08)
                        void *field_cls = ue::read_field<void *>(candidate, ue::ffield::CLASS_PRIVATE_OFF());
                        if (field_cls && ue::is_mapped_ptr(field_cls))
                        {
                            // Try to read the class name
                            int32_t cls_name_idx = ue::read_field<int32_t>(field_cls, 0);
                            std::string cls_name = reflection::fname_to_string(cls_name_idx);
                            if (!cls_name.empty() && cls_name.find("Property") != std::string::npos)
                            {
                                logger::log_warn("TARRAY", "  FOUND inner_prop at +0x%x: %p class='%s' — USING THIS",
                                                 scan_off, candidate, cls_name.c_str());
                                inner_prop = reinterpret_cast<ue::FProperty *>(candidate);
                                // Auto-correct the offset for future calls
                                ue::fprop::ARRAY_INNER_OFF() = scan_off;
                                break;
                            }
                            if (!cls_name.empty())
                            {
                                logger::log_info("TARRAY", "  +0x%x: %p cls='%s' (not a property)", scan_off, candidate, cls_name.c_str());
                            }
                        }
                    }
                }
                if (!inner_prop)
                {
                    logger::log_warn("TARRAY", "  Could not find inner_prop by scanning — TArray will be non-indexable");
                }
            }

            // Validate inner_prop is actually a mapped pointer (not just non-null)
            // Bad pointers like 0x1000000008 pass null checks but crash on dereference
            if (inner_prop && !ue::is_mapped_ptr(inner_prop))
            {
                logger::log_warn("TARRAY", "ArrayProperty '%s': inner_prop=%p fails is_mapped_ptr — treating as NULL",
                                 prop.name.c_str(), inner_prop);
                inner_prop = nullptr;
            }
            lua_tarray::LuaTArray arr;
            arr.array_ptr = const_cast<uint8_t *>(base + prop.offset);
            arr.inner_prop = inner_prop;
            arr.element_size = inner_prop ? ue::fprop_get_element_size(inner_prop) : 0;
            return sol::make_object(lua, arr);
        }

        case reflection::PropType::EnumProperty:
        {
            // Read the underlying integer value
            if (prop.element_size == 1)
                return sol::make_object(lua, static_cast<int>(base[prop.offset]));
            if (prop.element_size == 2)
                return sol::make_object(lua, static_cast<int>(*reinterpret_cast<const int16_t *>(base + prop.offset)));
            if (prop.element_size == 4)
                return sol::make_object(lua, *reinterpret_cast<const int32_t *>(base + prop.offset));
            return sol::make_object(lua, static_cast<int>(base[prop.offset]));
        }

        case reflection::PropType::MapProperty:
        {
            // Return as LuaTMap with key/value FProperty dispatch
            ue::FProperty *key_prop = nullptr;
            ue::FProperty *val_prop = nullptr;
            if (prop.raw && ue::is_valid_ptr(prop.raw))
            {
                key_prop = ue::read_field<ue::FProperty *>(prop.raw, ue::fprop::MAP_KEY_PROP_OFF());
                val_prop = ue::read_field<ue::FProperty *>(prop.raw, ue::fprop::MAP_VALUE_PROP_OFF());
            }
            // Validate pointers are actually mapped (not just non-null)
            if (key_prop && !ue::is_mapped_ptr(key_prop))
            {
                key_prop = nullptr;
            }
            if (val_prop && !ue::is_mapped_ptr(val_prop))
            {
                val_prop = nullptr;
            }

            // Auto-detect: if key or value prop is null/invalid, scan the FMapProperty
            // for valid FProperty* pointers. When the scanner runs (triggered by missing
            // key or val), we ALWAYS take the first two found as key+value to ensure
            // correct typing — the initial read at MAP_KEY_PROP_OFF may have accidentally
            // found the ValueProp instead of KeyProp if the offset was wrong.
            if (prop.raw && ue::is_valid_ptr(prop.raw) && (!key_prop || !val_prop))
            {
                static bool map_scan_logged = false;
                uintptr_t parent_addr = reinterpret_cast<uintptr_t>(prop.raw);
                std::vector<std::pair<uint32_t, ue::FProperty *>> found_props;
                for (uint32_t scan_off = 0x58; scan_off <= 0xB0; scan_off += 8)
                {
                    void *candidate = ue::read_field<void *>(prop.raw, scan_off);
                    if (!candidate || !ue::is_mapped_ptr(candidate))
                        continue;
                    void *field_cls = ue::read_field<void *>(candidate, ue::ffield::CLASS_PRIVATE_OFF());
                    if (!field_cls || !ue::is_mapped_ptr(field_cls))
                        continue;
                    int32_t cls_name_idx = ue::read_field<int32_t>(field_cls, 0);
                    std::string cls_name = reflection::fname_to_string(cls_name_idx);
                    if (!cls_name.empty() && cls_name.find("Property") != std::string::npos)
                    {
                        // Filter: only include CHILD properties whose Owner == this MapProperty.
                        // FField::Owner is at +0x10 (FFieldVariant, possibly tagged pointer).
                        // This filters out FProperty base linked-list pointers (PropertyLinkNext,
                        // NextRef, etc.) which point to sibling properties in the same class.
                        void *owner = ue::read_field<void *>(candidate, 0x10);
                        uintptr_t owner_addr = reinterpret_cast<uintptr_t>(owner);
                        if (owner_addr != parent_addr && (owner_addr & ~1ULL) != parent_addr)
                        {
                            if (!map_scan_logged)
                            {
                                logger::log_info("TMAP", "  MapProperty '%s': +0x%x: %p class='%s' SKIPPED (owner=%p != parent=%p)",
                                                 prop.name.c_str(), scan_off, candidate, cls_name.c_str(), owner, prop.raw);
                            }
                            continue; // Not a child property — skip
                        }
                        found_props.push_back({scan_off, reinterpret_cast<ue::FProperty *>(candidate)});
                        if (!map_scan_logged)
                        {
                            logger::log_info("TMAP", "  MapProperty '%s': found child FProperty at +0x%x: %p class='%s'",
                                             prop.name.c_str(), scan_off, candidate, cls_name.c_str());
                        }
                    }
                }
                // ALWAYS override both key and val from scanner results — the initial
                // read may have found the wrong property at the wrong offset
                if (found_props.size() >= 2)
                {
                    key_prop = found_props[0].second;
                    val_prop = found_props[1].second;
                    ue::fprop::MAP_KEY_PROP_OFF() = found_props[0].first;
                    ue::fprop::MAP_VALUE_PROP_OFF() = found_props[1].first;
                    if (!map_scan_logged)
                    {
                        logger::log_warn("TMAP", "Auto-corrected MAP offsets: key=0x%x val=0x%x",
                                         ue::fprop::MAP_KEY_PROP_OFF(), ue::fprop::MAP_VALUE_PROP_OFF());
                        map_scan_logged = true;
                    }
                }
                else if (found_props.size() == 1 && !key_prop)
                {
                    key_prop = found_props[0].second;
                }
            }

            lua_tarray::LuaTMap map;
            map.map_ptr = const_cast<uint8_t *>(base + prop.offset);
            map.key_prop = key_prop;
            map.value_prop = val_prop;
            map.key_size = key_prop ? ue::fprop_get_element_size(key_prop) : 0;
            map.value_size = val_prop ? ue::fprop_get_element_size(val_prop) : 0;

            // Read FScriptMapLayout directly from the FMapProperty.
            // Layout is stored right after the LAST FProperty* pointer in the struct.
            // Compute layout_off dynamically based on the actual val_prop offset.
            uint32_t layout_off = ue::fprop::MAP_VALUE_PROP_OFF() + 8;
            int32_t ml_value_offset = ue::read_field<int32_t>(prop.raw, layout_off + 0);  // MapLayout.ValueOffset
            int32_t ml_entry_stride = ue::read_field<int32_t>(prop.raw, layout_off + 12); // MapLayout.SetLayout.Size

            // Use engine-computed layout if it looks valid, else fall back to manual calculation
            if (ml_entry_stride > 0 && ml_entry_stride <= 4096 &&
                ml_value_offset >= 0 && ml_value_offset < ml_entry_stride)
            {
                map.entry_stride = ml_entry_stride;
                map.key_offset = 0;
                map.value_offset = ml_value_offset;
                logger::log_info("TMAP", "Using engine MapLayout: stride=%d key_off=0 val_off=%d (key_sz=%d val_sz=%d)",
                                 ml_entry_stride, ml_value_offset, map.key_size, map.value_size);
            }
            else
            {
                // Fallback: manual computation
                map.entry_stride = ((map.key_size + map.value_size + 8) + 7) & ~7;
                map.key_offset = 0;
                map.value_offset = map.entry_stride - map.value_size - 8;
                if (map.value_offset < map.key_size)
                    map.value_offset = map.key_size;
                logger::log_warn("TMAP", "MapLayout invalid (stride=%d val_off=%d), using manual: stride=%d val_off=%d",
                                 ml_entry_stride, ml_value_offset, map.entry_stride, map.value_offset);
            }

            return sol::make_object(lua, map);
        }

        default:
            // Raw memory access for unknown types — return as lightuserdata
            return sol::make_object(lua, sol::lightuserdata_value(const_cast<uint8_t *>(base + prop.offset)));
        }
    }

    // ═══ Property write helper ══════════════════════════════════════════════
    static bool write_property_value(ue::UObject *obj, const rebuilder::RebuiltProperty &prop,
                                     const sol::object &value)
    {
        if (!obj)
            return false;
        // msync page probe — catches freed/dangling UObject pointers before write
        if (!ue::is_mapped_ptr(obj))
            return false;
        // Probe target offset page too
        const uint8_t *target = reinterpret_cast<const uint8_t *>(obj) + prop.offset;
        if (!ue::is_mapped_ptr(target))
            return false;

        uint8_t *base = reinterpret_cast<uint8_t *>(obj);

        switch (prop.type)
        {
        case reflection::PropType::BoolProperty:
        {
            bool val = value.as<bool>();
            prop.write_bool(obj, val);
            return true;
        }

        case reflection::PropType::ByteProperty:
        case reflection::PropType::Int8Property:
        {
            int val = value.as<int>();
            base[prop.offset] = static_cast<uint8_t>(val);
            return true;
        }

        case reflection::PropType::Int16Property:
        case reflection::PropType::UInt16Property:
        {
            int val = value.as<int>();
            *reinterpret_cast<int16_t *>(base + prop.offset) = static_cast<int16_t>(val);
            return true;
        }

        case reflection::PropType::IntProperty:
        {
            int32_t val = value.as<int32_t>();
            *reinterpret_cast<int32_t *>(base + prop.offset) = val;
            return true;
        }

        case reflection::PropType::UInt32Property:
        {
            uint32_t val = static_cast<uint32_t>(value.as<double>());
            *reinterpret_cast<uint32_t *>(base + prop.offset) = val;
            return true;
        }

        case reflection::PropType::Int64Property:
        {
            int64_t val = static_cast<int64_t>(value.as<double>());
            *reinterpret_cast<int64_t *>(base + prop.offset) = val;
            return true;
        }

        case reflection::PropType::UInt64Property:
        {
            uint64_t val = static_cast<uint64_t>(value.as<double>());
            *reinterpret_cast<uint64_t *>(base + prop.offset) = val;
            return true;
        }

        case reflection::PropType::FloatProperty:
        {
            float val = value.as<float>();
            *reinterpret_cast<float *>(base + prop.offset) = val;
            return true;
        }

        case reflection::PropType::DoubleProperty:
        {
            double val = value.as<double>();
            *reinterpret_cast<double *>(base + prop.offset) = val;
            return true;
        }

        case reflection::PropType::ObjectProperty:
        case reflection::PropType::WeakObjectProperty:
        case reflection::PropType::ClassProperty:
        {
            if (value.is<LuaUObject>())
            {
                LuaUObject &wrapped = value.as<LuaUObject &>();
                // Guard: validate that the object is still alive before writing.
                // Writing a pointer to a GC-pending or destroyed object corrupts the
                // UE5 GC graph and causes infinite recursion in the destructor walker
                // → SIGSEGV on GameThread. Use the same validity check as Get().
                if (wrapped.ptr)
                {
                    if (!ue::is_mapped_ptr(wrapped.ptr) || !ue::is_valid_uobject(wrapped.ptr))
                    {
                        logger::log_warn("UOBJ", "Set('%s'): refusing to write invalid/GC-pending UObject %p — would corrupt GC graph",
                                         prop.name.c_str(), (void *)wrapped.ptr);
                        return false;
                    }
                    // Also check RF_BeginDestroyed / RF_FinishDestroyed flags.
                    // When GC marks an object for destruction it sets these flags before
                    // calling BeginDestroy(). Any reference written after this point
                    // will point to a dead object in the next GC sweep.
                    int32_t flags = ue::uobj_get_flags(wrapped.ptr);
                    constexpr int32_t RF_BeginDestroyed = 0x00008000;
                    constexpr int32_t RF_FinishDestroyed = 0x00010000;
                    if (flags & (RF_BeginDestroyed | RF_FinishDestroyed))
                    {
                        logger::log_warn("UOBJ", "Set('%s'): refusing to write RF_BeginDestroyed/RF_FinishDestroyed UObject %p (flags=0x%X) — GC is destroying it",
                                         prop.name.c_str(), (void *)wrapped.ptr, (unsigned)flags);
                        return false;
                    }
                }
                *reinterpret_cast<ue::UObject **>(base + prop.offset) = wrapped.ptr;
                return true;
            }
            else if (value.get_type() == sol::type::lightuserdata)
            {
                void *ptr = value.as<void *>();
                *reinterpret_cast<void **>(base + prop.offset) = ptr;
                return true;
            }
            else if (value == sol::nil)
            {
                *reinterpret_cast<void **>(base + prop.offset) = nullptr;
                return true;
            }
            return false;
        }

        case reflection::PropType::StructProperty:
        {
            // Accept LuaUStruct — memcpy into the property's memory
            if (value.is<lua_ustruct::LuaUStruct>())
            {
                const lua_ustruct::LuaUStruct &src = value.as<const lua_ustruct::LuaUStruct &>();
                if (src.data && src.size > 0)
                {
                    int32_t copy_size = (src.size < prop.element_size) ? src.size : prop.element_size;
                    std::memcpy(base + prop.offset, src.data, copy_size);
                }
                return true;
            }
            // Accept Lua table — fill fields via reflection
            if (value.get_type() == sol::type::table)
            {
                ue::UStruct *inner_struct = nullptr;
                if (prop.raw && ue::is_valid_ptr(prop.raw))
                {
                    ue::UStruct *candidate = ue::read_field<ue::UStruct *>(prop.raw, ue::fprop::STRUCT_INNER_STRUCT_OFF());
                    if (candidate && ue::is_mapped_ptr(candidate))
                        inner_struct = candidate;
                }
                if (inner_struct)
                {
                    lua_ustruct::fill_from_table(base + prop.offset, inner_struct, value.as<sol::table>());
                    return true;
                }
            }
            logger::log_warn("UOBJ", "Cannot write struct property '%s' — pass a UStruct or table", prop.name.c_str());
            return false;
        }

        case reflection::PropType::EnumProperty:
        {
            int val = value.as<int>();
            if (prop.element_size == 1)
                base[prop.offset] = static_cast<uint8_t>(val);
            else if (prop.element_size == 2)
                *reinterpret_cast<int16_t *>(base + prop.offset) = static_cast<int16_t>(val);
            else
                *reinterpret_cast<int32_t *>(base + prop.offset) = val;
            return true;
        }

        case reflection::PropType::NameProperty:
        {
            // FName = { int32 ComparisonIndex, int32 Number }
            uint8_t *ptr = base + prop.offset;
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
                logger::log_warn("UOBJ", "Cannot write FName '%s' — expected string or LuaFName", prop.name.c_str());
                return false;
            }
            return true;
        }

        case reflection::PropType::StrProperty:
        {
            uint8_t *ptr = base + prop.offset;
            std::string s;
            if (value.is<std::string>())
            {
                s = value.as<std::string>();
            }
            else if (value.is<lua_types::LuaFString>())
            {
                s = value.as<lua_types::LuaFString &>().to_string();
            }
            else
            {
                logger::log_warn("UOBJ", "Cannot write FString '%s' — expected string", prop.name.c_str());
                return false;
            }
            // Use shared utility — handles reuse of existing buffer or malloc()
            // (never new[] to avoid allocator mismatch with UE4's FMemory::Free)
            if (!lua_uobject::fstring_from_utf8(ptr, s))
            {
                logger::log_warn("UOBJ", "Failed to write FString '%s'", prop.name.c_str());
                return false;
            }
            return true;
        }

        case reflection::PropType::TextProperty:
        {
            // FText is 0x18 (24) bytes. Use Conv_StringToText via ProcessEvent
            // to ensure data is allocated by UE4's allocator (FMemory::Malloc),
            // avoiding heap corruption when UE4 later destroys the FText.
            std::string str;
            if (value.is<std::string>())
            {
                str = value.as<std::string>();
            }
            else if (value.is<lua_types::LuaFText>())
            {
                str = value.as<lua_types::LuaFText &>().to_string();
            }
            else if (value.is<lua_types::LuaFString>())
            {
                str = value.as<lua_types::LuaFString &>().to_string();
            }
            else
            {
                logger::log_warn("UOBJ", "Cannot write FText '%s' — expected string", prop.name.c_str());
                return false;
            }
            // Destroy old FText (decrement refcount)
            if (symbols::FText_Dtor)
            {
                symbols::FText_Dtor(base + prop.offset);
            }
            // Create new FText via ProcessEvent (UE4 allocator)
            if (!ftext_from_string_via_kismet(base + prop.offset, str))
            {
                // Fallback to arm64 asm if ProcessEvent approach fails
                if (symbols::FText_FromString)
                {
                    size_t wlen = str.size() + 1;
                    char16_t *wbuf = static_cast<char16_t *>(malloc(wlen * sizeof(char16_t)));
                    for (size_t i = 0; i < str.size(); i++)
                        wbuf[i] = static_cast<char16_t>(static_cast<unsigned char>(str[i]));
                    wbuf[str.size()] = 0;
                    struct
                    {
                        char16_t *data;
                        int32_t num;
                        int32_t max;
                    } tmp_fstr = {
                        wbuf, static_cast<int32_t>(wlen), static_cast<int32_t>(wlen)};
                    arm64_call_ftext_fromstring(base + prop.offset, &tmp_fstr);
                    // Don't free wbuf — ownership transferred to FText
                }
                else
                {
                    logger::log_warn("UOBJ", "Cannot write FText '%s' — no write method available", prop.name.c_str());
                    return false;
                }
            }
            logger::log_info("UOBJ", "Wrote FText '%s' = \"%s\"", prop.name.c_str(), str.c_str());
            return true;
        }

        default:
            logger::log_warn("UOBJ", "Cannot write property '%s' of unsupported type", prop.name.c_str());
            return false;
        }
    }

    // ═══ ProcessEvent call helper ═══════════════════════════════════════════
    static sol::object call_ufunction(sol::state_view lua, ue::UObject *obj,
                                      const std::string &func_name, sol::variadic_args va)
    {
        if (!obj || !symbols::ProcessEvent)
            return sol::nil;

        // Strong guard against stale/dangling LuaUObject wrappers.
        // Invalid-but-non-null UObject* causes native crashes inside ProcessEvent.
        if (!ue::is_mapped_ptr(obj) || !ue::is_valid_ptr(obj) || !ue::is_valid_uobject(obj))
        {
            logger::log_warn("CALL", "Skipping call on invalid UObject: %p::%s", obj, func_name.c_str());
            return sol::nil;
        }

        ue::UClass *cls = ue::uobj_get_class(obj);
        if (!cls)
            return sol::nil;

        std::string class_name = reflection::get_short_name(reinterpret_cast<const ue::UObject *>(cls));
        auto *rc = rebuilder::rebuild(class_name);
        if (!rc)
        {
            logger::log_warn("UOBJ", "Cannot call %s — class '%s' not rebuilt", func_name.c_str(), class_name.c_str());
            return sol::nil;
        }

        auto *rf = rc->find_function(func_name);
        if (!rf || !rf->raw)
        {
            logger::log_warn("UOBJ", "Function '%s' not found on class '%s'", func_name.c_str(), class_name.c_str());
            return sol::nil;
        }

        ue::UFunction *func = static_cast<ue::UFunction *>(rf->raw);
        if (!func || !ue::is_mapped_ptr(func) || !ue::is_valid_ptr(func))
        {
            logger::log_warn("CALL", "Skipping call %s::%s — invalid UFunction %p",
                             class_name.c_str(), func_name.c_str(), func);
            return sol::nil;
        }

        // Allocate params buffer
        uint16_t parms_size = ue::ufunc_get_parms_size(func);
        if (parms_size == 0)
        {
            // No params — just call (with crash guard)
            // Save FunctionFlags — ProcessEvent modifies them internally;
            // if we crash mid-call via siglongjmp, the flags stay corrupted
            // and ALL subsequent calls to this UFunction will also crash.
            uint32_t saved_flags = ue::ufunc_get_flags(func);
            g_in_call_ufunction = 1;
            int pe_crash_sig = sigsetjmp(g_call_ufunction_jmp, 1);
            if (pe_crash_sig != 0)
            {
                // Restore FunctionFlags even on crash — prevents cascade failures
                ue::write_field(func, ue::ufunc::FUNCTION_FLAGS_OFF(), saved_flags);
                logger::log_error("CALL", "ProcessEvent CRASHED (signal %d) calling %s::%s (no params) "
                                          "— recovered, returning nil",
                                  pe_crash_sig, class_name.c_str(), func_name.c_str());
                g_in_call_ufunction = 0;
                return sol::nil;
            }
            auto pe_fn = pe_hook::get_original();
            if (!pe_fn)
                pe_fn = symbols::ProcessEvent;
            pe_fn(obj, func, nullptr);
            ue::write_field(func, ue::ufunc::FUNCTION_FLAGS_OFF(), saved_flags);
            g_in_call_ufunction = 0;
            return sol::nil;
        }

        // ── Safety guard: detect async/latent functions that require a delegate ──
        // FScriptDelegate params that are left zeroed cause ProcessEvent to block
        // indefinitely (the native save/async code waits for a callback that never fires).
        // Count how many delegate-type CPF_Parm entries the function has vs how many
        // Lua args were provided.  If delegate params exceed supplied args, warn loudly.
        {
            int delegate_parms = 0;
            int total_parms = 0;
            for (const auto &pi : rf->params)
            {
                if (!(pi.flags & ue::CPF_Parm) || (pi.flags & ue::CPF_ReturnParm))
                    continue;
                total_parms++;
                if (pi.type == reflection::PropType::DelegateProperty ||
                    pi.type == reflection::PropType::MulticastDelegateProperty ||
                    pi.type == reflection::PropType::MulticastInlineDelegateProperty ||
                    pi.type == reflection::PropType::MulticastSparseDelegateProperty)
                {
                    delegate_parms++;
                }
            }
            int supplied_args = static_cast<int>(va.size());
            if (delegate_parms > 0 && supplied_args < total_parms)
            {
                logger::log_warn("CALL",
                                 "DANGER: '%s::%s' has %d delegate param(s) but only %d/%d args supplied — "
                                 "calling with zeroed delegate MAY BLOCK ProcessEvent forever. "
                                 "Pass a valid {Object=obj, FunctionName='name'} table or avoid calling this function.",
                                 class_name.c_str(), func_name.c_str(),
                                 delegate_parms, supplied_args, total_parms);
            }
        }

        // Allocate and zero the params buffer
        std::vector<uint8_t> params_buf(parms_size, 0);
        void *params = params_buf.data();

        // Track FString allocations to clean up after ProcessEvent
        std::vector<char16_t *> fstring_allocs;
        // Track FText param offsets to destroy after ProcessEvent
        std::vector<uint8_t *> ftext_params;

        // RAII scope for conversion buffer tracking — saves/restores on nesting
        lua_ustruct::ConvBufScope conv_scope;

        // Fill in parameters from variadic args — ONLY properties with CPF_Parm
        // (skip Blueprint local variables which are also stored as FProperties)
        // DIAGNOSTIC: log param details for Override functions
        if (func_name.find("Override") != std::string::npos)
        {
            logger::log_info("CALL", "Marshal params for %s::%s: %zu params, %d lua args",
                             class_name.c_str(), func_name.c_str(), rf->params.size(), (int)va.size());
            for (size_t pi_idx = 0; pi_idx < rf->params.size(); pi_idx++)
            {
                const auto &pi = rf->params[pi_idx];
                logger::log_info("CALL", "  param[%zu] '%s' type=%d offset=%d size=%d flags=0x%x",
                                 pi_idx, pi.name.c_str(), (int)pi.type, pi.offset, pi.element_size, pi.flags);
            }
        }
        int arg_idx = 0;
        for (const auto &pi : rf->params)
        {
            if (arg_idx >= static_cast<int>(va.size()))
                break;

            // Skip non-parameter properties (Blueprint locals, return values)
            if (!(pi.flags & ue::CPF_Parm) || (pi.flags & ue::CPF_ReturnParm))
                continue;

            if (!marshaled_range_valid(parms_size, pi, class_name, func_name, "CALL"))
            {
                for (auto *p : fstring_allocs)
                    delete[] p;
                return sol::nil;
            }

            sol::object arg = va[arg_idx];
            uint8_t *param_ptr = params_buf.data() + pi.offset;

            switch (pi.type)
            {
            case reflection::PropType::BoolProperty:
            {
                bool val = arg.as<bool>();
                if (pi.bool_byte_mask)
                {
                    if (val)
                        param_ptr[pi.bool_byte_offset] |= pi.bool_byte_mask;
                    else
                        param_ptr[pi.bool_byte_offset] &= ~pi.bool_byte_mask;
                }
                else
                {
                    *reinterpret_cast<bool *>(param_ptr) = val;
                }
                break;
            }
            case reflection::PropType::FloatProperty:
                *reinterpret_cast<float *>(param_ptr) = arg.as<float>();
                break;
            case reflection::PropType::DoubleProperty:
                *reinterpret_cast<double *>(param_ptr) = arg.as<double>();
                break;
            case reflection::PropType::IntProperty:
                *reinterpret_cast<int32_t *>(param_ptr) = arg.as<int32_t>();
                break;
            case reflection::PropType::UInt32Property:
                *reinterpret_cast<uint32_t *>(param_ptr) = static_cast<uint32_t>(arg.as<double>());
                break;
            case reflection::PropType::Int64Property:
                *reinterpret_cast<int64_t *>(param_ptr) = static_cast<int64_t>(arg.as<double>());
                break;
            case reflection::PropType::UInt64Property:
                *reinterpret_cast<uint64_t *>(param_ptr) = static_cast<uint64_t>(arg.as<double>());
                break;
            case reflection::PropType::Int16Property:
                *reinterpret_cast<int16_t *>(param_ptr) = static_cast<int16_t>(arg.as<int>());
                break;
            case reflection::PropType::UInt16Property:
                *reinterpret_cast<uint16_t *>(param_ptr) = static_cast<uint16_t>(arg.as<int>());
                break;
            case reflection::PropType::Int8Property:
                *reinterpret_cast<int8_t *>(param_ptr) = static_cast<int8_t>(arg.as<int>());
                break;
            case reflection::PropType::ByteProperty:
                *param_ptr = static_cast<uint8_t>(arg.as<int>());
                break;
            case reflection::PropType::NameProperty:
            {
                // FName = { int32 ComparisonIndex, int32 Number }
                if (arg.is<lua_types::LuaFName>())
                {
                    auto &fn = arg.as<lua_types::LuaFName &>();
                    *reinterpret_cast<int32_t *>(param_ptr) = fn.comparison_index;
                    *reinterpret_cast<int32_t *>(param_ptr + 4) = fn.number;
                }
                else if (arg.is<std::string>())
                {
                    std::string s = arg.as<std::string>();
                    int32_t idx = reflection::fname_string_to_index(s);
                    if (idx == 0 && symbols::FName_Init)
                    {
                        ue::FName fname;
                        std::u16string u16name(s.begin(), s.end());
                        symbols::FName_Init(&fname, u16name.c_str(), 0);
                        idx = fname.ComparisonIndex;
                    }
                    *reinterpret_cast<int32_t *>(param_ptr) = idx;
                    *reinterpret_cast<int32_t *>(param_ptr + 4) = 0;
                }
                else if (arg.is<int32_t>())
                {
                    *reinterpret_cast<int32_t *>(param_ptr) = arg.as<int32_t>();
                    *reinterpret_cast<int32_t *>(param_ptr + 4) = 0;
                }
                break;
            }
            case reflection::PropType::StrProperty:
            {
                // FString = { char16_t* Data; int32 Num; int32 Max; }
                struct FStr
                {
                    char16_t *data;
                    int32_t num;
                    int32_t max;
                };
                FStr *fstr = reinterpret_cast<FStr *>(param_ptr);
                if (arg.is<std::string>())
                {
                    std::string s = arg.as<std::string>();
                    // Convert UTF-8 to UTF-16 (ASCII subset)
                    size_t wlen = s.size() + 1; // +1 for null terminator
                    char16_t *wbuf = new char16_t[wlen];
                    fstring_allocs.push_back(wbuf);
                    for (size_t i = 0; i < s.size(); i++)
                    {
                        wbuf[i] = static_cast<char16_t>(static_cast<unsigned char>(s[i]));
                    }
                    wbuf[s.size()] = 0;
                    fstr->data = wbuf;
                    fstr->num = static_cast<int32_t>(wlen);
                    fstr->max = static_cast<int32_t>(wlen);
                }
                else if (arg.is<lua_types::LuaFString>())
                {
                    auto &fs = arg.as<lua_types::LuaFString &>();
                    std::string s = fs.to_string();
                    size_t wlen = s.size() + 1;
                    char16_t *wbuf = new char16_t[wlen];
                    fstring_allocs.push_back(wbuf);
                    for (size_t i = 0; i < s.size(); i++)
                    {
                        wbuf[i] = static_cast<char16_t>(static_cast<unsigned char>(s[i]));
                    }
                    wbuf[s.size()] = 0;
                    fstr->data = wbuf;
                    fstr->num = static_cast<int32_t>(wlen);
                    fstr->max = static_cast<int32_t>(wlen);
                }
                break;
            }
            case reflection::PropType::EnumProperty:
            {
                int val = arg.as<int>();
                if (pi.element_size == 1)
                    *param_ptr = static_cast<uint8_t>(val);
                else if (pi.element_size == 2)
                    *reinterpret_cast<int16_t *>(param_ptr) = static_cast<int16_t>(val);
                else
                    *reinterpret_cast<int32_t *>(param_ptr) = val;
                break;
            }
            case reflection::PropType::ObjectProperty:
            case reflection::PropType::ClassProperty:
            {
                if (arg.is<LuaUObject>())
                {
                    *reinterpret_cast<ue::UObject **>(param_ptr) = arg.as<LuaUObject &>().ptr;
                }
                else if (arg.get_type() == sol::type::lightuserdata)
                {
                    *reinterpret_cast<void **>(param_ptr) = arg.as<void *>();
                }
                break;
            }
            case reflection::PropType::StructProperty:
            {
                // Accept LuaUStruct — memcpy into params buffer
                if (arg.is<lua_ustruct::LuaUStruct>())
                {
                    const lua_ustruct::LuaUStruct &src = arg.as<const lua_ustruct::LuaUStruct &>();
                    if (src.data && src.size > 0)
                    {
                        int32_t copy_size = (src.size < pi.element_size) ? src.size : pi.element_size;
                        std::memcpy(param_ptr, src.data, copy_size);
                    }
                }
                // Accept Lua table — fill fields via reflection
                else if (arg.get_type() == sol::type::table)
                {
                    ue::UStruct *inner_struct = nullptr;
                    if (pi.raw && ue::is_valid_ptr(pi.raw))
                    {
                        ue::UStruct *candidate = ue::read_field<ue::UStruct *>(pi.raw, ue::fprop::STRUCT_INNER_STRUCT_OFF());
                        if (candidate && ue::is_mapped_ptr(candidate))
                            inner_struct = candidate;
                    }
                    if (inner_struct)
                    {
                        lua_ustruct::fill_from_table(param_ptr, inner_struct, arg.as<sol::table>());
                    }
                }

                // NOTE: TArray deep-copy removed — using malloc() for copies that
                // UE's frame destructor frees via FMemory::Free() causes heap corruption
                // (SIGABRT) when UE uses FMallocBinned2 instead of system malloc.
                // Flipper arm overrides are deferred 15s in Lua to avoid the original
                // frame-destructor-frees-live-data issue.
                break;
            }
            case reflection::PropType::TextProperty:
            {
                // FText = 0x18 (24) bytes in the params buffer
                // Use Conv_StringToText via ProcessEvent to ensure UE4 allocator
                std::string str;
                if (arg.is<std::string>())
                {
                    str = arg.as<std::string>();
                }
                else if (arg.is<lua_types::LuaFText>())
                {
                    str = arg.as<lua_types::LuaFText &>().to_string();
                }
                else if (arg.is<lua_types::LuaFString>())
                {
                    str = arg.as<lua_types::LuaFString &>().to_string();
                }
                if (!str.empty())
                {
                    logger::log_info("CALL", "DIAG: TextProperty arg='%s', param_ptr=%p, offset in params=%d",
                                     str.c_str(), param_ptr, (int)(param_ptr - params_buf.data()));
                    if (!ftext_from_string_via_kismet(param_ptr, str))
                    {
                        logger::log_info("CALL", "DIAG: kismet failed, trying arm64 fallback");
                        // Fallback to arm64 asm
                        if (symbols::FText_FromString && symbols::FText_Ctor)
                        {
                            symbols::FText_Ctor(param_ptr);
                            size_t wlen = str.size() + 1;
                            char16_t *wbuf = static_cast<char16_t *>(malloc(wlen * sizeof(char16_t)));
                            for (size_t i = 0; i < str.size(); i++)
                                wbuf[i] = static_cast<char16_t>(static_cast<unsigned char>(str[i]));
                            wbuf[str.size()] = 0;
                            struct
                            {
                                char16_t *data;
                                int32_t num;
                                int32_t max;
                            } tmp_fstr = {
                                wbuf, static_cast<int32_t>(wlen), static_cast<int32_t>(wlen)};
                            // NOTE: Do NOT call FText_Dtor before FromString —
                            // Ctor just initialized, Dtor would destroy it.
                            // FromString writes via X8 sret (overwrites completely).
                            arm64_call_ftext_fromstring(param_ptr, &tmp_fstr);
                        }
                    }
                    ftext_params.push_back(param_ptr);
                }
                break;
            }
            case reflection::PropType::DelegateProperty:
            {
                // FScriptDelegate layout: { FWeakObjectPtr Object(8b), FName FunctionName(8b) }
                // Accept a Lua table {Object=obj, FunctionName="EventName"} to fill the delegate.
                // If nil / not a table, leave zeroed (null delegate) — safe for ignored callbacks.
                if (arg.get_type() == sol::type::table)
                {
                    sol::table tbl = arg.as<sol::table>();
                    // Object pointer (FWeakObjectPtr stores the UObject index + serial, but
                    // for simplicity write the raw pointer into the first 8 bytes if given)
                    sol::optional<LuaUObject> delegate_obj = tbl.get<sol::optional<LuaUObject>>("Object");
                    if (delegate_obj)
                    {
                        *reinterpret_cast<ue::UObject **>(param_ptr) = delegate_obj->ptr;
                    }
                    // FunctionName (FName = ComparisonIndex + Number, 8 bytes at offset 8)
                    sol::optional<std::string> fname_str = tbl.get<sol::optional<std::string>>("FunctionName");
                    if (fname_str)
                    {
                        int32_t idx = reflection::fname_string_to_index(*fname_str);
                        *reinterpret_cast<int32_t *>(param_ptr + 8) = idx;
                        *reinterpret_cast<int32_t *>(param_ptr + 12) = 0;
                    }
                }
                // else: leave zeroed — null delegate is safe for fire-and-forget calls
                break;
            }
            case reflection::PropType::SoftObjectProperty:
            case reflection::PropType::SoftClassProperty:
            {
                // TSoftObjectPtr / TSoftClassPtr — auto-convert UObject* → FSoftObjectPath
                if (arg.is<LuaUObject>())
                {
                    ue::UObject *obj = arg.as<LuaUObject &>().ptr;
                    if (obj && ue::is_valid_ptr(obj) && pi.element_size >= 40 && symbols::FName_Init)
                    {
                        std::memset(param_ptr, 0, pi.element_size);

                        std::string full_path = reflection::get_full_name(obj);
                        if (!full_path.empty())
                        {
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

                            // TPersistentObjectPtr layout:
                            //   +0x00: TWeakObjectPtr {ObjectIndex(4), SerialNumber(4)}
                            //   +0x08: FTopLevelAssetPath.PackageName FName (8)
                            //   +0x10: FTopLevelAssetPath.AssetName FName (8)
                            //   +0x18: FString SubPathString (16) — zeroed

                            // Fill TWeakObjectPtr so TSoftObjectPtr::Get() can resolve
                            int32_t obj_index = ue::uobj_get_internal_index(obj);
                            int32_t obj_serial = reflection::get_object_serial_number(obj_index);
                            std::memcpy(param_ptr + 0, &obj_index, 4);
                            std::memcpy(param_ptr + 4, &obj_serial, 4);

                            std::memcpy(param_ptr + 8, &pkg_fname, 8);
                            std::memcpy(param_ptr + 16, &asset_fname, 8);

                            logger::log_info("CALL", "Auto-converted UObject* → FSoftObjectPath for '%s': %s",
                                             pi.name.c_str(), full_path.c_str());
                        }
                    }
                }
                else if (arg.is<lua_ustruct::LuaUStruct>())
                {
                    const lua_ustruct::LuaUStruct &src = arg.as<const lua_ustruct::LuaUStruct &>();
                    if (src.data && src.size > 0)
                    {
                        int32_t copy_size = (src.size < pi.element_size) ? src.size : pi.element_size;
                        std::memcpy(param_ptr, src.data, copy_size);
                    }
                }
                // nil → zeroed (already zero from memset)
                break;
            }
            case reflection::PropType::MulticastDelegateProperty:
            case reflection::PropType::MulticastInlineDelegateProperty:
            case reflection::PropType::MulticastSparseDelegateProperty:
                // Multicast delegates are output/event channels — not meaningful as
                // input params.  Leave zeroed (already initialised by memset above).
                break;
            default:
                // For unknown types, try raw pointer
                if (arg.get_type() == sol::type::lightuserdata)
                {
                    *reinterpret_cast<void **>(param_ptr) = arg.as<void *>();
                }
                break;
            }

            arg_idx++;
        }

        // Save FunctionFlags — ProcessEvent modifies them internally;
        // if we crash mid-call via siglongjmp, the flags stay corrupted
        // and ALL subsequent calls to this UFunction will also crash.
        uint32_t saved_func_flags = ue::ufunc_get_flags(func);

        // Save DestructorLink — we null it before ProcessEvent to prevent
        // DestroyValue from freeing shallow-copied TArray Data pointers
        // inside struct params (e.g., PFXTableCustomizationArmSkinData).
        void *saved_dtor_link = ue::read_field<void *>(func, ue::ustruct::DESTRUCTOR_LINK_OFF());

        // GUObjectArray round-trip: skip if object was freed/reallocated since captured.
        // Uses untagged (MTE-excluded) pointer read to detect stale MTE-tagged ptrs.
        if (!is_live_in_guobjectarray(obj))
        {
            logger::log_warn("CALL", "Skipping %s::%s on %p — not in GUObjectArray (stale/freed)",
                             class_name.c_str(), func_name.c_str(), obj);
            for (auto *p : fstring_allocs)
                delete[] p;
            conv_scope.cleanup();
            return sol::nil;
        }

        // sigsetjmp crash guard — catches SIGSEGV inside ProcessEvent
        g_in_call_ufunction = 1;
        g_call_ufunction_fault_addr = 0;
        int pe_crash_sig = sigsetjmp(g_call_ufunction_jmp, 1);
        if (pe_crash_sig != 0)
        {
            // Restore FunctionFlags — ProcessEvent modifies them internally;
            // if left corrupted all future calls to this UFunction will also crash.
            ue::write_field(func, ue::ufunc::FUNCTION_FLAGS_OFF(), saved_func_flags);
            // Restore DestructorLink
            ue::write_field(func, ue::ustruct::DESTRUCTOR_LINK_OFF(), saved_dtor_link);
            // Log crash details — every occurrence, no suppression, no state tracking
            uintptr_t fault = g_call_ufunction_fault_addr;
            uintptr_t obj_raw = reinterpret_cast<uintptr_t>(obj) & 0x00FFFFFFFFFFFFFFULL;
            intptr_t fault_offset = static_cast<intptr_t>(fault) - static_cast<intptr_t>(obj_raw);
            logger::log_error("CALL", "ProcessEvent CRASHED (signal %d, fault_addr=0x%lx) calling %s::%s on obj=%p — recovered, returning nil",
                              pe_crash_sig, (unsigned long)fault,
                              class_name.c_str(), func_name.c_str(), obj);
            if (fault_offset >= 0 && fault_offset < 0x10000)
                logger::log_error("CALL", "  FAULT at obj+0x%lx — bad field inside UObject at that offset",
                                  (unsigned long)fault_offset);
            else
                logger::log_error("CALL", "  FAULT at 0x%lx — dangling sub-object ptr (obj_raw=0x%lx)",
                                  (unsigned long)fault, (unsigned long)obj_raw);
            g_in_call_ufunction = 0;
            for (auto *p : fstring_allocs)
                delete[] p;
            conv_scope.cleanup();
            return sol::nil;
        }
        auto pe_fn = pe_hook::get_original();
        if (!pe_fn)
            pe_fn = symbols::ProcessEvent;
        // Null DestructorLink ONLY for Blueprint (non-native) functions.
        // Native functions handle their own param cleanup in their Func pointer body;
        // ProcessEvent does not walk DestructorLink for them after the call returns.
        // For Blueprint functions, DestructorLink IS walked by ProcessEvent to call
        // DestroyValue on any constructed params. We null it to prevent UE's allocator
        // from freeing shallow-copied TArray Data pointers from our source objects.
        //
        // CRITICAL: Do NOT null DestructorLink for native functions — it is a shared
        // field on the UFunction object. The game thread can call the same UFunction
        // concurrently; writing a null here creates a race that causes param destruction
        // to silently skip for the game's own calls.
        static constexpr uint32_t FUNC_Native = 0x00000400;
        const bool is_native_func = (saved_func_flags & FUNC_Native) != 0;
        if (!is_native_func)
        {
            ue::write_field(func, ue::ustruct::DESTRUCTOR_LINK_OFF(), static_cast<void *>(nullptr));
        }

        // DIAGNOSTIC: hex-dump params for Override functions to verify FSoftObjectPath encoding
        if (func_name.find("Override") != std::string::npos && parms_size >= 16)
        {
            char hex[512];
            int hlen = 0;
            size_t dump_sz = params_buf.size() < 128 ? params_buf.size() : 128;
            for (size_t b = 0; b < dump_sz && hlen < 500; b++)
            {
                hlen += snprintf(hex + hlen, 512 - hlen, "%02x", params_buf[b]);
                if ((b & 7) == 7 && b + 1 < dump_sz)
                    hlen += snprintf(hex + hlen, 512 - hlen, " ");
            }
            logger::log_info("CALL", "PE params[%zu] for %s::%s: %s",
                             params_buf.size(), class_name.c_str(), func_name.c_str(), hex);
        }

        pe_fn(obj, func, params);

        // Restore DestructorLink only if we nulled it (non-native functions only).
        if (!is_native_func)
        {
            ue::write_field(func, ue::ustruct::DESTRUCTOR_LINK_OFF(), saved_dtor_link);
        }

        ue::write_field(func, ue::ufunc::FUNCTION_FLAGS_OFF(), saved_func_flags);

        // Clean up TArray conversion buffers (must happen AFTER PE, BEFORE params_buf dies)
        conv_scope.cleanup();
        // NOTE: crash guard (g_in_call_ufunction) stays ON through return extraction
        // so that crashes in return value reading are also caught by siglongjmp.

        // Clean up FString allocations
        for (auto *p : fstring_allocs)
        {
            delete[] p;
        }

        // NOTE: Do NOT call FText_Dtor on FText params after ProcessEvent.
        // ProcessEvent may share/copy the ITextData pointer into the target object.
        // If we destroy our temporary FText, the shared ITextData refcount drops,
        // potentially freeing data the target object still references — causing a
        // use-after-free crash on the next render frame.
        // Accept a small leak of one ITextData per Call() with FText params.
        // The params_buf raw bytes will be freed when the vector goes out of scope,
        // but the ITextData itself persists (shared with the target object).

        // Extract return value (still under crash guard — cleared at exit)
        // RAII guard ensures g_in_call_ufunction is cleared on ALL return paths
        struct CallGuardCleaner
        {
            volatile int &f;
            ~CallGuardCleaner() { f = 0; }
        };
        CallGuardCleaner _call_guard{g_in_call_ufunction};

        if (rf->return_prop && rf->return_prop->raw &&
            marshaled_range_valid(parms_size, *rf->return_prop, class_name, func_name, "CALLRET"))
        {
            const uint8_t *ret_ptr = params_buf.data() + rf->return_offset;

            switch (rf->return_prop->type)
            {
            case reflection::PropType::BoolProperty:
            {
                if (rf->return_prop->bool_byte_mask)
                {
                    return sol::make_object(lua, (ret_ptr[rf->return_prop->bool_byte_offset] & rf->return_prop->bool_byte_mask) != 0);
                }
                return sol::make_object(lua, *reinterpret_cast<const bool *>(ret_ptr));
            }
            case reflection::PropType::FloatProperty:
                return sol::make_object(lua, *reinterpret_cast<const float *>(ret_ptr));
            case reflection::PropType::DoubleProperty:
                return sol::make_object(lua, *reinterpret_cast<const double *>(ret_ptr));
            case reflection::PropType::IntProperty:
                return sol::make_object(lua, *reinterpret_cast<const int32_t *>(ret_ptr));
            case reflection::PropType::UInt32Property:
                return sol::make_object(lua, static_cast<double>(*reinterpret_cast<const uint32_t *>(ret_ptr)));
            case reflection::PropType::Int64Property:
                return sol::make_object(lua, static_cast<double>(*reinterpret_cast<const int64_t *>(ret_ptr)));
            case reflection::PropType::UInt64Property:
                return sol::make_object(lua, static_cast<double>(*reinterpret_cast<const uint64_t *>(ret_ptr)));
            case reflection::PropType::Int16Property:
                return sol::make_object(lua, static_cast<int>(*reinterpret_cast<const int16_t *>(ret_ptr)));
            case reflection::PropType::UInt16Property:
                return sol::make_object(lua, static_cast<int>(*reinterpret_cast<const uint16_t *>(ret_ptr)));
            case reflection::PropType::Int8Property:
                return sol::make_object(lua, static_cast<int>(*reinterpret_cast<const int8_t *>(ret_ptr)));
            case reflection::PropType::ByteProperty:
                return sol::make_object(lua, static_cast<int>(*reinterpret_cast<const uint8_t *>(ret_ptr)));
            case reflection::PropType::ObjectProperty:
            {
                ue::UObject *ret_obj = *reinterpret_cast<ue::UObject *const *>(ret_ptr);
                if (ret_obj && ue::is_valid_ptr(ret_obj))
                {
                    LuaUObject wrapped;
                    wrapped.ptr = ret_obj;
                    return sol::make_object(lua, wrapped);
                }
                return sol::nil;
            }
            case reflection::PropType::StrProperty:
            {
                struct FStr
                {
                    char16_t *data;
                    int32_t num;
                    int32_t max;
                };
                const FStr *fstr = reinterpret_cast<const FStr *>(ret_ptr);
                if (fstr->data && fstr->num > 0 && ue::is_mapped_ptr(fstr->data))
                {
                    std::string utf8;
                    for (int i = 0; i < fstr->num - 1; i++)
                    {
                        char16_t c = fstr->data[i];
                        if (c < 0x80)
                            utf8 += static_cast<char>(c);
                        else if (c < 0x800)
                        {
                            utf8 += static_cast<char>(0xC0 | (c >> 6));
                            utf8 += static_cast<char>(0x80 | (c & 0x3F));
                        }
                        else
                        {
                            utf8 += static_cast<char>(0xE0 | (c >> 12));
                            utf8 += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                            utf8 += static_cast<char>(0x80 | (c & 0x3F));
                        }
                    }
                    return sol::make_object(lua, utf8);
                }
                return sol::make_object(lua, std::string(""));
            }
            case reflection::PropType::TextProperty:
            {
                // Read FText return value via UKismetTextLibrary::Conv_TextToString
                std::string utf8 = ftext_to_string_via_kismet(ret_ptr);
                return sol::make_object(lua, utf8);
            }
            case reflection::PropType::StructProperty:
            {
                // Return struct as owning LuaUStruct copy (params buffer is about to be freed)
                ue::UStruct *inner_struct = ue::read_field<ue::UStruct *>(rf->return_prop->raw, ue::fprop::STRUCT_INNER_STRUCT_OFF());
                if (inner_struct)
                {
                    return sol::make_object(lua, lua_ustruct::copy(ret_ptr, inner_struct, rf->return_prop->element_size));
                }
                return sol::make_object(lua, sol::lightuserdata_value(const_cast<uint8_t *>(ret_ptr)));
            }
            case reflection::PropType::NameProperty:
            {
                int32_t fname_idx = *reinterpret_cast<const int32_t *>(ret_ptr);
                return sol::make_object(lua, reflection::fname_to_string(fname_idx));
            }
            case reflection::PropType::EnumProperty:
            {
                int32_t elem_sz = rf->return_prop->element_size;
                if (elem_sz == 1)
                    return sol::make_object(lua, static_cast<int>(ret_ptr[0]));
                if (elem_sz == 2)
                    return sol::make_object(lua, static_cast<int>(*reinterpret_cast<const int16_t *>(ret_ptr)));
                return sol::make_object(lua, *reinterpret_cast<const int32_t *>(ret_ptr));
            }
            case reflection::PropType::SoftObjectProperty:
            case reflection::PropType::SoftClassProperty:
            {
                // TSoftObjectPtr layout:
                //   +0x00: TWeakObjectPtr (8 bytes — ObjectIndex + SerialNumber)
                //   +0x08: FTopLevelAssetPath.PackageName FName (8)
                //   +0x10: FTopLevelAssetPath.AssetName FName (8)
                //   +0x18: FString SubPathString (16)
                if (rf->return_prop->element_size >= 40)
                {
                    // DIAG: dump return bytes
                    {
                        char hexbuf[256] = {};
                        int hexlen = 0;
                        int dump_sz = (rf->return_prop->element_size < 48) ? rf->return_prop->element_size : 48;
                        for (int i = 0; i < dump_sz && hexlen < 240; i++)
                            hexlen += snprintf(hexbuf + hexlen, 256 - hexlen, "%02x", ret_ptr[i]);
                        logger::log_info("CALL", "SoftObj return[%d] hex: %s", rf->return_prop->element_size, hexbuf);
                    }
                    int32_t pkg_idx = *reinterpret_cast<const int32_t *>(ret_ptr + 8);
                    int32_t asset_idx = *reinterpret_cast<const int32_t *>(ret_ptr + 16);
                    if (pkg_idx != 0 || asset_idx != 0)
                    {
                        std::string pkg_name = reflection::fname_to_string(pkg_idx);
                        std::string asset_name = reflection::fname_to_string(asset_idx);
                        if (!pkg_name.empty() && !asset_name.empty())
                        {
                            return sol::make_object(lua, pkg_name + "." + asset_name);
                        }
                        else if (!pkg_name.empty())
                        {
                            return sol::make_object(lua, pkg_name);
                        }
                    }
                }
                return sol::nil;
            }
            case reflection::PropType::ClassProperty:
            case reflection::PropType::WeakObjectProperty:
            case reflection::PropType::LazyObjectProperty:
            case reflection::PropType::InterfaceProperty:
            {
                ue::UObject *ret_obj = *reinterpret_cast<ue::UObject *const *>(ret_ptr);
                if (ret_obj && ue::is_valid_ptr(ret_obj))
                {
                    LuaUObject wrapped;
                    wrapped.ptr = ret_obj;
                    return sol::make_object(lua, wrapped);
                }
                return sol::nil;
            }
            case reflection::PropType::ArrayProperty:
            {
                // IMPORTANT: Do not return LuaTArray backed by params_buf memory.
                // params_buf is stack-local and freed when Call() returns.
                // Return a copied Lua table snapshot instead.
                struct RawTArrayRet
                {
                    void *data;
                    int32_t num;
                    int32_t max;
                };

                const RawTArrayRet *arr = reinterpret_cast<const RawTArrayRet *>(ret_ptr);
                sol::table out = lua.create_table();
                if (!arr || !arr->data || arr->num <= 0)
                    return out;

                if (arr->num < 0 || arr->max < 0 || arr->num > arr->max || arr->num > 8192)
                {
                    logger::log_warn("CALLRET", "%s::%s returned suspicious TArray (num=%d max=%d)",
                                     class_name.c_str(), func_name.c_str(), arr->num, arr->max);
                    return out;
                }

                ue::FProperty *inner_prop = nullptr;
                if (rf->return_prop->raw && ue::is_valid_ptr(rf->return_prop->raw))
                {
                    inner_prop = ue::read_field<ue::FProperty *>(rf->return_prop->raw, ue::fprop::ARRAY_INNER_OFF());
                    if (inner_prop && !ue::is_mapped_ptr(inner_prop))
                        inner_prop = nullptr;
                }
                if (!inner_prop)
                {
                    logger::log_warn("CALLRET", "%s::%s returned ArrayProperty without valid inner prop",
                                     class_name.c_str(), func_name.c_str());
                    return out;
                }

                int32_t elem_sz = ue::fprop_get_element_size(inner_prop);
                if (elem_sz <= 0 || elem_sz > 4096 || !ue::is_mapped_ptr(arr->data))
                {
                    logger::log_warn("CALLRET", "%s::%s returned invalid TArray data (elem_sz=%d data=%p)",
                                     class_name.c_str(), func_name.c_str(), elem_sz, arr->data);
                    return out;
                }

                const uint8_t *arr_base = reinterpret_cast<const uint8_t *>(arr->data);
                for (int32_t i = 0; i < arr->num; ++i)
                {
                    const uint8_t *elem = arr_base + static_cast<size_t>(i) * static_cast<size_t>(elem_sz);
                    if (!ue::is_mapped_ptr(elem))
                    {
                        logger::log_warn("CALLRET", "%s::%s TArray element %d not mapped — truncating snapshot",
                                         class_name.c_str(), func_name.c_str(), i);
                        break;
                    }
                    out[i + 1] = lua_tarray::read_element(lua, elem, inner_prop);
                }
                return out;
            }
            default:
                return sol::make_object(lua, sol::lightuserdata_value(const_cast<uint8_t *>(ret_ptr)));
            }
        }

        return sol::nil;
        // _call_guard destructor clears g_in_call_ufunction = 0
    }

    // ═══ call_ufunction_bg ══════════════════════════════════════════════════
    // Same parameter serialization as call_ufunction, but dispatches
    // ProcessEvent to the game thread via throttled queue (max 10/tick).
    // This avoids game-thread deadlocks when called from Lua init context,
    // while still running ProcessEvent safely on the game thread.
    // Returns true if dispatch succeeded, false on error.
    // Fire-and-forget: no return value extraction.
    static bool call_ufunction_bg(sol::state_view lua, ue::UObject *obj,
                                  const std::string &func_name, sol::variadic_args va)
    {
        if (!obj || !symbols::ProcessEvent)
            return false;

        if (!ue::is_mapped_ptr(obj) || !ue::is_valid_ptr(obj) || !ue::is_valid_uobject(obj))
        {
            logger::log_warn("CALLBG", "Skipping background call on invalid UObject: %p::%s", obj, func_name.c_str());
            return false;
        }

        ue::UClass *cls = ue::uobj_get_class(obj);
        if (!cls)
            return false;

        std::string class_name = reflection::get_short_name(reinterpret_cast<const ue::UObject *>(cls));
        auto *rc = rebuilder::rebuild(class_name);
        if (!rc)
        {
            logger::log_warn("CALLBG", "Cannot call %s — class '%s' not rebuilt", func_name.c_str(), class_name.c_str());
            return false;
        }

        auto *rf = rc->find_function(func_name);
        if (!rf || !rf->raw)
        {
            logger::log_warn("CALLBG", "Function '%s' not found on class '%s'", func_name.c_str(), class_name.c_str());
            return false;
        }

        ue::UFunction *func = static_cast<ue::UFunction *>(rf->raw);
        if (!func || !ue::is_mapped_ptr(func) || !ue::is_valid_ptr(func))
        {
            logger::log_warn("CALLBG", "Skipping background call %s::%s — invalid UFunction %p",
                             class_name.c_str(), func_name.c_str(), func);
            return false;
        }
        uint16_t parms_size = ue::ufunc_get_parms_size(func);

        if (parms_size == 0)
        {
            // No params — enqueue bare call
            GtCallItem item;
            item.obj = obj;
            item.func = func;
            item.class_name = class_name;
            item.func_name = func_name;
            enqueue_gt_call(std::move(item));
            return true;
        }

        // ── Build params buffer on game thread (reflection access is safe here) ──
        // Pad to at least 32 bytes — ProcessEvent's Blueprint VM may read/write
        // past ParmsSize for alignment or return values.
        size_t buf_size = std::max(static_cast<size_t>(parms_size), static_cast<size_t>(32));
        auto params_buf = std::make_shared<std::vector<uint8_t>>(buf_size, 0);
        auto fstring_allocs = std::make_shared<std::vector<char16_t *>>();

        int arg_idx = 0;
        for (const auto &pi : rf->params)
        {
            if (arg_idx >= static_cast<int>(va.size()))
                break;
            if (!(pi.flags & ue::CPF_Parm) || (pi.flags & ue::CPF_ReturnParm))
                continue;

            if (!marshaled_range_valid(parms_size, pi, class_name, func_name, "CALLBG"))
            {
                for (auto *p : *fstring_allocs)
                    delete[] p;
                return false;
            }

            sol::object arg = va[arg_idx];
            uint8_t *param_ptr = params_buf->data() + pi.offset;

            switch (pi.type)
            {
            case reflection::PropType::BoolProperty:
            {
                bool val = arg.as<bool>();
                if (pi.bool_byte_mask)
                {
                    if (val)
                        param_ptr[pi.bool_byte_offset] |= pi.bool_byte_mask;
                    else
                        param_ptr[pi.bool_byte_offset] &= ~pi.bool_byte_mask;
                }
                else
                {
                    *reinterpret_cast<bool *>(param_ptr) = val;
                }
                break;
            }
            case reflection::PropType::FloatProperty:
                *reinterpret_cast<float *>(param_ptr) = arg.as<float>();
                break;
            case reflection::PropType::DoubleProperty:
                *reinterpret_cast<double *>(param_ptr) = arg.as<double>();
                break;
            case reflection::PropType::IntProperty:
                *reinterpret_cast<int32_t *>(param_ptr) = arg.as<int32_t>();
                break;
            case reflection::PropType::UInt32Property:
                *reinterpret_cast<uint32_t *>(param_ptr) = static_cast<uint32_t>(arg.as<double>());
                break;
            case reflection::PropType::Int64Property:
                *reinterpret_cast<int64_t *>(param_ptr) = static_cast<int64_t>(arg.as<double>());
                break;
            case reflection::PropType::UInt64Property:
                *reinterpret_cast<uint64_t *>(param_ptr) = static_cast<uint64_t>(arg.as<double>());
                break;
            case reflection::PropType::Int16Property:
                *reinterpret_cast<int16_t *>(param_ptr) = static_cast<int16_t>(arg.as<int>());
                break;
            case reflection::PropType::UInt16Property:
                *reinterpret_cast<uint16_t *>(param_ptr) = static_cast<uint16_t>(arg.as<int>());
                break;
            case reflection::PropType::Int8Property:
                *reinterpret_cast<int8_t *>(param_ptr) = static_cast<int8_t>(arg.as<int>());
                break;
            case reflection::PropType::ByteProperty:
                *param_ptr = static_cast<uint8_t>(arg.as<int>());
                break;
            case reflection::PropType::NameProperty:
            {
                if (arg.is<lua_types::LuaFName>())
                {
                    auto &fn = arg.as<lua_types::LuaFName &>();
                    *reinterpret_cast<int32_t *>(param_ptr) = fn.comparison_index;
                    *reinterpret_cast<int32_t *>(param_ptr + 4) = fn.number;
                }
                else if (arg.is<std::string>())
                {
                    std::string s = arg.as<std::string>();
                    int32_t idx = reflection::fname_string_to_index(s);
                    if (idx == 0 && symbols::FName_Init)
                    {
                        ue::FName fname;
                        std::u16string u16name(s.begin(), s.end());
                        symbols::FName_Init(&fname, u16name.c_str(), 0);
                        idx = fname.ComparisonIndex;
                    }
                    *reinterpret_cast<int32_t *>(param_ptr) = idx;
                    *reinterpret_cast<int32_t *>(param_ptr + 4) = 0;
                }
                else if (arg.is<int32_t>())
                {
                    *reinterpret_cast<int32_t *>(param_ptr) = arg.as<int32_t>();
                    *reinterpret_cast<int32_t *>(param_ptr + 4) = 0;
                }
                break;
            }
            case reflection::PropType::StrProperty:
            {
                struct FStr
                {
                    char16_t *data;
                    int32_t num;
                    int32_t max;
                };
                FStr *fstr = reinterpret_cast<FStr *>(param_ptr);
                if (arg.is<std::string>())
                {
                    std::string s = arg.as<std::string>();
                    size_t wlen = s.size() + 1;
                    char16_t *wbuf = new char16_t[wlen];
                    fstring_allocs->push_back(wbuf);
                    for (size_t i = 0; i < s.size(); i++)
                        wbuf[i] = static_cast<char16_t>(static_cast<unsigned char>(s[i]));
                    wbuf[s.size()] = 0;
                    fstr->data = wbuf;
                    fstr->num = static_cast<int32_t>(wlen);
                    fstr->max = static_cast<int32_t>(wlen);
                }
                break;
            }
            case reflection::PropType::EnumProperty:
            {
                int val = arg.as<int>();
                if (pi.element_size == 1)
                    *param_ptr = static_cast<uint8_t>(val);
                else if (pi.element_size == 2)
                    *reinterpret_cast<int16_t *>(param_ptr) = static_cast<int16_t>(val);
                else
                    *reinterpret_cast<int32_t *>(param_ptr) = val;
                break;
            }
            case reflection::PropType::ObjectProperty:
            case reflection::PropType::ClassProperty:
            {
                if (arg.is<LuaUObject>())
                    *reinterpret_cast<ue::UObject **>(param_ptr) = arg.as<LuaUObject &>().ptr;
                else if (arg.get_type() == sol::type::lightuserdata)
                    *reinterpret_cast<void **>(param_ptr) = arg.as<void *>();
                break;
            }
            case reflection::PropType::StructProperty:
            {
                if (arg.is<lua_ustruct::LuaUStruct>())
                {
                    const lua_ustruct::LuaUStruct &src = arg.as<const lua_ustruct::LuaUStruct &>();
                    if (src.data && src.size > 0)
                    {
                        int32_t copy_size = (src.size < pi.element_size) ? src.size : pi.element_size;
                        std::memcpy(param_ptr, src.data, copy_size);
                    }
                }
                else if (arg.get_type() == sol::type::table)
                {
                    ue::UStruct *inner_struct = nullptr;
                    if (pi.raw && ue::is_valid_ptr(pi.raw))
                    {
                        ue::UStruct *candidate = ue::read_field<ue::UStruct *>(pi.raw, ue::fprop::STRUCT_INNER_STRUCT_OFF());
                        if (candidate && ue::is_mapped_ptr(candidate))
                            inner_struct = candidate;
                    }
                    if (inner_struct)
                        lua_ustruct::fill_from_table(param_ptr, inner_struct, arg.as<sol::table>());
                }
                break;
            }
            case reflection::PropType::SoftObjectProperty:
            case reflection::PropType::SoftClassProperty:
            {
                if (arg.is<LuaUObject>())
                {
                    ue::UObject *obj_arg = arg.as<LuaUObject &>().ptr;
                    if (obj_arg && ue::is_valid_ptr(obj_arg) && pi.element_size >= 40 && symbols::FName_Init)
                    {
                        std::memset(param_ptr, 0, pi.element_size);
                        std::string full_path = reflection::get_full_name(obj_arg);
                        if (!full_path.empty())
                        {
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
                            // TPersistentObjectPtr: WeakPtr(8) + PackageName(8) + AssetName(8) + SubPath(16)
                            int32_t obj_index_bg = ue::uobj_get_internal_index(obj_arg);
                            int32_t obj_serial_bg = reflection::get_object_serial_number(obj_index_bg);
                            std::memcpy(param_ptr + 0, &obj_index_bg, 4);
                            std::memcpy(param_ptr + 4, &obj_serial_bg, 4);
                            std::memcpy(param_ptr + 8, &pkg_fname, 8);
                            std::memcpy(param_ptr + 16, &asset_fname, 8);
                            logger::log_info("CALLBG", "Auto-converted UObject* → FSoftObjectPath for '%s': %s",
                                             pi.name.c_str(), full_path.c_str());
                        }
                    }
                }
                else if (arg.is<lua_ustruct::LuaUStruct>())
                {
                    const lua_ustruct::LuaUStruct &src = arg.as<const lua_ustruct::LuaUStruct &>();
                    if (src.data && src.size > 0)
                    {
                        int32_t copy_size = (src.size < pi.element_size) ? src.size : pi.element_size;
                        std::memcpy(param_ptr, src.data, copy_size);
                    }
                }
                break;
            }
            default:
                if (arg.get_type() == sol::type::lightuserdata)
                    *reinterpret_cast<void **>(param_ptr) = arg.as<void *>();
                break;
            }

            arg_idx++;
        }

        // ── Enqueue to game-thread queue (throttled drain) ──
        // Params are in shared_ptr so they survive until game thread processes them.
        // Game-thread queue drain processes max 10 items per PE tick = safe.
        GtCallItem item;
        item.obj = obj;
        item.func = func;
        item.params = params_buf;
        item.fstring_allocs = fstring_allocs;
        item.class_name = class_name;
        item.func_name = func_name;
        // Record offsets of TArray params for post-PE zeroing
        for (const auto &pi : rf->params)
        {
            if (pi.type == reflection::PropType::ArrayProperty &&
                (pi.flags & ue::CPF_Parm) && !(pi.flags & ue::CPF_ReturnParm))
                item.array_param_offsets.push_back(static_cast<size_t>(pi.offset));
        }
        enqueue_gt_call(std::move(item));

        return true;
    }

    // ═══ call_ufunction_bg_raw ══════════════════════════════════════════════
    // Like CallBg but takes a raw params buffer (lightuserdata + size).
    // This allows Lua to construct exact param bytes for complex types
    // like GameplayTag (FName struct) that the reflection serializer
    // can't easily build from Lua values.
    // The raw buffer is COPIED — caller can free/reuse it immediately.
    static bool call_ufunction_bg_raw(ue::UObject *obj,
                                      const std::string &func_name,
                                      void *raw_params, int params_len)
    {
        if (!obj || !symbols::ProcessEvent || !raw_params || params_len <= 0)
            return false;

        if (!ue::is_mapped_ptr(obj) || !ue::is_valid_ptr(obj) || !ue::is_valid_uobject(obj))
        {
            logger::log_warn("CALLBG", "RawCallBg skipped on invalid UObject: %p::%s", obj, func_name.c_str());
            return false;
        }

        ue::UClass *cls = ue::uobj_get_class(obj);
        if (!cls)
            return false;

        std::string class_name = reflection::get_short_name(
            reinterpret_cast<const ue::UObject *>(cls));
        auto *rc = rebuilder::rebuild(class_name);
        if (!rc)
        {
            logger::log_warn("CALLBG", "RawCall: class '%s' not rebuilt",
                             class_name.c_str());
            return false;
        }

        auto *rf = rc->find_function(func_name);
        if (!rf || !rf->raw)
        {
            logger::log_warn("CALLBG", "RawCall: '%s' not found on '%s'",
                             func_name.c_str(), class_name.c_str());
            return false;
        }

        ue::UFunction *func = static_cast<ue::UFunction *>(rf->raw);
        if (!func || !ue::is_mapped_ptr(func) || !ue::is_valid_ptr(func))
        {
            logger::log_warn("CALLBG", "RawCall: invalid UFunction %p for %s::%s",
                             func, class_name.c_str(), func_name.c_str());
            return false;
        }

        // Allocate buffer sized to UFunction's ParmsSize (not just caller's buffer)
        // ProcessEvent may read/write beyond the caller's params for return values,
        // padding, or out-params. Using ParmsSize ensures no buffer overrun.
        uint16_t parms_size = ue::ufunc_get_parms_size(func);
        size_t alloc_size = std::max(static_cast<size_t>(parms_size),
                                     static_cast<size_t>(params_len));
        if (alloc_size < 32)
            alloc_size = 32; // minimum safety margin

        auto params_buf = std::make_shared<std::vector<uint8_t>>(alloc_size, 0);
        std::memcpy(params_buf->data(), raw_params,
                    std::min(static_cast<size_t>(params_len), alloc_size));

        GtCallItem item;
        item.obj = obj;
        item.func = func;
        item.params = params_buf;
        item.class_name = class_name;
        item.func_name = func_name;
        enqueue_gt_call(std::move(item));
        return true;
    }

    // ═══ call_ufunction_raw_sync ════════════════════════════════════════════
    // Synchronous raw-params ProcessEvent call — for use when already on game thread.
    // Lua constructs the exact param bytes; this function resolves the UFunction
    // and calls ProcessEvent immediately (no queuing).
    static bool call_ufunction_raw_sync(ue::UObject *obj,
                                        const std::string &func_name,
                                        void *raw_params, int params_len)
    {
        if (!obj || !symbols::ProcessEvent || !raw_params || params_len <= 0)
            return false;

        if (!ue::is_mapped_ptr(obj) || !ue::is_valid_ptr(obj) || !ue::is_valid_uobject(obj))
        {
            logger::log_warn("CALLRAW", "RawCall skipped on invalid UObject: %p::%s", obj, func_name.c_str());
            return false;
        }

        ue::UClass *cls = ue::uobj_get_class(obj);
        if (!cls)
            return false;

        std::string class_name = reflection::get_short_name(
            reinterpret_cast<const ue::UObject *>(cls));
        auto *rc = rebuilder::rebuild(class_name);
        if (!rc)
        {
            logger::log_warn("UOBJ", "CallRaw: class '%s' not rebuilt", class_name.c_str());
            return false;
        }

        auto *rf = rc->find_function(func_name);
        if (!rf || !rf->raw)
        {
            logger::log_warn("UOBJ", "CallRaw: '%s' not found on '%s'",
                             func_name.c_str(), class_name.c_str());
            return false;
        }

        ue::UFunction *func = static_cast<ue::UFunction *>(rf->raw);
        if (!func || !ue::is_mapped_ptr(func) || !ue::is_valid_ptr(func))
        {
            logger::log_warn("CALLRAW", "RawCall: invalid UFunction %p for %s::%s",
                             func, class_name.c_str(), func_name.c_str());
            return false;
        }

        // Allocate buffer sized to max(ParmsSize, params_len, 32)
        uint16_t parms_size = ue::ufunc_get_parms_size(func);
        size_t alloc_size = std::max({static_cast<size_t>(parms_size),
                                      static_cast<size_t>(params_len),
                                      static_cast<size_t>(32)});

        std::vector<uint8_t> params_buf(alloc_size, 0);
        std::memcpy(params_buf.data(), raw_params,
                    std::min(static_cast<size_t>(params_len), alloc_size));

        // Save FunctionFlags — ProcessEvent modifies them internally
        uint32_t saved_func_flags = ue::ufunc_get_flags(func);
        // Save DestructorLink — null it to prevent DestroyValue on our params
        void *saved_dtor_link = ue::read_field<void *>(func, ue::ustruct::DESTRUCTOR_LINK_OFF());

        // Crash guard — if ProcessEvent crashes, recover instead of killing game
        g_in_call_ufunction = 1;
        int pe_crash_sig = sigsetjmp(g_call_ufunction_jmp, 1);
        if (pe_crash_sig != 0)
        {
            ue::write_field(func, ue::ufunc::FUNCTION_FLAGS_OFF(), saved_func_flags);
            ue::write_field(func, ue::ustruct::DESTRUCTOR_LINK_OFF(), saved_dtor_link);
            uintptr_t fault = g_call_ufunction_fault_addr;
            logger::log_error("CALLRAW", "ProcessEvent CRASHED (signal %d, fault_addr=0x%lx) calling %s::%s on %p — recovered, returning false",
                              pe_crash_sig, (unsigned long)fault,
                              class_name.c_str(), func_name.c_str(), obj);
            g_in_call_ufunction = 0;
            // No ConvBufScope needed — call_ufunction_bg uses raw params, no Lua conversion
            return false;
        }
        auto pe_fn = pe_hook::get_original();
        if (!pe_fn)
            pe_fn = symbols::ProcessEvent;

        // Null DestructorLink to prevent DestroyValue on shallow-copied params
        ue::write_field(func, ue::ustruct::DESTRUCTOR_LINK_OFF(), static_cast<void *>(nullptr));
        pe_fn(obj, func, params_buf.data());
        ue::write_field(func, ue::ustruct::DESTRUCTOR_LINK_OFF(), saved_dtor_link);
        ue::write_field(func, ue::ufunc::FUNCTION_FLAGS_OFF(), saved_func_flags);
        // No ConvBufScope needed — call_ufunction_bg uses raw params, no Lua conversion
        g_in_call_ufunction = 0;
        return true;
    }

    // ═══ Register LuaUObject userdata type ══════════════════════════════════
    void register_types(sol::state &lua)
    {
        lua.new_usertype<LuaUObject>("UObject", sol::no_constructor,

                                     // ── __tostring: print(obj) shows ClassName(Name @0xADDR) ──
                                     sol::meta_function::to_string, [](const LuaUObject &self) -> std::string
                                     {
            if (!self.ptr) return "UObject(nil)";
            if (!ue::is_valid_ptr(self.ptr)) {
                char buf[64];
                snprintf(buf, sizeof(buf), "UObject(dead @0x%lX)", (unsigned long)(uintptr_t)self.ptr);
                return std::string(buf);
            }
            std::string name = reflection::get_short_name(self.ptr);
            std::string cls_name;
            ue::UClass* cls = ue::uobj_get_class(self.ptr);
            if (cls && ue::is_valid_ptr(cls)) {
                cls_name = reflection::get_short_name(reinterpret_cast<const ue::UObject*>(cls));
            }
            char buf[128];
            if (cls_name.empty()) {
                snprintf(buf, sizeof(buf), "UObject(%s @0x%lX)", name.c_str(), (unsigned long)(uintptr_t)self.ptr);
            } else {
                snprintf(buf, sizeof(buf), "%s(%s @0x%lX)", cls_name.c_str(), name.c_str(), (unsigned long)(uintptr_t)self.ptr);
            }
            return std::string(buf); },

                                     // ── Core methods ──
                                     "IsValid", [](const LuaUObject &self) -> bool
                                     { return self.ptr && ue::is_valid_ptr(self.ptr) && ue::is_valid_uobject(self.ptr); },

                                     "GetName", [](const LuaUObject &self) -> std::string
                                     {
            if (!self.ptr) return "";
            return reflection::get_short_name(self.ptr); },

                                     "GetFullName", [](const LuaUObject &self) -> std::string
                                     {
            if (!self.ptr) return "";
            return reflection::get_full_name(self.ptr); },

                                     "GetClass", [](sol::this_state ts, const LuaUObject &self) -> sol::object
                                     {
            if (!self.ptr) return sol::nil;
            ue::UClass* cls = ue::uobj_get_class(self.ptr);
            if (!cls) return sol::nil;
            LuaUObject wrapped;
            wrapped.ptr = reinterpret_cast<ue::UObject*>(cls);
            return sol::make_object(ts, wrapped); },

                                     "GetClassName", [](const LuaUObject &self) -> std::string
                                     {
            if (!self.ptr) return "";
            ue::UClass* cls = ue::uobj_get_class(self.ptr);
            if (!cls) return "";
            return reflection::get_short_name(reinterpret_cast<const ue::UObject*>(cls)); },

                                     "GetAddress", [](sol::this_state ts, const LuaUObject &self) -> sol::object
                                     {
            sol::state_view lua(ts);
            return sol::make_object(lua, sol::lightuserdata_value(self.ptr)); },

                                     "ToHex", [](const LuaUObject &self) -> std::string
                                     {
            char buf[32];
            snprintf(buf, sizeof(buf), "0x%016lX", (unsigned long)reinterpret_cast<uintptr_t>(self.ptr));
            return std::string(buf); },

                                     "GetOuter", [](sol::this_state ts, const LuaUObject &self) -> sol::object
                                     {
            sol::state_view lua(ts);
            if (!self.ptr) return sol::nil;
            ue::UObject* outer = ue::uobj_get_outer(self.ptr);
            if (!outer) return sol::nil;
            LuaUObject wrapped;
            wrapped.ptr = outer;
            return sol::make_object(lua, wrapped); },

                                     "IsA", [](const LuaUObject &self, sol::object class_arg) -> bool
                                     {
            if (!self.ptr) return false;
            ue::UClass* target_cls = nullptr;
            if (class_arg.is<std::string>()) {
                target_cls = reflection::find_class_ptr(class_arg.as<std::string>());
            } else if (class_arg.is<LuaUObject>()) {
                target_cls = reinterpret_cast<ue::UClass*>(class_arg.as<LuaUObject&>().ptr);
            }
            if (!target_cls) return false;
            ue::UClass* cls = ue::uobj_get_class(self.ptr);
            while (cls) {
                if (cls == target_cls) return true;
                cls = reinterpret_cast<ue::UClass*>(ue::ustruct_get_super(reinterpret_cast<ue::UStruct*>(cls)));
            }
            return false; },

                                     // ── Cast: validate IsA then return same object ──
                                     // Usage: local pc = obj:Cast("PlayerController")
                                     "Cast", [](sol::this_state ts, const LuaUObject &self, const std::string &class_name) -> sol::object
                                     {
            sol::state_view lua(ts);
            if (!self.ptr) return sol::nil;

            ue::UClass* target_cls = reflection::find_class_ptr(class_name);
            if (!target_cls) target_cls = reflection::find_class_ptr(class_name + "_C");
            if (!target_cls) {
                logger::log_warn("CAST", "Cast: class '%s' not found", class_name.c_str());
                return sol::nil;
            }
            ue::UClass* cls = ue::uobj_get_class(self.ptr);
            while (cls) {
                if (cls == target_cls) {
                    // IsA succeeded — return same wrapper
                    return sol::make_object(lua, self);
                }
                cls = reinterpret_cast<ue::UClass*>(ue::ustruct_get_super(reinterpret_cast<ue::UStruct*>(cls)));
            }
            logger::log_warn("CAST", "Cast failed: object is not a '%s'", class_name.c_str());
            return sol::nil; },

                                     "GetFName", [](const LuaUObject &self) -> std::string
                                     {
            if (!self.ptr) return "";
            return reflection::get_short_name(self.ptr); },

                                     // ── Property access ──
                                     "Get", [](sol::this_state ts, LuaUObject &self, const std::string &prop_name) -> sol::object
                                     {
            sol::state_view lua(ts);
            if (!self.ptr) return sol::nil;

            ue::UClass* cls = ue::uobj_get_class(self.ptr);
            if (!cls) return sol::nil;

            std::string class_name = reflection::get_short_name(reinterpret_cast<const ue::UObject*>(cls));
            auto* rc = rebuilder::rebuild(class_name);
            if (!rc) return sol::nil;

            auto* rp = rc->find_property(prop_name);
            if (!rp) return sol::nil;

            return read_property_value(lua, self.ptr, *rp); },

                                     "Set", [](LuaUObject &self, const std::string &prop_name, sol::object value) -> bool
                                     {
            if (!self.ptr) return false;

            ue::UClass* cls = ue::uobj_get_class(self.ptr);
            if (!cls) return false;

            std::string class_name = reflection::get_short_name(reinterpret_cast<const ue::UObject*>(cls));
            auto* rc = rebuilder::rebuild(class_name);
            if (!rc) return false;

            auto* rp = rc->find_property(prop_name);
            if (!rp) {
                logger::log_warn("UOBJ", "Property '%s' not found on '%s'", prop_name.c_str(), class_name.c_str());
                return false;
            }

            return write_property_value(self.ptr, *rp, value); },

                                     // GetProp / SetProp aliases
                                     "GetProp", [](sol::this_state ts, LuaUObject &self, const std::string &prop_name) -> sol::object
                                     {
            sol::state_view lua(ts);
            if (!self.ptr) return sol::nil;

            ue::UClass* cls = ue::uobj_get_class(self.ptr);
            if (!cls) return sol::nil;

            std::string class_name = reflection::get_short_name(reinterpret_cast<const ue::UObject*>(cls));
            auto* rc = rebuilder::rebuild(class_name);
            if (!rc) return sol::nil;

            auto* rp = rc->find_property(prop_name);
            if (!rp) return sol::nil;

            return read_property_value(lua, self.ptr, *rp); },

                                     "SetProp", [](LuaUObject &self, const std::string &prop_name, sol::object value) -> bool
                                     {
            if (!self.ptr) return false;

            ue::UClass* cls = ue::uobj_get_class(self.ptr);
            if (!cls) return false;

            std::string class_name = reflection::get_short_name(reinterpret_cast<const ue::UObject*>(cls));
            auto* rc = rebuilder::rebuild(class_name);
            if (!rc) return false;

            auto* rp = rc->find_property(prop_name);
            if (!rp) return false;

            return write_property_value(self.ptr, *rp, value); },

                                     // ── Function calls via ProcessEvent ──
                                     "Call", [](sol::this_state ts, LuaUObject &self, const std::string &func_name, sol::variadic_args va) -> sol::object
                                     {
            sol::state_view lua(ts);
            return call_ufunction(lua, self.ptr, func_name, va); },

                                     "CallFunc", [](sol::this_state ts, LuaUObject &self, const std::string &func_name, sol::variadic_args va) -> sol::object
                                     {
            sol::state_view lua(ts);
            return call_ufunction(lua, self.ptr, func_name, va); },

                                     // ── Background-thread function calls (avoids game-thread deadlocks) ──
                                     "CallBg", [](sol::this_state ts, LuaUObject &self, const std::string &func_name, sol::variadic_args va) -> bool
                                     {
            sol::state_view lua(ts);
            return call_ufunction_bg(lua, self.ptr, func_name, va); },

                                     // ── Background-thread raw-params function calls ──
                                     // Lua constructs exact param bytes via AllocateMemory+WriteU32 etc.
                                     // Buffer is copied — caller can reuse immediately.
                                     "CallBgRaw", [](LuaUObject &self, const std::string &func_name, void *params, int len) -> bool
                                     { return call_ufunction_bg_raw(self.ptr, func_name, params, len); },

                                     // ── Synchronous raw-params function calls ──
                                     // Like CallBgRaw but calls ProcessEvent immediately (no queuing).
                                     // Use when already on the game thread (e.g. inside ExecuteWithDelay callback).
                                     "CallRaw", [](LuaUObject &self, const std::string &func_name, void *params, int len) -> bool
                                     { return call_ufunction_raw_sync(self.ptr, func_name, params, len); },

                                     // ── Per-instance hooks ──
                                     "HookProp", [](LuaUObject &self, const std::string &prop_name, sol::function callback) -> uint64_t
                                     { return rebuilder::hook_property_instance(self.ptr, prop_name,
                                                                                [callback](ue::UObject *obj, const std::string &name, void *old_val, void *new_val) -> bool
                                                                                {
                                                                                    auto result = callback(LuaUObject{obj}, name);
                                                                                    if (result.valid() && result.get_type() == sol::type::boolean)
                                                                                    {
                                                                                        return result.get<bool>();
                                                                                    }
                                                                                    return false;
                                                                                }); },

                                     "HookFunc", [](LuaUObject &self, const std::string &func_name, sol::optional<sol::function> pre, sol::optional<sol::function> post) -> uint64_t
                                     {
            rebuilder::FuncPreCallback pre_cb = nullptr;
            rebuilder::FuncPostCallback post_cb = nullptr;

            if (pre && pre->valid()) {
                sol::function pre_fn = *pre;
                pre_cb = [pre_fn](ue::UObject* self_obj, ue::UFunction* func, void* parms) -> bool {
                    auto result = pre_fn(LuaUObject{self_obj});
                    if (result.valid() && result.get_type() == sol::type::string) {
                        std::string s = result;
                        if (s == "BLOCK") return true;
                    }
                    if (result.valid() && result.get_type() == sol::type::boolean) {
                        return result.get<bool>();
                    }
                    return false;
                };
            }

            if (post && post->valid()) {
                sol::function post_fn = *post;
                post_cb = [post_fn](ue::UObject* self_obj, ue::UFunction* func, void* parms) {
                    post_fn(LuaUObject{self_obj});
                };
            }

            return rebuilder::hook_function_instance(self.ptr, func_name, pre_cb, post_cb); },

                                     // ── __index for dynamic property AND function resolution ──
                                     // This enables BOTH obj.PropertyName AND obj:FunctionName(args) syntax.
                                     // Property lookup first, then UFunction lookup returning a bound callable.
                                     // This is the key to UE4SS-style: obj:K2_DestroyActor(), obj:OnUnlocked(true), etc.
                                     sol::meta_function::index, [](sol::this_state ts, LuaUObject &self, const std::string &key) -> sol::object
                                     {
            sol::state_view lua(ts);
            if (!self.ptr) return sol::nil;

            // First check: is this a built-in method name?
            // (IsValid, GetName, GetFullName, GetClass, GetClassName, GetAddress,
            //  ToHex, Get, Set, GetProp, SetProp, Call, CallFunc, HookProp, HookFunc)
            // Sol2 handles these via the usertype definition BEFORE __index,
            // so we only get here for keys that are NOT built-in methods.

            ue::UClass* cls = ue::uobj_get_class(self.ptr);
            if (!cls) return sol::nil;

            std::string class_name = reflection::get_short_name(reinterpret_cast<const ue::UObject*>(cls));
            auto* rc = rebuilder::rebuild(class_name);
            if (!rc) return sol::nil;

            // 1) Try property lookup first
            auto* rp = rc->find_property(key);
            if (rp) return read_property_value(lua, self.ptr, *rp);

            // 2) Try UFunction lookup — return a bound callable lambda
            //    This enables obj:FuncName(args) exactly like UE4SS
            auto* rf = rc->find_function(key);
            if (rf && rf->raw) {
                // Capture self.ptr and func_name to create a callable
                ue::UObject* obj_ptr = self.ptr;
                std::string func_name = key;
                return sol::make_object(lua, [obj_ptr, func_name](sol::this_state inner_ts,
                                                                    sol::variadic_args va) -> sol::object {
                    sol::state_view inner_lua(inner_ts);
                    return call_ufunction(inner_lua, obj_ptr, func_name, va);
                });
            }

            return sol::nil; },

                                     // ── __newindex for dynamic property writing ──
                                     sol::meta_function::new_index, [](LuaUObject &self, const std::string &key, sol::object value)
                                     {
            if (!self.ptr) return;

            ue::UClass* cls = ue::uobj_get_class(self.ptr);
            if (!cls) return;

            std::string class_name = reflection::get_short_name(reinterpret_cast<const ue::UObject*>(cls));
            auto* rc = rebuilder::rebuild(class_name);
            if (!rc) return;

            auto* rp = rc->find_property(key);
            if (rp) {
                write_property_value(self.ptr, *rp, value);
            } },

                                     // ── Equality ──
                                     sol::meta_function::equal_to, [](const LuaUObject &a, const LuaUObject &b) -> bool
                                     { return a.ptr == b.ptr; },

                                     // ── String representation ──
                                     sol::meta_function::to_string, [](const LuaUObject &self) -> std::string
                                     {
            if (!self.ptr) return "UObject(nil)";
            std::string name = reflection::get_short_name(self.ptr);
            char buf[64];
            snprintf(buf, sizeof(buf), "0x%lX", (unsigned long)reinterpret_cast<uintptr_t>(self.ptr));
            return "UObject(" + name + " @ " + buf + ")"; });

        logger::log_info("LUA", "UObject userdata type registered");
    }

    LuaUObject wrap(ue::UObject *obj)
    {
        LuaUObject wrapped;
        wrapped.ptr = obj;
        return wrapped;
    }

    ue::UObject *unwrap(const LuaUObject &wrapped)
    {
        return wrapped.ptr;
    }

    sol::object wrap_or_nil(sol::state_view lua, ue::UObject *obj)
    {
        if (!obj || !ue::is_valid_ptr(obj))
            return sol::nil;

        std::string name = reflection::get_short_name(obj);
        if (ue::is_default_object(name.c_str()))
        {
            logger::log_warn("FILTER", "Discarded %s — CDO cannot be used as instance", name.c_str());
            return sol::nil;
        }

        LuaUObject wrapped;
        wrapped.ptr = obj;
        return sol::make_object(lua, wrapped);
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Public shared utility functions — used by lua_tarray and lua_ustruct
    // ═══════════════════════════════════════════════════════════════════════

    std::string ftext_to_string(const void *ftext_ptr)
    {
        return ftext_to_string_via_kismet(ftext_ptr);
    }

    bool ftext_from_string(void *ftext_ptr, const std::string &str)
    {
        // Destroy old FText (decrement refcount) if destructor available
        if (symbols::FText_Dtor)
        {
            symbols::FText_Dtor(ftext_ptr);
        }
        // Primary: use Kismet ProcessEvent to create properly-allocated FText
        if (ftext_from_string_via_kismet(ftext_ptr, str))
            return true;
        // Fallback: direct FText::FromString via arm64 asm
        if (symbols::FText_FromString)
        {
            size_t wlen = str.size() + 1;
            char16_t *wbuf = static_cast<char16_t *>(malloc(wlen * sizeof(char16_t)));
            for (size_t i = 0; i < str.size(); i++)
                wbuf[i] = static_cast<char16_t>(static_cast<unsigned char>(str[i]));
            wbuf[str.size()] = 0;
            FStrRaw tmp_fstr = {wbuf, static_cast<int32_t>(wlen), static_cast<int32_t>(wlen)};
            arm64_call_ftext_fromstring(ftext_ptr, &tmp_fstr);
            // Don't free wbuf — ownership transferred to FText
            return true;
        }
        return false;
    }

    std::string fstring_to_utf8(const void *fstring_ptr)
    {
        if (!fstring_ptr)
            return "";
        const FStrRaw *fstr = reinterpret_cast<const FStrRaw *>(fstring_ptr);
        if (!fstr->data || fstr->num <= 0 || !ue::is_mapped_ptr(fstr->data))
            return "";

        std::string utf8;
        int count = fstr->num - 1; // num includes null terminator
        for (int i = 0; i < count; i++)
        {
            char16_t c = fstr->data[i];
            if (c == 0)
                break;
            if (c < 0x80)
                utf8 += static_cast<char>(c);
            else if (c < 0x800)
            {
                utf8 += static_cast<char>(0xC0 | (c >> 6));
                utf8 += static_cast<char>(0x80 | (c & 0x3F));
            }
            else
            {
                utf8 += static_cast<char>(0xE0 | (c >> 12));
                utf8 += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                utf8 += static_cast<char>(0x80 | (c & 0x3F));
            }
        }
        return utf8;
    }

    bool fstring_from_utf8(void *fstring_ptr, const std::string &str)
    {
        if (!fstring_ptr)
            return false;
        FStrRaw *fstr = reinterpret_cast<FStrRaw *>(fstring_ptr);

        size_t wlen = str.size() + 1;
        // Reuse existing buffer if capacity is sufficient
        if (fstr->data && fstr->max >= static_cast<int32_t>(wlen) && ue::is_mapped_ptr(fstr->data))
        {
            for (size_t i = 0; i < str.size(); i++)
                fstr->data[i] = static_cast<char16_t>(static_cast<unsigned char>(str[i]));
            fstr->data[str.size()] = 0;
            fstr->num = static_cast<int32_t>(wlen);
            return true;
        }

        // Need new allocation — use malloc (safe across allocator boundaries).
        // We deliberately do NOT free the old buffer because it was likely allocated
        // by UE4's FMemory::Malloc and we cannot call FMemory::Free from here.
        // This may leak the old buffer, but avoids heap corruption.
        char16_t *wbuf = static_cast<char16_t *>(malloc(wlen * sizeof(char16_t)));
        if (!wbuf)
            return false;
        for (size_t i = 0; i < str.size(); i++)
            wbuf[i] = static_cast<char16_t>(static_cast<unsigned char>(str[i]));
        wbuf[str.size()] = 0;

        fstr->data = wbuf;
        fstr->num = static_cast<int32_t>(wlen);
        fstr->max = static_cast<int32_t>(wlen);
        return true;
    }

} // namespace lua_uobject
