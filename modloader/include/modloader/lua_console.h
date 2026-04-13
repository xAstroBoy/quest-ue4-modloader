#pragma once
// modloader/include/modloader/lua_console.h
// Lua bindings for: Console commands, CheatManager, ViewModes, Stats,
// ShowFlags, CVars, safe_call stats, PE trace, diagnostics

#include <sol/sol.hpp>

namespace lua_console
{
    void register_all(sol::state &lua);
} // namespace lua_console
