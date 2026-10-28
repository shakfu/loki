/* spawn.h - loki.spawn: child processes with non-blocking pipes */

#ifndef LOKI_SPAWN_H
#define LOKI_SPAWN_H

#include <lua.h>

#define LOKI_SPAWN_MAX_PROCS   8
#define LOKI_SPAWN_MAX_QUEUED  (16 * 1024 * 1024)  /* unflushed stdin bytes */

/* Add loki.spawn to the global loki table. I/O is delivered when the
 * default libuv loop runs (the editor's idle hook). */
void loki_lua_bind_spawn(lua_State *L);

/* SIGTERM every live child and detach it from Lua. Call before the Lua
 * state closes and before anything else runs the libuv loop. */
void loki_spawn_shutdown(void);

/* Nonzero if a Lua callback ran since the last call; clears the flag. */
int loki_spawn_take_activity(void);

/* Number of children whose handles are still open. */
int loki_spawn_count(void);

#endif /* LOKI_SPAWN_H */
