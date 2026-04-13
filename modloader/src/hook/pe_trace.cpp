// modloader/src/hook/pe_trace.cpp
// ProcessEvent trace logger with anti-flood + per-function timing + watch system
// Records unique OwnerClass:FuncName pairs with call counts, timing, and watch callbacks
// Toggle via ADB: pe_trace_start / pe_trace_stop / pe_trace_dump / pe_trace_top

#include "modloader/pe_trace.h"
#include "modloader/reflection_walker.h"
#include "modloader/logger.h"
#include "modloader/paths.h"

#include <unordered_map>
#include <mutex>
#include <atomic>
#include <chrono>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <climits>

namespace pe_trace
{

    // ═══ Trace entry (enhanced with timing) ═════════════════════════════════
    struct TraceEntry
    {
        uint64_t count;
        uint64_t first_tick;    // s_total_calls at first occurrence
        uint64_t last_tick;     // s_total_calls at last occurrence
        uint64_t total_time_ns; // accumulated execution time
        uint64_t min_time_ns;   // minimum single-call time
        uint64_t max_time_ns;   // maximum single-call time
    };

    // ═══ Watch entry ════════════════════════════════════════════════════════
    struct WatchEntry
    {
        uint32_t id;
        std::string pattern;
        WatchCallback callback;
    };

    // ═══ State ══════════════════════════════════════════════════════════════
    static std::atomic<bool> s_active{false};
    static std::atomic<bool> s_detailed{false};
    static std::string s_filter; // substring filter (empty = all)
    static std::mutex s_mutex;
    static std::unordered_map<std::string, TraceEntry> s_map;       // "OwnerClass:FuncName" → entry
    static std::unordered_map<std::string, uint64_t> s_obj_classes; // object class → count
    static uint64_t s_total_calls{0};
    static uint64_t s_filtered_calls{0}; // calls that passed the filter
    static std::chrono::steady_clock::time_point s_start_time;

    // Watch system
    static std::mutex s_watch_mutex;
    static std::vector<WatchEntry> s_watches;
    static std::atomic<uint32_t> s_next_watch_id{1};

    // Anti-flood: log first N unique entries to main log as they appear
    static constexpr int FIRST_SEEN_LOG_LIMIT = 200;
    static int s_first_seen_logged{0};

    // ═══ Name resolution cache (avoid re-resolving same pointer every call) ═
    // These caches are cleared on start() and are only valid during a trace session.
    // Key: raw pointer (UFunction* or UClass*), Value: resolved name
    static std::unordered_map<uintptr_t, std::string> s_func_name_cache;
    static std::unordered_map<uintptr_t, std::string> s_class_name_cache;

    static std::string resolve_func_key(ue::UObject *self, ue::UFunction *func)
    {
        // Resolve function owner class name (cached)
        std::string owner_class;
        {
            uintptr_t func_key = reinterpret_cast<uintptr_t>(func);
            auto it = s_func_name_cache.find(func_key);
            if (it != s_func_name_cache.end())
            {
                return it->second; // full "OwnerClass:FuncName" cached
            }

            // Resolve function name
            std::string func_name = reflection::get_short_name(
                reinterpret_cast<const ue::UObject *>(func));

            // Resolve owner class (function's Outer)
            ue::UObject *func_outer = ue::uobj_get_outer(
                reinterpret_cast<const ue::UObject *>(func));
            if (func_outer && ue::is_valid_ptr(func_outer))
            {
                owner_class = reflection::get_short_name(func_outer);
            }
            else
            {
                owner_class = "???";
            }

            std::string key = owner_class + ":" + func_name;
            s_func_name_cache[func_key] = key;
            return key;
        }
    }

    static std::string resolve_obj_class(ue::UObject *self)
    {
        if (!self)
            return "NULL";

        uintptr_t obj_key = reinterpret_cast<uintptr_t>(ue::uobj_get_class(self));
        auto it = s_class_name_cache.find(obj_key);
        if (it != s_class_name_cache.end())
            return it->second;

        ue::UClass *cls = ue::uobj_get_class(self);
        if (!cls || !ue::is_valid_ptr(cls))
        {
            s_class_name_cache[obj_key] = "???";
            return "???";
        }

        std::string name = reflection::get_short_name(
            reinterpret_cast<const ue::UObject *>(cls));
        s_class_name_cache[obj_key] = name;
        return name;
    }

    // ═══ Public API ═════════════════════════════════════════════════════════

    void start(const std::string &filter)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_map.clear();
        s_obj_classes.clear();
        s_func_name_cache.clear();
        s_class_name_cache.clear();
        s_total_calls = 0;
        s_filtered_calls = 0;
        s_first_seen_logged = 0;
        s_filter = filter;
        s_start_time = std::chrono::steady_clock::now();
        s_active.store(true, std::memory_order_release);

        if (filter.empty())
        {
            logger::log_info("PETRACE", "Tracing started (no filter — capturing ALL PE calls)");
        }
        else
        {
            logger::log_info("PETRACE", "Tracing started (filter: '%s')", filter.c_str());
        }
    }

    void stop()
    {
        s_active.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(s_mutex);
        auto elapsed = std::chrono::steady_clock::now() - s_start_time;
        double secs = std::chrono::duration<double>(elapsed).count();
        logger::log_info("PETRACE", "Tracing stopped — %.1fs, %lu total calls, %lu filtered, %zu unique entries",
                         secs, (unsigned long)s_total_calls, (unsigned long)s_filtered_calls,
                         s_map.size());
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_map.clear();
        s_obj_classes.clear();
        s_func_name_cache.clear();
        s_class_name_cache.clear();
        s_total_calls = 0;
        s_filtered_calls = 0;
        s_first_seen_logged = 0;
        logger::log_info("PETRACE", "Trace data cleared");
    }

    bool is_active()
    {
        return s_active.load(std::memory_order_acquire);
    }

    void record(ue::UObject *self, ue::UFunction *func)
    {
        // This is the hot path — must be fast
        // Early out if not active (atomic load, ~1 cycle on ARM64)
        if (!s_active.load(std::memory_order_relaxed))
            return;

        // Resolve names (cached — only resolves once per unique pointer)
        std::lock_guard<std::mutex> lock(s_mutex);
        s_total_calls++;

        std::string func_key = resolve_func_key(self, func);

        // Apply filter if set
        if (!s_filter.empty())
        {
            // Also check object class for filter match
            std::string obj_class = resolve_obj_class(self);
            bool match = (func_key.find(s_filter) != std::string::npos) ||
                         (obj_class.find(s_filter) != std::string::npos);
            if (!match)
                return;
        }

        s_filtered_calls++;

        // Record object class
        {
            std::string obj_class = resolve_obj_class(self);
            s_obj_classes[obj_class]++;
        }

        // Record function call
        auto it = s_map.find(func_key);
        if (it != s_map.end())
        {
            it->second.count++;
            it->second.last_tick = s_total_calls;
        }
        else
        {
            // First occurrence — log to main log (anti-flood: first N only)
            TraceEntry entry;
            entry.count = 1;
            entry.first_tick = s_total_calls;
            entry.last_tick = s_total_calls;
            entry.total_time_ns = 0;
            entry.min_time_ns = UINT64_MAX;
            entry.max_time_ns = 0;
            s_map[func_key] = entry;

            if (s_first_seen_logged < FIRST_SEEN_LOG_LIMIT)
            {
                std::string obj_class = resolve_obj_class(self);
                logger::log_info("PETRACE", "[NEW] %s  (self: %s)  [#%d unique]",
                                 func_key.c_str(), obj_class.c_str(),
                                 (int)s_map.size());
                s_first_seen_logged++;
                if (s_first_seen_logged == FIRST_SEEN_LOG_LIMIT)
                {
                    logger::log_info("PETRACE",
                                     "First-seen log limit reached (%d) — new entries still tracked, not logged individually",
                                     FIRST_SEEN_LOG_LIMIT);
                }
            }
        }
    }

    // ═══ Detailed recording (with timing) ═══════════════════════════════════

    void record_detailed(ue::UObject *self, ue::UFunction *func,
                         void *parms, uint64_t exec_time_ns)
    {
        if (!s_active.load(std::memory_order_relaxed))
            return;

        std::lock_guard<std::mutex> lock(s_mutex);
        s_total_calls++;

        std::string func_key = resolve_func_key(self, func);

        if (!s_filter.empty())
        {
            std::string obj_class = resolve_obj_class(self);
            bool match = (func_key.find(s_filter) != std::string::npos) ||
                         (obj_class.find(s_filter) != std::string::npos);
            if (!match)
                return;
        }

        s_filtered_calls++;

        {
            std::string obj_class = resolve_obj_class(self);
            s_obj_classes[obj_class]++;
        }

        auto it = s_map.find(func_key);
        if (it != s_map.end())
        {
            it->second.count++;
            it->second.last_tick = s_total_calls;
            it->second.total_time_ns += exec_time_ns;
            if (exec_time_ns < it->second.min_time_ns)
                it->second.min_time_ns = exec_time_ns;
            if (exec_time_ns > it->second.max_time_ns)
                it->second.max_time_ns = exec_time_ns;
        }
        else
        {
            TraceEntry entry;
            entry.count = 1;
            entry.first_tick = s_total_calls;
            entry.last_tick = s_total_calls;
            entry.total_time_ns = exec_time_ns;
            entry.min_time_ns = exec_time_ns;
            entry.max_time_ns = exec_time_ns;
            s_map[func_key] = entry;

            if (s_first_seen_logged < FIRST_SEEN_LOG_LIMIT)
            {
                std::string obj_class = resolve_obj_class(self);
                logger::log_info("PETRACE", "[NEW] %s  (self: %s)  [#%d unique] [%lluus]",
                                 func_key.c_str(), obj_class.c_str(),
                                 (int)s_map.size(), (unsigned long long)(exec_time_ns / 1000));
                s_first_seen_logged++;
            }
        }

        // Fire watches
        if (!s_watches.empty())
        {
            std::lock_guard<std::mutex> wlock(s_watch_mutex);
            for (const auto &watch : s_watches)
            {
                if (func_key.find(watch.pattern) != std::string::npos)
                {
                    try
                    {
                        watch.callback(self, func, func_key, s_map[func_key].count);
                    }
                    catch (...)
                    {
                        // Watch callback threw — ignore
                    }
                }
            }
        }
    }

    bool is_detailed()
    {
        return s_detailed.load(std::memory_order_relaxed);
    }

    void set_detailed(bool enabled)
    {
        s_detailed.store(enabled, std::memory_order_release);
        logger::log_info("PETRACE", "Detailed tracing %s", enabled ? "ENABLED" : "DISABLED");
    }

    // ═══ Per-function stats API ═════════════════════════════════════════════

    bool get_function_stats(const std::string &func_key, FunctionStats &out)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto it = s_map.find(func_key);
        if (it == s_map.end())
            return false;

        out.func_key = func_key;
        out.call_count = it->second.count;
        out.total_time_ns = it->second.total_time_ns;
        out.min_time_ns = (it->second.min_time_ns == UINT64_MAX) ? 0 : it->second.min_time_ns;
        out.max_time_ns = it->second.max_time_ns;
        out.avg_time_ns = (it->second.count > 0) ? static_cast<double>(it->second.total_time_ns) / it->second.count : 0.0;
        out.first_tick = it->second.first_tick;
        out.last_tick = it->second.last_tick;
        return true;
    }

    std::vector<FunctionStats> get_all_stats()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        std::vector<FunctionStats> result;
        result.reserve(s_map.size());

        for (const auto &pair : s_map)
        {
            FunctionStats fs;
            fs.func_key = pair.first;
            fs.call_count = pair.second.count;
            fs.total_time_ns = pair.second.total_time_ns;
            fs.min_time_ns = (pair.second.min_time_ns == UINT64_MAX) ? 0 : pair.second.min_time_ns;
            fs.max_time_ns = pair.second.max_time_ns;
            fs.avg_time_ns = (pair.second.count > 0) ? static_cast<double>(pair.second.total_time_ns) / pair.second.count : 0.0;
            fs.first_tick = pair.second.first_tick;
            fs.last_tick = pair.second.last_tick;
            result.push_back(fs);
        }
        return result;
    }

    // ═══ Watch system ═══════════════════════════════════════════════════════

    uint32_t register_watch(const std::string &pattern, WatchCallback callback)
    {
        std::lock_guard<std::mutex> lock(s_watch_mutex);
        uint32_t id = s_next_watch_id.fetch_add(1);
        s_watches.push_back({id, pattern, callback});
        logger::log_info("PETRACE", "Watch registered: id=%u, pattern='%s'", id, pattern.c_str());
        return id;
    }

    void unregister_watch(uint32_t watch_id)
    {
        std::lock_guard<std::mutex> lock(s_watch_mutex);
        auto it = std::remove_if(s_watches.begin(), s_watches.end(),
                                 [watch_id](const WatchEntry &w)
                                 { return w.id == watch_id; });
        if (it != s_watches.end())
        {
            s_watches.erase(it, s_watches.end());
            logger::log_info("PETRACE", "Watch unregistered: id=%u", watch_id);
        }
    }

    void clear_watches()
    {
        std::lock_guard<std::mutex> lock(s_watch_mutex);
        s_watches.clear();
        logger::log_info("PETRACE", "All watches cleared");
    }

    // ═══ Top by time ════════════════════════════════════════════════════════

    std::string top_by_time(int n)
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_map.empty())
            return "(no data — start tracing first)";

        struct SortEntry
        {
            std::string key;
            uint64_t total_ns;
            uint64_t count;
        };
        std::vector<SortEntry> sorted;
        sorted.reserve(s_map.size());
        for (const auto &pair : s_map)
        {
            sorted.push_back({pair.first, pair.second.total_time_ns, pair.second.count});
        }
        std::sort(sorted.begin(), sorted.end(),
                  [](const SortEntry &a, const SortEntry &b)
                  { return a.total_ns > b.total_ns; });

        std::string out;
        out += "=== PE Trace: Top by execution time ===\n";
        out += "  Func                                                         TotalTime    Count  Avg\n";

        int shown = 0;
        for (const auto &e : sorted)
        {
            if (shown >= n)
                break;
            double avg_us = (e.count > 0) ? (double)e.total_ns / e.count / 1000.0 : 0;
            char line[256];
            snprintf(line, sizeof(line), "  %-60s %8.1fms %8lu %6.1fus\n",
                     e.key.c_str(), e.total_ns / 1e6, (unsigned long)e.count, avg_us);
            out += line;
            shown++;
        }
        return out;
    }

    std::string status()
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        char buf[512];
        if (s_active.load(std::memory_order_relaxed))
        {
            auto elapsed = std::chrono::steady_clock::now() - s_start_time;
            double secs = std::chrono::duration<double>(elapsed).count();
            snprintf(buf, sizeof(buf),
                     "ACTIVE — %.1fs, %lu total PE calls, %lu filtered, %zu unique funcs, %zu unique obj classes, filter: '%s'",
                     secs, (unsigned long)s_total_calls, (unsigned long)s_filtered_calls,
                     s_map.size(), s_obj_classes.size(), s_filter.c_str());
        }
        else
        {
            snprintf(buf, sizeof(buf),
                     "INACTIVE — %zu unique funcs, %zu unique obj classes in buffer (%lu total calls recorded)",
                     s_map.size(), s_obj_classes.size(), (unsigned long)s_total_calls);
        }
        return std::string(buf);
    }

    std::string top(int n)
    {
        std::lock_guard<std::mutex> lock(s_mutex);

        if (s_map.empty())
            return "(no data — start tracing first)";

        // Sort by count descending
        struct SortEntry
        {
            std::string key;
            uint64_t count;
        };
        std::vector<SortEntry> sorted;
        sorted.reserve(s_map.size());
        for (const auto &pair : s_map)
        {
            sorted.push_back({pair.first, pair.second.count});
        }
        std::sort(sorted.begin(), sorted.end(),
                  [](const SortEntry &a, const SortEntry &b)
                  { return a.count > b.count; });

        // Build output
        std::string out;
        auto elapsed = std::chrono::steady_clock::now() - s_start_time;
        double secs = std::chrono::duration<double>(elapsed).count();
        char header[256];
        snprintf(header, sizeof(header),
                 "=== PE Trace: %.1fs, %lu calls, %zu unique, filter: '%s' ===\n",
                 secs, (unsigned long)s_filtered_calls, s_map.size(), s_filter.c_str());
        out += header;
        out += "--- Functions by call count (top " + std::to_string(n) + ") ---\n";

        int shown = 0;
        for (const auto &e : sorted)
        {
            if (shown >= n)
                break;
            char line[256];
            snprintf(line, sizeof(line), "  %-60s %8lu\n", e.key.c_str(), (unsigned long)e.count);
            out += line;
            shown++;
        }

        // Also show top object classes
        if (!s_obj_classes.empty())
        {
            std::vector<SortEntry> obj_sorted;
            obj_sorted.reserve(s_obj_classes.size());
            for (const auto &pair : s_obj_classes)
            {
                obj_sorted.push_back({pair.first, pair.second});
            }
            std::sort(obj_sorted.begin(), obj_sorted.end(),
                      [](const SortEntry &a, const SortEntry &b)
                      { return a.count > b.count; });

            out += "\n--- Object classes (self) by call count ---\n";
            int obj_shown = 0;
            for (const auto &e : obj_sorted)
            {
                if (obj_shown >= n)
                    break;
                char line[256];
                snprintf(line, sizeof(line), "  %-60s %8lu\n", e.key.c_str(), (unsigned long)e.count);
                out += line;
                obj_shown++;
            }
        }

        return out;
    }

    std::string dump_to_file()
    {
        std::lock_guard<std::mutex> lock(s_mutex);

        if (s_map.empty())
            return "no data to dump";

        std::string out_path = paths::data_dir() + "/pe_trace.log";
        FILE *f = fopen(out_path.c_str(), "w");
        if (!f)
        {
            logger::log_error("PETRACE", "Failed to open %s for writing", out_path.c_str());
            return "failed to open " + out_path;
        }

        auto elapsed = std::chrono::steady_clock::now() - s_start_time;
        double secs = std::chrono::duration<double>(elapsed).count();

        fprintf(f, "═══════════════════════════════════════════════════════════════\n");
        fprintf(f, " ProcessEvent Trace Dump\n");
        fprintf(f, " Duration:       %.1f seconds\n", secs);
        fprintf(f, " Total PE calls: %lu\n", (unsigned long)s_total_calls);
        fprintf(f, " Filtered calls: %lu\n", (unsigned long)s_filtered_calls);
        fprintf(f, " Unique funcs:   %zu\n", s_map.size());
        fprintf(f, " Unique obj cls: %zu\n", s_obj_classes.size());
        fprintf(f, " Filter:         '%s'\n", s_filter.c_str());
        fprintf(f, "═══════════════════════════════════════════════════════════════\n\n");

        // Sort functions by count descending
        struct SortEntry
        {
            std::string key;
            uint64_t count;
            uint64_t first_tick;
            uint64_t last_tick;
            uint64_t total_ns;
            uint64_t min_ns;
            uint64_t max_ns;
        };
        std::vector<SortEntry> sorted;
        sorted.reserve(s_map.size());
        for (const auto &pair : s_map)
        {
            sorted.push_back({pair.first, pair.second.count,
                              pair.second.first_tick, pair.second.last_tick,
                              pair.second.total_time_ns,
                              pair.second.min_time_ns == UINT64_MAX ? 0 : pair.second.min_time_ns,
                              pair.second.max_time_ns});
        }
        std::sort(sorted.begin(), sorted.end(),
                  [](const SortEntry &a, const SortEntry &b)
                  { return a.count > b.count; });

        fprintf(f, "─── All Functions (sorted by call count) ───────────────────────\n");
        fprintf(f, "%-60s %10s %10s %10s %10s %10s %10s\n",
                "OwnerClass:FuncName", "Count", "FirstTick", "LastTick",
                "TotalMs", "MinUs", "MaxUs");
        fprintf(f, "%-60s %10s %10s %10s %10s %10s %10s\n",
                "───────────────────", "─────", "─────────", "────────",
                "───────", "─────", "─────");
        for (const auto &e : sorted)
        {
            fprintf(f, "%-60s %10lu %10lu %10lu %10.1f %10.1f %10.1f\n",
                    e.key.c_str(), (unsigned long)e.count,
                    (unsigned long)e.first_tick, (unsigned long)e.last_tick,
                    e.total_ns / 1e6, e.min_ns / 1e3, e.max_ns / 1e3);
        }

        // Object classes
        if (!s_obj_classes.empty())
        {
            struct ObjEntry
            {
                std::string name;
                uint64_t count;
            };
            std::vector<ObjEntry> obj_sorted;
            obj_sorted.reserve(s_obj_classes.size());
            for (const auto &pair : s_obj_classes)
            {
                obj_sorted.push_back({pair.first, pair.second});
            }
            std::sort(obj_sorted.begin(), obj_sorted.end(),
                      [](const ObjEntry &a, const ObjEntry &b)
                      { return a.count > b.count; });

            fprintf(f, "\n─── Object Classes (self) ───────────────────────────────────\n");
            fprintf(f, "%-60s %10s\n", "ObjectClass", "Count");
            fprintf(f, "%-60s %10s\n", "───────────", "─────");
            for (const auto &e : obj_sorted)
            {
                fprintf(f, "%-60s %10lu\n", e.name.c_str(), (unsigned long)e.count);
            }
        }

        fclose(f);

        char summary[256];
        snprintf(summary, sizeof(summary),
                 "Dumped %zu funcs, %zu obj classes to %s (%.1fs, %lu calls)",
                 s_map.size(), s_obj_classes.size(), out_path.c_str(),
                 secs, (unsigned long)s_filtered_calls);
        logger::log_info("PETRACE", "%s", summary);
        return std::string(summary);
    }

} // namespace pe_trace
