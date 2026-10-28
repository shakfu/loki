/* lua_buffers.h - Lua API for any open buffer (buffers, buffer_text, ...) */

#ifndef LOKI_LUA_BUFFERS_H
#define LOKI_LUA_BUFFERS_H

#include <lua.h>

/* Add the buffer functions to the global loki table. */
void loki_lua_bind_buffers(lua_State *L);

#endif /* LOKI_LUA_BUFFERS_H */
