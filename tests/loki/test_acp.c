/* test_acp.c - ACP client (acp.lua) against a scripted agent
 *
 * tests/acp_mock_agent.py plays the agent; its prompt text picks a script.
 * Permission prompts are answered through choice_handle_key, as a key would.
 */

#include "test_framework.h"
#include "loki/core.h"
#include "internal.h"
#include "buffers.h"
#include "choice.h"
#include "spawn.h"
#include "lua_helpers.h"
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static lua_State *L;
static char dir[] = "/tmp/loki_acpXXXXXX";

/* ask(text) sends a prompt; 'done' is set when the turn ends, with the stop
 * reason in 'stop'. reply() returns the *agent* text after the last '> '. */
static const char *prelude =
    "package.path = '" LOKI_MODULES_DIR "/?.lua;' .. package.path\n"
    "json = loki.json\n"
    "acp = require('acp')\n"
    "mcp = require('mcp')\n"
    "acp.command = {PYTHON3, MOCK_AGENT}\n"
    "acp.on_stop = function(reason) stop = reason; done = true end\n"
    "function ask(text)\n"
    "  done, stop = false, nil\n"
    "  assert(acp.ask(text))\n"
    "end\n"
    "function agent_text()\n"
    "  for _, b in ipairs(loki.buffers()) do\n"
    "    if b.name == '*agent*' then return loki.buffer_text(b.id) end\n"
    "  end\n"
    "end\n"
    "function reply()\n"
    "  local t = agent_text()\n"
    "  local last = 1\n"
    "  for s in t:gmatch('()> ') do last = s end\n"
    "  return (t:sub(last):match('^> [^\\n]*\\n(.*)$') or ''):gsub('%s+$', '')\n"
    "end\n";

static char *read_file(const char *name) {
    static char buf[256];
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* Pump until a choice is on screen. */
static int wait_for_choice(void) {
    for (int i = 0; i < 1000 && !choice_prompt(); i++) {
        uv_run(uv_default_loop(), UV_RUN_NOWAIT);
        nanosleep(&(struct timespec){0, 5 * 1000 * 1000}, NULL);
    }
    return choice_prompt() != NULL;
}

TEST(prompt_streams_into_agent_buffer) {
    ASSERT_LUA(L, "ask('hello')");
    ASSERT_TRUE(lua_test_pump(L, 10000));
    ASSERT_LUA(L,
        "assert(stop == 'end_turn', tostring(stop))\n"
        "assert(reply() == 'Hello, world', reply())\n"
        "for _, b in ipairs(loki.buffers()) do\n"
        "  if b.name == '*agent*' then assert(not b.dirty and not b.current) end\n"
        "end");
}

TEST(handshake_sends_capabilities_and_context) {
    ASSERT_LUA(L,
        "assert(mcp.start(dir .. '/run/mcp.sock'))\n"
        "acp.stop()\n"
        "loki.open(dir .. '/code.txt')\n"
        "ask('info')");
    ASSERT_TRUE(lua_test_pump(L, 10000));
    ASSERT_LUA(L,
        "local info = json.decode(reply())\n"
        "assert(info.init.protocolVersion == 1)\n"
        "assert(info.init.clientCapabilities.fs.readTextFile == true)\n"
        "assert(info.init.clientCapabilities.terminal == false)\n"
        "assert(info.new.cwd == loki.abspath('.'))\n"
        "local srv = info.new.mcpServers[1]\n"
        "assert(srv.name == 'loki' and srv.args[1] == '--mcp-shim', json.encode(srv))\n"
        "assert(srv.command:sub(1, 1) == '/')\n"
        "local link = info.prompt[2]\n"
        "assert(link.type == 'resource_link' and link.uri:sub(-8) == 'code.txt', json.encode(link))\n"
        "mcp.stop()");
}

TEST(fs_reads_unsaved_buffer_and_writes_through) {
    ASSERT_LUA(L,
        "id = loki.open(dir .. '/code.txt')\n"
        "local t = loki.buffer_text(id)\n"
        "assert(loki.buffer_replace(id, #t + 1, #t, '\\nx = 1'))  -- unsaved\n"
        "ask('edit ' .. loki.abspath(dir .. '/code.txt'))");
    ASSERT_TRUE(lua_test_pump(L, 10000));
    ASSERT_LUA(L,
        "assert(reply() == 'line2=b = 2', reply())\n"
        "assert(loki.buffer_text(id) == 'a = 2\\nb = 2\\nx = 2', loki.buffer_text(id))\n"
        "for _, b in ipairs(loki.buffers()) do if b.id == id then assert(not b.dirty) end end");
    /* The agent saw the unsaved line, and the result reached the disk. */
    ASSERT_STR_EQ(read_file("code.txt"), "a = 2\nb = 2\nx = 2\n");
}

TEST(fs_write_creates_unopened_file) {
    ASSERT_LUA(L, "ask('create ' .. loki.abspath(dir) .. '/made.txt')");
    ASSERT_TRUE(lua_test_pump(L, 10000));
    ASSERT_LUA(L,
        "assert(reply() == 'created', reply())\n"
        "assert(loki.buffer_find(dir .. '/made.txt') == nil)");
    ASSERT_STR_EQ(read_file("made.txt"), "new\n");
}

TEST(fs_refuses_paths_outside_root) {
    ASSERT_LUA(L, "ask('escape')");
    ASSERT_TRUE(lua_test_pump(L, 10000));
    ASSERT_LUA(L, "assert(reply() == 'denied', reply())");
}

TEST(permission_answered_by_key) {
    ASSERT_LUA(L, "ask('perm')");
    ASSERT_TRUE(wait_for_choice());
    ASSERT_TRUE(strstr(choice_prompt(), "Run tests") != NULL);
    ASSERT_TRUE(strstr(choice_prompt(), "[2] Reject") != NULL);
    ASSERT_EQ(choice_handle_key('x'), 1);   /* swallowed, still pending */
    ASSERT_NOT_NULL(choice_prompt());
    ASSERT_EQ(choice_handle_key('2'), 1);
    ASSERT_NULL(choice_prompt());
    ASSERT_TRUE(lua_test_pump(L, 10000));
    ASSERT_LUA(L, "assert(reply() == 'chose:reject-once', reply())");

    ASSERT_LUA(L, "ask('perm')");
    ASSERT_TRUE(wait_for_choice());
    ASSERT_EQ(choice_handle_key(ESC), 1);
    ASSERT_TRUE(lua_test_pump(L, 10000));
    ASSERT_LUA(L, "assert(reply() == 'chose:cancelled', reply())");
}

TEST(cancel_answers_pending_permission) {
    ASSERT_LUA(L, "ask('wait')");
    ASSERT_TRUE(wait_for_choice());
    ASSERT_LUA(L, "acp.cancel()");
    ASSERT_NULL(choice_prompt());
    ASSERT_TRUE(lua_test_pump(L, 10000));
    ASSERT_LUA(L,
        "assert(stop == 'cancelled', tostring(stop))\n"
        "assert(reply() == 'outcome:cancelled', reply())");
}

TEST(busy_agent_rejects_second_prompt) {
    ASSERT_LUA(L,
        "ask('perm')\n"
        "local ok, err = acp.ask('hello')\n"
        "assert(ok == nil and err:find('busy'), tostring(err))");
    ASSERT_TRUE(wait_for_choice());
    choice_handle_key('1');
    ASSERT_TRUE(lua_test_pump(L, 10000));
}

TEST(stop_ends_agent_process) {
    ASSERT_LUA(L,
        "assert(acp.running())\n"
        "acp.stop()\n"
        "assert(not acp.running())");
    for (int i = 0; i < 400 && loki_spawn_count() > 0; i++) {
        uv_run(uv_default_loop(), UV_RUN_NOWAIT);
        nanosleep(&(struct timespec){0, 5 * 1000 * 1000}, NULL);
    }
    ASSERT_EQ(loki_spawn_count(), 0);
}

TEST(missing_agent_reports_error) {
    ASSERT_LUA(L,
        "local ok, err = acp.start({'loki-no-such-agent'})\n"
        "assert(ok == nil and err:find('loki%-no%-such%-agent'), tostring(err))\n"
        "assert(not acp.running())");
}

BEGIN_TEST_SUITE("ACP client")
    if (!mkdtemp(dir)) return 1;
    char path[512];
    snprintf(path, sizeof(path), "%s/code.txt", dir);
    FILE *f = fopen(path, "w");
    fputs("a = 1\nb = 1\n", f);
    fclose(f);
    if (chdir(dir) != 0) return 1;  /* the agent's root is the cwd */

    editor_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.view.screencols = 80;
    ctx.view.screenrows = 24;
    buffers_init(&ctx);
    L = lua_test_state(buffer_get_current());
    lua_pushstring(L, dir);
    lua_setglobal(L, "dir");
    lua_pushstring(L, PYTHON3);
    lua_setglobal(L, "PYTHON3");
    lua_pushstring(L, MOCK_AGENT);
    lua_setglobal(L, "MOCK_AGENT");
    if (luaL_dostring(L, prelude) != LUA_OK) {
        printf("prelude: %s\n", lua_tostring(L, -1));
        return 1;
    }

    RUN_TEST(prompt_streams_into_agent_buffer);
    RUN_TEST(handshake_sends_capabilities_and_context);
    RUN_TEST(fs_reads_unsaved_buffer_and_writes_through);
    RUN_TEST(fs_write_creates_unopened_file);
    RUN_TEST(fs_refuses_paths_outside_root);
    RUN_TEST(permission_answered_by_key);
    RUN_TEST(cancel_answers_pending_permission);
    RUN_TEST(busy_agent_rejects_second_prompt);
    RUN_TEST(stop_ends_agent_process);
    RUN_TEST(missing_agent_reports_error);

    loki_spawn_shutdown();
    lua_close(L);
    buffers_free();
    if (chdir("/") != 0) return 1;
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd) != 0) printf("cleanup failed: %s\n", dir);
END_TEST_SUITE()
