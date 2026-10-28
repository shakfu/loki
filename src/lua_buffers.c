/* lua_buffers.c - Lua API for any open buffer, not just the current one
 *
 * Lines and columns are 1-based. Buffer text is the rows joined with '\n',
 * without a trailing newline; offsets for buffer_replace index that string
 * the way string.sub does.
 */

#include <lua.h>
#include <lauxlib.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "internal.h"
#include "buffers.h"
#include "undo.h"
#include "selection.h"
#include "choice.h"
#include "lua_buffers.h"

/* Absolute, symlink-free form of path. A missing file resolves through its
 * directory, so the result is safe for a prefix check against a root. Fails
 * if the directory does not exist or the last component is "." or "..". */
static int abs_path(const char *path, char *out, size_t size) {
    char resolved[PATH_MAX];
    if (realpath(path, resolved)) {
        int n = snprintf(out, size, "%s", resolved);
        return (n < 0 || (size_t)n >= size) ? -1 : 0;
    }
    char dir[PATH_MAX];
    if (snprintf(dir, sizeof(dir), "%s", path) >= (int)sizeof(dir)) return -1;
    char *slash = strrchr(dir, '/');
    const char *base;
    if (!slash) { base = path; snprintf(dir, sizeof(dir), "."); }
    else { base = path + (slash - dir) + 1; if (slash == dir) slash[1] = '\0'; else *slash = '\0'; }
    if (!*base || strcmp(base, ".") == 0 || strcmp(base, "..") == 0) return -1;
    if (!realpath(dir, resolved)) return -1;
    int n = snprintf(out, size, "%s%s%s", resolved,
                     strcmp(resolved, "/") == 0 ? "" : "/", base);
    return (n < 0 || (size_t)n >= size) ? -1 : 0;
}

/* loki.abspath(path) -> absolute symlink-free path, or nil */
static int l_abspath(lua_State *L) {
    char out[PATH_MAX];
    if (abs_path(luaL_checkstring(L, 1), out, sizeof(out)) != 0) return 0;
    lua_pushstring(L, out);
    return 1;
}

static editor_ctx_t *check_buffer(lua_State *L, int arg) {
    int id = (int)luaL_checkinteger(L, arg);
    return buffer_get(id);
}

/* Keep the cursor of ctx on an existing row and column after an edit. */
static void clamp_cursor(editor_ctx_t *ctx) {
    int row = ctx->view.rowoff + ctx->view.cy;
    int col = ctx->view.coloff + ctx->view.cx;
    if (row >= ctx->model.numrows) row = ctx->model.numrows > 0 ? ctx->model.numrows - 1 : 0;
    int size = row < ctx->model.numrows ? ctx->model.row[row].size : 0;
    if (col > size) col = size;
    if (row < ctx->view.rowoff) ctx->view.rowoff = row;
    if (col < ctx->view.coloff) ctx->view.coloff = col;
    ctx->view.cy = row - ctx->view.rowoff;
    ctx->view.cx = col - ctx->view.coloff;
}

/* loki.buffers() -> {{id, name, path, dirty, current, line, col}, ...} */
static int l_buffers(lua_State *L) {
    int ids[MAX_BUFFERS];
    int n = buffer_get_list(ids);
    int current = buffer_get_current_id();
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++) {
        editor_ctx_t *ctx = buffer_get(ids[i]);
        if (!ctx) continue;
        lua_createtable(L, 0, 7);
        lua_pushinteger(L, ids[i]);
        lua_setfield(L, -2, "id");
        lua_pushstring(L, buffer_get_display_name(ids[i]));
        lua_setfield(L, -2, "name");
        char path[PATH_MAX];
        if (ctx->model.filename && abs_path(ctx->model.filename, path, sizeof(path)) == 0) {
            lua_pushstring(L, path);
            lua_setfield(L, -2, "path");
        }
        lua_pushboolean(L, ctx->model.dirty != 0);
        lua_setfield(L, -2, "dirty");
        lua_pushboolean(L, ids[i] == current);
        lua_setfield(L, -2, "current");
        lua_pushinteger(L, ctx->view.rowoff + ctx->view.cy + 1);
        lua_setfield(L, -2, "line");
        lua_pushinteger(L, ctx->view.coloff + ctx->view.cx + 1);
        lua_setfield(L, -2, "col");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* loki.buffer_text(id [, first [, last]]) -> string, or nil if no buffer.
 * first/last are inclusive line numbers, clamped to the buffer. */
static int l_buffer_text(lua_State *L) {
    editor_ctx_t *ctx = check_buffer(L, 1);
    if (!ctx) return 0;
    lua_Integer first = luaL_optinteger(L, 2, 1);
    lua_Integer last = luaL_optinteger(L, 3, ctx->model.numrows);
    if (first < 1) first = 1;
    if (last > ctx->model.numrows) last = ctx->model.numrows;
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (lua_Integer r = first; r <= last; r++) {
        t_erow *row = &ctx->model.row[r - 1];
        luaL_addlstring(&b, row->chars, (size_t)row->size);
        if (r < last) luaL_addchar(&b, '\n');
    }
    luaL_pushresult(&b);
    return 1;
}

/* 0-based byte offset into the buffer text -> (row, col). Clamps past-end
 * offsets to the end of the last row. */
static void offset_to_pos(editor_ctx_t *ctx, size_t off, int *row, int *col) {
    for (int r = 0; r < ctx->model.numrows; r++) {
        size_t size = (size_t)ctx->model.row[r].size;
        if (off <= size) { *row = r; *col = (int)off; return; }
        off -= size + 1;
    }
    *row = ctx->model.numrows - 1;
    *col = ctx->model.row[*row].size;
}

/* loki.buffer_replace(id, i, j, text) -> true, or nil, message.
 * Replaces bytes i..j (1-based, inclusive) of buffer_text(id) with text, as
 * one undo step; j = i - 1 inserts before i. The buffer becomes dirty and is
 * not saved. */
static int l_buffer_replace(lua_State *L) {
    editor_ctx_t *ctx = check_buffer(L, 1);
    lua_Integer i = luaL_checkinteger(L, 2);
    lua_Integer j = luaL_checkinteger(L, 3);
    size_t len;
    const char *text = luaL_checklstring(L, 4, &len);
    const char *err = NULL;
    if (!ctx) err = "no such buffer";
    else if (i < 1 || j < i - 1) err = "invalid range";
    else if (len > INT_MAX || j - i + 1 > INT_MAX) err = "text too large";
    if (!err && ctx->model.numrows == 0) editor_insert_row(ctx, 0, "", 0);
    if (!err) {
        int row, col;
        offset_to_pos(ctx, (size_t)(i - 1), &row, &col);
        if (editor_replace_range(ctx, row, col, (int)(j - i + 1), text, (int)len) != 0)
            err = "out of memory";
        else
            clamp_cursor(ctx);
        if (buffer_is_scratch((int)lua_tointeger(L, 1))) ctx->model.dirty = 0;
    }
    if (err) {
        lua_pushnil(L);
        lua_pushstring(L, err);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* Id of the open buffer whose file is path (compared as absolute paths). */
static int find_buffer(const char *path) {
    char want[PATH_MAX], have[PATH_MAX];
    if (abs_path(path, want, sizeof(want)) != 0) return -1;
    int ids[MAX_BUFFERS];
    int n = buffer_get_list(ids);
    for (int i = 0; i < n; i++) {
        editor_ctx_t *ctx = buffer_get(ids[i]);
        if (ctx && ctx->model.filename &&
            abs_path(ctx->model.filename, have, sizeof(have)) == 0 &&
            strcmp(want, have) == 0)
            return ids[i];
    }
    return -1;
}

/* loki.buffer_find(path) -> id, or nil */
static int l_buffer_find(lua_State *L) {
    int id = find_buffer(luaL_checkstring(L, 1));
    if (id < 0) return 0;
    lua_pushinteger(L, id);
    return 1;
}

/* loki.open(path [, line]) -> id, or nil, message. Reuses an open buffer for
 * path, switches to it, and moves the cursor to line. */
static int l_open(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    lua_Integer line = luaL_optinteger(L, 2, 0);
    int id = find_buffer(path);
    if (id < 0) id = buffer_create(path);
    if (id < 0 || buffer_switch(id) != 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot open %s", path);
        return 2;
    }
    editor_ctx_t *ctx = buffer_get(id);
    if (ctx && line > 0) {
        int row = line > ctx->model.numrows ? ctx->model.numrows - 1 : (int)line - 1;
        if (row < 0) row = 0;
        int half = ctx->view.screenrows / 2;
        ctx->view.rowoff = row > half ? row - half : 0;
        ctx->view.cy = row - ctx->view.rowoff;
        ctx->view.coloff = 0;
        ctx->view.cx = 0;
    }
    lua_pushinteger(L, id);
    return 1;
}

/* loki.get_selection() -> {id, text, start_line, start_col, end_line,
 * end_col} for the current buffer, or nil. end_col is exclusive. */
static int l_get_selection(lua_State *L) {
    editor_ctx_t *ctx = buffer_get_current();
    if (!ctx || !ctx->view.sel_active) return 0;
    int sy = ctx->view.sel_start_y, sx = ctx->view.sel_start_x;
    int ey = ctx->view.sel_end_y, ex = ctx->view.sel_end_x;
    if (sy > ey || (sy == ey && sx > ex)) {
        int t = sy; sy = ey; ey = t;
        t = sx; sx = ex; ex = t;
    }
    char *text = get_selection_text(ctx);
    lua_createtable(L, 0, 6);
    lua_pushinteger(L, buffer_get_current_id());
    lua_setfield(L, -2, "id");
    lua_pushstring(L, text ? text : "");
    lua_setfield(L, -2, "text");
    lua_pushinteger(L, sy + 1);
    lua_setfield(L, -2, "start_line");
    lua_pushinteger(L, sx + 1);
    lua_setfield(L, -2, "start_col");
    lua_pushinteger(L, ey + 1);
    lua_setfield(L, -2, "end_line");
    lua_pushinteger(L, ex + 1);
    lua_setfield(L, -2, "end_col");
    free(text);
    return 1;
}

/* loki.buffer_new(name) -> id. A scratch buffer: shown as name, never
 * dirty, not switched to. */
static int l_buffer_new(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    int id = buffer_create(NULL);
    if (id < 0 || buffer_set_scratch(id, name) != 0) {
        lua_pushnil(L);
        lua_pushliteral(L, "no free buffer slot");
        return 2;
    }
    lua_pushinteger(L, id);
    return 1;
}

/* loki.buffer_save(id) -> true, or nil, message */
static int l_buffer_save(lua_State *L) {
    editor_ctx_t *ctx = check_buffer(L, 1);
    if (!ctx || editor_save(ctx) != 0) {
        lua_pushnil(L);
        lua_pushstring(L, ctx ? ctx->view.statusmsg : "no such buffer");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* Registry ref of the pending loki.choose callback; one at a time. */
static lua_State *choose_L;
static int choose_ref = LUA_NOREF;

static void on_choice(int index, void *ud) {
    (void)ud;
    lua_State *L = choose_L;
    int ref = choose_ref;
    choose_ref = LUA_NOREF;
    if (!L || ref == LUA_NOREF) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
    if (index >= 0) lua_pushinteger(L, index + 1);
    else lua_pushnil(L);
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        editor_ctx_t *ctx = buffer_get_current();
        if (ctx) editor_set_status_msg(ctx, "choose: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

/* loki.choose(question, labels, fn) -> true, or false if a choice is
 * already pending. fn(i) gets the 1-based label index, or nil on cancel. */
static int l_choose(lua_State *L) {
    const char *question = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    int n = (int)lua_rawlen(L, 2);
    if (n < 1 || n > CHOICE_MAX) return luaL_argerror(L, 2, "need 1 to 9 labels");
    const char *labels[CHOICE_MAX];
    for (int i = 0; i < n; i++) {
        lua_rawgeti(L, 2, i + 1);
        labels[i] = luaL_checkstring(L, -1);  /* the table keeps it alive */
        lua_pop(L, 1);
    }
    if (choice_prompt()) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
    choose_L = lua_tothread(L, -1);
    lua_pop(L, 1);
    lua_pushvalue(L, 3);
    choose_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    choice_begin(question, labels, n, on_choice, NULL);
    lua_pushboolean(L, 1);
    return 1;
}

/* loki.choose_cancel(): dismiss a pending choice; its callback gets nil */
static int l_choose_cancel(lua_State *L) {
    (void)L;
    choice_cancel();
    return 0;
}

void loki_lua_bind_buffers(lua_State *L) {
    static const luaL_Reg fns[] = {
        {"buffers", l_buffers},
        {"buffer_text", l_buffer_text},
        {"buffer_replace", l_buffer_replace},
        {"buffer_find", l_buffer_find},
        {"open", l_open},
        {"get_selection", l_get_selection},
        {"abspath", l_abspath},
        {"buffer_new", l_buffer_new},
        {"buffer_save", l_buffer_save},
        {"choose", l_choose},
        {"choose_cancel", l_choose_cancel},
        {NULL, NULL},
    };
    lua_getglobal(L, "loki");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
    }
    luaL_setfuncs(L, fns, 0);
    lua_setglobal(L, "loki");
}
