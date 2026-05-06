#pragma once
// modloader/include/modloader/lua_yup.h
// YUP Physics Engine compatibility layer for Pinball FX VR
// Exposes YUP native functions (table debug, ball control, property system)
// as Lua API. Only active when game_profile::is_pinball_fx() is true.

#include <sol/sol.hpp>

namespace lua_yup
{
    // Register the YUP table in Lua state (PFX only — no-op on other games)
    void register_all(sol::state &lua);

    // Resolve YUP function pointers from symbol cache (call after symbols::resolve_core_symbols)
    void resolve_symbols();

} // namespace lua_yup
