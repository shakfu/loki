/* listen.c - loki.listen: Unix-domain socket server for Lua (libuv)
 *
 * Same callback model as spawn.c: libuv runs callbacks on the main thread
 * from the idle hook, and objects hold registry refs to themselves until
 * their handles close.
 *
 * Access control is the socket's directory: it must be owned by the user
 * and closed to group and other, so only the user (and root) can connect.
 */

#include <lua.h>
#include <lauxlib.h>
#include <uv.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include "internal.h"
#include "loki/lua.h"
#include "listen.h"

#define SERVER_MT "loki.server"
#define CONN_MT "loki.conn"

typedef struct loki_server loki_server;

typedef struct loki_conn {
    uv_pipe_t pipe;
    loki_server *server;    /* NULL once the server is gone */
    lua_State *L;
    int self_ref;
    int open;               /* handle not yet closing */
    int closed_cb_done;     /* uv close callback ran */
    int collected;          /* userdata was garbage-collected */
    struct loki_conn *next;
} loki_conn;

struct loki_server {
    uv_pipe_t pipe;
    lua_State *L;
    int self_ref, on_connect, on_data, on_close;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    int open;
    int closed_cb_done;
    int collected;
    int nconns;
    loki_conn *conns;
    struct loki_server *next;
};

typedef struct { void *p; } ptr_ud;

static loki_server *servers;
static int activity;

static void report_error(lua_State *L, const char *what) {
    const char *msg = lua_tostring(L, -1);
    editor_ctx_t *ctx = loki_lua_get_editor_context(L);
    if (ctx) editor_set_status_msg(ctx, "listen %s: %s", what, msg ? msg : "error");
    else fprintf(stderr, "listen %s: %s\n", what, msg ? msg : "error");
    lua_pop(L, 1);
}

/* Call server callback ref with (conn, nargs already pushed). */
static void call_cb(loki_server *s, loki_conn *c, int ref, int nargs, const char *what) {
    lua_State *L = s->L;
    if (!L || ref == LUA_NOREF || c->self_ref == LUA_NOREF) {
        if (L) lua_pop(L, nargs);
        return;
    }
    activity = 1;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_rawgeti(L, LUA_REGISTRYINDEX, c->self_ref);
    lua_rotate(L, -(nargs + 2), 2);
    if (lua_pcall(L, nargs + 1, 0, 0) != LUA_OK) report_error(L, what);
}

/* ======================= Connections ====================================== */

static void maybe_free_server(loki_server *s);

static void unlink_conn(loki_conn *c) {
    loki_server *s = c->server;
    if (!s) return;
    for (loki_conn **pp = &s->conns; *pp; pp = &(*pp)->next)
        if (*pp == c) { *pp = c->next; s->nconns--; break; }
    c->server = NULL;
    maybe_free_server(s);
}

static void on_conn_closed(uv_handle_t *h) {
    loki_conn *c = h->data;
    c->closed_cb_done = 1;
    loki_server *s = c->server;
    if (s && s->L) call_cb(s, c, s->on_close, 0, "on_close");
    unlink_conn(c);
    if (c->L && c->self_ref != LUA_NOREF) luaL_unref(c->L, LUA_REGISTRYINDEX, c->self_ref);
    c->self_ref = LUA_NOREF;
    if (c->collected) free(c);
}

static void close_conn(loki_conn *c) {
    if (!c->open) return;
    c->open = 0;
    uv_close((uv_handle_t *)&c->pipe, on_conn_closed);
}

static void on_alloc(uv_handle_t *h, size_t suggested, uv_buf_t *buf) {
    (void)h;
    buf->base = malloc(suggested);
    buf->len = buf->base ? suggested : 0;
}

static void on_conn_read(uv_stream_t *st, ssize_t n, const uv_buf_t *buf) {
    loki_conn *c = st->data;
    if (n > 0 && c->server && c->server->L) {
        lua_pushlstring(c->server->L, buf->base, (size_t)n);
        call_cb(c->server, c, c->server->on_data, 1, "on_data");
    } else if (n < 0) {
        close_conn(c);
    }
    free(buf->base);
}

typedef struct {
    uv_write_t req;
    char data[];
} write_req;

static void on_write(uv_write_t *req, int status) {
    (void)status;
    free(req);
}

static loki_conn *check_conn(lua_State *L) {
    ptr_ud *ud = luaL_checkudata(L, 1, CONN_MT);
    return ud->p;
}

/* conn:write(s) -> true, or false, message */
static int l_conn_write(lua_State *L) {
    loki_conn *c = check_conn(L);
    size_t n;
    const char *s = luaL_checklstring(L, 2, &n);
    const char *err = NULL;
    if (!c || !c->open) err = "connection closed";
    else if (uv_stream_get_write_queue_size((uv_stream_t *)&c->pipe) + n > LOKI_LISTEN_MAX_QUEUED)
        err = "write queue full";
    if (!err && n > 0) {
        write_req *w = malloc(sizeof(*w) + n);
        if (!w) err = "out of memory";
        else {
            memcpy(w->data, s, n);
            uv_buf_t b = uv_buf_init(w->data, (unsigned)n);
            int rc = uv_write(&w->req, (uv_stream_t *)&c->pipe, &b, 1, on_write);
            if (rc < 0) { free(w); err = uv_strerror(rc); }
        }
    }
    if (err) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, err);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_conn_close(lua_State *L) {
    loki_conn *c = check_conn(L);
    if (c) close_conn(c);
    return 0;
}

static int l_conn_gc(lua_State *L) {
    ptr_ud *ud = luaL_checkudata(L, 1, CONN_MT);
    loki_conn *c = ud->p;
    ud->p = NULL;
    if (!c) return 0;
    if (c->closed_cb_done) free(c);
    else c->collected = 1;
    return 0;
}

static void on_connection(uv_stream_t *st, int status) {
    loki_server *s = st->data;
    if (status < 0 || !s->L) return;
    loki_conn *c = calloc(1, sizeof(*c));
    if (!c) return;
    uv_pipe_init(uv_default_loop(), &c->pipe, 0);
    c->pipe.data = c;
    c->self_ref = LUA_NOREF;
    c->open = 1;
    if (uv_accept(st, (uv_stream_t *)&c->pipe) != 0 || s->nconns >= LOKI_LISTEN_MAX_CONNS) {
        c->collected = 1;  /* no userdata: free on close */
        close_conn(c);
        return;
    }
    c->server = s;
    c->L = s->L;
    c->next = s->conns;
    s->conns = c;
    s->nconns++;

    lua_State *L = s->L;
    ptr_ud *ud = lua_newuserdata(L, sizeof(*ud));
    ud->p = c;
    luaL_setmetatable(L, CONN_MT);
    c->self_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    call_cb(s, c, s->on_connect, 0, "on_connect");
    if (c->open) uv_read_start((uv_stream_t *)&c->pipe, on_alloc, on_conn_read);
}

/* ======================= Server =========================================== */

static void unlink_server(loki_server *s) {
    for (loki_server **pp = &servers; *pp; pp = &(*pp)->next)
        if (*pp == s) { *pp = s->next; return; }
}

/* A server is finished once its handle and every connection have closed;
 * libuv may run the server's close callback before its connections'. */
static void maybe_free_server(loki_server *s) {
    if (!s->closed_cb_done || s->nconns > 0) return;
    if (s->L) {
        luaL_unref(s->L, LUA_REGISTRYINDEX, s->on_connect);
        luaL_unref(s->L, LUA_REGISTRYINDEX, s->on_data);
        luaL_unref(s->L, LUA_REGISTRYINDEX, s->on_close);
        luaL_unref(s->L, LUA_REGISTRYINDEX, s->self_ref);  /* may allow GC */
    }
    s->on_connect = s->on_data = s->on_close = s->self_ref = LUA_NOREF;
    if (s->collected) free(s);
}

static void on_server_closed(uv_handle_t *h) {
    loki_server *s = h->data;
    s->closed_cb_done = 1;
    unlink_server(s);
    maybe_free_server(s);
}

static void close_server(loki_server *s) {
    if (!s->open) return;
    s->open = 0;
    unlink(s->path);
    /* Connections outlive the server's callbacks: detach them first. */
    for (loki_conn *c = s->conns; c;) {
        loki_conn *next = c->next;
        close_conn(c);
        c = next;
    }
    uv_close((uv_handle_t *)&s->pipe, on_server_closed);
}

static loki_server *check_server(lua_State *L) {
    ptr_ud *ud = luaL_checkudata(L, 1, SERVER_MT);
    return ud->p;
}

static int l_server_close(lua_State *L) {
    loki_server *s = check_server(L);
    if (s) close_server(s);
    return 0;
}

static int l_server_path(lua_State *L) {
    loki_server *s = check_server(L);
    if (!s) return 0;
    lua_pushstring(L, s->path);
    return 1;
}

static int l_server_gc(lua_State *L) {
    ptr_ud *ud = luaL_checkudata(L, 1, SERVER_MT);
    loki_server *s = ud->p;
    ud->p = NULL;
    if (!s) return 0;
    if (s->closed_cb_done && s->nconns == 0) free(s);
    else s->collected = 1;
    return 0;
}

/* Create dir with mode 0700 if missing; then require that it is a directory
 * owned by us with no group/other access. Returns NULL or an error. */
static const char *check_private_dir(const char *dir) {
    struct stat st;
    if (lstat(dir, &st) != 0) {
        if (errno != ENOENT) return strerror(errno);
        if (mkdir(dir, 0700) != 0 && errno != EEXIST) return strerror(errno);
        if (lstat(dir, &st) != 0) return strerror(errno);
    }
    if (!S_ISDIR(st.st_mode)) return "socket directory is not a directory";
    if (st.st_uid != geteuid()) return "socket directory is owned by another user";
    if (st.st_mode & 077) return "socket directory is accessible to other users";
    return NULL;
}

/* A leftover socket from a crashed loki refuses connections: remove it. A
 * live one, or a non-socket file, is an error. */
static const char *clear_stale(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT ? NULL : strerror(errno);
    if (!S_ISSOCK(st.st_mode)) return "path exists and is not a socket";
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return strerror(errno);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    int live = connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
    close(fd);
    if (live) return "socket is in use";
    return unlink(path) == 0 ? NULL : strerror(errno);
}

/* loki.listen{path=, on_connect=, on_data=, on_close=} -> server, or nil, message */
static int l_listen(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "path");
    const char *path = luaL_checkstring(L, -1);
    static const char *const cb_names[] = {"on_connect", "on_data", "on_close"};
    for (int i = 0; i < 3; i++) {
        lua_getfield(L, 1, cb_names[i]);
        int bad = !lua_isnil(L, -1) && !lua_isfunction(L, -1);
        lua_pop(L, 1);
        if (bad) return luaL_error(L, "listen: %s must be a function", cb_names[i]);
    }

    const char *err = NULL;
    loki_server tmp;
    if (strlen(path) >= sizeof(tmp.path)) err = "socket path too long";

    char dir[sizeof(tmp.path)];
    if (!err) {
        snprintf(dir, sizeof(dir), "%s", path);
        char *slash = strrchr(dir, '/');
        if (!slash || slash == dir) err = "socket path needs a private parent directory";
        else *slash = '\0';
    }
    if (!err) err = check_private_dir(dir);
    if (!err) err = clear_stale(path);

    loki_server *s = NULL;
    if (!err) {
        s = calloc(1, sizeof(*s));
        if (!s) err = "out of memory";
    }
    if (!err) {
        snprintf(s->path, sizeof(s->path), "%s", path);
        uv_pipe_init(uv_default_loop(), &s->pipe, 0);
        s->pipe.data = s;
        s->open = 1;
        mode_t old = umask(0177);
        int rc = uv_pipe_bind(&s->pipe, path);
        umask(old);
        if (rc == 0) rc = uv_listen((uv_stream_t *)&s->pipe, 8, on_connection);
        if (rc < 0) {
            err = uv_strerror(rc);
            s->collected = 1;
            s->open = 0;
            uv_close((uv_handle_t *)&s->pipe, on_server_closed);
            s = NULL;
        }
    }
    if (err) {
        lua_pushnil(L);
        lua_pushstring(L, err);
        return 2;
    }

    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
    s->L = lua_tothread(L, -1);
    lua_pop(L, 1);
    int *refs[] = {&s->on_connect, &s->on_data, &s->on_close};
    for (int i = 0; i < 3; i++) {
        lua_getfield(L, 1, cb_names[i]);
        *refs[i] = lua_isnil(L, -1) ? (lua_pop(L, 1), LUA_NOREF)
                                    : luaL_ref(L, LUA_REGISTRYINDEX);
    }
    s->next = servers;
    servers = s;

    ptr_ud *ud = lua_newuserdata(L, sizeof(*ud));
    ud->p = s;
    luaL_setmetatable(L, SERVER_MT);
    lua_pushvalue(L, -1);
    s->self_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return 1;
}

int loki_socket_dir(char *out, size_t size) {
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    int n = (xdg && xdg[0] == '/')
        ? snprintf(out, size, "%s/loki", xdg)
        : snprintf(out, size, "/tmp/loki-%u", (unsigned)geteuid());
    return (n < 0 || (size_t)n >= size) ? -1 : 0;
}

/* loki.socket_path() -> "<socket dir>/<pid>.sock" for this process */
static int l_socket_path(lua_State *L) {
    char dir[256];
    if (loki_socket_dir(dir, sizeof(dir)) != 0) return 0;
    lua_pushfstring(L, "%s/%d.sock", dir, (int)getpid());
    return 1;
}

void loki_listen_shutdown(void) {
    for (loki_server *s = servers; s; s = s->next) {
        s->L = NULL;
        for (loki_conn *c = s->conns; c; c = c->next) c->L = NULL;
        close_server(s);
    }
}

int loki_listen_take_activity(void) {
    int a = activity;
    activity = 0;
    return a;
}

void loki_lua_bind_listen(lua_State *L) {
    static const luaL_Reg server_methods[] = {
        {"close", l_server_close},
        {"path", l_server_path},
        {NULL, NULL},
    };
    static const luaL_Reg conn_methods[] = {
        {"write", l_conn_write},
        {"close", l_conn_close},
        {NULL, NULL},
    };
    luaL_newmetatable(L, SERVER_MT);
    luaL_newlib(L, server_methods);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, l_server_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
    luaL_newmetatable(L, CONN_MT);
    luaL_newlib(L, conn_methods);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, l_conn_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);

    lua_getglobal(L, "loki");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
    }
    lua_pushcfunction(L, l_listen);
    lua_setfield(L, -2, "listen");
    lua_pushcfunction(L, l_socket_path);
    lua_setfield(L, -2, "socket_path");
    lua_setglobal(L, "loki");
}
