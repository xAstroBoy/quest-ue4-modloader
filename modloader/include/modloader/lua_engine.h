#pragma once
// modloader/include/modloader/lua_engine.h
// Sol2-based Lua 5.4 engine — creates the Lua state, exposes the full API

#include <string>
#include <mutex>

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

namespace lua_engine
{

    struct ExecResult
    {
        bool success;
        std::string output;
        std::string error;
    };

    // Initialize the Lua engine — creates the state, registers all bindings
    bool init();

    // Get the global Lua state
    sol::state &state();

    // Check if initialized
    bool is_initialized();

    // Execute a Lua string (for ADB exec_lua)
    // max_instructions: 0 = unlimited, >0 = abort after N Lua VM instructions (prevents infinite loops/recursion)
    ExecResult exec_string(const std::string &code, const std::string &chunk_name = "=exec", int max_instructions = 0);

    // Load and execute a Lua file
    ExecResult exec_file(const std::string &filepath);

    // Execute a Lua file in a sandboxed environment
    ExecResult exec_file_in_env(const std::string &filepath, sol::environment &env);

    // Create a sandboxed environment for a mod
    sol::environment create_mod_environment(const std::string &mod_name);

    // Shutdown — closes the Lua state
    void shutdown();

} // namespace lua_engine
