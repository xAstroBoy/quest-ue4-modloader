#pragma once
// modloader/include/modloader/safe_call.h
// ═══════════════════════════════════════════════════════════════════════════
// Safe-call wrapper for crash recovery in the modloader.
//
// DUAL PROTECTION STRATEGY:
//   1. try/catch — catches C++ exceptions (std::exception, bad_alloc, etc.)
//   2. sigsetjmp/siglongjmp — catches fatal signals (SIGSEGV, SIGBUS, SIGFPE)
//
// This gives us complete crash protection:
//   - Lua pcall errors → caught by Lua's own error handling
//   - C++ exceptions → caught by try/catch
//   - Null pointer / bad memory → caught by SIGSEGV signal handler
//   - Bus errors (alignment) → caught by SIGBUS signal handler
//   - Divide by zero → caught by SIGFPE signal handler
//
// The signal handler is per-thread (thread_local jmp_buf) and cooperates
// with the existing crash_handler.h signal infrastructure.
//
// WARNING on siglongjmp:
//   siglongjmp does NOT unwind C++ destructors. Only use execute() around
//   code that doesn't hold RAII resources (mutexes, smart ptrs, files).
//   For RAII-safe code, use execute_safe() which only catches exceptions.
// ═══════════════════════════════════════════════════════════════════════════

#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>

namespace safe_call
{

    // ═══ Result of a safe call ══════════════════════════════════════════════
    struct Result
    {
        bool ok;               // true if function completed normally
        int signal;            // signal number if crashed (SIGSEGV=11, SIGBUS=7, SIGFPE=8, 0=exception)
        uintptr_t fault_addr;  // faulting address (signals only)
        std::string error_msg; // human-readable error message

        // Convenience
        explicit operator bool() const { return ok; }
    };

    // ═══ Initialization ═════════════════════════════════════════════════════
    // Install signal-based crash recovery infrastructure.
    // Must be called once per process (usually during boot).
    // The crash_handler module cooperates with this — when it detects a
    // SIGSEGV/SIGBUS inside a safe_call region, it siglongjmps back
    // instead of killing the process.
    void init();

    // ═══ Full protection: try/catch + signal recovery ═══════════════════════
    // Catches BOTH C++ exceptions AND fatal signals.
    //
    // ⚠ WARNING: If a signal fires, siglongjmp skips destructors.
    //   Only use for code that doesn't hold RAII resources on the stack.
    //   Good for: pointer reads, UObject property access, function calls.
    //   Bad for: code holding std::lock_guard, unique_ptr, ofstream, etc.
    //
    // @param func     The function to execute
    // @param context  Human-readable context for error messages
    // @return Result  ok=true on success, ok=false with error details on failure
    Result execute(const std::function<void()> &func, const std::string &context = "");

    // ═══ Exception-only protection: try/catch only ══════════════════════════
    // Catches C++ exceptions but NOT fatal signals.
    // This is RAII-safe — destructors run normally on exception unwind.
    //
    // Use this when the code being called holds mutexes, smart pointers,
    // file handles, or any other RAII resource.
    //
    // @param func     The function to execute
    // @param context  Human-readable context for error messages
    // @return Result  ok=true on success, ok=false with exception details
    Result execute_safe(const std::function<void()> &func, const std::string &context = "");

    // ═══ Templated safe call with return value ══════════════════════════════
    // Returns the function's result on success, or default_val on failure.
    template <typename T>
    T execute_with_result(const std::function<T()> &func, T default_val = T{},
                          const std::string &context = "")
    {
        T result = default_val;
        auto wrapper = [&]()
        { result = func(); };
        auto r = execute(std::function<void()>(wrapper), context);
        return r.ok ? result : default_val;
    }

    // ═══ Pointer probe ══════════════════════════════════════════════════════
    // Returns true if the pointer can be safely dereferenced for `size` bytes.
    // Uses signal recovery — more reliable than msync() on some ARM64 kernels.
    bool probe_read(const void *addr, size_t size = 8);

    // ═══ Quick memory read with fallback ════════════════════════════════════
    // Read `size` bytes from `addr` into `out`, returns false on fault.
    bool safe_memcpy(void *out, const void *addr, size_t size);

    // ═══ Statistics ═════════════════════════════════════════════════════════
    // Total crashes recovered since boot (exceptions + signals)
    uint64_t crash_recovery_count();

    // Total C++ exceptions caught since boot
    uint64_t exception_count();

    // Total signal-based recoveries since boot
    uint64_t signal_recovery_count();

    // Last crash context string (for debugging)
    const std::string &last_crash_context();

    // ═══ Thread-local state (for crash_handler cooperation) ═════════════════
    // These are checked by the crash handler's signal handler to decide
    // whether to siglongjmp (safe recovery) or proceed with default behavior.

    // Returns true if the current thread is inside a safe_call::execute() region
    bool is_in_safe_region();

    // Called by the crash handler when a signal fires inside a safe region.
    // Sets the signal/fault info and calls siglongjmp to unwind.
    // NEVER call this directly — only the signal handler should call it.
    void signal_recovery(int sig, uintptr_t fault_addr);

} // namespace safe_call
