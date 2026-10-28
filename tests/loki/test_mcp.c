/* test_mcp.c - MCP server (mcp.lua), JSON-RPC core, loki.listen, the shim
 *
 * Protocol tests feed lines to a peer and decode what it writes. The socket
 * tests run the real `loki --mcp-shim` under loki.spawn against a server in
 * this process, so one libuv loop drives both ends.
 */

#include "test_framework.h"
#include "loki/core.h"
#include "internal.h"
#include "buffers.h"
#include "spawn.h"
#include "lua_helpers.h"
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static lua_State *L;
static char dir[] = "/tmp/loki_mcpXXXXXX";

/* call(method, params[, meta]) sends one request and returns the decoded
 * reply; meta adds a per-request protocol version. */
static const char *prelude =
    "package.path = '" LOKI_MODULES_DIR "/?.lua;' .. package.path\n"
    "json = loki.json\n"
    "mcp = require('mcp')\n"
    "jsonrpc = require('jsonrpc')\n"
    "mcp.root = loki.abspath(dir)\n"
    "out = {}\n"
    "peer = mcp.peer(function(s) out[#out + 1] = s end)\n"
    "next_id = 0\n"
    "function call(method, params, version)\n"
    "  next_id = next_id + 1\n"
    "  params = params or {}\n"
    "  if version then params._meta = {['io.modelcontextprotocol/protocolVersion'] = version} end\n"
    "  out = {}\n"
    "  peer.receive(json.encode({jsonrpc = '2.0', id = next_id, method = method, params = params}))\n"
    "  assert(#out == 1, 'expected one reply, got ' .. #out)\n"
    "  assert(out[1]:sub(-1) == '\\n' and not out[1]:sub(1, -2):find('\\n'), 'one line per message')\n"
    "  local r = json.decode(out[1])\n"
    "  assert(r.id == next_id)\n"
    "  return r\n"
    "end\n"
    "function tool(name, args)\n"
    "  local r = call('tools/call', {name = name, arguments = args}, '2026-07-28')\n"
    "  assert(r.result, json.encode(r))\n"
    "  return r.result.content[1].text, r.result.isError\n"
    "end\n";

TEST(legacy_initialize_negotiates_version) {
    ASSERT_LUA(L,
        "local r = call('initialize', {protocolVersion = '2025-06-18', capabilities = {},\n"
        "  clientInfo = {name = 't', version = '1'}})\n"
        "assert(r.result.protocolVersion == '2025-06-18')\n"
        "assert(r.result.capabilities.tools and r.result.serverInfo.name == 'loki')\n"
        "r = call('initialize', {protocolVersion = '1999-01-01'})\n"
        "assert(r.result.protocolVersion == mcp.LEGACY[1])\n"
        "assert(r.result.resultType == nil)");
}

TEST(notification_gets_no_reply) {
    ASSERT_LUA(L,
        "out = {}\n"
        "peer.receive('{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}')\n"
        "peer.receive('{\"jsonrpc\":\"2.0\",\"method\":\"no/such/notification\"}')\n"
        "assert(#out == 0)");
}

TEST(discover_lists_versions) {
    ASSERT_LUA(L,
        "local r = call('server/discover', {}, '2026-07-28')\n"
        "assert(r.result.resultType == 'complete')\n"
        "assert(r.result.supportedVersions[1] == '2026-07-28')\n"
        "assert(r.result._meta['io.modelcontextprotocol/serverInfo'].name == 'loki')");
}

TEST(unsupported_version_is_rejected_with_list) {
    ASSERT_LUA(L,
        "local r = call('tools/list', {}, '1900-01-01')\n"
        "assert(r.error.code == -32022, json.encode(r))\n"
        "assert(r.error.data.requested == '1900-01-01')\n"
        "assert(r.error.data.supported[1] == '2026-07-28')");
}

TEST(tools_list_is_stable_and_valid) {
    ASSERT_LUA(L,
        "local r = call('tools/list', {}, '2026-07-28')\n"
        "local names = {}\n"
        "for _, t in ipairs(r.result.tools) do\n"
        "  assert(t.inputSchema.type == 'object')\n"
        "  names[#names + 1] = t.name\n"
        "end\n"
        "assert(table.concat(names, ',') == 'list_buffers,read_buffer,get_selection,edit_buffer,open_file')\n"
        "assert(out[1]:find('\"properties\":{}', 1, true), 'empty properties must be an object')");
}

TEST(protocol_errors) {
    ASSERT_LUA(L,
        "assert(call('no/such/method').error.code == jsonrpc.METHOD_NOT_FOUND)\n"
        "assert(call('tools/call', {name = 'nope'}).error.code == jsonrpc.INVALID_PARAMS)\n"
        "out = {}\n"
        "peer.receive('{not json')\n"
        "assert(json.decode(out[1]).error.code == jsonrpc.PARSE_ERROR)");
}

TEST(tools_read_and_edit_buffers) {
    ASSERT_LUA(L,
        "local path = dir .. '/src.txt'\n"
        "assert(tool('open_file', {path = path}):find('opened'))\n"
        "local text = tool('read_buffer', {path = path})\n"
        "assert(text == 'local x = 1\\nlocal y = 1', text)\n"
        "assert(tool('read_buffer', {path = path, start_line = 2}) == 'local y = 1')\n"
        "local list = json.decode(tool('list_buffers'))\n"
        "local found = false\n"
        "for _, b in ipairs(list) do if b.path == loki.abspath(path) then found = true end end\n"
        "assert(found)\n"
        "local msg, is_err = tool('edit_buffer', {path = path, old_string = '= 1', new_string = '= 2'})\n"
        "assert(is_err and msg:find('2 times'), msg)\n"
        "msg, is_err = tool('edit_buffer', {path = path, old_string = 'y = 1', new_string = 'y = 2'})\n"
        "assert(not is_err, msg)\n"
        "assert(tool('read_buffer', {path = path}) == 'local x = 1\\nlocal y = 2')\n"
        "msg, is_err = tool('edit_buffer', {path = path, old_string = 'zzz', new_string = ''})\n"
        "assert(is_err and msg:find('not found'))");
    /* The edit is in the buffer, not on disk. */
    char path[512];
    snprintf(path, sizeof(path), "%s/src.txt", dir);
    FILE *f = fopen(path, "r");
    ASSERT_NOT_NULL(f);
    char buf[64] = {0};
    ASSERT_TRUE(fread(buf, 1, sizeof(buf) - 1, f) > 0);
    fclose(f);
    ASSERT_STR_EQ(buf, "local x = 1\nlocal y = 1\n");
}

TEST(tools_refuse_paths_outside_root) {
    ASSERT_LUA(L,
        "local msg, is_err = tool('open_file', {path = '/etc/hostname'})\n"
        "assert(is_err and msg:find('outside the project'), msg)\n"
        "msg, is_err = tool('open_file', {path = dir .. '/missing/..'})\n"
        "assert(is_err and msg:find('outside the project'), msg)\n"
        "msg, is_err = tool('open_file', {path = dir .. '/link/hostname'})\n"
        "assert(is_err and msg:find('outside the project'), msg)\n"
        "msg, is_err = tool('read_buffer', {path = dir .. '/never-opened.txt'})\n"
        "assert(is_err and msg:find('not open'), msg)");
}

TEST(lines_splits_and_joins_chunks) {
    ASSERT_LUA(L,
        "local got = {}\n"
        "local feed = jsonrpc.lines(function(l) got[#got + 1] = l end)\n"
        "feed('{\"a\"'); feed(':1}\\n{\"b\":2}\\n\\n{\"c\"'); feed(':3}\\n')\n"
        "assert(#got == 3 and got[1] == '{\"a\":1}' and got[3] == '{\"c\":3}')\n"
        "local over = false\n"
        "local saved = jsonrpc.MAX_LINE; jsonrpc.MAX_LINE = 4\n"
        "feed = jsonrpc.lines(function() end, function() over = true end)\n"
        "feed('123456')\n"
        "jsonrpc.MAX_LINE = saved\n"
        "assert(over)");
}

TEST(listen_refuses_insecure_paths) {
    ASSERT_LUA(L,
        "local s, err = loki.listen{path = '/tmp/loki-test.sock'}\n"
        "assert(s == nil and err:find('another user'), tostring(err))\n"
        "s, err = loki.listen{path = dir .. '/shared/x.sock'}\n"
        "assert(s == nil and err:find('other users'), tostring(err))\n"
        "s, err = loki.listen{path = dir .. '/src.txt'}\n"
        "assert(s == nil, 'listen over a regular file')\n"
        "s, err = loki.listen{path = dir .. '/' .. string.rep('x', 200)}\n"
        "assert(s == nil and err:find('too long'), tostring(err))");
}

TEST(shim_round_trip_over_socket) {
    ASSERT_LUA(L,
        "sock = dir .. '/run/test.sock'\n"
        "assert(mcp.start(sock) == sock)\n"
        "local again, err = loki.listen{path = sock}\n"
        "assert(again == nil and err:find('in use'), tostring(err))\n"
        "done = false; replies = {}\n"
        "local feed = jsonrpc.lines(function(l) replies[#replies + 1] = json.decode(l)\n"
        "  if #replies == 2 then done = true end end)\n"
        "shim = assert(loki.spawn{cmd = LOKI_BINARY, args = {'--mcp-shim', sock},\n"
        "  on_stdout = function(_, s) feed(s) end})\n"
        "shim:write(json.encode({jsonrpc = '2.0', id = 1, method = 'server/discover',\n"
        "  params = {_meta = {['io.modelcontextprotocol/protocolVersion'] = '2026-07-28'}}}) .. '\\n')\n"
        "shim:write(json.encode({jsonrpc = '2.0', id = 2, method = 'tools/call', params = {\n"
        "  name = 'read_buffer', arguments = {path = dir .. '/src.txt', end_line = 1},\n"
        "  _meta = {['io.modelcontextprotocol/protocolVersion'] = '2026-07-28'}}}) .. '\\n')");
    ASSERT_TRUE(lua_test_pump(L, 5000));
    ASSERT_LUA(L,
        "assert(replies[1].result.supportedVersions[1] == '2026-07-28')\n"
        "assert(replies[2].result.content[1].text == 'local x = 1', json.encode(replies[2]))\n"
        "shim:close_stdin()");
    ASSERT_LUA(L,
        "done = false\n"
        "local p = assert(loki.spawn{cmd = 'sh', args = {'-c', 'sleep 0.2'},\n"
        "  on_exit = function() done = true end})");
    ASSERT_TRUE(lua_test_pump(L, 5000));
    ASSERT_LUA(L,
        "assert(not shim:running(), 'shim exits when its stdin closes')\n"
        "mcp.stop()");
    /* The socket file is removed on close. */
    char sock[512];
    snprintf(sock, sizeof(sock), "%s/run/test.sock", dir);
    struct stat st;
    ASSERT_TRUE(stat(sock, &st) != 0);
}

TEST(stale_socket_is_replaced) {
    char sock[100];
    snprintf(sock, sizeof(sock), "%s/run/stale.sock", dir);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock);
    ASSERT_EQ(bind(fd, (struct sockaddr *)&addr, sizeof(addr)), 0);
    close(fd);  /* bound, never listened: connect is refused */
    ASSERT_LUA(L,
        "local s, err = loki.listen{path = dir .. '/run/stale.sock'}\n"
        "assert(s, tostring(err))\n"
        "s:close()");
}

BEGIN_TEST_SUITE("MCP server")
    if (!mkdtemp(dir)) return 1;
    char path[512];
    snprintf(path, sizeof(path), "%s/src.txt", dir);
    FILE *f = fopen(path, "w");
    fputs("local x = 1\nlocal y = 1\n", f);
    fclose(f);
    snprintf(path, sizeof(path), "%s/link", dir);
    if (symlink("/etc", path) != 0) return 1;
    snprintf(path, sizeof(path), "%s/shared", dir);
    if (mkdir(path, 0755) != 0 || chmod(path, 0755) != 0) return 1;

    editor_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.view.screencols = 80;
    ctx.view.screenrows = 24;
    buffers_init(&ctx);
    L = lua_test_state(buffer_get_current());
    lua_pushstring(L, dir);
    lua_setglobal(L, "dir");
    lua_pushstring(L, LOKI_BINARY);
    lua_setglobal(L, "LOKI_BINARY");
    if (luaL_dostring(L, prelude) != LUA_OK) {
        printf("prelude: %s\n", lua_tostring(L, -1));
        return 1;
    }

    RUN_TEST(legacy_initialize_negotiates_version);
    RUN_TEST(notification_gets_no_reply);
    RUN_TEST(discover_lists_versions);
    RUN_TEST(unsupported_version_is_rejected_with_list);
    RUN_TEST(tools_list_is_stable_and_valid);
    RUN_TEST(protocol_errors);
    RUN_TEST(tools_read_and_edit_buffers);
    RUN_TEST(tools_refuse_paths_outside_root);
    RUN_TEST(lines_splits_and_joins_chunks);
    RUN_TEST(listen_refuses_insecure_paths);
    RUN_TEST(shim_round_trip_over_socket);
    RUN_TEST(stale_socket_is_replaced);

    loki_spawn_shutdown();
    lua_close(L);
    buffers_free();
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd) != 0) printf("cleanup failed: %s\n", dir);
END_TEST_SUITE()
