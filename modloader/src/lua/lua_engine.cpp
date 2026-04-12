// modloader/src/lua/lua_engine.cpp
// Sol2-based Lua 5.4 engine — state creation, library registration, exec

#include "modloader/lua_engine.h"
#include "modloader/lua_bindings.h"
#include "modloader/lua_uobject.h"
#include "modloader/lua_types.h"
#include "modloader/lua_enums.h"
#include "modloader/lua_delayed_actions.h"
#include "modloader/lua_ue4ss_types.h"
#include "modloader/lua_ue4ss_globals.h"
#include "modloader/lua_tarray.h"
#include "modloader/lua_ustruct.h"
#include "modloader/lua_enum_ext.h"
#include "modloader/logger.h"
#include "modloader/paths.h"

#include <cstring>
#include <atomic>

extern "C"
{
#include <lua.h>
#include <lauxlib.h>
}

namespace lua_engine
{

    // ═══ Memory-limited Lua allocator ═══════════════════════════════════════
    // Prevents Lua scripts from exhausting device RAM (e.g., infinite string concat)
    struct LuaMemTracker
    {
        std::atomic<size_t> current{0};
        static constexpr size_t LIMIT = 256 * 1024 * 1024; // 256 MB max for all Lua memory
    };

    static LuaMemTracker s_mem_tracker;

    static void *limited_lua_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
    {
        auto *tracker = static_cast<LuaMemTracker *>(ud);

        // When ptr is NULL, osize is a type tag, not a real size
        size_t real_osize = (ptr != nullptr) ? osize : 0;

        if (nsize == 0)
        {
            // Free
            if (real_osize > 0)
            {
                tracker->current.fetch_sub(real_osize, std::memory_order_relaxed);
            }
            free(ptr);
            return nullptr;
        }

        // Check if new allocation would exceed the limit
        if (nsize > real_osize)
        {
            size_t needed = nsize - real_osize;
            size_t cur = tracker->current.load(std::memory_order_relaxed);
            if (cur + needed > LuaMemTracker::LIMIT)
            {
                // Refuse allocation → Lua raises "not enough memory" error
                return nullptr;
            }
        }

        void *newptr = realloc(ptr, nsize);
        if (newptr)
        {
            if (nsize > real_osize)
            {
                tracker->current.fetch_add(nsize - real_osize, std::memory_order_relaxed);
            }
            else if (real_osize > nsize)
            {
                tracker->current.fetch_sub(real_osize - nsize, std::memory_order_relaxed);
            }
        }
        return newptr;
    }

    // ═══ Instruction-limit hook ═════════════════════════════════════════════
    // Fires after N VM instructions — aborts runaway Lua code (infinite loops/recursion)
    static void instruction_limit_hook(lua_State *L, lua_Debug * /*ar*/)
    {
        luaL_error(L, "execution limit exceeded (possible infinite loop or recursion)");
    }

    static sol::state *s_lua = nullptr;
    static std::mutex s_exec_mutex;

    bool init()
    {
        if (s_lua)
        {
            logger::log_warn("LUA", "Lua engine already initialized");
            return true;
        }

        s_lua = new sol::state();
        if (!s_lua)
        {
            logger::log_error("LUA", "Failed to allocate Lua state");
            return false;
        }

        // Install memory-limited allocator to prevent OOM from malicious/buggy Lua code
        // Existing allocations from sol::state() constructor are negligible (~few KB)
        lua_setallocf(s_lua->lua_state(), limited_lua_alloc, &s_mem_tracker);
        logger::log_info("LUA", "Memory-limited allocator installed (limit: %zu MB)",
                         LuaMemTracker::LIMIT / (1024 * 1024));

        // Open standard libraries
        s_lua->open_libraries(
            sol::lib::base,
            sol::lib::package,
            sol::lib::string,
            sol::lib::table,
            sol::lib::math,
            sol::lib::io,
            sol::lib::os,
            sol::lib::coroutine,
            sol::lib::debug,
            sol::lib::utf8);

        // Set up package.path to include SDK directory and mods directory
        std::string sdk_dir = paths::sdk_dir();
        std::string mods_dir = paths::mods_dir();
        std::string data_dir = paths::data_dir();

        std::string pkg_path = (*s_lua)["package"]["path"];
        pkg_path += ";" + sdk_dir + "/?.lua";
        pkg_path += ";" + sdk_dir + "/?/init.lua";
        pkg_path += ";" + mods_dir + "/?.lua";
        pkg_path += ";" + mods_dir + "/?/init.lua";
        pkg_path += ";" + data_dir + "/?.lua";
        (*s_lua)["package"]["path"] = pkg_path;

        // Register all UObject userdata types
        lua_uobject::register_types(*s_lua);

        // Register TArray/TMap/TSet usertypes (must be before types that return them)
        lua_tarray::register_all(*s_lua);

        // Register UStruct usertype (enables struct.X, struct.Y field access)
        lua_ustruct::register_all(*s_lua);

        // Register UE4SS-compatible core types (FName, FText, FString, RemoteUnrealParam, etc.)
        lua_types::register_all(*s_lua);

        // Register UE4SS-compatible enum tables (Key, ModifierKey, EObjectFlags, etc.)
        lua_enums::register_all(*s_lua);

        // Register UE4SS-compatible extended types (UClass, UFunction, UEnum methods)
        lua_ue4ss_types::register_all(*s_lua);

        // Register enum extension API (FindEnum, GetEnumTable, AppendEnumValue, Enums.*)
        lua_enum_ext::register_all(*s_lua);

        // Register all global API functions (original modloader API)
        lua_bindings::register_all(*s_lua);

        // Register UE4SS-compatible global functions (print, FindObjects, Register*Hook family, etc.)
        lua_ue4ss_globals::register_all(*s_lua);

        // Register delayed action system (ExecuteWithDelay, LoopAsync, etc.)
        lua_delayed::init();
        lua_delayed::register_all(*s_lua);

        // Initialize SharedAPI as a bare Lua global table.
        // The modloader is GENERIC — it only provides the empty SharedAPI namespace.
        // Feature-specific APIs (DebugMenu, etc.) are registered by the mods that own them.
        // e.g. DebugMenuAPI mod creates SharedAPI.DebugMenu when it loads.
        const char *shared_api_init = R"lua(
SharedAPI = {}
)lua";
        auto shared_result = s_lua->safe_script(shared_api_init, sol::script_pass_on_error, "SharedAPI_init");
        if (!shared_result.valid())
        {
            sol::error err = shared_result;
            logger::log_error("LUA", "SharedAPI init failed: %s", err.what());
        }
        else
        {
            logger::log_info("LUA", "SharedAPI initialized (empty global table — mods register their own APIs)");
        }

        logger::log_info("LUA", "Lua 5.4 engine initialized — Sol2 state ready");
        logger::log_info("LUA", "package.path includes SDK: %s", sdk_dir.c_str());

        return true;
    }

    void shutdown()
    {
        if (s_lua)
        {
            delete s_lua;
            s_lua = nullptr;
            logger::log_info("LUA", "Lua engine shut down");
        }
    }

    sol::state &state()
    {
        return *s_lua;
    }

    bool is_initialized()
    {
        return s_lua != nullptr;
    }

    ExecResult exec_string(const std::string &code, const std::string &chunk_name, int max_instructions)
    {
        std::lock_guard<std::mutex> lock(s_exec_mutex);
        ExecResult result;

        if (!s_lua)
        {
            result.success = false;
            result.error = "Lua engine not initialized";
            return result;
        }

        lua_State *L = s_lua->lua_state();

        // Set instruction limit hook if requested (prevents infinite loops/recursion)
        if (max_instructions > 0)
        {
            lua_sethook(L, instruction_limit_hook, LUA_MASKCOUNT, max_instructions);
        }

        auto pr = s_lua->safe_script(code, sol::script_pass_on_error, chunk_name);

        // Always clear the hook after execution (whether it succeeded or not)
        if (max_instructions > 0)
        {
            lua_sethook(L, nullptr, 0, 0);
        }

        if (!pr.valid())
        {
            sol::error err = pr;
            result.success = false;
            result.error = err.what();
            logger::log_error("LUA", "exec_string error (%s): %s",
                              chunk_name.c_str(), result.error.c_str());
        }
        else
        {
            result.success = true;
            // Try to convert the result to a string for ADB bridge responses
            if (pr.return_count() > 0)
            {
                sol::object ret = pr;
                if (ret.is<std::string>())
                {
                    result.output = ret.as<std::string>();
                }
                else if (ret.is<double>())
                {
                    result.output = std::to_string(ret.as<double>());
                }
                else if (ret.is<int>())
                {
                    result.output = std::to_string(ret.as<int>());
                }
                else if (ret.is<bool>())
                {
                    result.output = ret.as<bool>() ? "true" : "false";
                }
                else if (ret.get_type() == sol::type::nil)
                {
                    result.output = "nil";
                }
                else
                {
                    result.output = "[" + std::string(sol::type_name(s_lua->lua_state(), ret.get_type())) + "]";
                }
            }
        }

        return result;
    }

    ExecResult exec_file(const std::string &filepath)
    {
        std::lock_guard<std::mutex> lock(s_exec_mutex);
        ExecResult result;

        if (!s_lua)
        {
            result.success = false;
            result.error = "Lua engine not initialized";
            return result;
        }

        auto pr = s_lua->safe_script_file(filepath, sol::script_pass_on_error);
        if (!pr.valid())
        {
            sol::error err = pr;
            result.success = false;
            result.error = err.what();
            logger::log_error("LUA", "exec_file error (%s): %s",
                              filepath.c_str(), result.error.c_str());
        }
        else
        {
            result.success = true;
        }

        return result;
    }

    ExecResult exec_file_in_env(const std::string &filepath, sol::environment &env)
    {
        std::lock_guard<std::mutex> lock(s_exec_mutex);
        ExecResult result;

        if (!s_lua)
        {
            result.success = false;
            result.error = "Lua engine not initialized";
            return result;
        }

        auto pr = s_lua->safe_script_file(filepath, env, sol::script_pass_on_error);
        if (!pr.valid())
        {
            sol::error err = pr;
            result.success = false;
            result.error = err.what();
        }
        else
        {
            result.success = true;
        }

        return result;
    }

    sol::environment create_mod_environment(const std::string &mod_name)
    {
        sol::environment env(*s_lua, sol::create, s_lua->globals());

        // Set mod-specific globals
        env["MOD_NAME"] = mod_name;
        env["MOD_DIR"] = paths::mods_dir() + "/" + mod_name;

        // Override Log/LogWarn/LogError to prepend mod name
        env["Log"] = [mod_name](const std::string &msg)
        {
            logger::log_info("LUA", "%s: %s", mod_name.c_str(), msg.c_str());
        };
        env["LogWarn"] = [mod_name](const std::string &msg)
        {
            logger::log_warn("LUA", "%s: %s", mod_name.c_str(), msg.c_str());
        };
        env["LogError"] = [mod_name](const std::string &msg)
        {
            logger::log_error("LUA", "%s: %s", mod_name.c_str(), msg.c_str());
        };

        return env;
    }

} // namespace lua_engine
