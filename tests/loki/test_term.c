/* test_term.c - terminal pane: layout, pty I/O, DECCKM, focus key, drawing */

#include "test_framework.h"
#include "loki/core.h"
#include "internal.h"
#include "terminal.h"
#include "command.h"
#include "term.h"
#include <ghostty/vt.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int cmd_set(editor_ctx_t *ctx, const char *args);

static editor_ctx_t ctx;
static int key;
static char *render_to_text(void);

static void setup(int rows, int cols) {
    loki_term_close();
    loki_term_set_layout("bottom", 0);
    memset(&ctx, 0, sizeof(ctx));
    terminal_set_window_size(&ctx, rows, cols);
    editor_update_repl_layout(&ctx);
}

static int open_sh(const char *script) {
    const char *argv[] = { "/bin/sh", "-c", script, NULL };
    if (loki_term_open(&ctx, argv) < 0) return -1;
    editor_update_repl_layout(&ctx);
    return 0;
}

/* Pump until the grid contains needle; 5 s limit. */
static int wait_text(const char *needle) {
    for (int i = 0; i < 500; i++) {
        loki_term_pump();
        char *t = loki_term_screen_text();
        int hit = t && strstr(t, needle);
        free(t);
        if (hit) return 1;
        nanosleep(&(struct timespec){0, 10 * 1000 * 1000}, NULL);
    }
    return 0;
}

static int wait_closed(void) {
    for (int i = 0; i < 500 && loki_term_is_open(); i++) {
        loki_term_pump();
        nanosleep(&(struct timespec){0, 10 * 1000 * 1000}, NULL);
    }
    return !loki_term_is_open();
}

TEST(term_bottom_layout) {
    setup(24, 80);
    ASSERT_EQ(ctx.view.screenrows, 22);
    ASSERT_EQ(open_sh("sleep 10"), 0);
    TermRect r;
    ASSERT_TRUE(loki_term_rect(&ctx, &r));
    ASSERT_EQ(r.top, 11);
    ASSERT_EQ(r.height, 13);
    ASSERT_EQ(r.grid_rows, 12);
    ASSERT_EQ(r.grid_cols, 80);
    ASSERT_EQ(ctx.view.screenrows, 9);
    ASSERT_EQ(ctx.view.screencols, 80);

    /* Oversized pane is clamped to leave one text row. */
    ASSERT_EQ(loki_term_set_layout(NULL, 30), 0);
    ASSERT_TRUE(loki_term_rect(&ctx, &r));
    ASSERT_EQ(r.height, 21);
    editor_update_repl_layout(&ctx);
    ASSERT_EQ(ctx.view.screenrows, 1);
    loki_term_close();
    editor_update_repl_layout(&ctx);
    ASSERT_EQ(ctx.view.screenrows, 22);
}

TEST(term_right_layout) {
    setup(24, 100);
    ASSERT_EQ(loki_term_set_layout("right", 0), 0);
    ASSERT_EQ(open_sh("sleep 10"), 0);
    TermRect r;
    ASSERT_TRUE(loki_term_rect(&ctx, &r));
    ASSERT_EQ(r.left, 49);
    ASSERT_EQ(r.width, 51);
    ASSERT_EQ(r.grid_rows, 23);
    ASSERT_EQ(r.grid_cols, 50);
    ASSERT_EQ(ctx.view.screencols, 49);
    ASSERT_EQ(ctx.view.screenrows, 22);

    /* Too narrow to show: no pane, full-width text. */
    terminal_set_window_size(&ctx, 24, 20);
    editor_update_repl_layout(&ctx);
    ASSERT_FALSE(loki_term_rect(&ctx, &r));
    ASSERT_EQ(ctx.view.screencols, 20);
    loki_term_close();
}

TEST(term_set_options) {
    setup(24, 80);
    ASSERT_EQ(cmd_set(&ctx, "termpos=right"), 1);
    ASSERT_EQ(loki_term_position(), TERM_POS_RIGHT);
    ASSERT_EQ(cmd_set(&ctx, "termsize=30"), 1);
    ASSERT_EQ(loki_term_size(), 30);
    ASSERT_EQ(cmd_set(&ctx, "termpos=left"), 0);
    ASSERT_EQ(cmd_set(&ctx, "termsize=-1"), 0);
    ASSERT_EQ(cmd_set(&ctx, "termsize=12x"), 0);
    ASSERT_EQ(loki_term_size(), 30);
    ASSERT_EQ(loki_term_set_layout("top", 0), -1);
}

TEST(term_output_reaches_grid) {
    setup(24, 80);
    ASSERT_EQ(open_sh("printf 'hello\\033[1mworld'; sleep 10"), 0);
    ASSERT_TRUE(wait_text("helloworld"));
    loki_term_close();
}

TEST(term_input_round_trip) {
    setup(24, 80);
    ASSERT_EQ(open_sh("exec cat"), 0);
    ASSERT_EQ(loki_term_send("abc\n", 4), 0);
    ASSERT_TRUE(wait_text("abc\nabc"));     /* tty echo, then cat */
    loki_term_close();
}

TEST(term_exit_closes_pane) {
    setup(24, 80);
    ASSERT_EQ(open_sh("exit 3"), 0);
    ASSERT_TRUE(wait_closed());
    ASSERT_EQ(loki_term_last_status(), 3);
    editor_update_repl_layout(&ctx);
    ASSERT_EQ(ctx.view.screenrows, 22);
}

TEST(term_cursor_keys_mode) {
    setup(24, 80);
    ASSERT_EQ(open_sh("stty raw -echo; printf '\\033[?1hREADY';"
                      " dd bs=1 count=3 2>/dev/null | od -An -tx1 | tr -s ' '; sleep 10"), 0);
    ASSERT_TRUE(wait_text("READY"));
    ASSERT_EQ(loki_term_send("\x1b[A", 3), 0);
    ASSERT_TRUE(wait_text("1b 4f 41"));     /* ESC O A */
    loki_term_close();
}

TEST(term_focus_key_returns_focus) {
    setup(24, 80);
    ASSERT_EQ(open_sh("exec cat"), 0);
    loki_term_set_focus(1);
    int p[2];
    ASSERT_EQ(pipe(p), 0);
    ASSERT_EQ(write(p[1], "xy\x1czz", 5), 5);
    ASSERT_EQ(loki_term_handle_input(p[0], &key), 0);
    ASSERT_FALSE(loki_term_focused());
    ASSERT_EQ(key, 'z');                    /* the next key goes to the editor */
    ASSERT_TRUE(wait_text("xy"));
    char *t = loki_term_screen_text();
    ASSERT_TRUE(t && !strstr(t, "zz"));
    free(t);

    /* Ctrl-\ alone: focus returns after the prefix timeout, no key. */
    loki_term_set_focus(1);
    ASSERT_EQ(write(p[1], "\x1c", 1), 1);
    ASSERT_EQ(loki_term_handle_input(p[0], &key), 0);
    ASSERT_FALSE(loki_term_focused());
    ASSERT_EQ(key, -1);

    /* Ctrl-\ Ctrl-\ sends one literal Ctrl-\ and keeps focus. */
    loki_term_set_focus(1);
    ASSERT_EQ(write(p[1], "\x1c\x1c", 2), 2);
    ASSERT_EQ(loki_term_handle_input(p[0], &key), 0);
    ASSERT_TRUE(loki_term_focused());
    ASSERT_EQ(key, -1);
    close(p[0]);
    close(p[1]);
    loki_term_close();
}

/* Feed one chunk of keys to the focused pane. */
static void keys(int wfd, int rfd, const char *s) {
    if (write(wfd, s, strlen(s)) < 0) return;
    loki_term_handle_input(rfd, &key);
}

TEST(term_scroll_mode) {
    setup(24, 80);
    ASSERT_EQ(open_sh("awk 'BEGIN { for (i = 0; i < 500; i++) print \"L\" i }'; exec cat"), 0);
    ASSERT_TRUE(wait_text("L499"));
    loki_term_set_focus(1);
    int p[2];
    ASSERT_EQ(pipe(p), 0);

    keys(p[1], p[0], "\x1c[");
    ASSERT_TRUE(loki_term_scrolling());
    ASSERT_TRUE(loki_term_focused());
    keys(p[1], p[0], "k");
    ASSERT_EQ(loki_term_scroll_offset(), 1);
    keys(p[1], p[0], "\x1b[A");             /* Up */
    ASSERT_EQ(loki_term_scroll_offset(), 2);
    keys(p[1], p[0], "j");
    ASSERT_EQ(loki_term_scroll_offset(), 1);
    keys(p[1], p[0], "b");                  /* page = 11 rows */
    ASSERT_EQ(loki_term_scroll_offset(), 12);
    keys(p[1], p[0], "\x1b[6~");            /* PgDn */
    ASSERT_EQ(loki_term_scroll_offset(), 1);
    keys(p[1], p[0], "\x15");               /* Ctrl-U, half page */
    ASSERT_EQ(loki_term_scroll_offset(), 6);
    keys(p[1], p[0], "g");
    ASSERT_TRUE(loki_term_scroll_offset() >= 480);
    char *t = render_to_text();
    ASSERT_TRUE(t && strstr(t, "SCROLL") && strstr(t, "\nL0\n"));
    free(t);
    keys(p[1], p[0], "G");
    ASSERT_EQ(loki_term_scroll_offset(), 0);

    /* Typed text does not reach the child in scroll mode. */
    keys(p[1], p[0], "hidden\r");
    ASSERT_FALSE(loki_term_scrolling());     /* Enter left */
    keys(p[1], p[0], "\x1c[");
    keys(p[1], p[0], "k");
    keys(p[1], p[0], "\x1b");               /* lone Esc leaves, back at bottom */
    ASSERT_FALSE(loki_term_scrolling());
    ASSERT_EQ(loki_term_scroll_offset(), 0);
    ASSERT_TRUE(loki_term_focused());
    keys(p[1], p[0], "shown\n");
    ASSERT_TRUE(wait_text("shown\nshown"));
    t = loki_term_screen_text();
    ASSERT_TRUE(t && !strstr(t, "hidden"));
    free(t);

    /* Keys after q in the same read go to the child, not scroll mode. */
    keys(p[1], p[0], "\x1c[");
    keys(p[1], p[0], "qafter\n");
    ASSERT_FALSE(loki_term_scrolling());
    ASSERT_TRUE(wait_text("after\nafter"));
    keys(p[1], p[0], "\x1c[");
    keys(p[1], p[0], "q\x1c");
    ASSERT_FALSE(loki_term_focused());
    loki_term_set_focus(1);

    /* Ctrl-\ in scroll mode returns focus to the editor. */
    keys(p[1], p[0], "\x1c[");
    keys(p[1], p[0], "\x1c");
    ASSERT_FALSE(loki_term_scrolling());
    ASSERT_FALSE(loki_term_focused());
    close(p[0]);
    close(p[1]);
    loki_term_close();
}

/* Replay loki's frame into a second terminal and read the screen back. */
TEST(term_render_places_pane) {
    setup(24, 80);
    ASSERT_EQ(open_sh("printf XYZ; sleep 10"), 0);
    ASSERT_TRUE(wait_text("XYZ"));
    struct abuf ab = ABUF_INIT;
    int crow = 0, ccol = 0;
    loki_term_set_focus(1);
    ASSERT_EQ(loki_term_render(&ctx, &ab, &crow, &ccol), 1);
    ASSERT_EQ(crow, 13);                    /* grid row 0 is screen row 13 */
    ASSERT_EQ(ccol, 4);

    GhosttyTerminal host = NULL;
    ASSERT_EQ(ghostty_terminal_new(NULL, &host, 80, 24), GHOSTTY_SUCCESS);
    ghostty_terminal_vt_write(host, (const uint8_t *)ab.b, (size_t)ab.len);
    terminal_buffer_free(&ab);
    GhosttyFormatter f = NULL;
    ASSERT_EQ(ghostty_formatter_terminal_new(NULL, &f, host), GHOSTTY_SUCCESS);
    uint8_t *out = NULL;
    size_t len = 0;
    ASSERT_EQ(ghostty_formatter_format_alloc(f, NULL, &out, &len), GHOSTTY_SUCCESS);
    char *text = strndup((const char *)out, len);
    ghostty_free(NULL, out, len);
    ghostty_formatter_free(f);
    ghostty_terminal_free(host);

    /* Rows 0-10 are left blank by the pane; row 11 is the header. */
    char *line = text;
    for (int i = 0; i < 11 && line; i++) {
        line = strchr(line, '\n');
        if (line) line++;
    }
    ASSERT_TRUE(line && strncmp(line, " [term] sh", 10) == 0);
    line = strchr(line, '\n');
    ASSERT_TRUE(line && strncmp(line + 1, "XYZ", 3) == 0);
    free(text);
    loki_term_close();
}

/* Render the pane and return the host screen as plain text (malloc'd). */
static char *render_to_text(void) {
    struct abuf ab = ABUF_INIT;
    int crow, ccol;
    loki_term_render(&ctx, &ab, &crow, &ccol);
    GhosttyTerminal host = NULL;
    if (ghostty_terminal_new(NULL, &host, 80, 24) != GHOSTTY_SUCCESS) return NULL;
    ghostty_terminal_vt_write(host, (const uint8_t *)ab.b, (size_t)ab.len);
    terminal_buffer_free(&ab);
    GhosttyFormatter f = NULL;
    char *text = NULL;
    if (ghostty_formatter_terminal_new(NULL, &f, host) == GHOSTTY_SUCCESS) {
        uint8_t *out = NULL;
        size_t len = 0;
        if (ghostty_formatter_format_alloc(f, NULL, &out, &len) == GHOSTTY_SUCCESS) {
            text = strndup((const char *)out, len);
            ghostty_free(NULL, out, len);
        }
        ghostty_formatter_free(f);
    }
    ghostty_terminal_free(host);
    return text;
}

TEST(term_scrollback_pages_and_snaps_back) {
    setup(24, 80);
    /* 5000 rows span many pages; the default cap keeps about one. */
    ASSERT_EQ(open_sh("awk 'BEGIN { for (i = 0; i < 5000; i++) print \"L\" i }'; exec cat"), 0);
    ASSERT_TRUE(wait_text("L4999"));
    ASSERT_EQ(loki_term_scroll_offset(), 0);

    loki_term_scroll(-100000);
    ASSERT_TRUE(loki_term_scroll_offset() >= 4980);
    char *t = render_to_text();
    ASSERT_TRUE(t && strstr(t, "\nL0\n") && !strstr(t, "L4999"));
    ASSERT_TRUE(strstr(t, "[scroll -"));
    free(t);
    loki_term_scroll(100000);
    ASSERT_EQ(loki_term_scroll_offset(), 0);

    /* Shift-PgUp scrolls one page (grid rows - 1) and is not forwarded. */
    loki_term_set_focus(1);
    int p[2];
    ASSERT_EQ(pipe(p), 0);
    ASSERT_EQ(write(p[1], "\x1b[5;2~", 6), 6);
    ASSERT_EQ(loki_term_handle_input(p[0], &key), 0);
    ASSERT_EQ(loki_term_scroll_offset(), 11);
    ASSERT_EQ(write(p[1], "\x1b[6;2~", 6), 6);
    ASSERT_EQ(loki_term_handle_input(p[0], &key), 0);
    ASSERT_EQ(loki_term_scroll_offset(), 0);

    /* Typing while scrolled returns to the bottom and reaches the child. */
    loki_term_scroll(-50);
    ASSERT_EQ(write(p[1], "typed\n", 6), 6);
    ASSERT_EQ(loki_term_handle_input(p[0], &key), 0);
    ASSERT_EQ(loki_term_scroll_offset(), 0);
    ASSERT_TRUE(wait_text("typed\ntyped"));
    t = loki_term_screen_text();
    ASSERT_TRUE(t && !strstr(t, "[5;2~"));
    free(t);
    close(p[0]);
    close(p[1]);
    loki_term_close();
}

/* Wait until pred() holds, pumping; secs limit. */
static int pump_until(int (*pred)(void), int secs) {
    for (int i = 0; i < secs * 100; i++) {
        loki_term_pump();
        if (pred()) return 1;
        nanosleep(&(struct timespec){0, 10 * 1000 * 1000}, NULL);
    }
    return 0;
}

static int has_compressed_page(void) {
    TermMemory m;
    return loki_term_memory(&m) == 0 && m.compressed_pages > 0;
}

TEST(term_scroll_mode_prompt_jumps) {
    setup(24, 80);
    /* Three prompts marked with OSC 133, each followed by 30 output rows. */
    ASSERT_EQ(open_sh("for i in 1 2 3 4; do printf '\\033]133;A\\007P%d$ \\033]133;B\\007\\n' $i;"
                      " [ $i = 4 ] && break; printf '\\033]133;C\\007';"
                      " awk -v i=$i 'BEGIN { for (j = 0; j < 30; j++) print \"o\" i \"_\" j }'; done;"
                      " exec cat"), 0);
    ASSERT_TRUE(wait_text("P4$"));
    loki_term_set_focus(1);
    int p[2];
    ASSERT_EQ(pipe(p), 0);
    ASSERT_EQ(write(p[1], "\x1c[[", 3), 3);
    ASSERT_EQ(loki_term_handle_input(p[0], &key), 0);
    ASSERT_TRUE(loki_term_scrolling());
    char *t = render_to_text();
    ASSERT_TRUE(t && strstr(t, "P3$") && !strstr(t, "P4$"));
    free(t);
    ASSERT_EQ(write(p[1], "[", 1), 1);
    ASSERT_EQ(loki_term_handle_input(p[0], &key), 0);
    t = render_to_text();
    ASSERT_TRUE(t && strstr(t, "P2$") && !strstr(t, "P3$"));
    free(t);
    ASSERT_EQ(write(p[1], "]", 1), 1);
    ASSERT_EQ(loki_term_handle_input(p[0], &key), 0);
    t = render_to_text();
    ASSERT_TRUE(t && strstr(t, "P3$") && !strstr(t, "P2$"));
    free(t);
    ASSERT_EQ(write(p[1], "q", 1), 1);
    ASSERT_EQ(loki_term_handle_input(p[0], &key), 0);
    ASSERT_EQ(loki_term_scroll_offset(), 0);
    close(p[0]);
    close(p[1]);
    loki_term_close();
}

TEST(term_scroll_mode_prompt_jump_without_marks) {
    setup(24, 80);
    ASSERT_EQ(open_sh("awk 'BEGIN { for (i = 0; i < 100; i++) print \"L\" i }'; exec cat"), 0);
    ASSERT_TRUE(wait_text("L99"));
    loki_term_set_focus(1);
    int p[2];
    ASSERT_EQ(pipe(p), 0);
    ASSERT_EQ(write(p[1], "\x1c[k[", 4), 4);
    ASSERT_EQ(loki_term_handle_input(p[0], &key), 0);
    ASSERT_EQ(loki_term_scroll_offset(), 1);   /* [ is a no-op */
    close(p[0]);
    close(p[1]);
    loki_term_close();
}

TEST(term_memory_and_idle_compression) {
    setup(24, 80);
    TermMemory m;
    ASSERT_EQ(loki_term_memory(&m), -1);
    ASSERT_EQ(open_sh("awk 'BEGIN { for (i = 0; i < 5000; i++) print \"L\" i }'; exec cat"), 0);
    ASSERT_TRUE(wait_text("L4999"));
    ASSERT_EQ(loki_term_memory(&m), 0);
    ASSERT_TRUE(m.pages > 1);
    ASSERT_TRUE(m.resident_bytes > 0 && m.virtual_bytes >= m.resident_bytes);
    ASSERT_EQ(m.compressed_pages, 0ULL);
    unsigned long long before = m.resident_bytes;
    if (!m.compression_supported) {
        printf("  (compression unsupported on this platform)\n");
        loki_term_close();
        return;
    }
    /* Quiet for TERM_COMPRESS_IDLE_MS, then steps run from the pump. */
    ASSERT_TRUE(pump_until(has_compressed_page, 8));
    ASSERT_EQ(loki_term_memory(&m), 0);
    ASSERT_TRUE(m.resident_bytes < before);
    /* History reads back unchanged. */
    loki_term_scroll(-100000);
    char *t = render_to_text();
    ASSERT_TRUE(t && strstr(t, "\nL0\n"));
    free(t);
    loki_term_close();
}

BEGIN_TEST_SUITE("terminal pane")
    RUN_TEST(term_bottom_layout);
    RUN_TEST(term_right_layout);
    RUN_TEST(term_set_options);
    RUN_TEST(term_output_reaches_grid);
    RUN_TEST(term_input_round_trip);
    RUN_TEST(term_exit_closes_pane);
    RUN_TEST(term_cursor_keys_mode);
    RUN_TEST(term_focus_key_returns_focus);
    RUN_TEST(term_render_places_pane);
    RUN_TEST(term_scrollback_pages_and_snaps_back);
    RUN_TEST(term_scroll_mode);
    RUN_TEST(term_scroll_mode_prompt_jumps);
    RUN_TEST(term_scroll_mode_prompt_jump_without_marks);
    RUN_TEST(term_memory_and_idle_compression);
END_TEST_SUITE()
