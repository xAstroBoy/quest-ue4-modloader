// modloader/src/hook/process_event_hook.cpp
// ProcessEvent Dobby hook with BLOCK support — O(1) dispatch via hashmap
// Dispatches pre/post callbacks to registered Lua hooks
// Returning true from a pre-hook cancels the ProcessEvent call entirely

#include "modloader/process_event_hook.h"
#include "modloader/pe_trace.h"
#include "modloader/symbols.h"
#include "modloader/logger.h"
#include "modloader/reflection_walker.h"
#include "modloader/class_rebuilder.h"
#include "modloader/lua_delayed_actions.h"
#include "modloader/lua_ue4ss_globals.h"
#include "modloader/types.h"
#include <dobby.h>
#include <unordered_map>
#include <shared_mutex>
#include <atomic>
#include <cstring>

namespace pe_hook
{

    // ═══ Defense-in-depth path normalization ════════════════════════════════
    // Strips /Script/Module.ClassName:FuncName → ClassName:FuncName
    // This catches any path that wasn't normalized by the Lua binding layer.
    static std::string normalize_func_path(const std::string &raw)
    {
        if (raw.empty() || raw[0] != '/')
            return raw; // already short
        if (raw.compare(0, 8, "/Script/") != 0 &&
            raw.compare(0, 6, "/Game/") != 0)
            return raw;

        size_t colon = raw.rfind(':');
        if (colon == std::string::npos)
        {
            size_t dot = raw.rfind('.');
            return (dot != std::string::npos) ? raw.substr(dot + 1) : raw;
        }
        std::string path_part = raw.substr(0, colon);
        std::string func_part = raw.substr(colon + 1);
        size_t dot = path_part.rfind('.');
        std::string class_part = (dot != std::string::npos) ? path_part.substr(dot + 1) : path_part;
        return class_part + ":" + func_part;
    }

    // Original ProcessEvent function pointer (set by Dobby)
    static ue::ProcessEventFn s_original = nullptr;

    // Hook registration
    struct HookEntry
    {
        HookId id;
        bool is_pre;
        std::string func_path;
        ue::UFunction *func_ptr; // direct pointer match (faster)
        PreCallback pre_cb;
        PostCallback post_cb;
    };

    // ═══ O(1) dispatch tables: UFunction* → callbacks ═══════════════════════
    // These are the HOT PATH — looked up on every ProcessEvent call via hash
    struct ResolvedCallbacks
    {
        std::vector<PreCallback> pre;
        std::vector<PostCallback> post;
        std::vector<HookId> pre_ids;
        std::vector<HookId> post_ids;
    };
    static std::unordered_map<ue::UFunction *, ResolvedCallbacks> s_resolved;

    // Unresolved hooks — checked lazily, NOT on every call
    struct UnresolvedHook
    {
        HookId id;
        bool is_pre;
        std::string func_path;
        PreCallback pre_cb;
        PostCallback post_cb;
        int resolve_attempts;
    };
    static std::vector<UnresolvedHook> s_unresolved;

    // Global hooks (fire on every call — keep minimal)
    static std::vector<HookEntry> s_global_pre_hooks;
    static std::vector<HookEntry> s_global_post_hooks;

    // Single shared_mutex: read-lock for hot path, write-lock for registration
    static std::shared_mutex s_hooks_rwlock;
    static std::atomic<HookId> s_next_id{1};
    static std::atomic<uint64_t> s_call_count{0};

    // Game thread callback queue (drained on every ProcessEvent call)
    static std::vector<std::function<void()>> s_game_thread_queue;
    static std::mutex s_game_thread_mutex;

    // Note: s_lazy_resolve_counter and LAZY_RESOLVE_INTERVAL removed — replaced by
    // live PE name matching on every call (try_to_lock, zero GUObjectArray scans)

    // Cache: func_path → UFunction* (used during registration only, not hot path)
    static std::unordered_map<std::string, ue::UFunction *> s_path_cache;

    // ═══ Resolve a "ClassName:FunctionName" path ════════════════════════════
    // Called during registration and lazy resolution — NOT on every PE call
    ue::UFunction *resolve_func_path(const std::string &path)
    {
        auto it = s_path_cache.find(path);
        if (it != s_path_cache.end())
            return it->second;

        // Parse "ClassName:FunctionName"
        size_t sep = path.find(':');
        if (sep == std::string::npos)
        {
            // Name-only path (e.g. "Tick") — cannot resolve to a specific UFunction*
            return nullptr;
        }

        std::string class_name = path.substr(0, sep);
        std::string func_name = path.substr(sep + 1);

        // Find the class
        ue::UClass *cls = reflection::find_class_ptr(class_name);
        if (!cls)
            return nullptr;

        // Walk functions to find the match
        ue::UField *child = ue::ustruct_get_children(reinterpret_cast<const ue::UStruct *>(cls));
        while (child && ue::is_valid_ptr(child))
        {
            std::string name = reflection::get_short_name(reinterpret_cast<const ue::UObject *>(child));
            if (name == func_name)
            {
                ue::UFunction *func = reinterpret_cast<ue::UFunction *>(child);
                s_path_cache[path] = func;
                return func;
            }
            child = ue::ufield_get_next(child);
        }

        // Check parent classes
        ue::UStruct *super = ue::ustruct_get_super(reinterpret_cast<const ue::UStruct *>(cls));
        while (super && ue::is_valid_ptr(super))
        {
            ue::UField *sc = ue::ustruct_get_children(super);
            while (sc && ue::is_valid_ptr(sc))
            {
                std::string name = reflection::get_short_name(reinterpret_cast<const ue::UObject *>(sc));
                if (name == func_name)
                {
                    ue::UFunction *func = reinterpret_cast<ue::UFunction *>(sc);
                    s_path_cache[path] = func;
                    return func;
                }
                sc = ue::ufield_get_next(sc);
            }
            super = ue::ustruct_get_super(super);
        }

        return nullptr;
    }

    // ═══ Try to resolve unresolved hooks (called infrequently) ══════════════
    static void try_resolve_pending()
    {
        // This runs under a WRITE lock — safe to modify s_resolved and s_unresolved
        auto it = s_unresolved.begin();
        while (it != s_unresolved.end())
        {
            ue::UFunction *ptr = resolve_func_path(it->func_path);
            if (ptr)
            {
                // Resolved! Move to the O(1) dispatch table
                auto &cbs = s_resolved[ptr];
                if (it->is_pre)
                {
                    cbs.pre.push_back(it->pre_cb);
                    cbs.pre_ids.push_back(it->id);
                }
                else
                {
                    cbs.post.push_back(it->post_cb);
                    cbs.post_ids.push_back(it->id);
                }
                logger::log_info("HOOK", "Lazy-resolved hook #%lu: %s -> 0x%lX",
                                 (unsigned long)it->id, it->func_path.c_str(),
                                 reinterpret_cast<uintptr_t>(ptr));
                it = s_unresolved.erase(it);
            }
            else
            {
                it->resolve_attempts++;
                ++it;
            }
        }
    }

    // ═══ The hooked ProcessEvent — OPTIMIZED HOT PATH ══════════════════════
    static void hooked_process_event(ue::UObject *self, ue::UFunction *func, void *parms)
    {
        s_call_count.fetch_add(1, std::memory_order_relaxed);

        // PE trace logger (atomic early-out if inactive — ~1 cycle cost when off)
        pe_trace::record(self, func);

        // Capture game thread ID on first call (UE4 always calls ProcessEvent on game thread)
        {
            static std::atomic<bool> s_game_thread_captured{false};
            if (!s_game_thread_captured.exchange(true, std::memory_order_relaxed))
            {
                lua_ue4ss_globals::set_game_thread_id();
            }
        }

        // ── Deferred GWorld resolution via GUObjectArray ─────────────────────
        // When the GWorld symbol is stripped (e.g. UE5 stripped binaries like PFX VR),
        // resolve it by finding the live UWorld instance through GUObjectArray.
        // This runs cheaply: only when GWorld is null, throttled to once per 500 PE calls.
        {
            static std::atomic<bool> s_gworld_resolved{false};
            static std::atomic<int> s_gworld_attempt_counter{0};
            static ue::UObject *s_gworld_holder = nullptr; // static storage for double-indirect

            if (!s_gworld_resolved.load(std::memory_order_relaxed) && !symbols::GWorld)
            {
                int counter = s_gworld_attempt_counter.fetch_add(1, std::memory_order_relaxed);
                // Try every 500 PE calls (not every call — too expensive)
                if (counter % 500 == 499)
                {
                    auto instances = reflection::find_all_instances("World");
                    if (!instances.empty())
                    {
                        s_gworld_holder = instances[0];
                        // GWorld is used as UObject** (double-indirect: *GWorld == UWorld*)
                        symbols::GWorld = &s_gworld_holder;
                        s_gworld_resolved.store(true, std::memory_order_release);
                        logger::log_info("HOOK", "GWorld resolved from GUObjectArray: UWorld*=%p (holder at %p)",
                                         (void *)s_gworld_holder, (void *)&s_gworld_holder);
                    }
                }
            }
        }

        // Drain game thread queue (ExecuteInGameThread + CallBg callbacks)
        // THROTTLED: Process at most N items per PE tick to avoid ANR.
        // With 5000+ CallBg items, draining all at once would freeze the game.
        // At 16 items/tick and ~60fps, 5000 items takes ~5.2 seconds = safe.
        // Increased from 4 to 16 to improve bridge response latency.
        //
        // Re-entrancy guard: callbacks may call original ProcessEvent which
        // triggers nested hooked_process_event. Without this guard, nested
        // calls would re-drain recursively → stack overflow.
        //
        // CRITICAL: tick_game_thread() and rebuilder::tick() are called INSIDE
        // the s_draining guard. This ensures that when LoopAsync/delayed action
        // callbacks call pe_fn() → nested hooked_process_event, the drain is
        // SKIPPED (s_draining=true), matching the behavior when bridge callbacks
        // call pe_fn() (bridge runs inside drain, so s_draining is also true).
        // Without this, nested PE calls from LoopAsync would re-drain the queue,
        // potentially executing bridge/CallBg items mid-callback and corrupting
        // the sigsetjmp crash guard's global jmp_buf.
        {
            static thread_local bool s_draining = false;
            static constexpr int MAX_PER_TICK = 16;
            if (!s_draining)
            {
                s_draining = true;
                std::vector<std::function<void()>> batch;
                batch.reserve(MAX_PER_TICK);
                {
                    std::lock_guard<std::mutex> lock(s_game_thread_mutex);
                    int count = std::min(static_cast<int>(s_game_thread_queue.size()), MAX_PER_TICK);
                    if (count > 0)
                    {
                        batch.assign(
                            std::make_move_iterator(s_game_thread_queue.begin()),
                            std::make_move_iterator(s_game_thread_queue.begin() + count));
                        s_game_thread_queue.erase(s_game_thread_queue.begin(),
                                                  s_game_thread_queue.begin() + count);
                    }
                }
                for (auto &fn : batch)
                {
                    fn();
                }

                // Tick delayed actions (ExecuteWithDelay, LoopAsync, etc.)
                // MUST be inside s_draining guard — see comment above.
                lua_delayed::tick_game_thread();

                // Notify class rebuilder for instance tracking (lightweight check)
                rebuilder::tick(self, func, parms);

                s_draining = false;
            }
        }

        bool blocked = false;

        // ── SNAPSHOT callbacks under read lock, execute OUTSIDE lock ─────────
        // CRITICAL FIX: Previously the read lock was held during the entire
        // callback execution (Lua pre-hooks → original PE → Lua post-hooks).
        // This caused WRITER STARVATION: the boot thread (registering hooks
        // via unique_lock/write) could never acquire the lock because the game
        // thread continuously held the shared/read lock across Lua callbacks.
        // With 22+ hooks registered, Lua execution took long enough that the
        // writer window was effectively zero → game froze during mod loading.
        //
        // Now we snapshot under lock (O(1) hashmap lookup + shallow copy of
        // 1-5 std::function objects = microseconds) and execute outside.
        {
            bool fast_path = true;
            std::vector<PreCallback> snap_pre;
            std::vector<PostCallback> snap_post;

            // ── Snapshot under read lock (brief hold) ───────────────────────
            {
                std::shared_lock<std::shared_mutex> rlock(s_hooks_rwlock);

                auto it = s_resolved.find(func);
                bool found = (it != s_resolved.end());
                bool has_globals = !s_global_pre_hooks.empty() ||
                                   !s_global_post_hooks.empty();

                if (found || has_globals)
                {
                    fast_path = false;

                    // Copy global pre-hooks (usually 0)
                    for (auto &h : s_global_pre_hooks)
                    {
                        if (h.pre_cb)
                            snap_pre.push_back(h.pre_cb);
                    }

                    // Copy resolved hooks for this UFunction*
                    if (found)
                    {
                        snap_pre.insert(snap_pre.end(),
                                        it->second.pre.begin(), it->second.pre.end());
                        snap_post.insert(snap_post.end(),
                                         it->second.post.begin(), it->second.post.end());
                    }

                    // Copy global post-hooks (usually 0)
                    for (auto &h : s_global_post_hooks)
                    {
                        if (h.post_cb)
                            snap_post.push_back(h.post_cb);
                    }
                }
            } // READ lock released — boot thread can now acquire WRITE lock

            // ── Execute callbacks WITHOUT any lock held ─────────────────────
            if (fast_path)
            {
                // No hooks at all — just call original (most common case)
                s_original(self, func, parms);
            }
            else
            {
                // Pre-hooks
                for (auto &cb : snap_pre)
                {
                    if (!cb)
                        continue; // defense-in-depth: skip null callbacks
                    if (cb(self, func, parms))
                    {
                        blocked = true;
                        break;
                    }
                }

                // Call original (unless BLOCKED)
                if (!blocked)
                {
                    s_original(self, func, parms);
                }

                // Post-hooks
                for (auto &cb : snap_post)
                {
                    if (!cb)
                        continue; // defense-in-depth: skip null callbacks
                    cb(self, func, parms);
                }
            }
        }

        // ── Live resolution of unresolved hooks ────────────────────────────
        // Matches EVERY PE call against pending hooks via fast name comparison.
        // Uses try_to_lock to avoid blocking — skips if lock is contended.
        // This replaces the old GUObjectArray scan which caused game freezes
        // (250ms+ under write lock when multiple hooks were unresolved).
        if (!s_unresolved.empty())
        {
            std::unique_lock<std::shared_mutex> wlock(s_hooks_rwlock, std::try_to_lock);
            if (wlock.owns_lock() && !s_unresolved.empty())
            {
                // Extract names from current PE call (fast FName lookups)
                std::string cur_func_name = reflection::get_short_name(
                    reinterpret_cast<const ue::UObject *>(func));

                std::string cur_owner_class;
                ue::UObject *func_outer = ue::uobj_get_outer(
                    reinterpret_cast<const ue::UObject *>(func));
                if (func_outer && ue::is_valid_ptr(func_outer))
                {
                    cur_owner_class = reflection::get_short_name(func_outer);
                }

                std::string obj_class_name;
                if (self)
                {
                    ue::UClass *obj_cls = ue::uobj_get_class(self);
                    if (obj_cls && ue::is_valid_ptr(obj_cls))
                    {
                        obj_class_name = reflection::get_short_name(
                            reinterpret_cast<const ue::UObject *>(obj_cls));
                    }
                }

                auto uit = s_unresolved.begin();
                while (uit != s_unresolved.end())
                {
                    bool resolved_this = false;
                    size_t sep = uit->func_path.find(':');

                    if (sep != std::string::npos)
                    {
                        // ClassName:FuncName hook — match against owner class OR object class
                        std::string hook_class = uit->func_path.substr(0, sep);
                        std::string hook_func = uit->func_path.substr(sep + 1);

                        if (hook_func == cur_func_name &&
                            (hook_class == cur_owner_class || hook_class == obj_class_name))
                        {
                            auto &cbs = s_resolved[func];
                            if (uit->is_pre)
                            {
                                cbs.pre.push_back(uit->pre_cb);
                                cbs.pre_ids.push_back(uit->id);
                            }
                            else
                            {
                                cbs.post.push_back(uit->post_cb);
                                cbs.post_ids.push_back(uit->id);
                            }
                            logger::log_info("HOOK",
                                             "Live-resolved hook #%lu: %s -> 0x%lX (obj:%s owner:%s)",
                                             (unsigned long)uit->id, uit->func_path.c_str(),
                                             reinterpret_cast<uintptr_t>(func),
                                             obj_class_name.c_str(), cur_owner_class.c_str());
                            uit = s_unresolved.erase(uit);
                            resolved_this = true;
                        }
                    }
                    else
                    {
                        // Name-only match
                        if (uit->func_path == cur_func_name ||
                            cur_func_name == ("Receive" + uit->func_path))
                        {
                            auto &cbs = s_resolved[func];
                            if (uit->is_pre)
                            {
                                cbs.pre.push_back(uit->pre_cb);
                                cbs.pre_ids.push_back(uit->id);
                            }
                            else
                            {
                                cbs.post.push_back(uit->post_cb);
                                cbs.post_ids.push_back(uit->id);
                            }
                            logger::log_info("HOOK",
                                             "Live-resolved name-only hook #%lu: '%s' -> 0x%lX",
                                             (unsigned long)uit->id, uit->func_path.c_str(),
                                             reinterpret_cast<uintptr_t>(func));
                            uit = s_unresolved.erase(uit);
                            resolved_this = true;
                        }
                    }

                    if (!resolved_this)
                        ++uit;
                }
            }
        }
    }

    // ═══ Initialization ═════════════════════════════════════════════════════
    void install()
    {
        if (!symbols::ProcessEvent)
        {
            logger::log_error("HOOK", "ProcessEvent not resolved — cannot install hook");
            return;
        }

        int status = DobbyHook(
            reinterpret_cast<void *>(symbols::ProcessEvent),
            reinterpret_cast<dobby_dummy_func_t>(hooked_process_event),
            reinterpret_cast<dobby_dummy_func_t *>(&s_original));

        if (status == 0)
        {
            logger::log_info("HOOK", "ProcessEvent hooked via Dobby at 0x%lX",
                             reinterpret_cast<uintptr_t>(symbols::ProcessEvent));
        }
        else
        {
            logger::log_error("HOOK", "Dobby failed to hook ProcessEvent (status=%d)", status);
        }
    }

    // ═══ Registration — insert into O(1) dispatch table or unresolved list ══
    HookId register_pre(const std::string &raw_path, PreCallback callback)
    {
        std::string func_path = normalize_func_path(raw_path);
        if (func_path != raw_path)
        {
            logger::log_info("HOOK", "register_pre: normalized '%s' → '%s'",
                             raw_path.c_str(), func_path.c_str());
        }
        std::unique_lock<std::shared_mutex> wlock(s_hooks_rwlock);
        HookId id = s_next_id.fetch_add(1);
        ue::UFunction *ptr = resolve_func_path(func_path);

        if (ptr)
        {
            auto &cbs = s_resolved[ptr];
            cbs.pre.push_back(callback);
            cbs.pre_ids.push_back(id);
            logger::log_info("HOOK", "Pre-hook #%lu registered: %s (resolved 0x%lX)",
                             (unsigned long)id, func_path.c_str(),
                             reinterpret_cast<uintptr_t>(ptr));
        }
        else
        {
            UnresolvedHook uh;
            uh.id = id;
            uh.is_pre = true;
            uh.func_path = func_path;
            uh.pre_cb = callback;
            uh.resolve_attempts = 0;
            s_unresolved.push_back(uh);
            logger::log_warn("HOOK", "Pre-hook #%lu registered: %s (lazy resolve — not yet found)",
                             (unsigned long)id, func_path.c_str());
        }
        return id;
    }

    HookId register_post(const std::string &raw_path, PostCallback callback)
    {
        std::string func_path = normalize_func_path(raw_path);
        if (func_path != raw_path)
        {
            logger::log_info("HOOK", "register_post: normalized '%s' → '%s'",
                             raw_path.c_str(), func_path.c_str());
        }
        std::unique_lock<std::shared_mutex> wlock(s_hooks_rwlock);
        HookId id = s_next_id.fetch_add(1);
        ue::UFunction *ptr = resolve_func_path(func_path);

        if (ptr)
        {
            auto &cbs = s_resolved[ptr];
            cbs.post.push_back(callback);
            cbs.post_ids.push_back(id);
            logger::log_info("HOOK", "Post-hook #%lu registered: %s (resolved 0x%lX)",
                             (unsigned long)id, func_path.c_str(),
                             reinterpret_cast<uintptr_t>(ptr));
        }
        else
        {
            UnresolvedHook uh;
            uh.id = id;
            uh.is_pre = false;
            uh.func_path = func_path;
            uh.post_cb = callback;
            uh.resolve_attempts = 0;
            s_unresolved.push_back(uh);
            logger::log_warn("HOOK", "Post-hook #%lu registered: %s (lazy resolve — not yet found)",
                             (unsigned long)id, func_path.c_str());
        }
        return id;
    }

    HookId register_pre_ptr(ue::UFunction *func, PreCallback callback)
    {
        std::unique_lock<std::shared_mutex> wlock(s_hooks_rwlock);
        HookId id = s_next_id.fetch_add(1);
        auto &cbs = s_resolved[func];
        cbs.pre.push_back(callback);
        cbs.pre_ids.push_back(id);
        return id;
    }

    HookId register_post_ptr(ue::UFunction *func, PostCallback callback)
    {
        std::unique_lock<std::shared_mutex> wlock(s_hooks_rwlock);
        HookId id = s_next_id.fetch_add(1);
        auto &cbs = s_resolved[func];
        cbs.post.push_back(callback);
        cbs.post_ids.push_back(id);
        return id;
    }

    HookId register_global_pre(PreCallback callback)
    {
        std::unique_lock<std::shared_mutex> wlock(s_hooks_rwlock);
        HookId id = s_next_id.fetch_add(1);
        HookEntry entry;
        entry.id = id;
        entry.is_pre = true;
        entry.pre_cb = callback;
        s_global_pre_hooks.push_back(entry);
        return id;
    }

    HookId register_global_post(PostCallback callback)
    {
        std::unique_lock<std::shared_mutex> wlock(s_hooks_rwlock);
        HookId id = s_next_id.fetch_add(1);
        HookEntry entry;
        entry.id = id;
        entry.is_pre = false;
        entry.post_cb = callback;
        s_global_post_hooks.push_back(entry);
        return id;
    }

    void unregister(HookId id)
    {
        std::unique_lock<std::shared_mutex> wlock(s_hooks_rwlock);
        // Remove from resolved dispatch tables
        for (auto &pair : s_resolved)
        {
            auto &cbs = pair.second;
            for (size_t i = 0; i < cbs.pre_ids.size(); ++i)
            {
                if (cbs.pre_ids[i] == id)
                {
                    cbs.pre.erase(cbs.pre.begin() + i);
                    cbs.pre_ids.erase(cbs.pre_ids.begin() + i);
                    return;
                }
            }
            for (size_t i = 0; i < cbs.post_ids.size(); ++i)
            {
                if (cbs.post_ids[i] == id)
                {
                    cbs.post.erase(cbs.post.begin() + i);
                    cbs.post_ids.erase(cbs.post_ids.begin() + i);
                    return;
                }
            }
        }
        // Remove from unresolved
        s_unresolved.erase(
            std::remove_if(s_unresolved.begin(), s_unresolved.end(),
                           [id](const UnresolvedHook &h)
                           { return h.id == id; }),
            s_unresolved.end());
        // Remove from globals
        auto remove_fn = [id](std::vector<HookEntry> &vec)
        {
            vec.erase(std::remove_if(vec.begin(), vec.end(),
                                     [id](const HookEntry &e)
                                     { return e.id == id; }),
                      vec.end());
        };
        remove_fn(s_global_pre_hooks);
        remove_fn(s_global_post_hooks);
    }

    uint64_t get_call_count()
    {
        return s_call_count.load(std::memory_order_relaxed);
    }

    std::vector<FuncStats> get_func_stats()
    {
        // No per-call tracking anymore — build stats from registered hooks
        std::shared_lock<std::shared_mutex> rlock(s_hooks_rwlock);
        std::vector<FuncStats> result;
        for (const auto &pair : s_resolved)
        {
            FuncStats fs;
            fs.name = reflection::get_short_name(reinterpret_cast<const ue::UObject *>(pair.first));
            fs.call_count = 0; // no longer tracked per-call for performance
            fs.pre_hook_count = pair.second.pre.size();
            fs.post_hook_count = pair.second.post.size();
            result.push_back(fs);
        }
        return result;
    }

    ue::ProcessEventFn get_original()
    {
        return s_original;
    }

    void queue_game_thread(std::function<void()> fn)
    {
        std::lock_guard<std::mutex> lock(s_game_thread_mutex);
        s_game_thread_queue.push_back(std::move(fn));
    }

} // namespace pe_hook
