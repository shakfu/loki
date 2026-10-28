/* spawn.c - loki.spawn: child processes with non-blocking pipes (libuv)
 *
 * libuv callbacks run inside uv_run() on the main thread (the editor's idle
 * hook), so they call Lua directly. A proc holds registry refs to itself and
 * its callbacks until its handles close, so dropping the Lua value does not
 * stop the callbacks.
 */

#include <lua.h>
#include <lauxlib.h>
#include <uv.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include "internal.h"
#include "loki/lua.h"
#include "spawn.h"

#define PROC_MT "loki.proc"

extern char **environ;

typedef struct loki_proc {
    uv_process_t proc;
    uv_pipe_t in, out, err;
    lua_State *L;           /* main thread; NULL after shutdown */
    int self_ref, on_stdout, on_stderr, on_exit;
    int open_handles;       /* uv handles not yet closed */
    int stdin_open;
    int out_eof, err_eof, exited;
    int64_t exit_status;
    int term_signal;
    size_t queued;          /* bytes written but not yet flushed */
    int collected;          /* Lua userdata was garbage-collected */
    struct loki_proc *next; /* live list, for shutdown */
} loki_proc;

/* The userdata holds a pointer: the struct must outlive the userdata while
 * libuv still owns its handles (shutdown closes Lua first). */
typedef struct { loki_proc *p; } proc_ud;

static loki_proc *live_procs;
static int live_count;
static int activity;  /* a Lua callback ran since the last take */

static void unlink_proc(loki_proc *p) {
    for (loki_proc **pp = &live_procs; *pp; pp = &(*pp)->next) {
        if (*pp == p) { *pp = p->next; live_count--; return; }
    }
}

/* Report a callback error on the status line (stderr would corrupt the
 * screen in raw mode; tests have no editor context). */
static void report_error(lua_State *L, const char *what) {
    const char *msg = lua_tostring(L, -1);
    editor_ctx_t *ctx = loki_lua_get_editor_context(L);
    if (ctx) editor_set_status_msg(ctx, "spawn %s: %s", what, msg ? msg : "error");
    else fprintf(stderr, "spawn %s: %s\n", what, msg ? msg : "error");
    lua_pop(L, 1);
}

/* Call the callback in ref with (proc, extra args already pushed). */
static void call_ref(loki_proc *p, int ref, int nargs, const char *what) {
    lua_State *L = p->L;
    if (!L || ref == LUA_NOREF) { if (L) lua_pop(L, nargs); return; }
    activity = 1;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_rawgeti(L, LUA_REGISTRYINDEX, p->self_ref);
    lua_rotate(L, -(nargs + 2), 2);  /* fn, self below the args */
    if (lua_pcall(L, nargs + 1, 0, 0) != LUA_OK) report_error(L, what);
}

static void release_refs(loki_proc *p) {
    if (!p->L) return;
    luaL_unref(p->L, LUA_REGISTRYINDEX, p->on_stdout);
    luaL_unref(p->L, LUA_REGISTRYINDEX, p->on_stderr);
    luaL_unref(p->L, LUA_REGISTRYINDEX, p->on_exit);
    luaL_unref(p->L, LUA_REGISTRYINDEX, p->self_ref);  /* may allow GC */
    p->on_stdout = p->on_stderr = p->on_exit = p->self_ref = LUA_NOREF;
}

static void on_handle_closed(uv_handle_t *h) {
    loki_proc *p = h->data;
    if (--p->open_handles > 0) return;
    unlink_proc(p);
    int collected = p->collected;  /* read first: releasing refs allows GC */
    release_refs(p);
    if (collected) free(p);  /* else the userdata's __gc frees it */
}

static void close_handle(uv_handle_t *h) {
    if (!uv_is_closing(h)) uv_close(h, on_handle_closed);
}

/* on_exit fires once the process has exited and both pipes reached EOF, so
 * the callback sees all output (libuv may report exit first). */
static void maybe_finish(loki_proc *p) {
    if (!p->exited || !p->out_eof || !p->err_eof) return;
    if (p->L) {
        lua_pushinteger(p->L, (lua_Integer)p->exit_status);
        if (p->term_signal) lua_pushinteger(p->L, p->term_signal);
        else lua_pushnil(p->L);
        call_ref(p, p->on_exit, 2, "on_exit");
    }
    close_handle((uv_handle_t *)&p->proc);
    close_handle((uv_handle_t *)&p->in);
}

static void on_proc_exit(uv_process_t *h, int64_t status, int sig) {
    loki_proc *p = h->data;
    p->exited = 1;
    p->exit_status = status;
    p->term_signal = sig;
    maybe_finish(p);
}

static void on_alloc(uv_handle_t *h, size_t suggested, uv_buf_t *buf) {
    (void)h;
    buf->base = malloc(suggested);
    buf->len = buf->base ? suggested : 0;
}

static void on_read(uv_stream_t *s, ssize_t n, const uv_buf_t *buf) {
    loki_proc *p = s->data;
    int is_out = (s == (uv_stream_t *)&p->out);
    if (n > 0 && p->L) {
        lua_pushlstring(p->L, buf->base, (size_t)n);
        call_ref(p, is_out ? p->on_stdout : p->on_stderr, 1,
                 is_out ? "on_stdout" : "on_stderr");
    } else if (n < 0) {  /* UV_EOF or a read error: the stream is done */
        close_handle((uv_handle_t *)s);
        if (is_out) p->out_eof = 1; else p->err_eof = 1;
        maybe_finish(p);
    }
    free(buf->base);
}

typedef struct {
    uv_write_t req;
    loki_proc *p;
    size_t len;
    char data[];
} write_req;

static void on_write(uv_write_t *req, int status) {
    (void)status;  /* EPIPE: the child closed stdin; on_exit reports it */
    write_req *w = (write_req *)req;
    w->p->queued -= w->len;
    free(w);
}

static loki_proc *check_proc(lua_State *L) {
    proc_ud *ud = luaL_checkudata(L, 1, PROC_MT);
    return ud->p;
}

/* proc:write(s) -> true, or false, message */
static int l_write(lua_State *L) {
    loki_proc *p = check_proc(L);
    size_t n;
    const char *s = luaL_checklstring(L, 2, &n);
    const char *err = NULL;
    if (!p || !p->stdin_open) err = "stdin is closed";
    else if (p->queued + n > LOKI_SPAWN_MAX_QUEUED) err = "write queue full";
    if (!err && n > 0) {
        write_req *w = malloc(sizeof(*w) + n);
        if (!w) err = "out of memory";
        else {
            memcpy(w->data, s, n);
            w->p = p;
            w->len = n;
            uv_buf_t b = uv_buf_init(w->data, (unsigned)n);
            int rc = uv_write(&w->req, (uv_stream_t *)&p->in, &b, 1, on_write);
            if (rc < 0) { free(w); err = uv_strerror(rc); }
            else p->queued += n;
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

static void on_shutdown(uv_shutdown_t *req, int status) {
    (void)status;
    free(req);
}

/* proc:close_stdin(): flush queued writes, then send EOF */
static int l_close_stdin(lua_State *L) {
    loki_proc *p = check_proc(L);
    if (p && p->stdin_open) {
        p->stdin_open = 0;
        uv_shutdown_t *req = malloc(sizeof(*req));
        if (!req || uv_shutdown(req, (uv_stream_t *)&p->in, on_shutdown) < 0) {
            free(req);
            close_handle((uv_handle_t *)&p->in);
        }
    }
    return 0;
}

static const struct { const char *name; int sig; } signals[] = {
    {"TERM", SIGTERM}, {"KILL", SIGKILL}, {"INT", SIGINT},
    {"HUP", SIGHUP}, {"QUIT", SIGQUIT}, {"USR1", SIGUSR1}, {"USR2", SIGUSR2},
};

/* proc:kill([sig]) -> true, or false, message. sig: name or number. */
static int l_kill(lua_State *L) {
    loki_proc *p = check_proc(L);
    int sig = SIGTERM;
    if (lua_type(L, 2) == LUA_TSTRING) {
        const char *name = lua_tostring(L, 2);
        sig = 0;
        for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++)
            if (strcmp(name, signals[i].name) == 0) sig = signals[i].sig;
        if (!sig) return luaL_argerror(L, 2, "unknown signal name");
    } else if (!lua_isnoneornil(L, 2)) {
        sig = (int)luaL_checkinteger(L, 2);
    }
    int rc = (p && !p->exited && p->proc.pid > 0) ? uv_process_kill(&p->proc, sig) : UV_ESRCH;
    if (rc < 0) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, uv_strerror(rc));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* proc:pid() -> integer */
static int l_pid(lua_State *L) {
    loki_proc *p = check_proc(L);
    lua_pushinteger(L, p ? p->proc.pid : 0);
    return 1;
}

/* proc:running() -> boolean */
static int l_running(lua_State *L) {
    loki_proc *p = check_proc(L);
    lua_pushboolean(L, p && !p->exited);
    return 1;
}

static int l_gc(lua_State *L) {
    proc_ud *ud = luaL_checkudata(L, 1, PROC_MT);
    loki_proc *p = ud->p;
    ud->p = NULL;
    if (!p) return 0;
    /* Reached only once self_ref is released (all handles closed), or when
     * lua_close() collects everything after loki_spawn_shutdown(). */
    if (p->open_handles == 0) free(p);
    else p->collected = 1;
    return 0;
}

/* Build "K=V" entries: environ, minus keys in the table at idx, plus the
 * table's string values (false removes a key). Caller frees with free_env. */
static char **build_env(lua_State *L, int idx) {
    size_t n = 0;
    for (char **e = environ; *e; e++) n++;
    lua_pushnil(L);
    while (lua_next(L, idx)) { lua_pop(L, 1); n++; }

    char **env = calloc(n + 1, sizeof(char *));
    if (!env) return NULL;
    size_t k = 0;
    for (char **e = environ; *e; e++) {
        const char *eq = strchr(*e, '=');
        if (!eq) continue;
        lua_pushlstring(L, *e, (size_t)(eq - *e));
        lua_rawget(L, idx);
        int overridden = !lua_isnil(L, -1);
        lua_pop(L, 1);
        if (!overridden) env[k++] = strdup(*e);
    }
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) != LUA_TBOOLEAN) {
            const char *key = lua_tostring(L, -2);
            const char *val = luaL_tolstring(L, -1, NULL);
            size_t len = strlen(key) + strlen(val) + 2;
            char *kv = malloc(len);
            if (kv) { snprintf(kv, len, "%s=%s", key, val); env[k++] = kv; }
            lua_pop(L, 1);  /* luaL_tolstring's copy */
        }
        lua_pop(L, 1);
    }
    env[k] = NULL;
    return env;
}

static void free_env(char **env) {
    if (!env) return;
    for (char **e = env; *e; e++) free(*e);
    free(env);
}

/* Registry ref to the function in field name (already type-checked). */
static int field_ref(lua_State *L, int tbl, const char *name) {
    lua_getfield(L, tbl, name);
    if (lua_isnil(L, -1)) { lua_pop(L, 1); return LUA_NOREF; }
    return luaL_ref(L, LUA_REGISTRYINDEX);
}

/* loki.spawn{cmd=, args=, cwd=, env=, on_stdout=, on_stderr=, on_exit=}
 *   -> proc, or nil, message */
static int l_spawn(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    if (live_count >= LOKI_SPAWN_MAX_PROCS) {
        lua_pushnil(L);
        lua_pushliteral(L, "too many child processes");
        return 2;
    }

    lua_getfield(L, 1, "cmd");
    const char *cmd = luaL_checkstring(L, -1);
    int cmd_idx = lua_gettop(L);

    lua_getfield(L, 1, "args");
    int args_idx = lua_gettop(L);
    size_t nargs = lua_istable(L, args_idx) ? (size_t)lua_rawlen(L, args_idx) : 0;
    char **argv = calloc(nargs + 2, sizeof(char *));
    if (!argv) return luaL_error(L, "spawn: out of memory");
    argv[0] = (char *)cmd;
    for (size_t i = 0; i < nargs; i++) {
        lua_rawgeti(L, args_idx, (lua_Integer)(i + 1));
        if (!lua_isstring(L, -1)) {
            free(argv);
            return luaL_error(L, "spawn: args[%d] must be a string", (int)(i + 1));
        }
        argv[i + 1] = (char *)lua_tostring(L, -1);  /* kept alive on the stack */
    }

    lua_getfield(L, 1, "cwd");
    const char *cwd = luaL_optstring(L, -1, NULL);

    char **env = NULL;
    lua_getfield(L, 1, "env");
    if (lua_istable(L, -1)) {
        env = build_env(L, lua_gettop(L));
        if (!env) { free(argv); return luaL_error(L, "spawn: out of memory"); }
    }

    /* Check callbacks before starting anything a raised error would leak. */
    static const char *const cb_names[] = {"on_stdout", "on_stderr", "on_exit"};
    for (int i = 0; i < 3; i++) {
        lua_getfield(L, 1, cb_names[i]);
        int bad = !lua_isnil(L, -1) && !lua_isfunction(L, -1);
        lua_pop(L, 1);
        if (bad) {
            free(argv);
            free_env(env);
            return luaL_error(L, "spawn: %s must be a function", cb_names[i]);
        }
    }

    loki_proc *p = calloc(1, sizeof(*p));
    if (!p) { free(argv); free_env(env); return luaL_error(L, "spawn: out of memory"); }
    p->on_stdout = p->on_stderr = p->on_exit = p->self_ref = LUA_NOREF;

    uv_loop_t *loop = uv_default_loop();
    uv_pipe_init(loop, &p->in, 0);
    uv_pipe_init(loop, &p->out, 0);
    uv_pipe_init(loop, &p->err, 0);
    p->proc.data = p->in.data = p->out.data = p->err.data = p;

    uv_stdio_container_t stdio[3] = {
        {.flags = UV_CREATE_PIPE | UV_READABLE_PIPE, .data.stream = (uv_stream_t *)&p->in},
        {.flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE, .data.stream = (uv_stream_t *)&p->out},
        {.flags = UV_CREATE_PIPE | UV_WRITABLE_PIPE, .data.stream = (uv_stream_t *)&p->err},
    };
    uv_process_options_t opts = {
        .exit_cb = on_proc_exit,
        .file = cmd,
        .args = argv,
        .env = env,
        .cwd = cwd,
        /* New session: the child has no controlling terminal, so it cannot
         * read the editor's keys or change its tty modes. */
        .flags = UV_PROCESS_DETACHED,
        .stdio_count = 3,
        .stdio = stdio,
    };

    /* A write to a child that closed stdin must fail with EPIPE, not kill
     * the editor. libuv resets signal dispositions in the child. */
    signal(SIGPIPE, SIG_IGN);

    int rc = uv_spawn(loop, &p->proc, &opts);
    free(argv);
    free_env(env);

    /* Handles are initialised either way; they must be closed to be freed. */
    p->open_handles = 4;
    p->next = live_procs;
    live_procs = p;
    live_count++;

    if (rc < 0) {
        p->collected = 1;  /* no userdata: free on close */
        p->exited = 1;     /* never started: pid is 0, and kill(0) hits our group */
        close_handle((uv_handle_t *)&p->proc);
        close_handle((uv_handle_t *)&p->in);
        close_handle((uv_handle_t *)&p->out);
        close_handle((uv_handle_t *)&p->err);
        lua_pushnil(L);
        lua_pushfstring(L, "%s: %s", cmd, uv_strerror(rc));
        return 2;
    }

    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
    p->L = lua_tothread(L, -1);
    lua_pop(L, 1);
    p->stdin_open = 1;
    p->on_stdout = field_ref(L, 1, "on_stdout");
    p->on_stderr = field_ref(L, 1, "on_stderr");
    p->on_exit = field_ref(L, 1, "on_exit");

    uv_read_start((uv_stream_t *)&p->out, on_alloc, on_read);
    uv_read_start((uv_stream_t *)&p->err, on_alloc, on_read);

    lua_settop(L, cmd_idx - 1);
    proc_ud *ud = lua_newuserdata(L, sizeof(*ud));
    ud->p = p;
    luaL_setmetatable(L, PROC_MT);
    lua_pushvalue(L, -1);
    p->self_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return 1;
}

void loki_spawn_shutdown(void) {
    for (loki_proc *p = live_procs; p; p = p->next) {
        if (!p->exited && p->proc.pid > 0) uv_process_kill(&p->proc, SIGTERM);
        p->L = NULL;  /* Lua is about to close; callbacks must not run */
        close_handle((uv_handle_t *)&p->proc);
        close_handle((uv_handle_t *)&p->in);
        close_handle((uv_handle_t *)&p->out);
        close_handle((uv_handle_t *)&p->err);
    }
}

int loki_spawn_take_activity(void) {
    int a = activity;
    activity = 0;
    return a;
}

int loki_spawn_count(void) {
    return live_count;
}

/* loki.exepath() -> absolute path of the running loki binary, or nil */
static int l_exepath(lua_State *L) {
    char buf[4096];
    size_t size = sizeof(buf);
    if (uv_exepath(buf, &size) != 0) return 0;
    lua_pushlstring(L, buf, size);
    return 1;
}

void loki_lua_bind_spawn(lua_State *L) {
    static const luaL_Reg methods[] = {
        {"write", l_write},
        {"close_stdin", l_close_stdin},
        {"kill", l_kill},
        {"pid", l_pid},
        {"running", l_running},
        {NULL, NULL},
    };
    luaL_newmetatable(L, PROC_MT);
    luaL_newlib(L, methods);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, l_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);

    lua_getglobal(L, "loki");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
    }
    lua_pushcfunction(L, l_spawn);
    lua_setfield(L, -2, "spawn");
    lua_pushcfunction(L, l_exepath);
    lua_setfield(L, -2, "exepath");
    lua_setglobal(L, "loki");
}
