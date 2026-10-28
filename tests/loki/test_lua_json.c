/* test_lua_json.c - loki.json encode/decode */

#include "test_framework.h"
#include "loki/core.h"
#include "internal.h"
#include "lua_helpers.h"

static lua_State *L;

TEST(json_round_trip_nested) {
    ASSERT_LUA(L,
        "local j = loki.json\n"
        "local v = j.decode(j.encode({a = {1, 2, {b = 'x'}}, n = -3, f = 1.5, t = true}))\n"
        "assert(v.a[3].b == 'x' and v.n == -3 and v.f == 1.5 and v.t == true)\n"
        "assert(math.type(v.n) == 'integer')");
}

TEST(json_null_sentinel) {
    ASSERT_LUA(L,
        "local j = loki.json\n"
        "assert(j.encode(j.null) == 'null')\n"
        "local v = j.decode('{\"result\":null}')\n"
        "assert(v.result == j.null)\n"
        "assert(j.encode({x = j.null}) == '{\"x\":null}')");
}

TEST(json_empty_table_is_object_unless_marked) {
    ASSERT_LUA(L,
        "local j = loki.json\n"
        "assert(j.encode({}) == '{}')\n"
        "assert(j.encode(j.array()) == '[]')\n"
        "assert(j.encode({c = j.array({})}) == '{\"c\":[]}')\n"
        "assert(j.encode(j.decode('[]')) == '[]')");
}

TEST(json_sequence_is_array) {
    ASSERT_LUA(L,
        "assert(loki.json.encode({'a', 'b'}) == '[\"a\",\"b\"]')");
}

TEST(json_keys_are_escaped) {
    ASSERT_LUA(L,
        "local s = loki.json.encode({['a\"b'] = 1})\n"
        "assert(s == '{\"a\\\\\"b\":1}', s)\n"
        "assert(loki.json.decode(s)['a\"b'] == 1)");
}

TEST(json_invalid_utf8_becomes_replacement) {
    /* \xe9 alone is Latin-1 e-acute: not UTF-8. */
    ASSERT_LUA(L,
        "local s = loki.json.encode('caf\\xe9')\n"
        "assert(s == '\"caf\\xef\\xbf\\xbd\"', s)\n"
        "assert(loki.json.encode('caf\\xc3\\xa9') == '\"caf\\xc3\\xa9\"')");
}

TEST(json_unicode_escape_decodes) {
    ASSERT_LUA(L,
        "assert(loki.json.decode('\"\\\\u00e9\"') == '\\xc3\\xa9')");
}

TEST(json_encode_errors) {
    ASSERT_LUA(L,
        "local function fails(v, pat)\n"
        "  local ok, err = pcall(loki.json.encode, v)\n"
        "  assert(not ok and err:find(pat), tostring(err))\n"
        "end\n"
        "fails(0/0, 'NaN')\n"
        "fails(function() end, 'type')\n"
        "fails({[true] = 1}, 'keys must be strings')\n"
        "local t = {}; t.self = t\n"
        "fails(t, 'too deep')");
}

TEST(json_decode_invalid_returns_nil) {
    ASSERT_LUA(L,
        "local v, err = loki.json.decode('{\"a\":')\n"
        "assert(v == nil and err == 'invalid JSON')");
}

TEST(json_large_integers) {
    ASSERT_LUA(L,
        "local j = loki.json\n"
        "assert(j.encode(2^62 | 0) == '4611686018427387904')\n"
        "assert(j.decode('4294967296') == 4294967296)\n"
        "assert(math.type(j.decode('4294967296')) == 'integer')");
}

BEGIN_TEST_SUITE("loki.json")
    L = lua_test_state(NULL);
    RUN_TEST(json_round_trip_nested);
    RUN_TEST(json_null_sentinel);
    RUN_TEST(json_empty_table_is_object_unless_marked);
    RUN_TEST(json_sequence_is_array);
    RUN_TEST(json_keys_are_escaped);
    RUN_TEST(json_invalid_utf8_becomes_replacement);
    RUN_TEST(json_unicode_escape_decodes);
    RUN_TEST(json_encode_errors);
    RUN_TEST(json_decode_invalid_returns_nil);
    RUN_TEST(json_large_integers);
    lua_close(L);
END_TEST_SUITE()
