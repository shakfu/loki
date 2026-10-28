/* lua_helpers.h - Run Lua snippets from C tests
 *
 * ASSERT_LUA(L, code) fails the test with the Lua error message, so test
 * bodies can be written as Lua assert() calls.
 */

#ifndef LOKI_TEST_LUA_HELPERS_H
#define LOKI_TEST_LUA_HELPERS_H

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <lua.h>
#include <lauxlib.h>
#include <uv.h>
#include "test_framework.h"
#include "loki/lua.h"

/* A Lua state with the editor bindings and no user config. */
static inline lua_State *lua_test_state(editor_ctx_t *ctx) {
    struct loki_lua_opts opts = { .bind_editor = 1 };
    return loki_lua_bootstrap(ctx, &opts);
}

/* Run code; NULL on success, else the error (valid until the next call). */
static inline const char *lua_test_run(lua_State *L, const char *code) {
    static char err[1024];
    if (luaL_dostring(L, code) == LUA_OK) {
        lua_settop(L, 0);
        return NULL;
    }
    snprintf(err, sizeof(err), "%s", lua_tostring(L, -1));
    lua_settop(L, 0);
    return err;
}

#define ASSERT_LUA(L, code) do { \
    const char *lua_err_ = lua_test_run((L), (code)); \
    if (lua_err_) { \
        printf(COLOR_RED "  ✗ " COLOR_RESET "%s:%d: Lua error: %s\n", \
               __FILE__, __LINE__, lua_err_); \
        test_stats.current_test_failed = 1; \
        test_stats.failed_tests++; \
        return; \
    } \
} while (0)

/* Run the default libuv loop until the global 'done' is truthy or
 * timeout_ms passes. Returns nonzero if 'done' was set. */
static inline int lua_test_pump(lua_State *L, int timeout_ms) {
    for (int waited = 0; waited <= timeout_ms; waited += 5) {
        uv_run(uv_default_loop(), UV_RUN_NOWAIT);
        lua_getglobal(L, "done");
        int done = lua_toboolean(L, -1);
        lua_pop(L, 1);
        if (done) return 1;
        nanosleep(&(struct timespec){0, 5 * 1000 * 1000}, NULL);
    }
    return 0;
}

#endif /* LOKI_TEST_LUA_HELPERS_H */
