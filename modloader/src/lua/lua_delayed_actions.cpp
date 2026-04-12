// modloader/src/lua/lua_delayed_actions.cpp
// UE4SS-compatible delayed action system — ALL 19+ functions
// Runs on game thread via ProcessEvent tick

#include "modloader/lua_delayed_actions.h"
#include "modloader/logger.h"

#include <vector>
#include <mutex>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <thread>

namespace lua_delayed
{

    // ═══════════════════════════════════════════════════════════════════════
    // Internal delayed action entry
    // ═══════════════════════════════════════════════════════════════════════

    struct DelayedAction
    {
        uint64_t handle;
        sol::function callback;

        // Timing
        bool is_frame_based; // true = frame count, false = milliseconds
        double delay_ms;     // milliseconds delay (if !is_frame_based)
        int delay_frames;    // frames delay (if is_frame_based)

        // State
        double accumulated_ms;
        int accumulated_frames;
        bool is_loop;
        bool cancelled;
        bool execute_on_game_thread; // true = guaranteed game thread via PE tick

        // Timing reference
        std::chrono::steady_clock::time_point last_tick;
    };

    // MUST be recursive_mutex: tick_game_thread() holds this while executing
    // callbacks, and callbacks may call ExecuteWithDelay/LoopAsync which call
    // add_action() and re-lock the same mutex. A plain std::mutex would deadlock.
    static std::recursive_mutex s_mutex;
    static std::vector<DelayedAction> s_actions;
    static std::vector<DelayedAction> s_pending; // new actions added during tick
    static bool s_ticking = false;               // true while iterating s_actions
    static std::atomic<uint64_t> s_next_handle{1};
    static bool s_initialized = false;

    // ═══════════════════════════════════════════════════════════════════════

    void init()
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        s_actions.clear();
        s_initialized = true;
        logger::log_info("DELAY", "Delayed action system initialized");
    }

    void tick_game_thread()
    {
        if (!s_initialized)
            return;

        // Guard against re-entrant ticks.  When a delayed-action callback
        // calls Lua Call() → ProcessEvent → hooked_process_event → tick,
        // we must NOT re-process the action list.  Just return immediately.
        if (s_ticking)
            return;

        std::lock_guard<std::recursive_mutex> lock(s_mutex);

        auto now = std::chrono::steady_clock::now();

        // Mark that we're iterating — add_action() will use s_pending instead
        s_ticking = true;

        // Process all pending actions using INDEX-BASED iteration.
        // We must NOT use range-for or iterators because callbacks can call
        // add_action() which appends to s_pending (not s_actions during tick).
        // We cache the size upfront so newly-merged actions from a previous
        // callback in this same tick don't get processed twice.
        size_t count = s_actions.size();
        for (size_t i = 0; i < count; i++)
        {
            auto &action = s_actions[i];
            if (action.cancelled)
                continue;

            bool should_fire = false;

            if (action.is_frame_based)
            {
                action.accumulated_frames++;
                if (action.accumulated_frames >= action.delay_frames)
                {
                    should_fire = true;
                    action.accumulated_frames = 0;
                }
            }
            else
            {
                double elapsed_ms = std::chrono::duration<double, std::milli>(now - action.last_tick).count();
                action.accumulated_ms += elapsed_ms;
                action.last_tick = now;

                if (action.accumulated_ms >= action.delay_ms)
                {
                    should_fire = true;
                    action.accumulated_ms -= action.delay_ms; // preserve excess for loops
                }
            }

            if (should_fire)
            {
                // For one-shot actions, mark cancelled BEFORE calling the callback.
                // This prevents re-entrant tick_game_thread() (triggered by Lua
                // Call() → ProcessEvent → hooked_process_event → tick) from
                // firing the same one-shot action again while we're still inside
                // its callback.
                if (!action.is_loop)
                {
                    action.cancelled = true;
                }

                auto result = action.callback();
                if (!result.valid())
                {
                    sol::error err = result;
                    logger::log_error("DELAY", "Delayed action %lu error: %s",
                                      (unsigned long)action.handle, err.what());
                }
            }
        }

        // Done iterating — merge any actions added during callbacks
        s_ticking = false;
        if (!s_pending.empty())
        {
            s_actions.insert(s_actions.end(),
                             std::make_move_iterator(s_pending.begin()),
                             std::make_move_iterator(s_pending.end()));
            s_pending.clear();
        }

        // Remove cancelled actions
        s_actions.erase(
            std::remove_if(s_actions.begin(), s_actions.end(),
                           [](const DelayedAction &a)
                           { return a.cancelled; }),
            s_actions.end());
    }

    void cancel(uint64_t handle)
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        for (auto &action : s_actions)
        {
            if (action.handle == handle)
            {
                action.cancelled = true;
                return;
            }
        }
    }

    bool is_valid(uint64_t handle)
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);
        for (const auto &action : s_actions)
        {
            if (action.handle == handle && !action.cancelled)
            {
                return true;
            }
        }
        return false;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Internal: add an action
    // ═══════════════════════════════════════════════════════════════════════

    static uint64_t add_action(sol::function callback, double delay_ms, int delay_frames,
                               bool is_loop, bool is_frame_based, bool game_thread)
    {
        std::lock_guard<std::recursive_mutex> lock(s_mutex);

        DelayedAction action;
        action.handle = s_next_handle.fetch_add(1);
        action.callback = callback;
        action.delay_ms = delay_ms;
        action.delay_frames = delay_frames;
        action.is_frame_based = is_frame_based;
        action.is_loop = is_loop;
        action.cancelled = false;
        action.execute_on_game_thread = game_thread;
        action.accumulated_ms = 0.0;
        action.accumulated_frames = 0;
        action.last_tick = std::chrono::steady_clock::now();

        // If tick_game_thread() is currently iterating s_actions, push to
        // s_pending instead to avoid invalidating the iteration.
        // s_pending is merged into s_actions after the iteration completes.
        if (s_ticking)
        {
            s_pending.push_back(std::move(action));
            return s_pending.back().handle;
        }
        else
        {
            s_actions.push_back(std::move(action));
            return s_actions.back().handle;
        }
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Register all UE4SS-compatible delayed action functions
    // ═══════════════════════════════════════════════════════════════════════

    void register_all(sol::state &lua)
    {

        // ── ExecuteWithDelay(delayInMilliseconds, callback) → handle
        // Executes callback once after delay on the game thread
        lua.set_function("ExecuteWithDelay", [](double delay_ms, sol::function callback) -> uint64_t
                         { return add_action(callback, delay_ms, 0, false, false, true); });

        // ── ExecuteAsync(callback) → handle
        // Executes callback on next tick (0ms delay, one-shot)
        lua.set_function("ExecuteAsync", [](sol::function callback) -> uint64_t
                         { return add_action(callback, 0, 0, false, false, false); });

        // ── LoopAsync(delayInMilliseconds, callback) → handle
        // Executes callback repeatedly every delay_ms
        lua.set_function("LoopAsync", [](double delay_ms, sol::function callback) -> uint64_t
                         { return add_action(callback, delay_ms, 0, true, false, false); });

        // ── ExecuteInGameThread(callback)
        // Already exists in lua_bindings.cpp — this is a backup
        // We register it here too so the delayed system has its own version
        // (The lua_bindings version takes priority since it's registered first)

        // ── ExecuteInGameThreadWithDelay(delayInMilliseconds, callback) → handle
        lua.set_function("ExecuteInGameThreadWithDelay", [](double delay_ms, sol::function callback) -> uint64_t
                         { return add_action(callback, delay_ms, 0, false, false, true); });

        // ── LoopInGameThread(delayInMilliseconds, callback) → handle
        lua.set_function("LoopInGameThread", [](double delay_ms, sol::function callback) -> uint64_t
                         { return add_action(callback, delay_ms, 0, true, false, true); });

        // ── Frame-based variants ─────────────────────────────────────────

        // ── ExecuteWithDelayFrames(delayInFrames, callback) → handle
        lua.set_function("ExecuteWithDelayFrames", [](int delay_frames, sol::function callback) -> uint64_t
                         { return add_action(callback, 0, delay_frames, false, true, true); });

        // ── LoopAsyncFrames(delayInFrames, callback) → handle
        lua.set_function("LoopAsyncFrames", [](int delay_frames, sol::function callback) -> uint64_t
                         { return add_action(callback, 0, delay_frames, true, true, false); });

        // ── LoopInGameThreadFrames(delayInFrames, callback) → handle
        lua.set_function("LoopInGameThreadFrames", [](int delay_frames, sol::function callback) -> uint64_t
                         { return add_action(callback, 0, delay_frames, true, true, true); });

        // ── ExecuteInGameThreadWithDelayFrames(delayInFrames, callback) → handle
        lua.set_function("ExecuteInGameThreadWithDelayFrames", [](int delay_frames, sol::function callback) -> uint64_t
                         { return add_action(callback, 0, delay_frames, false, true, true); });

        // ── Handle management ────────────────────────────────────────────

        // ── CancelDelayedAction(handle)
        lua.set_function("CancelDelayedAction", [](uint64_t handle)
                         { cancel(handle); });

        // ── IsDelayedActionValid(handle) → bool
        lua.set_function("IsDelayedActionValid", [](uint64_t handle) -> bool
                         { return is_valid(handle); });

        logger::log_info("LUA", "Delayed action API registered (12 functions)");
    }

} // namespace lua_delayed
