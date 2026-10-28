/* test_lua_buffers.c - Lua API for any open buffer */

#include "test_framework.h"
#include "loki/core.h"
#include "internal.h"
#include "buffers.h"
#include "undo.h"
#include "lua_helpers.h"
#include <stdlib.h>
#include <unistd.h>

static lua_State *L;
static char dir[] = "/tmp/loki_bufXXXXXX";

static void write_file(const char *name, const char *text) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
}

/* Run Lua that leaves an integer global 'id'; return that buffer's ctx. */
static editor_ctx_t *buffer_from_global(void) {
    lua_getglobal(L, "id");
    editor_ctx_t *ctx = buffer_get((int)lua_tointeger(L, -1));
    lua_pop(L, 1);
    return ctx;
}

TEST(open_reads_file_and_reuses_buffer) {
    ASSERT_LUA(L,
        "id = assert(loki.open(dir .. '/a.txt'))\n"
        "assert(loki.buffer_text(id) == 'one\\ntwo\\nthree')\n"
        "assert(loki.buffer_text(id, 2, 3) == 'two\\nthree')\n"
        "assert(loki.buffer_text(id, 2, 2) == 'two')\n"
        "assert(loki.open(dir .. '/./a.txt') == id)\n"
        "assert(loki.buffer_find(dir .. '/a.txt') == id)\n"
        "assert(loki.buffer_find(dir .. '/missing.txt') == nil)");
}

TEST(buffers_lists_path_dirty_current) {
    ASSERT_LUA(L,
        "id = assert(loki.open(dir .. '/a.txt', 2))\n"
        "local found\n"
        "for _, b in ipairs(loki.buffers()) do if b.id == id then found = b end end\n"
        "assert(found and found.current and not found.dirty)\n"
        "assert(found.path:sub(-6) == '/a.txt' and found.path:sub(1, 1) == '/')\n"
        "assert(found.line == 2 and found.col == 1)");
}

TEST(replace_within_line) {
    ASSERT_LUA(L,
        "id = assert(loki.open(dir .. '/b.txt'))\n"
        "local t = loki.buffer_text(id)\n"
        "local s, e = t:find('beta', 1, true)\n"
        "assert(loki.buffer_replace(id, s, e, 'BETA'))\n"
        "assert(loki.buffer_text(id) == 'alpha BETA\\ngamma')\n"
        "assert(loki.buffers and true)");
    ASSERT_TRUE(buffer_from_global()->model.dirty);
}

TEST(replace_across_lines_and_undo_as_one_step) {
    ASSERT_LUA(L,
        "id = assert(loki.open(dir .. '/c.txt'))\n"
        "local t = loki.buffer_text(id)\n"
        "local s, e = t:find('2\\n3', 1, true)\n"
        "assert(loki.buffer_replace(id, s, e, 'X\\nY\\nZ'))\n"
        "assert(loki.buffer_text(id) == '1\\nX\\nY\\nZ\\n4', loki.buffer_text(id))");
    editor_ctx_t *ctx = buffer_from_global();
    ASSERT_EQ(undo_perform(ctx), 1);
    ASSERT_LUA(L, "assert(loki.buffer_text(id) == '1\\n2\\n3\\n4', loki.buffer_text(id))");
    ASSERT_EQ(redo_perform(ctx), 1);
    ASSERT_LUA(L, "assert(loki.buffer_text(id) == '1\\nX\\nY\\nZ\\n4', loki.buffer_text(id))");
}

TEST(undo_of_replace_keeps_earlier_typing) {
    ASSERT_LUA(L, "id = assert(loki.open(dir .. '/f.txt'))");
    editor_ctx_t *ctx = buffer_from_global();
    editor_insert_char(ctx, 'Z');   /* typed at 1:1, same position as the replace */
    ASSERT_LUA(L,
        "assert(loki.buffer_text(id) == 'Zxy')\n"
        "assert(loki.buffer_replace(id, 1, 0, 'Q'))\n"
        "assert(loki.buffer_text(id) == 'QZxy')");
    ASSERT_EQ(undo_perform(ctx), 1);
    ASSERT_LUA(L, "assert(loki.buffer_text(id) == 'Zxy', loki.buffer_text(id))");
    ASSERT_EQ(undo_perform(ctx), 1);
    ASSERT_LUA(L, "assert(loki.buffer_text(id) == 'xy', loki.buffer_text(id))");
}

TEST(replace_insert_and_delete) {
    ASSERT_LUA(L,
        "id = assert(loki.open(dir .. '/d.txt'))\n"
        "assert(loki.buffer_replace(id, 1, 0, 'start\\n'))\n"
        "assert(loki.buffer_text(id) == 'start\\nab\\ncd')\n"
        "local t = loki.buffer_text(id)\n"
        "assert(loki.buffer_replace(id, #t + 1, #t, '\\nend'))\n"
        "assert(loki.buffer_text(id) == 'start\\nab\\ncd\\nend')\n"
        "assert(loki.buffer_replace(id, 7, 9, ''))\n"
        "assert(loki.buffer_text(id) == 'start\\ncd\\nend', loki.buffer_text(id))");
}

TEST(replace_whole_buffer_clamps_cursor) {
    ASSERT_LUA(L,
        "id = assert(loki.open(dir .. '/e.txt', 5))\n"
        "local t = loki.buffer_text(id)\n"
        "assert(loki.buffer_replace(id, 1, #t, 'only'))\n"
        "assert(loki.buffer_text(id) == 'only')\n"
        "for _, b in ipairs(loki.buffers()) do\n"
        "  if b.id == id then assert(b.line == 1 and b.col <= 5, b.line .. ':' .. b.col) end\n"
        "end");
    editor_ctx_t *ctx = buffer_from_global();
    ASSERT_EQ(ctx->model.numrows, 1);
    ASSERT_EQ(undo_perform(ctx), 1);
    ASSERT_EQ(ctx->model.numrows, 6);
}

TEST(replace_rejects_bad_input) {
    ASSERT_LUA(L,
        "local ok, err = loki.buffer_replace(9999, 1, 0, 'x')\n"
        "assert(ok == nil and err == 'no such buffer')\n"
        "id = assert(loki.open(dir .. '/a.txt'))\n"
        "ok, err = loki.buffer_replace(id, 0, 0, 'x')\n"
        "assert(ok == nil and err == 'invalid range')\n"
        "ok, err = loki.buffer_replace(id, 3, 1, 'x')\n"
        "assert(ok == nil and err == 'invalid range')\n"
        "assert(loki.buffer_text(9999) == nil)");
}

TEST(selection_reports_range_and_text) {
    ASSERT_LUA(L, "id = assert(loki.open(dir .. '/c.txt')); assert(loki.get_selection() == nil)");
    editor_ctx_t *ctx = buffer_from_global();
    ctx->view.sel_active = 1;
    ctx->view.sel_start_y = 2;  /* selected backwards: end before start */
    ctx->view.sel_start_x = 1;
    ctx->view.sel_end_y = 1;
    ctx->view.sel_end_x = 0;
    ASSERT_LUA(L,
        "local s = assert(loki.get_selection())\n"
        "assert(s.id == id and s.start_line == 2 and s.start_col == 1)\n"
        "assert(s.end_line == 3 and s.end_col == 2)\n"
        "assert(s.text == loki.buffer_text(id):sub(3, 5), s.text)");
    ctx->view.sel_active = 0;
}

BEGIN_TEST_SUITE("Lua buffer API")
    if (!mkdtemp(dir)) return 1;
    write_file("a.txt", "one\ntwo\nthree\n");
    write_file("b.txt", "alpha beta\ngamma\n");
    write_file("c.txt", "1\n2\n3\n4\n");
    write_file("d.txt", "ab\ncd\n");
    write_file("e.txt", "1\n2\n3\n4\n5\n6\n");
    write_file("f.txt", "xy\n");

    editor_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.view.screencols = 80;
    ctx.view.screenrows = 24;
    buffers_init(&ctx);
    L = lua_test_state(buffer_get_current());
    lua_pushstring(L, dir);
    lua_setglobal(L, "dir");

    RUN_TEST(open_reads_file_and_reuses_buffer);
    RUN_TEST(buffers_lists_path_dirty_current);
    RUN_TEST(replace_within_line);
    RUN_TEST(replace_across_lines_and_undo_as_one_step);
    RUN_TEST(undo_of_replace_keeps_earlier_typing);
    RUN_TEST(replace_insert_and_delete);
    RUN_TEST(replace_whole_buffer_clamps_cursor);
    RUN_TEST(replace_rejects_bad_input);
    RUN_TEST(selection_reports_range_and_text);

    lua_close(L);
    buffers_free();
    const char *names[] = {"a.txt", "b.txt", "c.txt", "d.txt", "e.txt", "f.txt"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        unlink(path);
    }
    rmdir(dir);
END_TEST_SUITE()
