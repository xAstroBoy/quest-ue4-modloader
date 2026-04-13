// modloader/src/core/config.cpp
// Runtime configuration loaded from config.json in the app data directory.
// Uses nlohmann/json for parsing. Creates defaults if file is missing.

#include "modloader/config.h"
#include "modloader/logger.h"

#define JSON_NOEXCEPTION
#include <nlohmann/json.hpp>
#include <fstream>
#include <sys/stat.h>
#include <type_traits>

namespace config
{

    // ═══ Default values ═════════════════════════════════════════════════════
    // These MUST match the header docs, build_defaults(), and safe_get() fallbacks.
    static bool s_auto_dump_on_boot = true;
    static bool s_auto_dump_on_level_change = false;
    static bool s_object_monitor_enabled = false;
    static int s_monitor_poll_interval_ms = 5000;
    static int s_monitor_growth_threshold = 500;
    static int s_monitor_cooldown_ms = 30000;

    static bool s_loaded = false;

    // ═══ Helpers ════════════════════════════════════════════════════════════
    static bool file_exists(const std::string &path)
    {
        struct stat st;
        return stat(path.c_str(), &st) == 0;
    }

    static nlohmann::json build_defaults()
    {
        nlohmann::json j;
        j["auto_dump_on_boot"] = true;
        j["auto_dump_on_level_change"] = false;
        j["object_monitor_enabled"] = false;
        j["monitor_poll_interval_ms"] = 5000;
        j["monitor_growth_threshold"] = 500;
        j["monitor_cooldown_ms"] = 30000;
        return j;
    }

    template <typename T>
    static T safe_get(const nlohmann::json &j, const std::string &key, T def)
    {
        if (j.contains(key) && !j[key].is_null())
        {
            if constexpr (std::is_same_v<T, bool>)
            {
                if (j[key].is_boolean())
                    return j[key].get<bool>();
            }
            else if constexpr (std::is_same_v<T, int>)
            {
                if (j[key].is_number_integer())
                    return j[key].get<int>();
            }
            logger::log_warn("CONFIG", "Invalid type for key '%s' — using default", key.c_str());
        }
        return def;
    }

    // ═══ Load ═══════════════════════════════════════════════════════════════
    void load(const std::string &data_dir)
    {
        std::string config_path = data_dir + "/config.json";

        if (!file_exists(config_path))
        {
            logger::log_info("CONFIG", "config.json not found — creating with defaults at %s", config_path.c_str());
            save(data_dir);
            s_loaded = true;
            return;
        }

        logger::log_info("CONFIG", "Loading config from %s", config_path.c_str());

        std::ifstream ifs(config_path);
        if (!ifs.is_open())
        {
            logger::log_warn("CONFIG", "Failed to open config.json — using defaults");
            s_loaded = true;
            return;
        }

        nlohmann::json j;
        nlohmann::json j_candidate = nlohmann::json::parse(ifs, nullptr, false);
        if (j_candidate.is_discarded())
        {
            logger::log_error("CONFIG", "config.json parse error — using defaults");
            s_loaded = true;
            return;
        }
        j = j_candidate;

        s_auto_dump_on_boot = safe_get<bool>(j, "auto_dump_on_boot", true);
        s_auto_dump_on_level_change = safe_get<bool>(j, "auto_dump_on_level_change", false);
        s_object_monitor_enabled = safe_get<bool>(j, "object_monitor_enabled", false);
        s_monitor_poll_interval_ms = safe_get<int>(j, "monitor_poll_interval_ms", 5000);
        s_monitor_growth_threshold = safe_get<int>(j, "monitor_growth_threshold", 500);
        s_monitor_cooldown_ms = safe_get<int>(j, "monitor_cooldown_ms", 30000);

        logger::log_info("CONFIG", "  auto_dump_on_boot         = %s", s_auto_dump_on_boot ? "true" : "false");
        logger::log_info("CONFIG", "  auto_dump_on_level_change = %s", s_auto_dump_on_level_change ? "true" : "false");
        logger::log_info("CONFIG", "  object_monitor_enabled    = %s", s_object_monitor_enabled ? "true" : "false");
        logger::log_info("CONFIG", "  monitor_poll_interval_ms  = %d", s_monitor_poll_interval_ms);
        logger::log_info("CONFIG", "  monitor_growth_threshold  = %d", s_monitor_growth_threshold);
        logger::log_info("CONFIG", "  monitor_cooldown_ms       = %d", s_monitor_cooldown_ms);

        s_loaded = true;
    }

    // ═══ Save ═══════════════════════════════════════════════════════════════
    void save(const std::string &data_dir)
    {
        std::string config_path = data_dir + "/config.json";

        nlohmann::json j;
        j["auto_dump_on_boot"] = s_auto_dump_on_boot;
        j["auto_dump_on_level_change"] = s_auto_dump_on_level_change;
        j["object_monitor_enabled"] = s_object_monitor_enabled;
        j["monitor_poll_interval_ms"] = s_monitor_poll_interval_ms;
        j["monitor_growth_threshold"] = s_monitor_growth_threshold;
        j["monitor_cooldown_ms"] = s_monitor_cooldown_ms;

        std::ofstream ofs(config_path);
        if (!ofs.is_open())
        {
            logger::log_error("CONFIG", "Failed to write config.json at %s", config_path.c_str());
            return;
        }

        ofs << j.dump(4) << std::endl;
        logger::log_info("CONFIG", "config.json saved to %s", config_path.c_str());
    }

    // ═══ Accessors ══════════════════════════════════════════════════════════
    bool auto_dump_on_boot() { return s_auto_dump_on_boot; }
    bool auto_dump_on_level_change() { return s_auto_dump_on_level_change; }
    bool object_monitor_enabled() { return s_object_monitor_enabled; }
    int monitor_poll_interval_ms() { return s_monitor_poll_interval_ms; }
    int monitor_growth_threshold() { return s_monitor_growth_threshold; }
    int monitor_cooldown_ms() { return s_monitor_cooldown_ms; }

} // namespace config
