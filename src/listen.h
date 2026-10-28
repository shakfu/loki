/* listen.h - loki.listen: Unix-domain socket server for Lua */

#ifndef LOKI_LISTEN_H
#define LOKI_LISTEN_H

#include <stddef.h>
#include <lua.h>

#define LOKI_LISTEN_MAX_CONNS   8
#define LOKI_LISTEN_MAX_QUEUED  (16 * 1024 * 1024)  /* unflushed bytes per conn */

/* Add loki.listen to the global loki table. */
void loki_lua_bind_listen(lua_State *L);

/* Close every server and connection, remove the socket files, and detach
 * them from Lua. Call before the Lua state closes. */
void loki_listen_shutdown(void);

/* Directory for loki's sockets: $XDG_RUNTIME_DIR/loki, else
 * /tmp/loki-<uid>. Returns 0, or -1 if it does not fit. */
int loki_socket_dir(char *out, size_t size);

/* Nonzero if a Lua callback ran since the last call; clears the flag. */
int loki_listen_take_activity(void);

#endif /* LOKI_LISTEN_H */
