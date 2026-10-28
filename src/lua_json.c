/* lua_json.c - loki.json: JSON encode/decode for Lua, built on json.c
 *
 * JSON has two things Lua tables cannot express directly:
 * - null: decoded as the sentinel loki.json.null, encoded from it or nil.
 * - empty array vs empty object: decoded arrays carry the metatable that
 *   loki.json.array() sets. An unmarked empty table encodes as {}.
 */

#include <lua.h>
#include <lauxlib.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
#include "lua_json.h"

#define ARRAY_MT "loki.json.array"

/* Address of this byte is the null sentinel (a light userdata). */
static const char json_null_sentinel = 0;

static int is_null(lua_State *L, int idx) {
    return lua_islightuserdata(L, idx) &&
           lua_touserdata(L, idx) == (void *)&json_null_sentinel;
}

/* ======================= UTF-8 ============================================ */

/* Length of the valid UTF-8 sequence at s (n bytes left), or 0 if invalid.
 * Rejects overlong forms, surrogates and code points above U+10FFFF. */
static size_t utf8_valid_seq(const unsigned char *s, size_t n) {
    if (s[0] < 0x80) return 1;
    size_t len;
    unsigned cp;
    if (s[0] >= 0xC2 && s[0] <= 0xDF) { len = 2; cp = s[0] & 0x1F; }
    else if (s[0] >= 0xE0 && s[0] <= 0xEF) { len = 3; cp = s[0] & 0x0F; }
    else if (s[0] >= 0xF0 && s[0] <= 0xF4) { len = 4; cp = s[0] & 0x07; }
    else return 0;
    if (n < len) return 0;
    for (size_t i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    if ((len == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) ||
        (len == 4 && (cp < 0x10000 || cp > 0x10FFFF))) return 0;
    return len;
}

/* JSON text must be UTF-8, but Lua strings hold any bytes (a Latin-1 file,
 * binary output). Write s as a JSON string with each invalid byte replaced
 * by U+FFFD. */
static void put_string(JsonBuilder *jb, const char *s, size_t n, int key) {
    const unsigned char *u = (const unsigned char *)s;
    size_t i = 0, bad = 0;
    while (i < n) {
        size_t k = utf8_valid_seq(u + i, n - i);
        if (!k) { bad++; k = 1; }
        i += k;
    }
    if (!bad) {
        if (key) json_key_len(jb, s, n); else json_string_len(jb, s, n);
        return;
    }
    char *clean = malloc(n + 2 * bad);   /* 1 bad byte -> 3 bytes */
    if (!clean) { jb->error = 1; return; }
    size_t o = 0;
    for (i = 0; i < n;) {
        size_t k = utf8_valid_seq(u + i, n - i);
        if (k) { memcpy(clean + o, s + i, k); o += k; i += k; }
        else { memcpy(clean + o, "\xEF\xBF\xBD", 3); o += 3; i++; }
    }
    if (key) json_key_len(jb, clean, o); else json_string_len(jb, clean, o);
    free(clean);
}

/* ======================= Encode =========================================== */

/* Is the table at idx a sequence 1..n with n > 0, or marked as an array?
 * Stores n in *len. */
static int table_is_array(lua_State *L, int idx, size_t *len) {
    *len = (size_t)lua_rawlen(L, idx);
    if (lua_getmetatable(L, idx)) {
        luaL_getmetatable(L, ARRAY_MT);
        int marked = lua_rawequal(L, -1, -2);
        lua_pop(L, 2);
        if (marked) return 1;
    }
    if (*len == 0) return 0;
    size_t keys = 0;
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        lua_pop(L, 1);
        keys++;
    }
    return keys == *len;  /* lua_rawlen found a border; no other keys */
}

/* Encode the value at idx. Returns NULL, or a static error message. */
static const char *encode(lua_State *L, int idx, JsonBuilder *jb, int depth) {
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TNIL:
        json_null(jb);
        return NULL;
    case LUA_TBOOLEAN:
        json_bool(jb, lua_toboolean(L, idx));
        return NULL;
    case LUA_TNUMBER:
#if LUA_VERSION_NUM >= 503
        if (lua_isinteger(L, idx)) {
            json_int64(jb, (long long)lua_tointeger(L, idx));
            return NULL;
        }
#endif
        {
            double d = lua_tonumber(L, idx);
            if (d == floor(d) && fabs(d) < 9007199254740992.0) {
                json_int64(jb, (long long)d);
                return NULL;
            }
            return json_double(jb, d) == 0 ? NULL : "cannot encode NaN or infinity";
        }
    case LUA_TSTRING: {
        size_t n;
        const char *s = lua_tolstring(L, idx, &n);
        put_string(jb, s, n, 0);
        return NULL;
    }
    case LUA_TLIGHTUSERDATA:
        if (is_null(L, idx)) { json_null(jb); return NULL; }
        return "cannot encode userdata";
    case LUA_TTABLE:
        break;
    default:
        return "cannot encode this Lua type";
    }

    if (depth >= JSON_MAX_DEPTH) return "nesting too deep (or a cycle)";
    luaL_checkstack(L, 4, "json encode");

    size_t len;
    if (table_is_array(L, idx, &len)) {
        json_array_start(jb);
        for (size_t i = 1; i <= len; i++) {
            lua_rawgeti(L, idx, (lua_Integer)i);
            const char *err = encode(L, -1, jb, depth + 1);
            lua_pop(L, 1);
            if (err) return err;
        }
        json_array_end(jb);
        return NULL;
    }

    json_object_start(jb);
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        if (lua_type(L, -2) != LUA_TSTRING) {
            lua_pop(L, 2);
            return "object keys must be strings";
        }
        size_t kn;
        const char *k = lua_tolstring(L, -2, &kn);
        put_string(jb, k, kn, 1);
        jb->need_comma = 0;
        const char *err = encode(L, -1, jb, depth + 1);
        lua_pop(L, 1);
        if (err) { lua_pop(L, 1); return err; }
    }
    json_object_end(jb);
    return NULL;
}

/* loki.json.encode(value) -> string; raises on unencodable input */
static int l_encode(lua_State *L) {
    luaL_checkany(L, 1);
    JsonBuilder jb;
    json_builder_init(&jb);
    const char *err = encode(L, 1, &jb, 0);
    if (!err && jb.error) err = "out of memory";
    if (err) {
        json_builder_free(&jb);
        return luaL_error(L, "json.encode: %s", err);
    }
    lua_pushlstring(L, jb.buf, jb.len);
    json_builder_free(&jb);
    return 1;
}

/* ======================= Decode =========================================== */

static void push_value(lua_State *L, const JsonValue *v) {
    luaL_checkstack(L, 3, "json decode");
    switch (v->type) {
    case JSON_NULL:
        lua_pushlightuserdata(L, (void *)&json_null_sentinel);
        break;
    case JSON_BOOL:
        lua_pushboolean(L, v->data.bool_val);
        break;
    case JSON_INT:
        lua_pushinteger(L, v->data.int_val);
        break;
    case JSON_DOUBLE: {
        double d = v->data.double_val;
        if (d == floor(d) && fabs(d) < 9007199254740992.0)
            lua_pushinteger(L, (lua_Integer)d);
        else
            lua_pushnumber(L, d);
        break;
    }
    case JSON_STRING:
        lua_pushlstring(L, v->data.string_val.str, v->data.string_val.len);
        break;
    case JSON_ARRAY:
        lua_createtable(L, (int)v->data.array_val.count, 0);
        for (size_t i = 0; i < v->data.array_val.count; i++) {
            push_value(L, &v->data.array_val.items[i]);
            lua_rawseti(L, -2, (lua_Integer)(i + 1));
        }
        luaL_setmetatable(L, ARRAY_MT);
        break;
    case JSON_OBJECT:
        lua_createtable(L, 0, (int)v->data.object_val.count);
        for (size_t i = 0; i < v->data.object_val.count; i++) {
            push_value(L, &v->data.object_val.values[i]);
            lua_setfield(L, -2, v->data.object_val.keys[i]);
        }
        break;
    default:
        lua_pushnil(L);
        break;
    }
}

/* loki.json.decode(string) -> value, or nil, message */
static int l_decode(lua_State *L) {
    const char *s = luaL_checkstring(L, 1);
    JsonValue v = json_parse(s);
    if (v.type == JSON_ERROR) {
        lua_pushnil(L);
        lua_pushliteral(L, "invalid JSON");
        return 2;
    }
    push_value(L, &v);
    json_value_free(&v);
    return 1;
}

/* loki.json.array([t]) -> t, marked to encode as an array even when empty */
static int l_array(lua_State *L) {
    if (lua_isnoneornil(L, 1)) lua_newtable(L);
    else luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 1);
    luaL_setmetatable(L, ARRAY_MT);
    return 1;
}

void loki_lua_bind_json(lua_State *L) {
    luaL_newmetatable(L, ARRAY_MT);
    lua_pop(L, 1);

    lua_getglobal(L, "loki");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
    }
    lua_newtable(L);
    lua_pushcfunction(L, l_encode);
    lua_setfield(L, -2, "encode");
    lua_pushcfunction(L, l_decode);
    lua_setfield(L, -2, "decode");
    lua_pushcfunction(L, l_array);
    lua_setfield(L, -2, "array");
    lua_pushlightuserdata(L, (void *)&json_null_sentinel);
    lua_setfield(L, -2, "null");
    lua_setfield(L, -2, "json");
    lua_setglobal(L, "loki");
}
