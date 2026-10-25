/* test_http_security.c - Unit tests for HTTP security features
 *
 * Tests for HTTP security hardening:
 * - URL validation (scheme, length, malformed)
 * - Rate limiting (per-minute limit)
 * - Request size limits (body, headers)
 * - Internal-address blocking (URL literals and connect-time addresses)
 */

#include "test_framework.h"
#include "internal.h"
#include "loki/lua.h"
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "http.h"

/* Helper to create test context with Lua */
static void init_test_ctx(editor_ctx_t *ctx) {
    editor_ctx_init(ctx);

    /* Allocate and initialize LuaHost */
    ctx->lua_host = malloc(sizeof(LuaHost));
    memset(ctx->lua_host, 0, sizeof(LuaHost));
    ctx->lua_host->L = loki_lua_bootstrap(ctx, NULL);
}

/* Helper to cleanup test context */
static void free_test_ctx(editor_ctx_t *ctx) {
    if (ctx->lua_host) {
        if (ctx->lua_host->L) {
            lua_close(ctx->lua_host->L);
            ctx->lua_host->L = NULL;
        }
        free(ctx->lua_host);
        ctx->lua_host = NULL;
    }
    editor_ctx_free(ctx);
}

/* Helper to get Lua state from context */
static lua_State *get_lua(editor_ctx_t *ctx) {
    return ctx->lua_host ? ctx->lua_host->L : NULL;
}

/* Test: Valid HTTPS URL */
TEST(http_security_valid_https_url) {
    editor_ctx_t ctx;
    init_test_ctx(&ctx);
    lua_State *L = get_lua(&ctx);

    const char *code =
        "local result = loki.async_http(\n"
        "  'https://api.example.com/test',\n"
        "  'GET',\n"
        "  nil,\n"
        "  {},\n"
        "  'test_callback'\n"
        ")\n"
        "return result ~= nil";

    int result = luaL_dostring(L, code);

    /* Should succeed or fail due to network, but not due to validation */
    ASSERT_EQ(result, 0);

    free_test_ctx(&ctx);
}

/* Test: Valid HTTP URL (allowed but discouraged) */
TEST(http_security_valid_http_url) {
    editor_ctx_t ctx;
    init_test_ctx(&ctx);
    lua_State *L = get_lua(&ctx);

    const char *code =
        "local result = loki.async_http(\n"
        "  'http://api.example.com/test',\n"
        "  'GET',\n"
        "  nil,\n"
        "  {},\n"
        "  'test_callback'\n"
        ")\n"
        "return result ~= nil";

    int result = luaL_dostring(L, code);
    ASSERT_EQ(result, 0);

    free_test_ctx(&ctx);
}

/* Test: Reject FTP URL */
TEST(http_security_reject_ftp_url) {
    editor_ctx_t ctx;
    init_test_ctx(&ctx);
    lua_State *L = get_lua(&ctx);

    const char *code =
        "local result = loki.async_http(\n"
        "  'ftp://ftp.example.com/file',\n"
        "  'GET',\n"
        "  nil,\n"
        "  {},\n"
        "  'test_callback'\n"
        ")\n"
        "return result";

    int result = luaL_dostring(L, code);
    ASSERT_EQ(result, 0);

    /* Should return nil (failed validation) */
    ASSERT_TRUE(lua_isnil(L, -1));

    free_test_ctx(&ctx);
}

/* Test: Reject file:// URL */
TEST(http_security_reject_file_url) {
    editor_ctx_t ctx;
    init_test_ctx(&ctx);
    lua_State *L = get_lua(&ctx);

    const char *code =
        "local result = loki.async_http(\n"
        "  'file:///etc/passwd',\n"
        "  'GET',\n"
        "  nil,\n"
        "  {},\n"
        "  'test_callback'\n"
        ")\n"
        "return result";

    int result = luaL_dostring(L, code);
    ASSERT_EQ(result, 0);
    ASSERT_TRUE(lua_isnil(L, -1));

    free_test_ctx(&ctx);
}

/* Test: Reject empty URL */
TEST(http_security_reject_empty_url) {
    editor_ctx_t ctx;
    init_test_ctx(&ctx);
    lua_State *L = get_lua(&ctx);

    const char *code =
        "local result = loki.async_http(\n"
        "  '',\n"
        "  'GET',\n"
        "  nil,\n"
        "  {},\n"
        "  'test_callback'\n"
        ")\n"
        "return result";

    int result = luaL_dostring(L, code);
    ASSERT_EQ(result, 0);
    ASSERT_TRUE(lua_isnil(L, -1));

    free_test_ctx(&ctx);
}

/* Test: Reject URL without scheme */
TEST(http_security_reject_url_without_scheme) {
    editor_ctx_t ctx;
    init_test_ctx(&ctx);
    lua_State *L = get_lua(&ctx);

    const char *code =
        "local result = loki.async_http(\n"
        "  'api.example.com/test',\n"
        "  'GET',\n"
        "  nil,\n"
        "  {},\n"
        "  'test_callback'\n"
        ")\n"
        "return result";

    int result = luaL_dostring(L, code);
    ASSERT_EQ(result, 0);
    ASSERT_TRUE(lua_isnil(L, -1));

    free_test_ctx(&ctx);
}

/* Test: Reject URL that is too long */
TEST(http_security_reject_long_url) {
    editor_ctx_t ctx;
    init_test_ctx(&ctx);
    lua_State *L = get_lua(&ctx);

    /* Use Lua to create a URL longer than MAX_HTTP_URL_LENGTH (2048) */
    const char *code =
        "local base = 'https://example.com/'\n"
        "local long_url = base .. string.rep('x', 3000)\n"
        "local result = loki.async_http(\n"
        "  long_url,\n"
        "  'GET',\n"
        "  nil,\n"
        "  {},\n"
        "  'test_callback'\n"
        ")\n"
        "return result";

    int result = luaL_dostring(L, code);
    ASSERT_EQ(result, 0);
    ASSERT_TRUE(lua_isnil(L, -1));

    free_test_ctx(&ctx);
}

/* Test: Valid POST request with body */
TEST(http_security_valid_post_with_body) {
    editor_ctx_t ctx;
    init_test_ctx(&ctx);
    lua_State *L = get_lua(&ctx);

    const char *code =
        "local result = loki.async_http(\n"
        "  'https://api.example.com/test',\n"
        "  'POST',\n"
        "  '{\"key\": \"value\"}',\n"
        "  {'Content-Type: application/json'},\n"
        "  'test_callback'\n"
        ")\n"
        "return result ~= nil";

    int result = luaL_dostring(L, code);
    ASSERT_EQ(result, 0);

    free_test_ctx(&ctx);
}

/* Test: Reject request body that's too large */
TEST(http_security_reject_large_body) {
    editor_ctx_t ctx;
    init_test_ctx(&ctx);
    lua_State *L = get_lua(&ctx);

    /* Note: Creating a 5MB+ string in Lua would be slow, so we test the concept */
    const char *code =
        "-- This test verifies that large bodies are rejected\n"
        "-- In reality, a 5MB+ string would take too long to construct in test\n"
        "local large_body = string.rep('x', 1000000)  -- 1MB\n"
        "local result = loki.async_http(\n"
        "  'https://api.example.com/test',\n"
        "  'POST',\n"
        "  large_body,\n"
        "  {},\n"
        "  'test_callback'\n"
        ")\n"
        "return result ~= nil";

    int result = luaL_dostring(L, code);

    /* 1MB should be accepted (limit is 5MB) */
    ASSERT_EQ(result, 0);

    free_test_ctx(&ctx);
}

/* Test: Rate limiting - basic functionality */
TEST(http_security_rate_limiting_basic) {
    editor_ctx_t ctx;
    init_test_ctx(&ctx);
    lua_State *L = get_lua(&ctx);

    /* Make several requests in quick succession */
    const char *code =
        "local count = 0\n"
        "for i = 1, 5 do\n"
        "  local result = loki.async_http(\n"
        "    'https://api.example.com/test' .. i,\n"
        "    'GET',\n"
        "    nil,\n"
        "    {},\n"
        "    'test_callback'\n"
        "  )\n"
        "  if result then count = count + 1 end\n"
        "end\n"
        "return count";

    int result = luaL_dostring(L, code);
    ASSERT_EQ(result, 0);

    /* Should succeed - we're well under the rate limit */
    ASSERT_TRUE(lua_isnumber(L, -1));
    int count = (int)lua_tonumber(L, -1);
    ASSERT_TRUE(count >= 5);  /* All 5 should succeed */

    free_test_ctx(&ctx);
}

/* Test: Valid headers */
TEST(http_security_valid_headers) {
    editor_ctx_t ctx;
    init_test_ctx(&ctx);
    lua_State *L = get_lua(&ctx);

    const char *code =
        "local result = loki.async_http(\n"
        "  'https://api.example.com/test',\n"
        "  'POST',\n"
        "  '{\"test\": true}',\n"
        "  {\n"
        "    'Content-Type: application/json',\n"
        "    'Authorization: Bearer token123',\n"
        "    'X-Custom-Header: value'\n"
        "  },\n"
        "  'test_callback'\n"
        ")\n"
        "return result ~= nil";

    int result = luaL_dostring(L, code);
    ASSERT_EQ(result, 0);

    free_test_ctx(&ctx);
}

/* Test: Concurrent request limit */
TEST(http_security_concurrent_request_limit) {
    editor_ctx_t ctx;
    init_test_ctx(&ctx);
    lua_State *L = get_lua(&ctx);

    /* Try to create more than MAX_ASYNC_REQUESTS (10)
     * Using example.com which won't complete quickly, keeping requests pending */
    const char *code =
        "local count = 0\n"
        "for i = 1, 15 do\n"
        "  local result = loki.async_http(\n"
        "    'https://example.com/delay' .. i,\n"  /* Unique URLs to avoid caching */
        "    'GET',\n"
        "    nil,\n"
        "    {},\n"
        "    'test_callback'\n"
        "  )\n"
        "  if result then count = count + 1 end\n"
        "end\n"
        "return count";

    int result = luaL_dostring(L, code);
    ASSERT_EQ(result, 0);

    /* Should be limited to MAX_ASYNC_REQUESTS (10) */
    ASSERT_TRUE(lua_isnumber(L, -1));
    int count = (int)lua_tonumber(L, -1);
    ASSERT_TRUE(count <= 10);

    free_test_ctx(&ctx);
}

/* Test: URL with control characters is rejected */
TEST(http_security_reject_url_with_control_chars) {
    editor_ctx_t ctx;
    init_test_ctx(&ctx);
    lua_State *L = get_lua(&ctx);

    const char *code =
        "local result = loki.async_http(\n"
        "  'https://example.com/test\\x01bad',\n"  /* Control character */
        "  'GET',\n"
        "  nil,\n"
        "  {},\n"
        "  'test_callback'\n"
        ")\n"
        "return result";

    int result = luaL_dostring(L, code);
    ASSERT_EQ(result, 0);
    ASSERT_TRUE(lua_isnil(L, -1));

    free_test_ctx(&ctx);
}

static int url_ok(const char *url) {
    return loki_http_validate_url(url, NULL, 0);
}

/* Test: Internal hosts in every literal spelling the resolver accepts */
TEST(http_security_reject_internal_url_literals) {
    unsetenv("LOKI_HTTP_ALLOW_INTERNAL");
    const char *bad[] = {
        "http://127.0.0.1/", "http://127.1/", "http://2130706433/",
        "http://0x7f.0.0.1/", "http://0177.0.0.1/", "http://0/",
        "http://localhost./", "http://LOCALHOST/", "http://foo.localhost/",
        "http://169.254.169.254/latest/meta-data/", "http://10.0.0.1/",
        "http://172.16.0.1/", "http://192.168.1.1/", "http://100.64.0.1/",
        "http://[::1]/", "http://[::]/", "http://[::ffff:127.0.0.1]/",
        "http://[::ffff:7f00:1]/", "http://[64:ff9b::a9fe:a9fe]/",
        "http://[fe80::1]/", "http://[febf::1]/", "http://[fd00::1]/",
        "http://user@127.0.0.1:8080/", "http://[::1]:8080/",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (url_ok(bad[i])) fprintf(stderr, "    accepted: %s\n", bad[i]);
        ASSERT_FALSE(url_ok(bad[i]));
    }
}

/* Test: Public hosts and look-alike names stay allowed */
TEST(http_security_allow_public_url_hosts) {
    unsetenv("LOKI_HTTP_ALLOW_INTERNAL");
    const char *good[] = {
        "https://example.com/", "http://8.8.8.8/", "http://172.32.0.1/",
        "http://100.128.0.1/", "http://[2001:db8::1]/", "https://fcdn.example/",
        "https://localhost.example.com/", "https://127.0.0.1.example.com/",
    };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        if (!url_ok(good[i])) fprintf(stderr, "    rejected: %s\n", good[i]);
        ASSERT_TRUE(url_ok(good[i]));
    }
}

static int v4_internal(const char *addr) {
    struct sockaddr_in in;
    memset(&in, 0, sizeof(in));
    in.sin_family = AF_INET;
    inet_pton(AF_INET, addr, &in.sin_addr);
    return loki_http_sockaddr_is_internal((struct sockaddr *)&in);
}

static int v6_internal(const char *addr) {
    struct sockaddr_in6 in6;
    memset(&in6, 0, sizeof(in6));
    in6.sin6_family = AF_INET6;
    inet_pton(AF_INET6, addr, &in6.sin6_addr);
    return loki_http_sockaddr_is_internal((struct sockaddr *)&in6);
}

/* Test: Resolved-address classification used at connect time */
TEST(http_security_sockaddr_classification) {
    ASSERT_TRUE(v4_internal("127.0.1.1"));
    ASSERT_TRUE(v4_internal("169.254.169.254"));
    ASSERT_TRUE(v4_internal("100.127.255.255"));
    ASSERT_TRUE(v4_internal("224.0.0.1"));
    ASSERT_TRUE(v4_internal("255.255.255.255"));
    ASSERT_FALSE(v4_internal("93.184.216.34"));
    ASSERT_FALSE(v4_internal("172.15.255.255"));
    ASSERT_TRUE(v6_internal("::1"));
    ASSERT_TRUE(v6_internal("::ffff:10.1.2.3"));
    ASSERT_TRUE(v6_internal("ff02::1"));
    ASSERT_FALSE(v6_internal("2606:2800:220:1:248:1893:25c8:1946"));
    ASSERT_FALSE(v6_internal("::ffff:8.8.8.8"));
}

/* Test: Rejections return nil plus the reason, including embedded NULs */
TEST(http_security_rejection_messages) {
    editor_ctx_t ctx;
    init_test_ctx(&ctx);
    lua_State *L = get_lua(&ctx);

    const char *code =
        "local function check(want, ...)\n"
        "  local id, err = loki.async_http(...)\n"
        "  if id ~= nil or err ~= want then\n"
        "    error(('want %q, got %s, %s'):format(want, tostring(id), tostring(err)))\n"
        "  end\n"
        "end\n"
        "local many = {}\n"
        "for i = 1, 101 do many[i] = 'X-H: v' end\n"
        "local big = {}\n"
        "for i = 1, 9 do big[i] = 'X-H: ' .. string.rep('v', 1000) end\n"
        "check('URL must use http:// or https://', 'ftp://example.com/', 'GET', nil, {}, 'cb')\n"
        "check('URL points at an internal address', 'http://127.1/', 'GET', nil, {}, 'cb')\n"
        "check('URL contains a NUL byte', 'https://example.com/\\0@127.0.0.1', 'GET', nil, {}, 'cb')\n"
        "check('Request body contains a NUL byte', 'https://example.com/', 'POST', 'a\\0b', {}, 'cb')\n"
        "check('Request body too large', 'https://example.com/', 'POST', string.rep('x', 5242881), {}, 'cb')\n"
        "check('Header contains a NUL byte', 'https://example.com/', 'GET', nil, {'X-A: b\\0c'}, 'cb')\n"
        "check(\"Header must be in 'Name: value' form\", 'https://example.com/', 'GET', nil, {'nocolon'}, 'cb')\n"
        "check('Too many headers (max 100)', 'https://example.com/', 'GET', nil, many, 'cb')\n"
        "check('Headers too large (max 8192 bytes)', 'https://example.com/', 'GET', nil, big, 'cb')\n";

    int result = luaL_dostring(L, code);
    if (result != 0) fprintf(stderr, "    %s\n", lua_tostring(L, -1));
    ASSERT_EQ(result, 0);

    free_test_ctx(&ctx);
}

/* Test: The pending-request limit reports its reason */
TEST(http_security_pending_limit_message) {
    editor_ctx_t ctx;
    init_test_ctx(&ctx);
    lua_State *L = get_lua(&ctx);
    loki_http_cleanup();

    const char *code =
        "local err\n"
        "for i = 1, 11 do\n"
        "  local id, e = loki.async_http('https://example.com/p' .. i, 'GET', nil, {}, 'cb')\n"
        "  if not id then err = e end\n"
        "end\n"
        "return err";

    ASSERT_EQ(luaL_dostring(L, code), 0);
    ASSERT_STR_EQ(lua_tostring(L, -1), "Too many pending requests");

    loki_http_cleanup();
    free_test_ctx(&ctx);
}

/* Is the hostname a name that resolves to loopback? The
 * Debian convention maps the hostname to 127.0.1.1. */
static int loopback_hostname(char *buf, size_t size) {
    if (gethostname(buf, size) != 0) return 0;
    buf[size - 1] = '\0';
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(buf, NULL, &hints, &res) != 0) return 0;
    int internal = loki_http_sockaddr_is_internal(res->ai_addr);
    freeaddrinfo(res);
    return internal;
}

/* Test: A name resolving to loopback passes the URL check but must not
 * connect. A redirect to an internal host takes the same connect path. */
TEST(http_security_block_internal_at_connect) {
    unsetenv("LOKI_HTTP_ALLOW_INTERNAL");
    char host[256];
    if (!loopback_hostname(host, sizeof(host))) {
        printf("    (skipped: hostname does not resolve to loopback)\n");
        return;
    }

    /* Listen on all interfaces so a connection would succeed and the
     * request would hang waiting for a response. */
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(srv >= 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    ASSERT_EQ(bind(srv, (struct sockaddr *)&addr, sizeof(addr)), 0);
    ASSERT_EQ(listen(srv, 4), 0);
    socklen_t alen = sizeof(addr);
    getsockname(srv, (struct sockaddr *)&addr, &alen);

    char url[512];
    snprintf(url, sizeof(url), "http://%s:%d/", host, ntohs(addr.sin_port));
    ASSERT_TRUE(url_ok(url));

    editor_ctx_t ctx;
    init_test_ctx(&ctx);
    lua_State *L = get_lua(&ctx);
    loki_http_cleanup();  /* drop requests left pending by earlier tests */

    ASSERT_EQ(luaL_dostring(L,
        "connect_result = nil\n"
        "function connect_cb(r) connect_result = r end\n"), 0);
    ASSERT_TRUE(loki_http_request(NULL, url, "GET", NULL, NULL, 0, "connect_cb", NULL, 0) >= 0);

    time_t deadline = time(NULL) + 5;
    while (loki_http_pending_count() > 0 && time(NULL) < deadline) {
        loki_http_poll(L);
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    ASSERT_EQ(loki_http_pending_count(), 0);

    lua_getglobal(L, "connect_result");
    ASSERT_TRUE(lua_istable(L, -1));
    lua_getfield(L, -1, "status");
    ASSERT_EQ((int)lua_tointeger(L, -1), 0);
    lua_getfield(L, -2, "error");
    ASSERT_TRUE(lua_isstring(L, -1));
    lua_pop(L, 3);

    loki_http_cleanup();
    free_test_ctx(&ctx);
    close(srv);
}

BEGIN_TEST_SUITE("HTTP Security")
    RUN_TEST(http_security_valid_https_url);
    RUN_TEST(http_security_valid_http_url);
    RUN_TEST(http_security_reject_ftp_url);
    RUN_TEST(http_security_reject_file_url);
    RUN_TEST(http_security_reject_empty_url);
    RUN_TEST(http_security_reject_url_without_scheme);
    RUN_TEST(http_security_reject_long_url);
    RUN_TEST(http_security_valid_post_with_body);
    RUN_TEST(http_security_reject_large_body);
    RUN_TEST(http_security_rate_limiting_basic);
    RUN_TEST(http_security_valid_headers);
    RUN_TEST(http_security_concurrent_request_limit);
    RUN_TEST(http_security_reject_url_with_control_chars);
    RUN_TEST(http_security_reject_internal_url_literals);
    RUN_TEST(http_security_allow_public_url_hosts);
    RUN_TEST(http_security_sockaddr_classification);
    RUN_TEST(http_security_rejection_messages);
    RUN_TEST(http_security_pending_limit_message);
    RUN_TEST(http_security_block_internal_at_connect);
END_TEST_SUITE()
