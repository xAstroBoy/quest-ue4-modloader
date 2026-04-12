// modloader/src/util/crash_handler.cpp
// Signal handler for SIGSEGV, SIGABRT, SIGBUS, SIGFPE
// Writes modloader_crash.log with:
//   - fault address and signal info
//   - register dump
//   - native backtrace (via _Unwind_Backtrace or android backtrace)
//   - last 500 lines of UEModLoader.log
// Posts a notification before dying

#include "modloader/crash_handler.h"
#include "modloader/logger.h"
#include "modloader/paths.h"
#include "modloader/notification.h"
#include <signal.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <unistd.h>
#include <ucontext.h>
#include <unwind.h>
#include <dlfcn.h>
#include <link.h>
#include <sys/time.h>
#include <time.h>
#include <setjmp.h>

// Safe-call crash recovery state — defined in native_hooks.cpp
// When a hooked original function crashes, dispatch_full() has set
// g_in_hook_original_call=1. We detect this and siglongjmp back to
// dispatch_full() which returns safe defaults instead of crashing.
extern thread_local volatile int g_in_hook_original_call;
extern thread_local sigjmp_buf g_hook_recovery_jmp;

// Hook installation crash recovery — defined in native_hooks.cpp
// When DobbyHook() crashes during trampoline installation (bad address,
// unmapped memory, function too small), install_at() has set
// g_in_hook_install=1. We siglongjmp back so install_at() returns 0.
extern thread_local volatile int g_in_hook_install;
extern thread_local sigjmp_buf g_hook_install_jmp;

// ProcessEvent crash recovery — defined in lua_uobject.cpp
// When Call()/CallBg() invokes ProcessEvent and the target UFunction
// crashes (SIGSEGV/SIGBUS), we siglongjmp back to the Call() wrapper
// which returns nil/false instead of killing the process.
namespace lua_uobject
{
    extern thread_local volatile int g_in_call_ufunction;
    extern thread_local sigjmp_buf g_call_ufunction_jmp;
}

namespace crash_handler
{

    static struct sigaction s_old_sigsegv;
    static struct sigaction s_old_sigabrt;
    static struct sigaction s_old_sigbus;
    static struct sigaction s_old_sigfpe;

    // Track our own library's address range so we can distinguish
    // modloader crashes from game crashes
    static uintptr_t s_modloader_base = 0;
    static uintptr_t s_modloader_end = 0;

    static int find_modloader_cb(struct dl_phdr_info *info, size_t, void *)
    {
        if (info->dlpi_name && strstr(info->dlpi_name, "libmodloader.so"))
        {
            uintptr_t lo = UINTPTR_MAX, hi = 0;
            for (int i = 0; i < info->dlpi_phnum; i++)
            {
                if (info->dlpi_phdr[i].p_type == PT_LOAD)
                {
                    uintptr_t seg_lo = info->dlpi_addr + info->dlpi_phdr[i].p_vaddr;
                    uintptr_t seg_hi = seg_lo + info->dlpi_phdr[i].p_memsz;
                    if (seg_lo < lo)
                        lo = seg_lo;
                    if (seg_hi > hi)
                        hi = seg_hi;
                }
            }
            s_modloader_base = lo;
            s_modloader_end = hi;
            return 1;
        }
        return 0;
    }

    static bool pc_in_modloader(uintptr_t pc)
    {
        if (s_modloader_base == 0)
            return false;
        return pc >= s_modloader_base && pc < s_modloader_end;
    }

    struct BacktraceState
    {
        void **frames;
        int max_depth;
        int count;
    };

    static _Unwind_Reason_Code unwind_callback(struct _Unwind_Context *context, void *arg)
    {
        auto *state = static_cast<BacktraceState *>(arg);
        uintptr_t pc = _Unwind_GetIP(context);
        if (pc != 0)
        {
            if (state->count < state->max_depth)
            {
                state->frames[state->count] = reinterpret_cast<void *>(pc);
                state->count++;
            }
            else
            {
                return _URC_END_OF_STACK;
            }
        }
        return _URC_NO_REASON;
    }

    static int capture_backtrace(void **frames, int max_depth)
    {
        BacktraceState state;
        state.frames = frames;
        state.max_depth = max_depth;
        state.count = 0;
        _Unwind_Backtrace(unwind_callback, &state);
        return state.count;
    }

    static const char *signal_name(int sig)
    {
        switch (sig)
        {
        case SIGSEGV:
            return "SIGSEGV";
        case SIGABRT:
            return "SIGABRT";
        case SIGBUS:
            return "SIGBUS";
        case SIGFPE:
            return "SIGFPE";
        default:
            return "UNKNOWN";
        }
    }

    static const char *signal_code_name(int sig, int code)
    {
        if (sig == SIGSEGV)
        {
            switch (code)
            {
            case SEGV_MAPERR:
                return "SEGV_MAPERR (address not mapped)";
            case SEGV_ACCERR:
                return "SEGV_ACCERR (access permission fault)";
            default:
                return "unknown code";
            }
        }
        if (sig == SIGBUS)
        {
            switch (code)
            {
            case BUS_ADRALN:
                return "BUS_ADRALN (alignment fault)";
            case BUS_ADRERR:
                return "BUS_ADRERR (nonexistent physical address)";
            case BUS_OBJERR:
                return "BUS_OBJERR (object-specific HW error)";
            default:
                return "unknown code";
            }
        }
        return "unknown code";
    }

    static void crash_handler_fn(int sig, siginfo_t *info, void *ucontext_raw)
    {
        // ── HOOK INSTALL CRASH RECOVERY ──────────────────────────────────────
        // If this crash happened while DobbyHook() was installing a trampoline
        // (inside install_at), recover via siglongjmp. install_at() returns 0
        // and the mod continues loading without the failed hook.
        if (g_in_hook_install)
        {
            g_in_hook_install = 0;
            siglongjmp(g_hook_install_jmp, sig);
            // Never reaches here
        }

        // ── HOOK SAFE-CALL RECOVERY ──────────────────────────────────────────
        // If this crash happened while executing a hooked original function
        // (inside the asm blr call in dispatch_full), recover via siglongjmp
        // instead of killing the process. dispatch_full() returns safe defaults.
        // This catches crashes from dangling pointers, corrupted vtables, etc.
        // that happen inside game functions called through modloader hooks.
        if (g_in_hook_original_call)
        {
            g_in_hook_original_call = 0;
            siglongjmp(g_hook_recovery_jmp, sig);
            // Never reaches here — siglongjmp jumps to sigsetjmp in dispatch_full
        }

        // ── PROCESSEVENT CRASH RECOVERY ──────────────────────────────────────
        // If this crash happened inside ProcessEvent called from Lua's Call(),
        // CallBg(), or CallRaw(), recover via siglongjmp. The Call() wrapper
        // returns nil/false and logs the crash instead of killing the process.
        // This protects against UFunction implementations that crash due to
        // uninitialized state, null pointers, or unsupported parameter combos.
        if (lua_uobject::g_in_call_ufunction)
        {
            lua_uobject::g_in_call_ufunction = 0;
            siglongjmp(lua_uobject::g_call_ufunction_jmp, sig);
            // Never reaches here
        }

        // Check if crash PC is inside libmodloader.so — if not, this is a game crash,
        // not ours. Pass directly to previous handler without writing crash log or
        // posting notification. The game or ART may handle it gracefully.
        ucontext_t *uc = static_cast<ucontext_t *>(ucontext_raw);
        bool is_our_crash = false;
        if (uc)
        {
            uintptr_t pc = static_cast<uintptr_t>(uc->uc_mcontext.pc);
            is_our_crash = pc_in_modloader(pc);
        }

        if (!is_our_crash)
        {
            // Not our crash — forward to previous handler immediately
            struct sigaction *old_action = nullptr;
            switch (sig)
            {
            case SIGSEGV:
                old_action = &s_old_sigsegv;
                break;
            case SIGABRT:
                old_action = &s_old_sigabrt;
                break;
            case SIGBUS:
                old_action = &s_old_sigbus;
                break;
            case SIGFPE:
                old_action = &s_old_sigfpe;
                break;
            }
            if (old_action && old_action->sa_sigaction)
            {
                old_action->sa_sigaction(sig, info, ucontext_raw);
            }
            else if (old_action && old_action->sa_handler != SIG_DFL && old_action->sa_handler != SIG_IGN)
            {
                old_action->sa_handler(sig);
            }
            else
            {
                signal(sig, SIG_DFL);
                raise(sig);
            }
            return;
        }

        // Crash IS in libmodloader.so — write full crash report
        FILE *f = fopen(paths::crash_log().c_str(), "w");
        if (f)
        {
            // Timestamp
            struct timeval tv;
            gettimeofday(&tv, nullptr);
            struct tm tm_info;
            localtime_r(&tv.tv_sec, &tm_info);
            char date_buf[64];
            strftime(date_buf, sizeof(date_buf), "%Y-%m-%d %H:%M:%S", &tm_info);

            fprintf(f, "=== MODLOADER CRASH REPORT ===\n");
            fprintf(f, "Time: %s\n", date_buf);
            fprintf(f, "Signal: %s (%d)\n", signal_name(sig), sig);
            fprintf(f, "Code: %s (%d)\n", signal_code_name(sig, info->si_code), info->si_code);
            fprintf(f, "Fault address: %p\n", info->si_addr);

            // Register dump (ARM64)
            ucontext_t *uc = static_cast<ucontext_t *>(ucontext_raw);
            if (uc)
            {
                fprintf(f, "\n=== REGISTERS ===\n");
                for (int i = 0; i < 31; i++)
                {
                    fprintf(f, "X%-2d = 0x%016llx\n", i,
                            (unsigned long long)uc->uc_mcontext.regs[i]);
                }
                fprintf(f, "SP  = 0x%016llx\n", (unsigned long long)uc->uc_mcontext.sp);
                fprintf(f, "PC  = 0x%016llx\n", (unsigned long long)uc->uc_mcontext.pc);
            }

            // Backtrace
            fprintf(f, "\n=== BACKTRACE ===\n");
            void *frames[64];
            int depth = capture_backtrace(frames, 64);
            for (int i = 0; i < depth; i++)
            {
                Dl_info dl_info;
                if (dladdr(frames[i], &dl_info))
                {
                    uintptr_t offset = reinterpret_cast<uintptr_t>(frames[i]) -
                                       reinterpret_cast<uintptr_t>(dl_info.dli_fbase);
                    fprintf(f, "#%02d  %p  %s + 0x%lx (%s)\n",
                            i, frames[i],
                            dl_info.dli_sname ? dl_info.dli_sname : "???",
                            (unsigned long)offset,
                            dl_info.dli_fname ? dl_info.dli_fname : "???");
                }
                else
                {
                    fprintf(f, "#%02d  %p  ???\n", i, frames[i]);
                }
            }

            // Append last 500 lines of UEModLoader.log
            fprintf(f, "\n=== LAST 500 LOG LINES ===\n");
            auto tail_text = logger::get_tail(500);
            fprintf(f, "%s", tail_text.c_str());

            fprintf(f, "\n=== END CRASH REPORT ===\n");
            fflush(f);
            fclose(f);
        }

        // Post crash notification (best effort — may fail in signal context)
        notification::post_crash();

        // Re-raise with original handler
        struct sigaction *old_action = nullptr;
        switch (sig)
        {
        case SIGSEGV:
            old_action = &s_old_sigsegv;
            break;
        case SIGABRT:
            old_action = &s_old_sigabrt;
            break;
        case SIGBUS:
            old_action = &s_old_sigbus;
            break;
        case SIGFPE:
            old_action = &s_old_sigfpe;
            break;
        }

        if (old_action && old_action->sa_sigaction)
        {
            old_action->sa_sigaction(sig, info, ucontext_raw);
        }
        else if (old_action && old_action->sa_handler != SIG_DFL && old_action->sa_handler != SIG_IGN)
        {
            old_action->sa_handler(sig);
        }
        else
        {
            // Restore default and re-raise
            signal(sig, SIG_DFL);
            raise(sig);
        }
    }

    void install()
    {
        // Find our own library's address range first
        dl_iterate_phdr(find_modloader_cb, nullptr);
        if (s_modloader_base != 0)
        {
            logger::log_info("CRASH", "libmodloader.so range: 0x%lX - 0x%lX",
                             (unsigned long)s_modloader_base, (unsigned long)s_modloader_end);
        }
        else
        {
            logger::log_warn("CRASH", "Could not find libmodloader.so address range — all crashes will be reported");
        }

        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = crash_handler_fn;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);

        // Handle hardware faults only — NOT SIGABRT.
        // SIGABRT is excluded because Frida's gadget (libfrda.so) calls abort()
        // ~1.4s after boot. If we intercept SIGABRT and forward to the saved
        // handler (which may be SIG_DFL), the process dies immediately instead
        // of Frida handling it gracefully. Leave SIGABRT alone.
        sigaction(SIGSEGV, &sa, &s_old_sigsegv);
        sigaction(SIGBUS, &sa, &s_old_sigbus);
        sigaction(SIGFPE, &sa, &s_old_sigfpe);
    }

    void reinstall()
    {
        // Re-assert our signal handler on top of whatever replaced it.
        // The Oculus VR runtime (libvrapi.so) and Frida both install their own
        // SIGSEGV handlers after our initial install(). This overwrites ours,
        // which means crashes inside hooked functions (where g_in_hook_original_call
        // is set) won't be caught by siglongjmp — they'll kill the process.
        //
        // We save the current handler as our new "old" handler (to chain to),
        // then re-install ours on top.
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = crash_handler_fn;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);

        // Save whatever is currently installed (Oculus/Frida handler) as our chain target
        sigaction(SIGSEGV, &sa, &s_old_sigsegv);
        sigaction(SIGBUS, &sa, &s_old_sigbus);
        sigaction(SIGFPE, &sa, &s_old_sigfpe);

        logger::log_info("CRASH", "Signal handler re-installed (protecting against runtime handler replacement)");
    }

} // namespace crash_handler
