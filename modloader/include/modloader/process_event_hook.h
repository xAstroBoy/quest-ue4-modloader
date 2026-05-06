#pragma once
// modloader/include/modloader/process_event_hook.h
// ProcessEvent Dobby hook with BLOCK support
// Dispatches pre/post callbacks to registered Lua hooks
// Returning "BLOCK" from a pre-hook cancels the ProcessEvent call entirely

#include <string>
#include <functional>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <mutex>
#include "modloader/types.h"

namespace pe_hook {

// Hook ID returned by register functions
using HookId = uint64_t;

// Pre-hook callback: (UObject* self, UFunction* func, void* parms) -> bool (true = BLOCK)
using PreCallback = std::function<bool(ue::UObject*, ue::UFunction*, void*)>;

// Post-hook callback: (UObject* self, UFunction* func, void* parms) -> void
using PostCallback = std::function<void(ue::UObject*, ue::UFunction*, void*)>;

// Install the Dobby hook on ProcessEvent
void install();

// Register a pre-hook for a specific UFunction path (e.g. "BP_PlayerCharacter_C:TakeDamage")
// Returns a hook ID for later removal
// The callback receives (self, func, parms). Return true to BLOCK (cancel) the call.
HookId register_pre(const std::string& func_path, PreCallback callback);

// Register a post-hook for a specific UFunction path
HookId register_post(const std::string& func_path, PostCallback callback);

// Register hooks for a specific UFunction pointer (faster matching, no name lookup per call)
HookId register_pre_ptr(ue::UFunction* func, PreCallback callback);
HookId register_post_ptr(ue::UFunction* func, PostCallback callback);

// Register a global pre/post hook that fires for ALL ProcessEvent calls
HookId register_global_pre(PreCallback callback);
HookId register_global_post(PostCallback callback);

// Remove a previously registered hook
void unregister(HookId id);

// Get the total number of ProcessEvent calls dispatched since boot
uint64_t get_call_count();

// Per-function statistics
struct FuncStats {
    std::string name;
    uint64_t    call_count;
    int         pre_hook_count;
    int         post_hook_count;
};

// Get per-function call statistics
std::vector<FuncStats> get_func_stats();

// Install the ProcessEvent hook using Dobby (legacy — may freeze)
void install();
// Install using manual ARM64 LDR+BR patch — bypasses Dobby entirely
void install_manual();

// Get the original (un-hooked) ProcessEvent function pointer
ue::ProcessEventFn get_original();
// Get the unhooked ProcessEvent pointer (saved before Dobby hook install).
// This is the REAL function address, unlike get_original() which returns
// a Dobby trampoline that may crash when called outside hook context.
ue::ProcessEventFn get_unhooked();

// Returns true if the calling thread is the game thread (the one that
// fired the first ProcessEvent hook callback). Returns false if the
// game thread hasn't been seen yet, or if called from any other thread.
// Use this before calling ProcessEvent from C++ code outside hook callbacks.
bool is_game_thread();

// Queue a function to run on the game thread (drained on every ProcessEvent call)
// This is the C++ backend for Lua's ExecuteInGameThread(fn)
void queue_game_thread(std::function<void()> fn);

// Resolve a "ClassName:FunctionName" path to a UFunction* (cached)
ue::UFunction* resolve_func_path(const std::string& path);

} // namespace pe_hook
