// modloader/src/util/safe_call.cpp
// ═══════════════════════════════════════════════════════════════════════════
// Safe-call wrapper — DUAL crash recovery
//
// 1. try/catch — catches C++ exceptions (std::exception, std::bad_alloc, etc.)
// 2. sigsetjmp/siglongjmp — catches fatal signals (SIGSEGV, SIGBUS, SIGFPE)
//
// The signal recovery cooperates with crash_handler.h:
//   - crash_handler installs a global SIGSEGV/SIGBUS/SIGFPE handler
//   - when a signal fires, it checks safe_call::is_in_safe_region()
//   - if true, it calls safe_call::signal_recovery() which does siglongjmp
//   - if false, it proceeds with the default crash behavior (tombstone/abort)
//
// Thread safety: all mutable state is thread_local. No contention.
// ═══════════════════════════════════════════════════════════════════════════

#include "modloader/safe_call.h"
#include "modloader/logger.h"
#include <atomic>
#include <cstring>
#include <csignal>
#include <csetjmp>
#include <cerrno>
#include <exception>
#include <stdexcept>

namespace safe_call
{

    // ═══ Thread-local state ═════════════════════════════════════════════════
    // Each thread has its own recovery point and flags.
    // Only one safe_call region can be active per thread at a time.

    struct ThreadState
    {
        sigjmp_buf jmp_buf;          // recovery point for siglongjmp
        volatile int in_safe_region; // 1 if inside execute(), 0 otherwise
        int recovered_signal;        // signal number that triggered recovery
        uintptr_t recovered_fault;   // fault address from siginfo
    };

    static thread_local ThreadState t_state = {};

    // ═══ Global statistics ══════════════════════════════════════════════════
    static std::atomic<uint64_t> s_total_recoveries{0};
    static std::atomic<uint64_t> s_exception_count{0};
    static std::atomic<uint64_t> s_signal_count{0};
    static thread_local std::string t_last_context;

    // ═══ Initialization ═════════════════════════════════════════════════════
    void init()
    {
        // Nothing heavy to do — thread_local state is zero-initialized.
        // The crash_handler module does the actual signal handler installation.
        // We just provide the is_in_safe_region() / signal_recovery() API
        // that the crash handler calls into.
        logger::log_info("SAFECALL", "Safe-call subsystem initialized (try/catch + signal recovery)");
    }

    // ═══ Full protection: try/catch + signal recovery ═══════════════════════
    Result execute(const std::function<void()> &func, const std::string &context)
    {
        Result result;
        result.ok = false;
        result.signal = 0;
        result.fault_addr = 0;

        // ── Layer 1: sigsetjmp for signal-based crashes ─────────────────────
        // Set up the recovery point FIRST so that if a signal fires inside
        // the try block, we have somewhere to jump back to.
        t_state.recovered_signal = 0;
        t_state.recovered_fault = 0;
        t_state.in_safe_region = 1;

        int sig = sigsetjmp(t_state.jmp_buf, 1); // save signal mask
        if (sig != 0)
        {
            // ── SIGNAL RECOVERED ────────────────────────────────────────────
            // siglongjmp brought us here from the signal handler.
            t_state.in_safe_region = 0;

            result.signal = t_state.recovered_signal;
            result.fault_addr = t_state.recovered_fault;

            const char *sig_name = "UNKNOWN";
            switch (result.signal)
            {
            case SIGSEGV:
                sig_name = "SIGSEGV";
                break;
            case SIGBUS:
                sig_name = "SIGBUS";
                break;
            case SIGFPE:
                sig_name = "SIGFPE";
                break;
            case SIGABRT:
                sig_name = "SIGABRT";
                break;
            }

            char msg[512];
            snprintf(msg, sizeof(msg),
                     "Signal %s (%d) at fault_addr=0x%lX [%s]",
                     sig_name, result.signal,
                     static_cast<unsigned long>(result.fault_addr),
                     context.c_str());
            result.error_msg = msg;

            s_total_recoveries.fetch_add(1, std::memory_order_relaxed);
            s_signal_count.fetch_add(1, std::memory_order_relaxed);
            t_last_context = context;

            logger::log_error("SAFECALL",
                              "SIGNAL RECOVERY: %s (sig=%d, fault=0x%lX) — recovered",
                              context.c_str(), result.signal,
                              static_cast<unsigned long>(result.fault_addr));

            return result;
        }

        // ── Layer 2: try/catch for C++ exceptions ───────────────────────────
        try
        {
            func();
            result.ok = true;
        }
        catch (const std::exception &e)
        {
            result.error_msg = std::string("C++ exception: ") + e.what() + " [" + context + "]";
            s_total_recoveries.fetch_add(1, std::memory_order_relaxed);
            s_exception_count.fetch_add(1, std::memory_order_relaxed);
            t_last_context = context;
            logger::log_error("SAFECALL", "EXCEPTION: %s — %s", context.c_str(), e.what());
        }
        catch (...)
        {
            result.error_msg = std::string("Unknown C++ exception [") + context + "]";
            s_total_recoveries.fetch_add(1, std::memory_order_relaxed);
            s_exception_count.fetch_add(1, std::memory_order_relaxed);
            t_last_context = context;
            logger::log_error("SAFECALL", "UNKNOWN EXCEPTION: %s", context.c_str());
        }

        t_state.in_safe_region = 0;
        return result;
    }

    // ═══ Exception-only protection (RAII-safe) ══════════════════════════════
    Result execute_safe(const std::function<void()> &func, const std::string &context)
    {
        Result result;
        result.ok = false;
        result.signal = 0;
        result.fault_addr = 0;

        try
        {
            func();
            result.ok = true;
        }
        catch (const std::exception &e)
        {
            result.error_msg = std::string("C++ exception: ") + e.what() + " [" + context + "]";
            s_total_recoveries.fetch_add(1, std::memory_order_relaxed);
            s_exception_count.fetch_add(1, std::memory_order_relaxed);
            t_last_context = context;
            logger::log_error("SAFECALL", "EXCEPTION (safe): %s — %s", context.c_str(), e.what());
        }
        catch (...)
        {
            result.error_msg = std::string("Unknown C++ exception [") + context + "]";
            s_total_recoveries.fetch_add(1, std::memory_order_relaxed);
            s_exception_count.fetch_add(1, std::memory_order_relaxed);
            t_last_context = context;
            logger::log_error("SAFECALL", "UNKNOWN EXCEPTION (safe): %s", context.c_str());
        }

        return result;
    }

    // ═══ Pointer probe ══════════════════════════════════════════════════════
    bool probe_read(const void *addr, size_t size)
    {
        if (!addr)
            return false;

        volatile uint8_t sink = 0;
        auto result = execute([&]()
                              {
            const volatile uint8_t* p = static_cast<const volatile uint8_t*>(addr);
            // Read first and last byte to verify the full range
            sink = p[0];
            if (size > 1) sink = p[size - 1]; }, "probe_read");
        (void)sink;
        return result.ok;
    }

    // ═══ Safe memcpy ════════════════════════════════════════════════════════
    bool safe_memcpy(void *out, const void *addr, size_t size)
    {
        if (!addr || !out || size == 0)
            return false;

        auto result = execute([&]()
                              { std::memcpy(out, addr, size); }, "safe_memcpy");
        return result.ok;
    }

    // ═══ Statistics ═════════════════════════════════════════════════════════
    uint64_t crash_recovery_count()
    {
        return s_total_recoveries.load(std::memory_order_relaxed);
    }

    uint64_t exception_count()
    {
        return s_exception_count.load(std::memory_order_relaxed);
    }

    uint64_t signal_recovery_count()
    {
        return s_signal_count.load(std::memory_order_relaxed);
    }

    const std::string &last_crash_context()
    {
        return t_last_context;
    }

    // ═══ Thread-local state queries (for crash_handler) ═════════════════════
    bool is_in_safe_region()
    {
        return t_state.in_safe_region != 0;
    }

    void signal_recovery(int sig, uintptr_t fault_addr)
    {
        // Called by the crash handler's signal handler.
        // Store the signal info so execute() can read it after siglongjmp.
        t_state.recovered_signal = sig;
        t_state.recovered_fault = fault_addr;
        t_state.in_safe_region = 0;

        // Jump back to the sigsetjmp in execute().
        // The non-zero value tells execute() that a signal was caught.
        siglongjmp(t_state.jmp_buf, sig);
    }

} // namespace safe_call
