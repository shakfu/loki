/* lua_json.h - loki.json (encode, decode, array, null) */

#ifndef LOKI_LUA_JSON_H
#define LOKI_LUA_JSON_H

#include <lua.h>

/* Add loki.json to the global loki table, creating it if needed. */
void loki_lua_bind_json(lua_State *L);

#endif /* LOKI_LUA_JSON_H */
