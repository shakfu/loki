/* cui_frontend.c - loki in a native window, drawn with CUI.
 *
 * The editor core is unchanged. A grid Renderer captures each refresh as
 * character cells, a CUI canvas paints the cells in a monospace font, and CUI
 * key events become loki keycodes fed to modal_process_event(). Per frame:
 * resize -> background work -> input -> refresh -> paint.
 *
 * Not supported here: interactive search ('/', Ctrl-F), which reads keys
 * from a terminal fd in its own loop (search.c).
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cui/cui.h>
#include <cui/cui_app.h>

#include "internal.h"
#include "renderer.h"
#include "buffers.h"
#include "choice.h"
#include "event.h"
#include "cui_frontend.h"

#define FONT_SIZE 14.0f
#define PAD 6.0f

/* ======================= Cell grid ======================================== */

enum {
    INK_HL = 0,        /* 0..8: HL_* index into view.colors */
    INK_GUTTER = 9,
    INK_PLAIN = 10,    /* status, message, tabs, REPL */
};

typedef struct {
    char ch[4];
    uint8_t len;       /* bytes in ch; 0 = blank */
    uint8_t ink;
    uint8_t inverse;
    uint8_t bold;
} cell_t;

typedef struct {
    int cols, rows;
    cell_t *cells;
    int row, col;            /* write position */
    int cur_row, cur_col;    /* cursor, 0-based */
} grid_t;

static grid_t g_grid;

static int grid_resize(grid_t *g, int rows, int cols) {
    cell_t *c = calloc((size_t)rows * (size_t)cols, sizeof *c);
    if (!c) return -1;
    free(g->cells);
    g->cells = c;
    g->rows = rows;
    g->cols = cols;
    return 0;
}

static void grid_newline(grid_t *g) { g->row++; g->col = 0; }

/* Write bytes at the write position, one cell per UTF-8 code point. */
static void grid_put(grid_t *g, const char *s, int len, int ink, int inverse, int bold) {
    if (g->row < 0 || g->row >= g->rows) return;
    int i = 0;
    while (i < len && g->col < g->cols) {
        unsigned char b = (unsigned char)s[i];
        int n = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 1;
        if (i + n > len) n = len - i;
        cell_t *c = &g->cells[g->row * g->cols + g->col];
        memcpy(c->ch, s + i, (size_t)n);
        c->len = (uint8_t)n;
        c->ink = (uint8_t)ink;
        c->inverse = (uint8_t)inverse;
        c->bold = (uint8_t)bold;
        g->col++;
        i += n;
    }
}

/* Fill the rest of the current row with styled blanks (bars). */
static void grid_pad(grid_t *g, int ink, int inverse) {
    while (g->row >= 0 && g->row < g->rows && g->col < g->cols) grid_put(g, " ", 1, ink, inverse, 0);
}

/* ======================= Grid renderer ==================================== */
/* One instance per buffer (editor_ctx_free destroys it); all share g_grid. */

static void gr_begin_frame(Renderer *r, int cols, int rows) {
    (void)r; (void)cols; (void)rows;
    memset(g_grid.cells, 0, (size_t)g_grid.rows * (size_t)g_grid.cols * sizeof(cell_t));
    g_grid.row = g_grid.col = 0;
}

static void gr_end_frame(Renderer *r) { (void)r; }

static void gr_render_tabs(Renderer *r, const char **tabs, int tab_count, int active_tab, int width) {
    (void)r; (void)width;
    for (int i = 0; i < tab_count; i++) {
        const char *t = tabs[i] ? tabs[i] : "???";
        int len = (int)strlen(t);
        if (len > 20) len = 20;
        grid_put(&g_grid, " ", 1, INK_PLAIN, 1, 0);
        grid_put(&g_grid, t, len, INK_PLAIN, 1, i == active_tab);
        grid_put(&g_grid, " ", 1, INK_PLAIN, 1, 0);
        if (i < tab_count - 1) grid_put(&g_grid, "|", 1, INK_PLAIN, 1, 0);
    }
    grid_pad(&g_grid, INK_PLAIN, 1);
    grid_newline(&g_grid);
}

static int hl_index(HighlightType t) {
    switch (t) {
        case HL_TYPE_COMMENT:  return HL_COMMENT;
        case HL_TYPE_KEYWORD1: return HL_KEYWORD1;
        case HL_TYPE_KEYWORD2: return HL_KEYWORD2;
        case HL_TYPE_STRING:   return HL_STRING;
        case HL_TYPE_NUMBER:   return HL_NUMBER;
        case HL_TYPE_MATCH:    return HL_MATCH;
        case HL_TYPE_NONPRINT: return HL_NONPRINT;
        default:               return HL_NORMAL;
    }
}

static void gr_render_row(Renderer *r, int row_num, const RenderSegment *segs, int seg_count,
                          int gutter_width, int is_empty) {
    (void)r;
    char buf[16];
    if (gutter_width > 0) {
        if (is_empty) snprintf(buf, sizeof buf, "%*s~", gutter_width - 1, "");
        else snprintf(buf, sizeof buf, "%*d ", gutter_width - 1, row_num);
        grid_put(&g_grid, buf, (int)strlen(buf), INK_GUTTER, 0, 0);
    } else if (is_empty) {
        grid_put(&g_grid, "~", 1, INK_GUTTER, 0, 0);
    }
    for (int i = 0; i < seg_count; i++) {
        const RenderSegment *s = &segs[i];
        if (s->hl_type == HL_TYPE_NONPRINT && s->len == 1) {
            char sym = (s->text[0] <= 26) ? (char)('@' + s->text[0]) : '?';
            grid_put(&g_grid, &sym, 1, HL_NORMAL, !s->selected, 0);
        } else {
            grid_put(&g_grid, s->text, s->len, hl_index(s->hl_type), s->selected, 0);
        }
    }
    grid_newline(&g_grid);
}

static void gr_render_status(Renderer *r, const StatusInfo *info, int width) {
    (void)r;
    char left[160], right[64];
    snprintf(left, sizeof left, " %s%s  %.20s - %d lines %s",
             info->lang ? info->lang : "", info->mode ? info->mode : "NORMAL",
             info->filename ? info->filename : "[No Name]", info->numrows,
             info->dirty ? "(modified)" : "");
    int rlen = snprintf(right, sizeof right, "%s%d/%d ", info->playing ? "[PLAYING] " : "",
                        info->current_row, info->numrows);
    grid_put(&g_grid, left, (int)strlen(left), INK_PLAIN, 1, 0);
    while (g_grid.col < width - rlen && g_grid.col < g_grid.cols) grid_put(&g_grid, " ", 1, INK_PLAIN, 1, 0);
    grid_put(&g_grid, right, rlen, INK_PLAIN, 1, 0);
    grid_pad(&g_grid, INK_PLAIN, 1);
    grid_newline(&g_grid);
}

static void gr_render_message(Renderer *r, const char *msg, int width) {
    (void)r; (void)width;
    if (msg) grid_put(&g_grid, msg, (int)strlen(msg), INK_PLAIN, 0, 0);
    grid_newline(&g_grid);
}

static void gr_render_repl(Renderer *r, const ReplInfo *info, int width) {
    (void)r;
    int start = info->log_count - info->max_display_lines;
    if (start < 0) start = 0;
    int shown = 0;
    for (int i = start; i < info->log_count; i++, shown++) {
        const char *line = info->log_lines[i] ? info->log_lines[i] : "";
        grid_put(&g_grid, line, (int)strlen(line), INK_PLAIN, 0, 0);
        grid_newline(&g_grid);
    }
    for (; shown < info->max_display_lines; shown++) grid_newline(&g_grid);
    int plen = info->prompt ? (int)strlen(info->prompt) : 0;
    if (plen) grid_put(&g_grid, info->prompt, plen, INK_PLAIN, 0, 1);
    int avail = width - plen;
    if (avail > 0 && info->input_len > 0)
        grid_put(&g_grid, info->input, info->input_len < avail ? info->input_len : avail, INK_PLAIN, 0, 0);
}

static void gr_set_cursor(Renderer *r, int row, int col) {
    (void)r;
    g_grid.cur_row = row - 1;
    g_grid.cur_col = col - 1;
}

static void gr_noop(Renderer *r) { (void)r; }

static cui_ctx *g_ui;   /* the window's context, for the clipboard */

static int gr_clipboard_copy(Renderer *r, const char *text, size_t len) {
    (void)r;
    char *s = malloc(len + 1);
    if (!s) return -1;
    memcpy(s, text, len);
    s[len] = '\0';
    int ok = g_ui && cui_clipboard_set(g_ui, s);
    free(s);
    return ok ? 0 : -1;
}

static void gr_destroy(Renderer *r) { free(r); }

static Renderer *grid_renderer_create(void) {
    Renderer *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->begin_frame = gr_begin_frame;
    r->end_frame = gr_end_frame;
    r->render_tabs = gr_render_tabs;
    r->render_row = gr_render_row;
    r->render_status = gr_render_status;
    r->render_message = gr_render_message;
    r->render_repl = gr_render_repl;
    r->set_cursor = gr_set_cursor;
    r->show_cursor = gr_noop;
    r->hide_cursor = gr_noop;
    r->clipboard_copy = gr_clipboard_copy;
    r->destroy = gr_destroy;
    return r;
}

/* ======================= Painting ========================================= */

typedef struct { float cw, ch; } cell_metrics;

static cui_color rgb(const t_hlcolor *c) {
    return (cui_color){ (uint8_t)c->r, (uint8_t)c->g, (uint8_t)c->b, 255 };
}

static cui_color ink_color(cui_ctx *ui, const editor_ctx_t *ctx, int ink) {
    if (ink == INK_GUTTER) return cui_color_of(ui, CUI_COLOR_TEXT_MUTED);
    if (ink == INK_PLAIN || ink == HL_NORMAL || !ctx) return cui_color_of(ui, CUI_COLOR_FOREGROUND);
    return rgb(&ctx->view.colors[ink]);
}

static void paint_grid(cui_ctx *ui, cui_rect rect, void *user) {
    const cell_metrics *m = user;
    const editor_ctx_t *ctx = buffer_get_current();
    cui_color bg = cui_color_of(ui, CUI_COLOR_BACKGROUND);
    cui_color fg = cui_color_of(ui, CUI_COLOR_FOREGROUND);
    cui_draw_rect(ui, rect, CUI_CORNERS(0), bg);

    char run[1024];
    for (int y = 0; y < g_grid.rows; y++) {
        float py = rect.y + PAD + (float)y * m->ch;
        int x = 0;
        while (x < g_grid.cols) {
            /* A run: consecutive cells with the same style. */
            const cell_t *c0 = &g_grid.cells[y * g_grid.cols + x];
            int x1 = x, n = 0;
            while (x1 < g_grid.cols) {
                const cell_t *c = &g_grid.cells[y * g_grid.cols + x1];
                if (c->ink != c0->ink || c->inverse != c0->inverse || c->bold != c0->bold) break;
                if (n + 4 >= (int)sizeof run) break;
                if (c->len) { memcpy(run + n, c->ch, c->len); n += c->len; }
                else run[n++] = ' ';
                x1++;
            }
            float px = rect.x + PAD + (float)x * m->cw;
            cui_color ink = ink_color(ui, ctx, c0->ink);
            if (c0->inverse) {
                cui_draw_rect(ui, (cui_rect){ px, py, (float)(x1 - x) * m->cw, m->ch }, CUI_CORNERS(0), ink);
                ink = bg;
            }
            cui_draw_text(ui, CUI_FONT_MONO, FONT_SIZE, (cui_vec2){ px, py }, ink, run, n);
            if (c0->bold)   /* no bold mono face: overdraw 0.6 pt right */
                cui_draw_text(ui, CUI_FONT_MONO, FONT_SIZE, (cui_vec2){ px + 0.6f, py }, ink, run, n);
            x = x1;
        }
    }

    /* Cursor: bar for text entry (INSERT, REPL), block otherwise. */
    if (g_grid.cur_row >= 0 && g_grid.cur_row < g_grid.rows && g_grid.cur_col >= 0 && g_grid.cur_col < g_grid.cols) {
        float px = rect.x + PAD + (float)g_grid.cur_col * m->cw;
        float py = rect.y + PAD + (float)g_grid.cur_row * m->ch;
        t_lua_repl *repl = ctx ? ctx_repl((editor_ctx_t *)ctx) : NULL;
        bool bar = ctx && (ctx->view.mode == MODE_INSERT || (repl && repl->active));
        if (bar) {
            cui_draw_rect(ui, (cui_rect){ px, py, 2, m->ch }, CUI_CORNERS(0), fg);
        } else {
            cui_draw_rect(ui, (cui_rect){ px, py, m->cw, m->ch }, CUI_CORNERS(0), cui_alpha(fg, 0.55f));
        }
    }
}

/* ======================= Input ============================================ */

/* One loki keycode into the current buffer, as modal_process_keypress does. */
static void feed_key(int c) {
    editor_ctx_t *ctx = buffer_get_current();
    if (!ctx) return;
    if (choice_handle_key(c)) return;
    if (c == CTRL_F) {
        t_lua_repl *repl = ctx_repl(ctx);
        if (!repl || !repl->active) {
            editor_set_status_msg(ctx, "Search is not available in loki-cui yet");
            return;
        }
    }
    EditorEvent ev = event_from_keycode(c);
    modal_process_event(ctx, &ev);
}

static void feed_bytes(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char b = (unsigned char)s[i];
        if (b == '\r') continue;
        feed_key(b == '\n' ? ENTER : b);
    }
}

/* CUI key with modifiers -> loki keycode, or 0 when the key is not handled
 * here (printable keys arrive as text events). */
static int map_key(cui_key k, int mods) {
    bool shift = mods & CUI_MOD_SHIFT;
    if ((mods & CUI_MOD_CTRL) && k >= CUI_KEY_A && k <= CUI_KEY_Z) return (int)(k - CUI_KEY_A) + 1;
    switch (k) {
        case CUI_KEY_ESCAPE:    return ESC;
        case CUI_KEY_ENTER:     return shift ? SHIFT_RETURN : ENTER;
        case CUI_KEY_TAB:       return TAB;
        case CUI_KEY_BACKSPACE: return BACKSPACE;
        case CUI_KEY_DELETE:    return DEL_KEY;
        case CUI_KEY_LEFT:      return shift ? SHIFT_ARROW_LEFT : ARROW_LEFT;
        case CUI_KEY_RIGHT:     return shift ? SHIFT_ARROW_RIGHT : ARROW_RIGHT;
        case CUI_KEY_UP:        return shift ? SHIFT_ARROW_UP : ARROW_UP;
        case CUI_KEY_DOWN:      return shift ? SHIFT_ARROW_DOWN : ARROW_DOWN;
        case CUI_KEY_HOME:      return HOME_KEY;
        case CUI_KEY_END:       return END_KEY;
        case CUI_KEY_PAGE_UP:   return PAGE_UP;
        case CUI_KEY_PAGE_DOWN: return PAGE_DOWN;
        default:                return 0;
    }
}

/* Paste only where text is entered; in NORMAL or VISUAL mode the pasted
 * bytes would run as commands. */
static void paste(cui_ctx *ui) {
    editor_ctx_t *ctx = buffer_get_current();
    if (!ctx) return;
    t_lua_repl *repl = ctx_repl(ctx);
    bool entering = ctx->view.mode == MODE_INSERT || ctx->view.mode == MODE_COMMAND || (repl && repl->active);
    if (!entering) {
        editor_set_status_msg(ctx, "Paste works in INSERT, COMMAND or the REPL");
        return;
    }
    const char *clip = cui_clipboard_get(ui);
    if (clip) feed_bytes(clip, strlen(clip));
}

static void handle_input(cui_ctx *ui) {
    int n = 0;
    const cui_key_event *ev = cui_key_events(ui, &n);
    for (int i = 0; i < n; i++) {
        if (ev[i].key == CUI_KEY_NONE) {
            feed_bytes(ev[i].text, strnlen(ev[i].text, sizeof ev[i].text));
            continue;
        }
#ifdef __APPLE__
        /* Command shortcuts: no text event follows them. */
        if (ev[i].mods & CUI_MOD_SUPER) {
            if (ev[i].key == CUI_KEY_V) paste(ui);
            else if (ev[i].key == CUI_KEY_S) feed_key(CTRL_S);
            else if (ev[i].key == CUI_KEY_Q) feed_key(CTRL_Q);
            continue;
        }
#else
        if ((ev[i].mods & CUI_MOD_CTRL) && (ev[i].mods & CUI_MOD_SHIFT) && ev[i].key == CUI_KEY_V) {
            paste(ui);
            continue;
        }
#endif
        int c = map_key(ev[i].key, ev[i].mods);
        if (c) feed_key(c);
    }
}

static void handle_click(const cui_canvas_result *res, const cell_metrics *m) {
    if (!res->clicked) return;
    editor_ctx_t *ctx = buffer_get_current();
    if (!ctx) return;
    EditorEvent ev = { .type = EVENT_MOUSE };
    ev.data.mouse.x = (int)((res->mouse.x - res->rect.x - PAD) / m->cw) + 1;
    ev.data.mouse.y = (int)((res->mouse.y - res->rect.y - PAD) / m->ch) + 1;
    ev.data.mouse.button = 0;
    ev.data.mouse.pressed = 1;
    modal_process_event(ctx, &ev);
}

/* ======================= Frame ============================================ */

static int any_dirty(void) {
    int ids[MAX_BUFFERS];
    int n = buffer_get_list(ids);
    for (int i = 0; i < n; i++) {
        editor_ctx_t *c = buffer_get(ids[i]);
        if (c && c->model.dirty) return 1;
    }
    return 0;
}

int loki_cui_any_dirty(void) { return any_dirty(); }

int loki_cui_frame(cui_ctx *ui, cui_vec2 size) {
    g_ui = ui;

    cui_text_metrics tm = cui_measure_text(ui, CUI_FONT_MONO, FONT_SIZE, "M", 1);
    cell_metrics m = { tm.width > 0 ? tm.width : 8.0f, tm.line_height > 0 ? tm.line_height : 17.0f };
    int cols = (int)((size.x - 2 * PAD) / m.cw);
    int rows = (int)((size.y - 2 * PAD) / m.ch);
    if (cols < 20) cols = 20;
    if (rows < STATUS_ROWS + LUA_REPL_TOTAL_ROWS + 2) rows = STATUS_ROWS + LUA_REPL_TOTAL_ROWS + 2;
    if (cols != g_grid.cols || rows != g_grid.rows) {
        if (grid_resize(&g_grid, rows, cols) != 0) return -1;
        editor_resize_buffers(rows, cols);
    }

    editor_ctx_t *ctx = buffer_get_current();
    if (!ctx) return -1;
    editor_run_background(ctx);
    handle_input(ui);

    ctx = buffer_get_current();   /* input may switch buffers */
    if (!ctx) return -1;
    if (!ctx->renderer) editor_ctx_set_renderer(ctx, grid_renderer_create());
    if (ctx->renderer) editor_refresh_screen(ctx);

    cui_canvas_result res = cui_canvas(ui, "loki-grid", CUI_FILL, CUI_FILL, paint_grid, &m, sizeof m);
    handle_click(&res, &m);

    /* Background work (timers, spawn, HTTP, status timeout) needs frames
     * without input; 50 ms keeps an idle window near zero CPU. */
    cui_request_wakeup(ui, 0.05f);
    return 0;
}

void loki_cui_shutdown(void) {
    free(g_grid.cells);
    g_grid = (grid_t){ 0 };
    g_ui = NULL;
}

int loki_cui_grid_size(int *rows, int *cols) {
    *rows = g_grid.rows;
    *cols = g_grid.cols;
    return g_grid.cells ? 0 : -1;
}

int loki_cui_grid_row(int row, char *buf, size_t cap) {
    if (!g_grid.cells || row < 0 || row >= g_grid.rows || cap == 0) return -1;
    size_t n = 0;
    for (int x = 0; x < g_grid.cols; x++) {
        const cell_t *c = &g_grid.cells[row * g_grid.cols + x];
        int len = c->len ? c->len : 1;
        if (n + (size_t)len >= cap) break;
        if (c->len) memcpy(buf + n, c->ch, c->len); else buf[n] = ' ';
        n += (size_t)len;
    }
    while (n > 0 && buf[n - 1] == ' ') n--;
    buf[n] = '\0';
    return 0;
}

void loki_cui_cursor(int *row, int *col) {
    *row = g_grid.cur_row;
    *col = g_grid.cur_col;
}
