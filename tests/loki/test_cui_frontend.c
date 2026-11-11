/* test_cui_frontend.c - loki-cui frame loop, headless.
 *
 * Drives loki_cui_frame() through a CUI context with the software renderer,
 * injecting input the way a window backend does, and reads the cell grid.
 */
#include "test_framework.h"
#include "loki/core.h"
#include "internal.h"
#include "buffers.h"
#include "cui_frontend.h"
#include <cui/cui_render.h>
#include <string.h>

#define VIEW_W 800.0f
#define VIEW_H 600.0f

#ifdef __APPLE__
#define PASTE_MODS CUI_MOD_SUPER
#else
#define PASTE_MODS (CUI_MOD_CTRL | CUI_MOD_SHIFT)
#endif

static cui_sw_renderer *sw;
static cui_ctx *ui;
static editor_ctx_t seed;

static const char *fake_clip;
static char copied[256];
static const char *fake_get(void *user) { (void)user; return fake_clip; }
static void fake_set(void *user, const char *s) {
    (void)user;
    snprintf(copied, sizeof copied, "%s", s);
}

static void setup(void) {
    memset(&seed, 0, sizeof seed);
    editor_ctx_init(&seed);
    seed.view.screencols = 80;
    seed.view.screenrows = 24;
    buffers_init(&seed);
    sw = cui_sw_create();
    ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw), .theme = CUI_THEME_DARK });
    cui_set_platform(ui, &(cui_platform_iface){ .get_clipboard = fake_get, .set_clipboard = fake_set });
    fake_clip = NULL;
    copied[0] = '\0';
}

static void teardown(void) {
    buffers_free();
    loki_cui_shutdown();
    cui_destroy(ui);
    cui_sw_destroy(sw);
}

static int frame(void) {
    cui_set_viewport(ui, VIEW_W, VIEW_H, 1.0f);
    cui_begin_frame(ui, 1.0f / 60.0f);
    int rc = loki_cui_frame(ui, (cui_vec2){ VIEW_W, VIEW_H });
    cui_end_frame(ui);
    return rc;
}

static void press(cui_key k, int mods) {
    cui_input_key(ui, k, true, mods);
    cui_input_key(ui, k, false, mods);
}

static const char *grid_row(int row) {
    static char buf[1024];
    if (loki_cui_grid_row(row, buf, sizeof buf) != 0) return "";
    return buf;
}

static const char *row0(void) {
    editor_ctx_t *c = buffer_get_current();
    return (c && c->model.numrows > 0) ? c->model.row[0].chars : "";
}

/* The grid follows the viewport, and loki's buffers follow the grid. */
TEST(cui_grid_sizes_buffers) {
    setup();
    ASSERT_EQ(frame(), 0);
    int rows = 0, cols = 0;
    ASSERT_EQ(loki_cui_grid_size(&rows, &cols), 0);
    ASSERT_TRUE(rows > STATUS_ROWS && cols >= 20);
    ASSERT_EQ(buffer_get_current()->view.screencols, cols);
    ASSERT_EQ(buffer_get_current()->view.screenrows, rows - STATUS_ROWS);
    ASSERT_TRUE(strstr(grid_row(rows - STATUS_ROWS), "NORMAL") != NULL);
    teardown();
}

/* Text and keys that land in one frame apply in arrival order: with
 * ESC first, the buffer would stay in INSERT and hold "hello" typed later. */
TEST(cui_input_keeps_order_within_frame) {
    setup();
    frame();
    cui_input_text(ui, "ihello");
    press(CUI_KEY_ESCAPE, 0);
    cui_input_text(ui, "x");
    frame();
    ASSERT_EQ(buffer_get_current()->view.mode, MODE_NORMAL);
    ASSERT_EQ(buffer_get_current()->model.row[0].size, 4);
    ASSERT_STR_EQ(grid_row(0), row0());
    teardown();
}

/* Printable keys arrive as text; the key event itself must not double them. */
TEST(cui_printable_key_not_doubled) {
    setup();
    frame();
    cui_input_text(ui, "i");
    frame();
    press(CUI_KEY_A, 0);
    cui_input_text(ui, "a");
    frame();
    ASSERT_STR_EQ(row0(), "a");
    teardown();
}

TEST(cui_ctrl_letter_maps_to_control_code) {
    setup();
    frame();
    press(CUI_KEY_T, CUI_MOD_CTRL);   /* Ctrl-T: new buffer */
    frame();
    ASSERT_EQ(buffer_count(), 2);
    ASSERT_NOT_NULL(buffer_get_current()->renderer);   /* new buffer drawn too */
    teardown();
}

TEST(cui_cursor_follows_insert) {
    setup();
    frame();
    cui_input_text(ui, "iabc");
    frame();
    int r = -1, c = -1;
    loki_cui_cursor(&r, &c);
    ASSERT_EQ(r, 0);
    ASSERT_EQ(c, 3);
    teardown();
}

/* Paste in NORMAL mode would run the clipboard as commands ("dd"). */
TEST(cui_paste_refused_in_normal_mode) {
    setup();
    frame();
    cui_input_text(ui, "ikeep");
    press(CUI_KEY_ESCAPE, 0);
    frame();
    fake_clip = "dd";
    press(CUI_KEY_V, PASTE_MODS);
    frame();
    ASSERT_EQ(buffer_get_current()->model.numrows, 1);
    ASSERT_STR_EQ(row0(), "keep");
    teardown();
}

TEST(cui_paste_inserts_in_insert_mode) {
    setup();
    frame();
    cui_input_text(ui, "i");
    frame();
    fake_clip = "ab\ncd";
    press(CUI_KEY_V, PASTE_MODS);
    frame();
    editor_ctx_t *c = buffer_get_current();
    ASSERT_EQ(c->model.numrows, 2);
    ASSERT_STR_EQ(c->model.row[0].chars, "ab");
    ASSERT_STR_EQ(c->model.row[1].chars, "cd");
    teardown();
}

/* Visual-mode yank goes to the system clipboard through the renderer.
 * Selection end is exclusive (test_selection.c), so "vll" covers "hi". */
TEST(cui_yank_reaches_clipboard) {
    setup();
    frame();
    cui_input_text(ui, "ihi");
    press(CUI_KEY_ESCAPE, 0);
    cui_input_text(ui, "hvlly");
    frame();
    ASSERT_STR_EQ(copied, "hi");
    teardown();
}

BEGIN_TEST_SUITE("CUI frontend")
    RUN_TEST(cui_grid_sizes_buffers);
    RUN_TEST(cui_input_keeps_order_within_frame);
    RUN_TEST(cui_printable_key_not_doubled);
    RUN_TEST(cui_ctrl_letter_maps_to_control_code);
    RUN_TEST(cui_cursor_follows_insert);
    RUN_TEST(cui_paste_refused_in_normal_mode);
    RUN_TEST(cui_paste_inserts_in_insert_mode);
    RUN_TEST(cui_yank_reaches_clipboard);
END_TEST_SUITE()
