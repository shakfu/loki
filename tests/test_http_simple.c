/* Simple test to debug HTTP security */
#include "test_framework.h"
#include "internal.h"
#include "loki/lua.h"
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <stdlib.h>
#include <string.h>

TEST(simple_test) {
    editor_ctx_t ctx;
    editor_ctx_init(&ctx);

    /* Allocate and initialize LuaHost */
    ctx.lua_host = malloc(sizeof(LuaHost));
    memset(ctx.lua_host, 0, sizeof(LuaHost));
    ctx.lua_host->L = loki_lua_bootstrap(&ctx, NULL);

    ASSERT_NOT_NULL(ctx.lua_host->L);

    /* Just try to call the function and see if it exists */
    lua_getglobal(ctx.lua_host->L, "loki");
    ASSERT_TRUE(lua_istable(ctx.lua_host->L, -1));

    lua_getfield(ctx.lua_host->L, -1, "async_http");
    ASSERT_TRUE(lua_isfunction(ctx.lua_host->L, -1));

    /* Cleanup */
    lua_close(ctx.lua_host->L);
    free(ctx.lua_host);
    ctx.lua_host = NULL;
    editor_ctx_free(&ctx);
}

BEGIN_TEST_SUITE("HTTP Simple")
    RUN_TEST(simple_test);
END_TEST_SUITE()
