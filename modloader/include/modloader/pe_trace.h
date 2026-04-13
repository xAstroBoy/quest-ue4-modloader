#pragma once
// modloader/include/modloader/pe_trace.h
// ProcessEvent trace logger — toggle via ADB, anti-flood, writes to pe_trace.log
// Records unique OwnerClass:FuncName pairs with call counts for fast class discovery
//
// ENHANCED FEATURES:
//   - Per-function timing (min/max/avg execution time)
//   - Parameter data logging (optional — captures parms buffer for selected functions)
//   - Real-time Lua API (register_watch, unregister_watch, get_stats)
//   - Breakpoint-style tracing (pause on specific function call)

#include <string>
#include <functional>
#include <vector>
#include "modloader/types.h"

namespace pe_trace
{

    // ═══ Core trace control ═════════════════════════════════════════════════

    // Start tracing PE calls. Optional filter = substring match on class or func name.
    // If filter is empty, traces everything.
    void start(const std::string &filter = "");

    // Stop tracing (keeps accumulated data)
    void stop();

    // Clear all accumulated trace data
    void clear();

    // Record a PE call (called from hooked_process_event — cheap early-out if not active)
    void record(ue::UObject *self, ue::UFunction *func);

    // Record a PE call with parameter data and timing
    // Called from hooked_process_event when detailed tracing is enabled
    void record_detailed(ue::UObject *self, ue::UFunction *func,
                         void *parms, uint64_t exec_time_ns);

    // Check if tracing is active
    bool is_active();

    // Check if detailed (timed) tracing is enabled
    bool is_detailed();

    // Enable/disable detailed timing for all traced calls
    // When enabled, record_detailed() should be called instead of record()
    void set_detailed(bool enabled);

    // ═══ Output ═════════════════════════════════════════════════════════════

    // Get status string (active/inactive, duration, unique count, total count)
    std::string status();

    // Get top N entries sorted by call count, formatted as text
    std::string top(int n = 50);

    // Get top N entries sorted by total execution time
    std::string top_by_time(int n = 50);

    // Dump all accumulated data to pe_trace.log on device, returns path + summary
    std::string dump_to_file();

    // ═══ Per-function stats ═════════════════════════════════════════════════

    struct FunctionStats
    {
        std::string func_key; // "OwnerClass:FuncName"
        uint64_t call_count;
        uint64_t total_time_ns; // total execution time in nanoseconds
        uint64_t min_time_ns;
        uint64_t max_time_ns;
        double avg_time_ns;
        uint64_t first_tick;
        uint64_t last_tick;
    };

    // Get stats for a specific function (by key "OwnerClass:FuncName")
    bool get_function_stats(const std::string &func_key, FunctionStats &out);

    // Get stats for all functions
    std::vector<FunctionStats> get_all_stats();

    // ═══ Watch system (for Lua real-time monitoring) ═════════════════════════

    // Watch callback — called on every ProcessEvent for watched functions
    // Parameters: self, func, func_key, call_count
    using WatchCallback = std::function<void(ue::UObject *, ue::UFunction *,
                                             const std::string &, uint64_t)>;

    // Register a watch on a function pattern (substring match)
    // Returns watch ID (0 on failure)
    uint32_t register_watch(const std::string &pattern, WatchCallback callback);

    // Unregister a watch by ID
    void unregister_watch(uint32_t watch_id);

    // Unregister all watches
    void clear_watches();

} // namespace pe_trace
