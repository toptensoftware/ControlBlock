// Lua 5.4 builds in the host tests (docs/standalone-ops-plan.md step 6): the same
// sources as the device, with Lua's default 64-bit integers and doubles (the
// same numbers as bake's wasmoon check).

#include "UnitTesting.h"
#include "Core.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

using namespace SimpleLib;

Fact("Lua Builds And Runs")
{
    lua_State *L = luaL_newstate();
    Assert(L != nullptr);
    Assert(luaL_dostring(L, "return 1 + 1") == LUA_OK);
    Assert(lua_tointeger(L, -1) == 2);
    lua_close(L);
}

Fact("Lua Uses 64 Bit Numbers")
{
    Assert(sizeof(lua_Integer) == 8);
    Assert(sizeof(lua_Number) == 8);
}
