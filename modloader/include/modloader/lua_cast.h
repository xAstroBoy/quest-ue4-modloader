#pragma once
// modloader/include/modloader/lua_cast.h
// ═══════════════════════════════════════════════════════════════════════════
// Cast layer for Lua — provides typed access to UObject properties and
// hook parameters. Enables .Cast("ClassName") on UObjects and hook parms.
//
// SDK-engine style: obj:Cast("PlayerController") returns a typed wrapper
// that resolves properties against the cast class instead of the runtime class.
//
// Hook parms: CastParms(parms_ptr, "ClassName:FunctionName") returns a
// table with named parameter access via reflection — no more raw pointer math.
// ═══════════════════════════════════════════════════════════════════════════

#include <sol/sol.hpp>

namespace lua_cast
{
    void register_all(sol::state &lua);
} // namespace lua_cast
