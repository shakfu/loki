/* test_lua_modules.c - Tests for the bundled Lua modules in .loki/modules
 *
 * Loads ai, example and test against the real loki API, with
 * loki.async_http replaced by a stub so no request leaves the process.
 */

#include "test_framework.h"
#include <stdlib.h>
#include <string.h>
#include "loki/core.h"
#include "loki/lua.h"
#include "internal.h"
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

/* Loads the modules and defines expect(want), which checks the last
 * loki.status() message. stub_result holds what async_http returns. */
static const char *prelude =
    "package.path = '" LOKI_MODULES_DIR "/?.lua;' .. package.path\n"
    "stub_result = {nil, 'URL points at an internal address'}\n"
    "loki.async_http = function() return table.unpack(stub_result, 1, 2) end\n"
    "local real_status, real_insert = loki.status, loki.insert_text\n"
    "last_status, last_insert = nil, nil\n"
    "loki.status = function(s) last_status = s; return real_status(s) end\n"
    "loki.insert_text = function(t) last_insert = t; return real_insert(t) end\n"
    "function expect(want)\n"
    "  if last_status ~= want then\n"
    "    error(('want %q, got %q'):format(want, tostring(last_status)), 2)\n"
    "  end\n"
    "end\n"
    "ai, example, test = require('ai'), require('example'), require('test')\n";

static void init_ctx(editor_ctx_t *ctx) {
    editor_ctx_init(ctx);
    ctx->lua_host = lua_host_create();
    if (ctx->lua_host) ctx->lua_host->L = loki_lua_bootstrap(ctx, NULL);
    setenv("OPENAI_API_KEY", "test-key", 1);
}

static void free_ctx(editor_ctx_t *ctx) {
    if (ctx->lua_host) {
        lua_host_free(ctx->lua_host);
        ctx->lua_host = NULL;
    }
    editor_ctx_free(ctx);
}

/* Run prelude + code; print the Lua error on failure. */
static int run(editor_ctx_t *ctx, const char *code) {
    lua_State *L = ctx_L(ctx);
    if (luaL_dostring(L, prelude) != LUA_OK || luaL_dostring(L, code) != LUA_OK) {
        fprintf(stderr, "    %s\n", lua_tostring(L, -1));
        return -1;
    }
    return 0;
}

/* Test: A rejected request shows the error, not "request sent" */
TEST(modules_report_rejected_request) {
    editor_ctx_t ctx;
    init_ctx(&ctx);
    ASSERT_NOT_NULL(ctx_L(&ctx));

    int rc = run(&ctx,
        "ai.complete('hi');     expect('Error: URL points at an internal address')\n"
        "ai.explain('x = 1');   expect('Error: URL points at an internal address')\n"
        "example.fetch_quote(); expect('Error: URL points at an internal address')\n"
        "test.http();           expect('HTTP test failed: URL points at an internal address')\n");
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(ctx.view.statusmsg, "HTTP test failed: URL points at an internal address");

    free_ctx(&ctx);
}

/* Test: An accepted request shows the in-progress message */
TEST(modules_report_sent_request) {
    editor_ctx_t ctx;
    init_ctx(&ctx);
    ASSERT_NOT_NULL(ctx_L(&ctx));

    int rc = run(&ctx,
        "stub_result = {3, nil}\n"
        "ai.complete('hi');     expect('AI request sent... (will appear when ready)')\n"
        "example.fetch_quote(); expect('Fetching random quote...')\n"
        "test.http();           expect('Testing HTTP...')\n");
    ASSERT_EQ(rc, 0);

    free_ctx(&ctx);
}

/* Test: Callbacks read the response table's body and error fields */
TEST(modules_handle_response_tables) {
    editor_ctx_t ctx;
    init_ctx(&ctx);
    ASSERT_NOT_NULL(ctx_L(&ctx));

    int rc = run(&ctx,
        "example_quote_handler({status = 200, body = '{\"content\":\"Be brief.\"}'})\n"
        "expect('Quote inserted!')\n"
        "assert(last_insert == '\\nBe brief.\\n', last_insert)\n"
        "example_quote_handler({status = 0, error = \"Couldn't connect\"})\n"
        "expect(\"Error: Couldn't connect\")\n"
        "test_http_handler({status = 200, body = 'Keep it simple.'})\n"
        "expect('HTTP test successful!')\n"
        "test_http_handler({status = 0, error = 'timeout'})\n"
        "expect('HTTP test failed: timeout')\n"
        "test_http_handler({status = 204})\n"
        "expect('HTTP test failed: empty response')\n"
        "ai_response_handler({status = 0, error = 'timeout'})\n"
        "expect('Error: timeout')\n");
    ASSERT_EQ(rc, 0);

    free_ctx(&ctx);
}

BEGIN_TEST_SUITE("Lua Modules")
    RUN_TEST(modules_report_rejected_request);
    RUN_TEST(modules_report_sent_request);
    RUN_TEST(modules_handle_response_tables);
END_TEST_SUITE()
