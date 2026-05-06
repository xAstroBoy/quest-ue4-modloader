// modloader/src/lua/lua_yup.cpp
// ═══════════════════════════════════════════════════════════════════════════
// YUP Physics Engine — FULL native integration for Pinball FX VR
//
// All YUP callbacks follow the pattern:
//   void callback(void* context, Type* param)
// where context = BallInformator singleton, param = value passed by pointer.
//
// TogglePause/ToggleGodMode take only (void* context) — no extra param.
//
// The BallInformator singleton is the "self" for ALL YUP functions:
//   - SetPause/SetGodMode:  [self+0x48] → inner → write byte at +0x701/+0x754
//   - SetSpeed:             [self+0x08] → table  → write float at +0x4F4
//   - BallGetSpeed:         [self+0x08] → chain  → sqrt(vx²+vy²+vz²)
//   - TogglePause/GodMode:  [self+0x48] → inner  → flip byte
//
// All calls are signal-safe: SIGSEGV/SIGBUS → caught, never crashes game.
// ═══════════════════════════════════════════════════════════════════════════

#include "modloader/lua_yup.h"
#include "modloader/game_profile.h"
#include "modloader/symbols.h"
#include "modloader/logger.h"
#include "modloader/safe_call.h"
#include <dobby.h>
#include <cstdint>
#include <cstring>
#include <atomic>

namespace lua_yup
{

    // ═══ YUP Function Typedefs ══════════════════════════════════════════════
    // YUP callbacks pass ALL params by pointer (scripting engine convention)
    using SetBoolFn = void (*)(void *self, bool *value);        // SetPause, SetGodMode
    using ToggleFn = void (*)(void *self);                      // TogglePause, ToggleGodMode
    using SetFloatFn = void (*)(void *self, float *value);      // SetSpeed
    using VoidFn = void (*)(void *self);                        // DumpAction, DebugLoad, Ball ops
    using GetSingletonFn = void *(*)();                         // BallGetSingleton
    using GetSpeedFn = float (*)(void *self, int32_t *ballIdx); // BallGetSpeed
    using AwardTableFn = void (*)(void *unused, int32_t tableId);

    // ═══ GetSpeed hook — captures the live ball_struct pointer ══════════════
    // YUP_BallInformator_GetSpeed(arg1, &ballIdx):
    //   ball_struct = [[[[[[arg1+8]+0x550]+0xB8]+0x5B8]+ball_idx*8]+0xA0]
    // Velocity: ball_struct+0x38=Vx, +0x3C=Vy, +0x40=Vz
    // Position is adjacent (likely +0x28 or +0x10)
    static std::atomic<uintptr_t> s_captured_ball_struct{0};
    static std::atomic<uintptr_t> s_captured_getspeed_arg1{0};
    static GetSpeedFn s_orig_GetSpeed = nullptr;

    static float hooked_GetSpeed(void *arg1, int32_t *ballIdx)
    {
        // ARM64 Android userspace pointers are always > 0x1000000.
        // Reject sentinel/integer values immediately — no safe_call needed.
        constexpr uintptr_t MIN_PTR = 0x100000UL;

        auto valid_ptr = [&](uintptr_t p) { return p > MIN_PTR; };

        // Replicate the chain from the decompiled function
        uintptr_t a1 = reinterpret_cast<uintptr_t>(arg1);
        if (valid_ptr(a1))
        {
            s_captured_getspeed_arg1.store(a1);

            uintptr_t p1 = *reinterpret_cast<uintptr_t*>(a1 + 0x08);
            if (valid_ptr(p1))
            {
                uintptr_t p2 = *reinterpret_cast<uintptr_t*>(p1 + 0x550);
                if (valid_ptr(p2))
                {
                    uintptr_t p3 = *reinterpret_cast<uintptr_t*>(p2 + 0xB8);
                    if (valid_ptr(p3))
                    {
                        uintptr_t p4 = *reinterpret_cast<uintptr_t*>(p3 + 0x5B8);
                        if (valid_ptr(p4))
                        {
                            int32_t idx = ballIdx ? *ballIdx : 0;
                            uintptr_t p5 = *reinterpret_cast<uintptr_t*>(p4 + (uintptr_t)(idx * 8));
                            if (valid_ptr(p5))
                            {
                                uintptr_t bs = *reinterpret_cast<uintptr_t*>(p5 + 0xA0);
                                if (valid_ptr(bs))
                                {
                                    s_captured_ball_struct.store(bs);
                                    logger::log_info("YUP",
                                        "*** CAPTURED ball_struct=0x%lX (arg1=0x%lX p1=0x%lX p2=0x%lX p3=0x%lX p4=0x%lX p5=0x%lX) ***",
                                        (unsigned long)bs,   (unsigned long)a1,
                                        (unsigned long)p1,   (unsigned long)p2,
                                        (unsigned long)p3,   (unsigned long)p4,
                                        (unsigned long)p5);
                                }
                            }
                        }
                    }
                }
            }
        }
        if (s_orig_GetSpeed)
            return s_orig_GetSpeed(arg1, ballIdx);
        return 0.0f;
    }

    // ═══ Resolved Function Pointers ═════════════════════════════════════════
    static struct
    {
        SetBoolFn SetPause = nullptr;
        SetBoolFn SetGodMode = nullptr;
        ToggleFn PauseToggle = nullptr;
        ToggleFn ToggleGodMode = nullptr;
        SetFloatFn SetSpeed = nullptr;
        VoidFn DumpAction = nullptr;
        VoidFn DebugLoad = nullptr;

        VoidFn BallAttach = nullptr;
        VoidFn BallDetach = nullptr;
        VoidFn BallReset = nullptr;
        GetSingletonFn BallGetSingleton = nullptr;
        GetSpeedFn BallGetSpeed = nullptr;

        AwardTableFn AwardUnlockTable = nullptr;
        AwardTableFn AwardLockTable = nullptr;

        bool resolved = false;
        int count = 0;
    } s_yup;

    // ═══ PendulumBallConstraint Capture — captures the PBC object from BallAttach/Detach ══
    // PBC layout (from Detach decompile):
    //   +0x40: uint32  — attached balls bitmask
    //   +0x58: vec3    — ballCenter (world position)
    //   +0x68: ptr     — attached actor
    //   +0x70: ptr     — physics world container  ← THE KEY
    // Chain: pbc[+0x70][+0xB8][+0x5B8][idx*8][+0xA0] = ball_struct
    static std::atomic<uintptr_t> s_captured_pbc{0};
    // BallAttach/Detach signature: void fn(void* self, int32_t* ballIdx)
    // We only need self (x0), x1 passes through untouched
    using BallOpFn = void (*)(void *self, void *ballIdxPtr);
    static BallOpFn s_orig_BallAttachHook = nullptr;
    static BallOpFn s_orig_BallDetachHook = nullptr;

    static void hooked_BallAttachCapture(void *self, void *ballIdxPtr)
    {
        if (self && reinterpret_cast<uintptr_t>(self) > 0x100000UL)
        {
            uintptr_t prev = s_captured_pbc.exchange(reinterpret_cast<uintptr_t>(self));
            if (prev == 0)
                logger::log_info("YUP", "*** CAPTURED PBC from BallAttach = 0x%lX ***",
                                 (unsigned long)(uintptr_t)self);
        }
        if (s_orig_BallAttachHook)
            s_orig_BallAttachHook(self, ballIdxPtr);
    }
    static void hooked_BallDetachCapture(void *self, void *ballIdxPtr)
    {
        if (self && reinterpret_cast<uintptr_t>(self) > 0x100000UL)
        {
            uintptr_t prev = s_captured_pbc.exchange(reinterpret_cast<uintptr_t>(self));
            if (prev == 0)
                logger::log_info("YUP", "*** CAPTURED PBC from BallDetach = 0x%lX ***",
                                 (unsigned long)(uintptr_t)self);
        }
        if (s_orig_BallDetachHook)
            s_orig_BallDetachHook(self, ballIdxPtr);
    }

    // ═══ Self-Capture Hooks — Captures the real self pointer from YUP engine ══
    static std::atomic<uintptr_t> s_captured_self{0};
    static SetBoolFn s_orig_SetPause = nullptr;
    static SetBoolFn s_orig_SetGodMode = nullptr;
    static ToggleFn s_orig_TogglePause = nullptr;
    static ToggleFn s_orig_ToggleGodMode = nullptr;
    static SetFloatFn s_orig_SetSpeed = nullptr;
    static VoidFn s_orig_DumpAction = nullptr;

    static void capture_self(const char *name, void *self)
    {
        if (s_captured_self.load() == 0 && self)
        {
            s_captured_self.store(reinterpret_cast<uintptr_t>(self));
            logger::log_info("YUP", "*** CAPTURED self from %s = 0x%lX ***",
                             name, (unsigned long)(uintptr_t)self);
        }
    }

    static void hooked_SetPause(void *self, bool *value)
    {
        capture_self("SetPause", self);
        if (s_orig_SetPause)
            s_orig_SetPause(self, value);
    }
    static void hooked_SetGodMode(void *self, bool *value)
    {
        capture_self("SetGodMode", self);
        if (s_orig_SetGodMode)
            s_orig_SetGodMode(self, value);
    }
    static void hooked_TogglePause(void *self)
    {
        capture_self("TogglePause", self);
        if (s_orig_TogglePause)
            s_orig_TogglePause(self);
    }
    static void hooked_ToggleGodMode(void *self)
    {
        capture_self("ToggleGodMode", self);
        if (s_orig_ToggleGodMode)
            s_orig_ToggleGodMode(self);
    }
    static void hooked_SetSpeed(void *self, float *value)
    {
        capture_self("SetSpeed", self);
        if (s_orig_SetSpeed)
            s_orig_SetSpeed(self, value);
    }
    static void hooked_DumpAction(void *self)
    {
        capture_self("DumpAction", self);
        if (s_orig_DumpAction)
            s_orig_DumpAction(self);
    }

    // ═══ Safe Call Wrappers ═════════════════════════════════════════════════

    template <typename Fn, typename... Args>
    static bool safe_yup_call(const char *name, Fn fn, Args... args)
    {
        if (!fn)
        {
            logger::log_warn("YUP", "%s: not resolved", name);
            return false;
        }
        auto r = safe_call::execute([&]()
                                    { fn(args...); },
                                    std::string("YUP::") + name);
        if (!r.ok)
        {
            logger::log_error("YUP", "%s: CRASHED (sig=%d fault=0x%lX) %s",
                              name, r.signal, (unsigned long)r.fault_addr, r.error_msg.c_str());
            return false;
        }
        return true;
    }

    template <typename Ret, typename Fn, typename... Args>
    static Ret safe_yup_ret(const char *name, Ret def, Fn fn, Args... args)
    {
        if (!fn)
        {
            logger::log_warn("YUP", "%s: not resolved", name);
            return def;
        }
        Ret val = def;
        auto r = safe_call::execute([&]()
                                    { val = fn(args...); },
                                    std::string("YUP::") + name);
        if (!r.ok)
        {
            logger::log_error("YUP", "%s: CRASHED (sig=%d fault=0x%lX) %s",
                              name, r.signal, (unsigned long)r.fault_addr, r.error_msg.c_str());
            return def;
        }
        return val;
    }

    // ═══ Get Singleton Helper ═══════════════════════════════════════════════
    static void *get_singleton(const char *caller)
    {
        if (!s_yup.BallGetSingleton)
        {
            logger::log_warn("YUP", "%s: BallGetSingleton not resolved", caller);
            return nullptr;
        }
        void *s = safe_yup_ret<void *>("BallGetSingleton", nullptr, s_yup.BallGetSingleton);
        if (!s)
        {
            logger::log_warn("YUP", "%s: singleton is NULL (no table active?)", caller);
        }
        return s;
    }

    // ═══ Resolve YUP Symbols ════════════════════════════════════════════════
    void resolve_symbols()
    {
        if (!game_profile::is_pinball_fx())
            return;

        logger::log_info("YUP", "Resolving YUP physics engine symbols...");

        int found = 0;
        auto try_resolve = [&](const char *name, auto &ptr)
        {
            void *sym = symbols::resolve(name);
            if (sym)
            {
                ptr = reinterpret_cast<std::remove_reference_t<decltype(ptr)>>(sym);
                found++;
                logger::log_info("YUP", "  ✓ %s @ 0x%lX", name, (unsigned long)(uintptr_t)sym);
            }
            else
            {
                logger::log_warn("YUP", "  ✗ %s: NOT FOUND", name);
            }
        };

        try_resolve("YUP_TableDebug_SetPause", s_yup.SetPause);
        try_resolve("YUP_TableDebug_SetGodMode", s_yup.SetGodMode);
        try_resolve("YUP_TableDebug_PauseToggle", s_yup.PauseToggle);
        try_resolve("YUP_TableDebug_ToggleGodMode", s_yup.ToggleGodMode);
        try_resolve("YUP_TableDebug_SetSpeed", s_yup.SetSpeed);
        try_resolve("YUP_TableDebug_DumpAction", s_yup.DumpAction);
        try_resolve("YUP_TableDebug_DebugLoad", s_yup.DebugLoad);
        try_resolve("YUP_PendulumBall_Attach", s_yup.BallAttach);
        try_resolve("YUP_PendulumBall_Detach", s_yup.BallDetach);
        try_resolve("YUP_PendulumBall_Reset", s_yup.BallReset);
        try_resolve("YUP_BallInformator_GetSingleton", s_yup.BallGetSingleton);
        try_resolve("YUP_BallInformator_GetSpeed", s_yup.BallGetSpeed);
        try_resolve("PFX_AwardUnlockTable", s_yup.AwardUnlockTable);
        try_resolve("PFX_AwardLockTable", s_yup.AwardLockTable);

        s_yup.count = found;
        s_yup.resolved = (found > 0);
        logger::log_info("YUP", "Symbol resolution: %d/14 found", found);

        // Install Dobby hooks on ALL TDI functions to capture the real self pointer
        auto hook = [](const char *name, void *target, void *replacement, void **orig)
        {
            if (!target)
                return;
            int status = DobbyHook(target,
                                   reinterpret_cast<dobby_dummy_func_t>(replacement),
                                   reinterpret_cast<dobby_dummy_func_t *>(orig));
            if (status == 0)
                logger::log_info("YUP", "✓ Hooked %s for self-capture", name);
            else
                logger::log_warn("YUP", "✗ Failed to hook %s (status=%d)", name, status);
        };
        hook("SetPause", (void *)s_yup.SetPause, (void *)hooked_SetPause, (void **)&s_orig_SetPause);
        hook("SetGodMode", (void *)s_yup.SetGodMode, (void *)hooked_SetGodMode, (void **)&s_orig_SetGodMode);
        hook("TogglePause", (void *)s_yup.PauseToggle, (void *)hooked_TogglePause, (void **)&s_orig_TogglePause);
        hook("ToggleGodMode", (void *)s_yup.ToggleGodMode, (void *)hooked_ToggleGodMode, (void **)&s_orig_ToggleGodMode);
        hook("SetSpeed", (void *)s_yup.SetSpeed, (void *)hooked_SetSpeed, (void **)&s_orig_SetSpeed);
        hook("DumpAction", (void *)s_yup.DumpAction, (void *)hooked_DumpAction, (void **)&s_orig_DumpAction);
        // Hook GetSpeed to capture the live ball_struct pointer at call time
        hook("GetSpeed", (void *)s_yup.BallGetSpeed, (void *)hooked_GetSpeed, (void **)&s_orig_GetSpeed);
        // Hook BallAttach/Detach to capture the live PendulumBallConstraint (PBC) self ptr.
        // The PBC's [+0x70] field is the physics world: pbc[+0x70][+0xB8][+0x5B8][idx*8][+0xA0] = ball_struct
        hook("BallAttach", (void *)s_yup.BallAttach, (void *)hooked_BallAttachCapture, (void **)&s_orig_BallAttachHook);
        hook("BallDetach", (void *)s_yup.BallDetach, (void *)hooked_BallDetachCapture, (void **)&s_orig_BallDetachHook);
    }

    // ═══ Lua Registration ═══════════════════════════════════════════════════
    void register_all(sol::state &lua)
    {
        if (!game_profile::is_pinball_fx())
        {
            logger::log_info("YUP", "YUP: SKIPPED (not Pinball FX VR)");
            return;
        }

        logger::log_info("YUP", "Registering YUP Lua API...");
        resolve_symbols();

        sol::table yup = lua.create_named_table("YUP");

        // ── Info ────────────────────────────────────────────────────────────
        yup["IsAvailable"] = []() -> bool
        { return s_yup.resolved; };
        yup["FunctionCount"] = []() -> int
        { return s_yup.count; };
        yup["GameEngine"] = "YUP";
        yup["GameName"] = "Pinball FX VR";

        // ═══ TABLE DEBUG — auto-discovers self from BallInformator singleton
        // Usage: YUP.SetPause(true)  — auto-finds singleton
        //   or:  YUP.SetPause(selfPtr, true)  — explicit self

        yup["SetPause"] = [](sol::variadic_args va) -> bool
        {
            void *self = nullptr;
            bool value = true;

            if (va.size() >= 2 && va[0].is<uintptr_t>())
            {
                self = reinterpret_cast<void *>(va[0].as<uintptr_t>());
                value = va[1].as<bool>();
            }
            else if (va.size() >= 1)
            {
                self = get_singleton("SetPause");
                value = va[0].as<bool>();
            }
            else
            {
                self = get_singleton("SetPause");
            }
            if (!self)
                return false;
            return safe_yup_call("SetPause", s_yup.SetPause, self, &value);
        };

        yup["SetGodMode"] = [](sol::variadic_args va) -> bool
        {
            void *self = nullptr;
            bool value = true;

            if (va.size() >= 2 && va[0].is<uintptr_t>())
            {
                self = reinterpret_cast<void *>(va[0].as<uintptr_t>());
                value = va[1].as<bool>();
            }
            else if (va.size() >= 1)
            {
                self = get_singleton("SetGodMode");
                value = va[0].as<bool>();
            }
            else
            {
                self = get_singleton("SetGodMode");
            }
            if (!self)
                return false;
            return safe_yup_call("SetGodMode", s_yup.SetGodMode, self, &value);
        };

        yup["TogglePause"] = [](sol::optional<uintptr_t> selfAddr) -> bool
        {
            void *self;
            if (selfAddr && *selfAddr)
                self = reinterpret_cast<void *>(*selfAddr);
            else
                self = get_singleton("TogglePause");
            if (!self)
                return false;
            return safe_yup_call("TogglePause", s_yup.PauseToggle, self);
        };

        yup["ToggleGodMode"] = [](sol::optional<uintptr_t> selfAddr) -> bool
        {
            void *self;
            if (selfAddr && *selfAddr)
                self = reinterpret_cast<void *>(*selfAddr);
            else
                self = get_singleton("ToggleGodMode");
            if (!self)
                return false;
            return safe_yup_call("ToggleGodMode", s_yup.ToggleGodMode, self);
        };

        yup["SetSpeed"] = [](sol::variadic_args va) -> bool
        {
            void *self = nullptr;
            float speed = 1.0f;

            if (va.size() >= 2 && va[0].is<uintptr_t>())
            {
                self = reinterpret_cast<void *>(va[0].as<uintptr_t>());
                speed = va[1].as<float>();
            }
            else if (va.size() >= 1)
            {
                self = get_singleton("SetSpeed");
                speed = va[0].as<float>();
            }
            else
            {
                return false;
            }
            if (!self)
                return false;
            return safe_yup_call("SetSpeed", s_yup.SetSpeed, self, &speed);
        };

        yup["DumpAction"] = [](sol::optional<uintptr_t> selfAddr) -> bool
        {
            void *self;
            if (selfAddr && *selfAddr)
                self = reinterpret_cast<void *>(*selfAddr);
            else
                self = get_singleton("DumpAction");
            if (!self)
                return false;
            return safe_yup_call("DumpAction", s_yup.DumpAction, self);
        };

        yup["DebugLoad"] = [](sol::optional<uintptr_t> selfAddr) -> bool
        {
            void *self;
            if (selfAddr && *selfAddr)
                self = reinterpret_cast<void *>(*selfAddr);
            else
                self = get_singleton("DebugLoad");
            if (!self)
                return false;
            return safe_yup_call("DebugLoad", s_yup.DebugLoad, self);
        };

        // ═══ BALL CONTROL — requires PendulumBall self pointer ══════════════

        yup["AttachBall"] = [](uintptr_t self) -> bool
        {
            if (!self)
                return false;
            return safe_yup_call("AttachBall", s_yup.BallAttach,
                                 reinterpret_cast<void *>(self));
        };

        yup["DetachBall"] = [](uintptr_t self) -> bool
        {
            if (!self)
                return false;
            return safe_yup_call("DetachBall", s_yup.BallDetach,
                                 reinterpret_cast<void *>(self));
        };

        yup["ResetBall"] = [](uintptr_t self) -> bool
        {
            if (!self)
                return false;
            return safe_yup_call("ResetBall", s_yup.BallReset,
                                 reinterpret_cast<void *>(self));
        };

        // ═══ BALL INFORMATOR — static singleton ═════════════════════════════

        yup["GetBallSpeed"] = [](sol::optional<int> ballIdx) -> float
        {
            void *singleton = get_singleton("GetBallSpeed");
            if (!singleton)
                return -1.0f;
            int32_t idx = ballIdx.value_or(0);
            return safe_yup_ret<float>(
                "BallGetSpeed", -1.0f, s_yup.BallGetSpeed, singleton, &idx);
        };

        yup["GetBallSpeedFrom"] = [](uintptr_t informator, sol::optional<int> ballIdx) -> float
        {
            if (!informator)
                return -1.0f;
            int32_t idx = ballIdx.value_or(0);
            return safe_yup_ret<float>(
                "BallGetSpeed", -1.0f, s_yup.BallGetSpeed,
                reinterpret_cast<void *>(informator), &idx);
        };

        yup["GetBallInformator"] = []() -> uintptr_t
        {
            void *p = get_singleton("GetBallInformator");
            return reinterpret_cast<uintptr_t>(p);
        };

        // ═══ TABLE AWARDS — static (no self) ════════════════════════════════

        yup["AwardUnlockTable"] = [](int tableId) -> bool
        {
            bool ok = safe_yup_call("AwardUnlockTable", s_yup.AwardUnlockTable,
                                    (void *)nullptr, (int32_t)tableId);
            if (ok)
                logger::log_info("YUP", "AwardUnlockTable(%d) OK", tableId);
            return ok;
        };

        yup["AwardLockTable"] = [](int tableId) -> bool
        {
            bool ok = safe_yup_call("AwardLockTable", s_yup.AwardLockTable,
                                    (void *)nullptr, (int32_t)tableId);
            if (ok)
                logger::log_info("YUP", "AwardLockTable(%d) OK", tableId);
            return ok;
        };

        // ═══ TABLE DEBUG INTERFACE — global object at known offset ════════════
        constexpr uintptr_t TDI_OFFSET = 0x7457858;

        yup["GetTableDebugInterface"] = []() -> uintptr_t
        {
            uintptr_t base = reinterpret_cast<uintptr_t>(symbols::get_lib_base());
            if (!base)
                return 0;
            return base + TDI_OFFSET;
        };

        yup["GetLibBase"] = []() -> uintptr_t
        {
            return reinterpret_cast<uintptr_t>(symbols::get_lib_base());
        };

        yup["GetCapturedSelf"] = []() -> uintptr_t
        {
            return s_captured_self.load();
        };

        // ═══ PendulumBallConstraint (PBC) — captured from BallAttach/Detach hooks ════
        // PBC[+0x70] = physics world container
        // Chain: pbc[+0x70][+0xB8][+0x5B8][idx*8][+0xA0] = ball_struct
        yup["GetCapturedPBC"] = []() -> uintptr_t
        {
            return s_captured_pbc.load();
        };

        yup["ResetCapturedPBC"] = []()
        {
            s_captured_pbc.store(0);
            logger::log_info("YUP", "PBC capture reset");
        };

        // Walk the PBC chain and capture ball_struct
        yup["CaptureBallStructFromPBC"] = [](sol::optional<int> ballIdx) -> uintptr_t
        {
            uintptr_t pbc = s_captured_pbc.load();
            if (!pbc) {
                logger::log_warn("YUP", "CaptureBallStructFromPBC: no PBC captured yet (hook BallAttach/Detach first)");
                return 0;
            }
            int idx = ballIdx.value_or(0);
            uintptr_t bs = 0;
            uintptr_t p70=0, pB8=0, p5B8=0, ph=0;
            safe_call::execute([&]() {
                p70  = *reinterpret_cast<uintptr_t*>(pbc + 0x70);
                if (p70 > 0x100000UL) {
                    pB8  = *reinterpret_cast<uintptr_t*>(p70 + 0xB8);
                    if (pB8 > 0x100000UL) {
                        p5B8 = *reinterpret_cast<uintptr_t*>(pB8 + 0x5B8);
                        if (p5B8 > 0x100000UL) {
                            ph   = *reinterpret_cast<uintptr_t*>(p5B8 + (uintptr_t)(idx * 8));
                            if (ph > 0x100000UL) {
                                bs = *reinterpret_cast<uintptr_t*>(ph + 0xA0);
                            }
                        }
                    }
                }
            }, "YUP::CaptureBallStructFromPBC");
            logger::log_info("YUP",
                "CaptureBallStructFromPBC: pbc=0x%lX p70=0x%lX pB8=0x%lX p5B8=0x%lX ph=0x%lX bs=0x%lX",
                (unsigned long)pbc, (unsigned long)p70, (unsigned long)pB8,
                (unsigned long)p5B8, (unsigned long)ph, (unsigned long)bs);
            if (bs > 0x100000UL) {
                s_captured_ball_struct.store(bs);
                logger::log_info("YUP", "*** ball_struct CAPTURED from PBC chain: 0x%lX ***", (unsigned long)bs);
            }
            return bs;
        };

        // ═══ GetLastBallStruct — live ball_struct from GetSpeed hook ════════
        // Populated by hooked_GetSpeed() when the YUP engine calls BallGetSpeed.
        // Call YUP.GetBallSpeed() to trigger it if the table is active.
        // Once captured, scan offsets for the UE ball position (XYZ floats).
        yup["GetLastBallStruct"] = []() -> uintptr_t
        {
            return s_captured_ball_struct.load();
        };
        yup["GetLastGetSpeedArg1"] = []() -> uintptr_t
        {
            return s_captured_getspeed_arg1.load();
        };

        // ═══ ScanBallStructForXYZ — scan ball_struct for a float triple ═══
        // Usage: YUP.ScanBallStructForXYZ(x, y, z, tolerance_opt)
        // Returns: byte offset of the X float, or -1 if not found
        yup["ScanBallStructForXYZ"] = [](float tx, float ty, float tz, sol::optional<float> tol_opt) -> int
        {
            uintptr_t bs = s_captured_ball_struct.load();
            if (!bs) return -1;
            float tol = tol_opt.value_or(1.0f);
            int result_off = -1;
            safe_call::execute([&]() {
                for (int off = 0; off <= 0x300 - 12; off += 4)
                {
                    float fx = *reinterpret_cast<float*>(bs + off);
                    float fy = *reinterpret_cast<float*>(bs + off + 4);
                    float fz = *reinterpret_cast<float*>(bs + off + 8);
                    auto close = [&](float a, float b) { return std::abs(a - b) <= tol; };
                    if (close(fx, tx) && close(fy, ty) && close(fz, tz))
                    {
                        logger::log_info("YUP", "*** BALL_STRUCT POSITION OFFSET: +0x%X → (%.3f, %.3f, %.3f) ***",
                            off, fx, fy, fz);
                        result_off = off;
                        break;
                    }
                }
                if (result_off < 0)
                    logger::log_warn("YUP", "ScanBallStructForXYZ: no match for (%.3f, %.3f, %.3f) tol=%.3f in 0x%lX",
                        tx, ty, tz, tol, (unsigned long)bs);
            }, "ScanBallStructForXYZ");
            return result_off;
        };

        // ═══ CaptureBallStructFromTableWorld ═══════════════════════════════
        // Walk: tw→[+0x550]→[+0xB8]→[+0x5B8]→[idx*8]→[+0xA0] = ball_struct
        // Bypasses the BallInformator[+8] problem (only valid during active table).
        // Call this while a table is loaded to populate s_captured_ball_struct.
        yup["CaptureBallStructFromTableWorld"] = [](sol::optional<int> ballIdx) -> uintptr_t
        {
            uintptr_t base = reinterpret_cast<uintptr_t>(symbols::get_lib_base());
            if (!base) return 0;
            uintptr_t tw = 0;
            auto r0 = safe_call::execute([&]() {
                tw = *reinterpret_cast<uintptr_t*>(base + 0x768A358UL);
            }, "YUP::CaptureBall::getTW");
            if (!r0.ok || !tw) {
                logger::log_warn("YUP", "CaptureBallStructFromTableWorld: no TableWorld (no table active?)");
                return 0;
            }
            int idx = ballIdx.value_or(0);
            uintptr_t bs = 0;
            uintptr_t p1=0, p2=0, p3=0, p4=0, p5=0;
            safe_call::execute([&]() {
                p1 = *reinterpret_cast<uintptr_t*>(tw + 0x550);
                if (p1 > 0x100000) {
                    p2 = *reinterpret_cast<uintptr_t*>(p1 + 0xB8);
                    if (p2 > 0x100000) {
                        p3 = *reinterpret_cast<uintptr_t*>(p2 + 0x5B8);
                        if (p3 > 0x100000) {
                            p4 = *reinterpret_cast<uintptr_t*>(p3 + (uintptr_t)(idx * 8));
                            if (p4 > 0x100000) {
                                p5 = *reinterpret_cast<uintptr_t*>(p4 + 0xA0);
                                if (p5 > 0x100000) bs = p5;
                            }
                        }
                    }
                }
            }, "YUP::CaptureBall::walk");
            logger::log_info("YUP",
                "CaptureBallStructFromTableWorld: tw=0x%lX p1=0x%lX p2=0x%lX p3=0x%lX p4=0x%lX p5=0x%lX bs=0x%lX",
                (unsigned long)tw, (unsigned long)p1, (unsigned long)p2,
                (unsigned long)p3, (unsigned long)p4, (unsigned long)p5, (unsigned long)bs);
            if (bs) {
                s_captured_ball_struct.store(bs);
                logger::log_info("YUP", "*** ball_struct CAPTURED from TableWorld chain: 0x%lX ***", (unsigned long)bs);
            }
            return bs;
        };

        // ═══ DirectReadBallStructFloat / DirectWriteBallStructFloat ════════
        // After ScanBallStructForXYZ confirms the offset, read/write live.
        yup["ReadBallStructFloat"] = [](int off) -> float
        {
            uintptr_t bs = s_captured_ball_struct.load();
            if (!bs) return 0.0f;
            float v = 0.0f;
            safe_call::execute([&]() {
                v = *reinterpret_cast<float*>(bs + off);
            }, "ReadBallStructFloat");
            return v;
        };
        yup["WriteBallStructFloat"] = [](int off, float val)
        {
            uintptr_t bs = s_captured_ball_struct.load();
            if (!bs) return;
            safe_call::execute([&]() {
                *reinterpret_cast<float*>(bs + off) = val;
            }, "WriteBallStructFloat");
        };
        // Dump 0x100 bytes of ball_struct as floats for manual inspection
        yup["DumpBallStruct"] = [](sol::this_state ts) -> sol::table
        {
            sol::state_view L(ts);
            sol::table t = L.create_table();
            uintptr_t bs = s_captured_ball_struct.load();
            if (!bs) return t;
            safe_call::execute([&]() {
                for (int off = 0; off <= 0x200; off += 4)
                {
                    float v = *reinterpret_cast<float*>(bs + off);
                    t[off] = v;
                }
            }, "DumpBallStruct");
            return t;
        };

        // ═══ TABLE WORLD — the YUP physics world object ═════════════════════
        // Global pointer at base + 0x768A358 holds the table world.
        // This is the object that the game's pause getter/setter/tick use.
        // Key offsets on the table world:
        //   +0x4F4  float  — speed multiplier
        //   +0x700  byte   — pause request flag
        //   +0x701  byte   — pause state
        //   +0x754  byte   — god mode (SetGodMode target via TDI context)
        //   +0x756  byte   — god mode flag (read by tick function)
        //   +0x3B8  ptr    — sub-object (valid only when table active)
        //   +0x570  ptr    — ball data (valid only when table active)
        //   +0x9C4  float  — speed multiplier 2
        constexpr uintptr_t TABLE_WORLD_GLOBAL_OFFSET = 0x768A358;

        yup["GetTableWorld"] = []() -> uintptr_t
        {
            uintptr_t base = reinterpret_cast<uintptr_t>(symbols::get_lib_base());
            if (!base)
                return 0;
            uintptr_t globalAddr = base + TABLE_WORLD_GLOBAL_OFFSET;
            uintptr_t tw = 0;
            auto r = safe_call::execute([&]()
                                        { tw = *reinterpret_cast<uintptr_t *>(globalAddr); }, "YUP::GetTableWorld");
            if (!r.ok || !tw)
            {
                logger::log_warn("YUP", "GetTableWorld: NULL (no table active?)");
                return (uintptr_t)0;
            }
            return tw;
        };

        // ─── Direct Pause Control ───────────────────────────────────────────
        yup["DirectSetPause"] = [](bool paused) -> bool
        {
            uintptr_t base = reinterpret_cast<uintptr_t>(symbols::get_lib_base());
            if (!base)
                return false;
            uintptr_t tw = 0;
            auto r = safe_call::execute([&]()
                                        { tw = *reinterpret_cast<uintptr_t *>(base + TABLE_WORLD_GLOBAL_OFFSET); }, "YUP::DirectSetPause::read");
            if (!r.ok || !tw)
                return false;
            auto r2 = safe_call::execute([&]()
                                         {
                *reinterpret_cast<uint8_t *>(tw + 0x700) = paused ? 1 : 0;
                *reinterpret_cast<uint8_t *>(tw + 0x701) = paused ? 1 : 0; }, "YUP::DirectSetPause::write");
            if (r2.ok)
                logger::log_info("YUP", "DirectSetPause(%s) OK @ tw=0x%lX", paused ? "true" : "false", (unsigned long)tw);
            return r2.ok;
        };

        yup["DirectGetPause"] = []() -> int
        {
            uintptr_t base = reinterpret_cast<uintptr_t>(symbols::get_lib_base());
            if (!base)
                return -1;
            uintptr_t tw = 0;
            int result = -1;
            auto r = safe_call::execute([&]()
                                        {
                tw = *reinterpret_cast<uintptr_t *>(base + TABLE_WORLD_GLOBAL_OFFSET);
                if (tw) result = *reinterpret_cast<uint8_t *>(tw + 0x701); }, "YUP::DirectGetPause");
            return r.ok ? result : -1;
        };

        // ─── Direct God Mode Control ────────────────────────────────────────
        yup["DirectSetGodMode"] = [](bool enabled) -> bool
        {
            uintptr_t base = reinterpret_cast<uintptr_t>(symbols::get_lib_base());
            if (!base)
                return false;
            uintptr_t tw = 0;
            auto r = safe_call::execute([&]()
                                        { tw = *reinterpret_cast<uintptr_t *>(base + TABLE_WORLD_GLOBAL_OFFSET); }, "YUP::DirectSetGodMode::read");
            if (!r.ok || !tw)
                return false;
            auto r2 = safe_call::execute([&]()
                                         {
                uint8_t val = enabled ? 1 : 0;
                *reinterpret_cast<uint8_t *>(tw + 0x754) = val;
                *reinterpret_cast<uint8_t *>(tw + 0x756) = val; }, "YUP::DirectSetGodMode::write");
            if (r2.ok)
                logger::log_info("YUP", "DirectSetGodMode(%s) OK", enabled ? "true" : "false");
            return r2.ok;
        };

        yup["DirectGetGodMode"] = []() -> int
        {
            uintptr_t base = reinterpret_cast<uintptr_t>(symbols::get_lib_base());
            if (!base)
                return -1;
            uintptr_t tw = 0;
            int result = -1;
            auto r = safe_call::execute([&]()
                                        {
                tw = *reinterpret_cast<uintptr_t *>(base + TABLE_WORLD_GLOBAL_OFFSET);
                if (tw) result = *reinterpret_cast<uint8_t *>(tw + 0x756); }, "YUP::DirectGetGodMode");
            return r.ok ? result : -1;
        };

        // ─── Direct Speed Control ───────────────────────────────────────────
        yup["DirectSetSpeed"] = [](float speed) -> bool
        {
            uintptr_t base = reinterpret_cast<uintptr_t>(symbols::get_lib_base());
            if (!base)
                return false;
            uintptr_t tw = 0;
            auto r = safe_call::execute([&]()
                                        { tw = *reinterpret_cast<uintptr_t *>(base + TABLE_WORLD_GLOBAL_OFFSET); }, "YUP::DirectSetSpeed::read");
            if (!r.ok || !tw)
                return false;
            auto r2 = safe_call::execute([&]()
                                         { *reinterpret_cast<float *>(tw + 0x4F4) = speed; }, "YUP::DirectSetSpeed::write");
            if (r2.ok)
                logger::log_info("YUP", "DirectSetSpeed(%.2f) OK", speed);
            return r2.ok;
        };

        yup["DirectGetSpeed"] = []() -> float
        {
            uintptr_t base = reinterpret_cast<uintptr_t>(symbols::get_lib_base());
            if (!base)
                return -1.0f;
            uintptr_t tw = 0;
            float result = -1.0f;
            auto r = safe_call::execute([&]()
                                        {
                tw = *reinterpret_cast<uintptr_t *>(base + TABLE_WORLD_GLOBAL_OFFSET);
                if (tw) result = *reinterpret_cast<float *>(tw + 0x4F4); }, "YUP::DirectGetSpeed");
            return r.ok ? result : -1.0f;
        };

        // ═══ MEMORY INSPECTION — walk raw singleton pointers ════════════════

        yup["ReadSingletonPtr"] = [](int offset) -> uintptr_t
        {
            void *singleton = get_singleton("ReadSingletonPtr");
            if (!singleton)
                return 0;
            uintptr_t result = 0;
            auto r = safe_call::execute([&]()
                                        {
                uintptr_t base = reinterpret_cast<uintptr_t>(singleton);
                result = *reinterpret_cast<uintptr_t *>(base + offset); }, "YUP::ReadSingletonPtr");
            if (!r.ok)
            {
                logger::log_error("YUP", "ReadSingletonPtr(+0x%X): CRASHED", offset);
                return (uintptr_t)0;
            }
            return result;
        };

        yup["ReadSingletonByte"] = [](int offset) -> int
        {
            void *singleton = get_singleton("ReadSingletonByte");
            if (!singleton)
                return -1;
            int result = -1;
            auto r = safe_call::execute([&]()
                                        {
                uintptr_t base = reinterpret_cast<uintptr_t>(singleton);
                result = *reinterpret_cast<uint8_t *>(base + offset); }, "YUP::ReadSingletonByte");
            if (!r.ok)
                return -1;
            return result;
        };

        // Read a pointer at any address (for chain walking)
        yup["ReadPtrAt"] = [](uintptr_t addr, int offset) -> uintptr_t
        {
            if (!addr)
                return 0;
            uintptr_t result = 0;
            auto r = safe_call::execute([&]()
                                        { result = *reinterpret_cast<uintptr_t *>(addr + offset); }, "YUP::ReadPtrAt");
            if (!r.ok)
                return 0;
            return result;
        };

        yup["ReadByteAt"] = [](uintptr_t addr, int offset) -> int
        {
            if (!addr)
                return -1;
            int result = -1;
            auto r = safe_call::execute([&]()
                                        { result = *reinterpret_cast<uint8_t *>(addr + offset); }, "YUP::ReadByteAt");
            if (!r.ok)
                return -1;
            return result;
        };

        yup["WriteByteAt"] = [](uintptr_t addr, int offset, int value) -> bool
        {
            if (!addr)
                return false;
            auto r = safe_call::execute([&]()
                                        { *reinterpret_cast<uint8_t *>(addr + offset) = (uint8_t)value; }, "YUP::WriteByteAt");
            return r.ok;
        };

        yup["ReadFloatAt"] = [](uintptr_t addr, int offset) -> float
        {
            if (!addr)
                return -1.0f;
            float result = -1.0f;
            auto r = safe_call::execute([&]()
                                        { result = *reinterpret_cast<float *>(addr + offset); }, "YUP::ReadFloatAt");
            if (!r.ok)
                return -1.0f;
            return result;
        };

        yup["WriteFloatAt"] = [](uintptr_t addr, int offset, float value) -> bool
        {
            if (!addr)
                return false;
            auto r = safe_call::execute([&]()
                                        { *reinterpret_cast<float *>(addr + offset) = value; }, "YUP::WriteFloatAt");
            return r.ok;
        };

        // ═══ DEBUG / INTROSPECTION ══════════════════════════════════════════

        yup["ListFunctions"] = [&lua]() -> sol::table
        {
            sol::table t = lua.create_table();
            struct E
            {
                const char *n;
                void *p;
            };
            E entries[] = {
                {"SetPause", (void *)s_yup.SetPause},
                {"SetGodMode", (void *)s_yup.SetGodMode},
                {"PauseToggle", (void *)s_yup.PauseToggle},
                {"ToggleGodMode", (void *)s_yup.ToggleGodMode},
                {"SetSpeed", (void *)s_yup.SetSpeed},
                {"DumpAction", (void *)s_yup.DumpAction},
                {"DebugLoad", (void *)s_yup.DebugLoad},
                {"BallAttach", (void *)s_yup.BallAttach},
                {"BallDetach", (void *)s_yup.BallDetach},
                {"BallReset", (void *)s_yup.BallReset},
                {"BallGetSingleton", (void *)s_yup.BallGetSingleton},
                {"BallGetSpeed", (void *)s_yup.BallGetSpeed},
                {"AwardUnlockTable", (void *)s_yup.AwardUnlockTable},
                {"AwardLockTable", (void *)s_yup.AwardLockTable},
            };
            for (auto &e : entries)
            {
                if (e.p)
                {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "0x%lX", (unsigned long)(uintptr_t)e.p);
                    t[e.n] = std::string(buf);
                }
                else
                {
                    t[e.n] = sol::nil;
                }
            }
            return t;
        };

        // Safe probe: test native calls without Lua-side crash
        yup["Probe"] = [](const std::string &name) -> std::string
        {
            if (name == "singleton")
            {
                void *p = get_singleton("Probe");
                char buf[64];
                snprintf(buf, sizeof(buf), "singleton=%s (0x%lX)",
                         p ? "OK" : "NULL", (unsigned long)(uintptr_t)p);
                return buf;
            }
            if (name == "speed")
            {
                void *s = get_singleton("Probe");
                if (!s)
                    return "singleton=NULL (no table?)";
                int32_t idx = 0;
                float v = safe_yup_ret<float>(
                    "BallGetSpeed", -1.0f, s_yup.BallGetSpeed, s, &idx);
                char buf[64];
                snprintf(buf, sizeof(buf), "speed=%.4f", v);
                return buf;
            }
            if (name == "walk")
            {
                void *s = get_singleton("Probe");
                if (!s)
                    return "singleton=NULL";
                char buf[512];
                int pos = 0;
                pos += snprintf(buf + pos, sizeof(buf) - pos,
                                "singleton=0x%lX\n", (unsigned long)(uintptr_t)s);
                int offsets[] = {0, 8, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50};
                for (int off : offsets)
                {
                    uintptr_t val = 0;
                    auto r = safe_call::execute([&]()
                                                { val = *reinterpret_cast<uintptr_t *>(
                                                      reinterpret_cast<uintptr_t>(s) + off); }, "Probe::walk");
                    if (r.ok)
                        pos += snprintf(buf + pos, sizeof(buf) - pos,
                                        "+0x%02X = 0x%lX\n", off, (unsigned long)val);
                    else
                        pos += snprintf(buf + pos, sizeof(buf) - pos,
                                        "+0x%02X = CRASH\n", off);
                }
                return std::string(buf, pos);
            }
            return "probes: singleton, speed, walk";
        };

        logger::log_info("YUP", "YUP API registered — %d/14 functions, signal-safe", s_yup.count);
    }

} // namespace lua_yup
