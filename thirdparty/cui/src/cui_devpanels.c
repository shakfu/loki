/* More developer tool panels: the regex tester, the stack trace, the HTTP
 * request bar (with URL query helpers), feature flags and the API explorer.
 * Like the others they are built from the regular widgets and layout boxes,
 * so they follow the theme, the reading direction and the width they get. */
#include "cui_internal.h"

/* the colors of HTTP methods: GET blue, POST green, PUT orange, PATCH teal, DELETE red, others gray */
static cui_color cui__dp_method_color(cui_ctx *ui, const char *m) {
    if (!m) return CUI_C(ui, TEXT_MUTED);
    switch (cui__ascii_lower((unsigned char)m[0])) {
    case 'g': return cui__tone_color(ui, CUI_TONE_INFO);
    case 'p': return cui__ascii_lower((unsigned char)m[1]) == 'o' ? cui__tone_color(ui, CUI_TONE_SUCCESS)
                   : cui__ascii_lower((unsigned char)m[1]) == 'u' ? cui__tone_color(ui, CUI_TONE_WARNING) : cui__tone_color(ui, CUI_TONE_TEAL);
    case 'd': return CUI_C(ui, DANGER);
    default: return CUI_C(ui, TEXT_MUTED);
    }
}

/* scrolls a scroll area so a row that was laid out last frame is in view */
static void cui__dp_reveal(cui_ctx *ui, cui_id scroll_id, cui_id row_id) {
    if (!cui__has_prev_rect(ui, row_id) || !cui__has_prev_rect(ui, scroll_id)) return;
    cui_rect r = cui__prev_rect(ui, row_id), c = cui__prev_rect(ui, scroll_id);
    cui_scroll_state *ss = cui_state(ui, scroll_id, sizeof *ss);
    if (r.y < c.y) ss->ty -= c.y - r.y;
    else if (r.y + r.h > c.y + c.h) ss->ty += r.y + r.h - (c.y + c.h);
    if (ss->ty < 0) ss->ty = 0;
}

/* ======================================================================== */
/* Regex tester                                                             */
/* ======================================================================== */

#define CUI__RT_MAX 2000          /* matches kept */
#define CUI__RT_LIST 6            /* matches listed under the text */

typedef struct { int start, end, gs[CUI_REGEX_GROUPS], ge[CUI_REGEX_GROUPS]; } cui__rt_hit;

typedef struct {
    bool init;
    int own_flags;
    uint32_t key;               /* pattern, flags and text the results are for */
    int status;                 /* last search: 1 or 0 fine, -1 invalid, -2 too complex */
    const char *error;
    int groups;
    char names[CUI_REGEX_GROUPS][24];
    cui__rt_hit *hits; int nhits, hcap;
    cui_text_mark *marks; int nmarks, mcap;
    bool more;                  /* stopped at CUI__RT_MAX */
} cui__rt_state;

static void cui__rt_dtor(cui_ctx *ui, void *data) {
    cui__rt_state *s = data;
    cui__free(ui, s->hits); cui__free(ui, s->marks);
    s->hits = NULL; s->marks = NULL; s->nhits = s->hcap = s->nmarks = s->mcap = 0;
}

static int cui__rt_mark_cmp(const void *a, const void *b) {
    const cui_text_mark *x = a, *y = b;
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    return x->color < y->color ? -1 : x->color > y->color;   /* the match before its groups */
}

/* every match, once per change of the pattern, the flags or the text */
static void cui__rt_run(cui_ctx *ui, cui__rt_state *s, const char *pattern, int flags, const char *text) {
    int len = (int)strlen(text);
    uint32_t key = cui__hash_str(pattern, (uint32_t)flags * 31u + 0x5EEDu);
    key = cui__hash(text, (size_t)len, key);
    if (s->key == key && s->init) return;
    s->key = key; s->init = true;
    s->nhits = s->nmarks = 0; s->more = false; s->error = NULL; s->status = 0; s->groups = 0;
    if (!pattern[0]) return;
    int from = 0;
    while (from <= len) {
        cui_regex_match m;
        const char *err = NULL;
        int r = cui_regex_search(pattern, flags, text, len, from, &m, &err);
        if (r < 0) { s->status = r; s->error = err; s->nhits = s->nmarks = 0; return; }
        s->status = 1;
        if (r == 0) break;
        if (s->nhits >= CUI__RT_MAX) { s->more = true; break; }
        s->groups = m.groups;
        memcpy(s->names, m.group_name, sizeof s->names);
        CUI__GROW(ui, s->hits, s->nhits, s->hcap, 1);
        CUI__GROW(ui, s->marks, s->nmarks, s->mcap, 1 + m.groups);
        if (!s->hits || !s->marks) { s->nhits = s->nmarks = 0; return; }
        cui__rt_hit *h = &s->hits[s->nhits++];
        h->start = m.start; h->end = m.end;
        for (int g = 0; g < CUI_REGEX_GROUPS; g++) { h->gs[g] = g <= m.groups ? m.group_start[g] : -1; h->ge[g] = g <= m.groups ? m.group_end[g] : -1; }
        s->marks[s->nmarks++] = (cui_text_mark){ m.start, m.end, 0 };
        for (int g = 1; g <= m.groups; g++)
            if (m.group_start[g] >= 0 && m.group_end[g] > m.group_start[g]) s->marks[s->nmarks++] = (cui_text_mark){ m.group_start[g], m.group_end[g], 1 + (g - 1) % 6 };
        int next = m.end > m.start ? m.end : m.start + 1;   /* an empty match moves on one character */
        while (next < len && ((unsigned char)text[next] & 0xC0) == 0x80) next++;
        from = next;
    }
    if (s->nmarks > 1) qsort(s->marks, (size_t)s->nmarks, sizeof *s->marks, cui__rt_mark_cmp);
}

static cui_color cui__rt_color(cui_ctx *ui, int k) {
    cui_color base = k == 0 ? (ui->accent == CUI_ACCENT_NEUTRAL ? cui_oklch(0.6f, 0.13f, 250, 1) : CUI_C(ui, ACCENT))
                            : ui->theme.colors[CUI_TOKEN_SERIES_1 + (k - 1) % 6];
    return cui_alpha(base, ui->mode == CUI_THEME_DARK ? 0.34f : 0.26f);
}

/* a piece of the text as a chip in its mark color; line breaks and long runs shortened */
static void cui__rt_chip(cui_ctx *ui, const char *text, int a, int b, int color) {
    char buf[64];
    int n = 0;
    for (int i = a; i < b && n < 40; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n') { buf[n++] = '\\'; buf[n++] = 'n'; continue; }
        if (c == '\t') { buf[n++] = ' '; continue; }
        buf[n++] = (char)c;
    }
    while (n > 0 && ((unsigned char)buf[n - 1] & 0xC0) == 0x80) n--;   /* not half a character */
    if (n > 0 && ((unsigned char)buf[n - 1] & 0xC0) == 0xC0) n--;
    if (b - a > 40) { memcpy(buf + n, "\xe2\x80\xa6", 3); n += 3; }
    int box = cui__node_new(ui, 0);
    cui_node *bn = &ui->nodes[box];
    bn->bg = cui__rt_color(ui, color);
    bn->radius = CUI_CORNERS(4);
    bn->pad[0] = bn->pad[2] = 1; bn->pad[1] = bn->pad[3] = 5;
    bn->direction = CUI_DIRECTION_LTR;
    cui__open(ui, box);
    cui__text_node(ui, n ? buf : "(empty)", n ? n : -1, n ? CUI_FONT_MONO : CUI_FONT_BODY, cui__fs(ui, ui->theme.text_xs),
                   n ? CUI_C(ui, FOREGROUND) : CUI_C(ui, TEXT_MUTED), false);
    cui__close(ui);
}

static void cui__rt_list(cui_ctx *ui, const cui__rt_state *s, const char *text) {
    float fx = cui__fs(ui, ui->theme.text_xs);
    int shown = CUI_MIN(s->nhits, CUI__RT_LIST);
    for (int k = 0; k < shown; k++) {
        const cui__rt_hit *h = &s->hits[k];
        int row = cui__node_new(ui, 0);
        cui_node *rn = &ui->nodes[row];
        rn->dir = CUI_ROW; rn->gap = 6; rn->align = CUI_ALIGN_CENTER; rn->w = CUI_FILL; rn->flags |= CUI_NF_ROW_WRAP;
        cui__open(ui, row);
        int num = cui__text_node(ui, cui__frame_printf(ui, "%d", k + 1), -1, CUI_FONT_BODY, fx, CUI_C(ui, TEXT_MUTED), false);
        ui->nodes[num].w = CUI_PX(22); ui->nodes[num].flags |= CUI_NF_TABULAR;
        cui__rt_chip(ui, text, h->start, h->end, 0);
        int at = cui__text_node(ui, cui__frame_printf(ui, "%d\xe2\x80\x93%d", h->start, h->end), -1, CUI_FONT_BODY, fx, CUI_C(ui, TEXT_MUTED), false);
        ui->nodes[at].flags |= CUI_NF_TABULAR; ui->nodes[at].direction = CUI_DIRECTION_LTR;
        for (int g = 1; g <= s->groups && g < CUI_REGEX_GROUPS; g++) {
            const char *nm = s->names[g][0] ? s->names[g] : cui__frame_printf(ui, "%d", g);
            cui__text_node(ui, nm, -1, CUI_FONT_MEDIUM, fx, CUI_C(ui, TEXT_SECONDARY), false);
            if (h->gs[g] >= 0) cui__rt_chip(ui, text, h->gs[g], h->ge[g], 1 + (g - 1) % 6);
            else cui__text_node(ui, "\xe2\x80\x94", -1, CUI_FONT_BODY, fx, CUI_C(ui, TEXT_MUTED), false);
        }
        cui__close(ui);
    }
    if (s->nhits > shown)
        cui__text_node(ui, cui__frame_printf(ui, s->more ? "and more than %d others" : "and %d more", s->nhits - shown), -1, CUI_FONT_BODY, fx, CUI_C(ui, TEXT_MUTED), false);
}

cui_regex_tester_result cui_regex_tester_ex(cui_ctx *ui, const char *id_str, char *pattern, size_t pattern_cap, char *text, size_t text_cap,
                                            const cui_regex_tester_opts *o) {
    cui_regex_tester_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui_regex_tester_result res = { false, 0, NULL };
    if (!pattern || !text || pattern_cap == 0 || text_cap == 0) return res;
    cui_id id = cui_make_id(ui, id_str);
    cui__rt_state *s = cui_state(ui, id ^ 0x2E6Eu, sizeof *s);
    cui__state_set_dtor(ui, id ^ 0x2E6Eu, cui__rt_dtor);
    int *flags = o->flags ? o->flags : &s->own_flags;
    int col = cui__node_new(ui, id);
    cui_node *cn = &ui->nodes[col];
    cn->gap = 10;
    cn->w = o->width.kind ? o->width : CUI_FILL;
    cui__open(ui, col);
    cui_push_id(ui, id_str);
    /* the pattern and its flags; the results are worked out before the field shows its error */
    cui__rt_run(ui, s, pattern, *flags, text);
    int row = cui__node_new(ui, 0);
    cui_node *rn = &ui->nodes[row];
    rn->dir = CUI_ROW; rn->gap = 8; rn->w = CUI_FILL; rn->align = CUI_ALIGN_START; rn->flags |= CUI_NF_ROW_WRAP;
    cui__open(ui, row);
    cui_input_opts io;
    memset(&io, 0, sizeof io);
    io.width = CUI_FILL; io.prefix = "/"; io.suffix = "/"; io.placeholder = "Pattern";
    io.error = s->status < 0 ? s->error : NULL;
    cui_input_ex(ui, "pattern", pattern, pattern_cap, &io);
    ui->nodes[ui->last_node].min_w = 260;   /* on a phone the flags go under it */
    static const char *const flag_names[3] = { "i", "m", "s" };
    bool fv[3] = { (*flags & CUI_REGEX_ICASE) != 0, (*flags & CUI_REGEX_MULTILINE) != 0, (*flags & CUI_REGEX_DOTALL) != 0 };
    cui_toggle_group_opts go;
    memset(&go, 0, sizeof go);
    if (cui_toggle_group_ex(ui, "flags", flag_names, 3, fv, &go))
        *flags = (fv[0] ? CUI_REGEX_ICASE : 0) | (fv[1] ? CUI_REGEX_MULTILINE : 0) | (fv[2] ? CUI_REGEX_DOTALL : 0);
    cui__close(ui);
    cui__rt_run(ui, s, pattern, *flags, text);
    /* the text, with the matches marked */
    cui_code_editor_opts eo;
    memset(&eo, 0, sizeof eo);
    eo.height = o->height > 0 ? o->height : 180;
    eo.no_line_numbers = true;
    eo.lang = "text";
    eo.placeholder = "Text to test the pattern on";
    eo.marks = s->marks; eo.mark_count = s->nmarks;
    cui_code_editor_ex(ui, "text", text, text_cap, &eo);
    cui__rt_run(ui, s, pattern, *flags, text);
    /* how many, then each */
    float fx = cui__fs(ui, ui->theme.text_sm);
    const char *sum = s->status < 0 ? (s->status == -2 ? "Too complex to finish" : "Invalid pattern")
                    : !pattern[0] ? "Type a pattern" : s->nhits == 0 ? "No match"
                    : cui__frame_printf(ui, s->nhits == 1 ? "1 match" : s->more ? "%d+ matches" : "%d matches", s->nhits);
    cui__text_node(ui, sum, -1, CUI_FONT_MEDIUM, fx, s->status < 0 ? CUI_C(ui, DANGER) : CUI_C(ui, TEXT_SECONDARY), false);
    if (s->status >= 0 && s->nhits > 0) cui__rt_list(ui, s, text);
    cui_pop_id(ui);
    cui__close(ui);
    res.valid = s->status >= 0;
    res.matches = s->nhits;
    res.error = s->status < 0 ? s->error : NULL;
    ui->last_id = id;
    return res;
}

/* ======================================================================== */
/* Stack trace                                                              */
/* ======================================================================== */

#define CUI__STK_FRAMES 256        /* frames that remember being open */

typedef struct {
    bool init;
    uint32_t key;               /* the frames this state is for */
    uint8_t open[CUI__STK_FRAMES];       /* a frame's source shown */
    uint8_t lib_open[CUI__STK_FRAMES];   /* a run of library frames unfolded (by its first frame) */
} cui__stk_state;

typedef struct {
    const cui_stack_frame *fr;
    const char *lang;
    float size, lh, cw;
    cui_color colors[CUI__TK_COUNT];
    cui_color num, band;
} cui__stk_src_paint;

/* the lines around a frame: numbers, highlighted code, the frame's line marked */
static void cui__paint_stk_source(cui_ctx *ui, cui_node *n, void *data) {
    const cui__stk_src_paint *p = data;
    const cui_stack_frame *fr = p->fr;
    cui_rect r = n->rect;
    float fh = cui__font_line_height(ui, CUI_FONT_MONO, p->size);
    int last = fr->source_first + fr->source_count - 1;
    const char *ln = cui__frame_printf(ui, "%d", last);
    float gw = cui__text_width(ui, CUI_FONT_MONO, p->size, ln, -1, true) + 20;
    cui_push_clip(ui, r);
    bool block = false;
    for (int k = 0; k < fr->source_count; k++) {
        int line = fr->source_first + k;
        float y = r.y + 6 + k * p->lh;
        if (line == fr->line) {
            cui_draw_rect(ui, (cui_rect){ r.x, y, r.w, p->lh }, CUI_CORNERS(0), p->band);
            cui_draw_rect(ui, (cui_rect){ r.x, y, 2, p->lh }, CUI_CORNERS(0), CUI_C(ui, DANGER));
        }
        const char *num = cui__frame_printf(ui, "%d", line);
        float nw = cui__text_width(ui, CUI_FONT_MONO, p->size, num, -1, true);
        cui__draw_text_line(ui, CUI_FONT_MONO, p->size, r.x + gw - 10 - nw, y + (p->lh - fh) * 0.5f, line == fr->line ? CUI_C(ui, DANGER) : p->num, num, -1, true);
        const char *s = fr->source[k] ? fr->source[k] : "";
        int len = (int)strlen(s);
        float x = r.x + gw;
        cui__draw_code_line(ui, p->lang, s, CUI_MIN(len, 400), &block, x, y + (p->lh - fh) * 0.5f, p->size, p->colors, r.x + r.w);
    }
    cui_pop_clip(ui);
}

static void cui__stk_source(cui_ctx *ui, const cui_stack_frame *fr, const char *lang, bool narrow) {
    cui__stk_src_paint *p = cui__frame_alloc(ui, sizeof *p);
    memset(p, 0, sizeof *p);
    p->fr = fr; p->lang = lang;
    p->size = cui__fs(ui, narrow ? 11.5f : 12.5f);
    p->lh = roundf(p->size * 1.6f);
    cui__syntax_colors(ui, p->colors);
    p->num = cui_alpha(CUI_C(ui, TEXT_MUTED), 0.8f);
    p->band = cui_alpha(CUI_C(ui, DANGER), ui->mode == CUI_THEME_DARK ? 0.16f : 0.09f);
    int b = cui__node_new(ui, 0);
    cui_node *bn = &ui->nodes[b];
    bn->w = CUI_FILL; bn->h = CUI_PX(fr->source_count * p->lh + 12);
    bn->bg = CUI_C(ui, SURFACE_MUTED);
    bn->radius = CUI_CORNERS(6);
    bn->direction = CUI_DIRECTION_LTR;   /* code */
    bn->paint = cui__paint_stk_source; bn->paint_data = p;
}

/* the whole trace as text, for the copy button */
static const char *cui__stk_text(cui_ctx *ui, const cui_stack_frame *f, int n, const cui_stack_trace_opts *o) {
    size_t cap = 256;
    for (int i = 0; i < n; i++) cap += (f[i].function ? strlen(f[i].function) : 0) + (f[i].file ? strlen(f[i].file) : 0) + 40;
    char *buf = cui__frame_alloc(ui, cap);
    size_t len = (size_t)snprintf(buf, cap, "%s%s%s\n", o->title ? o->title : "", o->title && o->message ? ": " : "", o->message ? o->message : "");
    for (int i = 0; i < n && len < cap; i++) {
        len += (size_t)snprintf(buf + len, cap - len, "    at %s (%s", f[i].function ? f[i].function : "<anonymous>", f[i].file ? f[i].file : "unknown");
        if (f[i].line > 0 && len < cap) len += (size_t)snprintf(buf + len, cap - len, f[i].column > 0 ? ":%d:%d" : ":%d", f[i].line, f[i].column);
        if (len < cap) len += (size_t)snprintf(buf + len, cap - len, ")\n");
    }
    return buf;
}

static void cui__stk_frame_row(cui_ctx *ui, cui__stk_state *s, const cui_stack_frame *fr, int i, const char *lang, bool narrow, cui_stack_trace_result *res) {
    bool has_src = fr->source && fr->source_count > 0;
    cui_id rid = cui__hash(&i, sizeof i, cui_make_id(ui, "frame"));
    cui_interact ri = cui__interact(ui, rid, CUI_IF_FOCUSABLE);
    bool open = i < CUI__STK_FRAMES && s->open[i];
    if (ri.clicked || (ri.focused && (cui_key_pressed(ui, CUI_KEY_ENTER) || cui_key_pressed(ui, CUI_KEY_SPACE)))) {
        res->clicked = i;
        if (has_src && i < CUI__STK_FRAMES) s->open[i] = !s->open[i];
        open = i < CUI__STK_FRAMES && s->open[i];
    }
    int box = cui__node_new(ui, 0);
    ui->nodes[box].gap = 6; ui->nodes[box].w = CUI_FILL;
    ui->nodes[box].pad[0] = ui->nodes[box].pad[2] = 6; ui->nodes[box].pad[1] = ui->nodes[box].pad[3] = 10;
    cui__open(ui, box);
    int row = cui__node_new(ui, rid);
    cui_node *rn = &ui->nodes[row];
    rn->dir = CUI_ROW; rn->gap = 8; rn->align = CUI_ALIGN_CENTER; rn->w = CUI_FILL; rn->flags |= CUI_NF_ROW_WRAP;
    rn->radius = CUI_CORNERS(4);
    rn->bg = ri.hovered ? CUI_C(ui, HOVER) : cui__rgba(0, 0, 0, 0);
    cui__mark_interactive(ui, row, CUI_IF_FOCUSABLE);
    cui__focus_ring(ui, row, ri.focus_visible ? 1.0f : 0.0f);
    if (ri.hovered) cui__set_cursor(ui, CUI_CURSOR_HAND);
    cui__open(ui, row);
    cui__icon_node(ui, has_src ? (open ? CUI_ICON_CHEVRON_DOWN : CUI_ICON_CHEVRON_RIGHT) : CUI_ICON_MINUS, 14, has_src ? CUI_C(ui, TEXT_SECONDARY) : cui__rgba(0, 0, 0, 0));
    float fs = cui__fs(ui, ui->theme.text_sm), fx = cui__fs(ui, ui->theme.text_xs);
    cui_color fg = fr->external ? CUI_C(ui, TEXT_MUTED) : CUI_C(ui, FOREGROUND);
    int fn = cui__text_node(ui, fr->function && fr->function[0] ? fr->function : "<anonymous>", -1, CUI_FONT_MONO, fs, fg, false);
    ui->nodes[fn].direction = CUI_DIRECTION_LTR;
    const char *pos = fr->line > 0 ? (fr->column > 0 ? cui__frame_printf(ui, "%s:%d:%d", fr->file ? fr->file : "", fr->line, fr->column)
                                                      : cui__frame_printf(ui, "%s:%d", fr->file ? fr->file : "", fr->line))
                                   : (fr->file ? fr->file : "");
    if (narrow && fr->file) {   /* the file's name only */
        const char *b = pos;
        for (const char *c = pos; *c; c++) if (*c == '/' || *c == '\\') b = c + 1;
        pos = b;
    }
    int pt = cui__text_node(ui, pos, -1, CUI_FONT_MONO, fx, CUI_C(ui, TEXT_MUTED), false);
    ui->nodes[pt].direction = CUI_DIRECTION_LTR; ui->nodes[pt].flags |= CUI_NF_CLIP;
    cui__close(ui);
    if (open && has_src) cui__stk_source(ui, fr, lang, narrow);
    cui__close(ui);
}

cui_stack_trace_result cui_stack_trace_ex(cui_ctx *ui, const char *id_str, const cui_stack_frame *frames, int count, const cui_stack_trace_opts *o) {
    cui_stack_trace_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui_stack_trace_result res = { -1 };
    if (!frames || count < 0) count = 0;
    cui_id id = cui_make_id(ui, id_str);
    cui__stk_state *s = cui_state(ui, id ^ 0x57ACu, sizeof *s);
    uint32_t key = cui__hash(&count, sizeof count, 0x57u);
    for (int i = 0; i < count; i++) key = cui__hash_str(frames[i].function ? frames[i].function : "", key);
    if (!s->init || s->key != key) {   /* new trace: open the first frame of your own code that has source */
        memset(s, 0, sizeof *s);
        s->init = true; s->key = key;
        for (int i = 0; i < count && i < CUI__STK_FRAMES; i++)
            if (!frames[i].external && frames[i].source && frames[i].source_count > 0) { s->open[i] = 1; break; }
    }
    cui_rect pr = cui__prev_rect(ui, id);
    bool narrow = pr.w > 0 && pr.w < 480;
    int outer = cui__node_new(ui, id);
    cui_node *on = &ui->nodes[outer];
    on->w = o->width.kind ? o->width : CUI_FILL;
    on->bg = CUI_C(ui, SURFACE);
    on->border = 1; on->border_color = CUI_C(ui, BORDER);
    on->radius = CUI_CORNERS(ui->theme.radius_panel);
    on->flags |= CUI_NF_CLIP;
    cui__open(ui, outer);
    cui_push_id(ui, id_str);
    /* the error */
    int head = cui__node_new(ui, 0);
    cui_node *hn = &ui->nodes[head];
    hn->dir = CUI_ROW; hn->gap = 10; hn->w = CUI_FILL; hn->align = CUI_ALIGN_START;
    hn->pad[0] = hn->pad[2] = 12; hn->pad[1] = hn->pad[3] = 14;
    hn->bg = cui_mix(CUI_C(ui, SURFACE), CUI_C(ui, DANGER), ui->mode == CUI_THEME_DARK ? 0.10f : 0.05f);
    cui__open(ui, head);
    cui__icon_node(ui, CUI_ICON_CIRCLE_X, 18, CUI_C(ui, DANGER));
    int tx = cui__node_new(ui, 0);
    ui->nodes[tx].gap = 2; ui->nodes[tx].w = CUI_FILL;
    cui__open(ui, tx);
    if (o->title) cui__text_node(ui, o->title, -1, CUI_FONT_MEDIUM, cui__fs(ui, ui->theme.text_sm), CUI_C(ui, DANGER), false);
    if (o->message) {
        int m = cui__text_node(ui, o->message, -1, CUI_FONT_BODY, cui__fs(ui, ui->theme.text_sm), CUI_C(ui, FOREGROUND), true);
        ui->nodes[m].w = CUI_FILL;
    }
    cui__close(ui);
    cui_copy_button(ui, "copy", cui__stk_text(ui, frames, count, o));
    cui__close(ui);
    int line = cui__node_new(ui, 0);
    ui->nodes[line].w = CUI_FILL; ui->nodes[line].h = CUI_PX(1); ui->nodes[line].bg = CUI_C(ui, BORDER);
    /* the frames; runs of library frames fold */
    int list = cui__node_new(ui, 0);
    ui->nodes[list].w = CUI_FILL; ui->nodes[list].pad[0] = ui->nodes[list].pad[2] = 4;
    cui__open(ui, list);
    for (int i = 0; i < count;) {
        if (!frames[i].external) { cui__stk_frame_row(ui, s, &frames[i], i, o->lang, narrow, &res); i++; continue; }
        int j = i;
        while (j < count && frames[j].external) j++;
        bool lib_open = i < CUI__STK_FRAMES && s->lib_open[i];
        cui_id lid = cui__hash(&i, sizeof i, cui_make_id(ui, "lib"));
        cui_interact li = cui__interact(ui, lid, CUI_IF_FOCUSABLE);
        if ((li.clicked || (li.focused && (cui_key_pressed(ui, CUI_KEY_ENTER) || cui_key_pressed(ui, CUI_KEY_SPACE)))) && i < CUI__STK_FRAMES) {
            s->lib_open[i] = !s->lib_open[i]; lib_open = s->lib_open[i];
        }
        int row = cui__node_new(ui, lid);
        cui_node *rn = &ui->nodes[row];
        rn->dir = CUI_ROW; rn->gap = 8; rn->align = CUI_ALIGN_CENTER; rn->w = CUI_FILL;
        rn->pad[0] = rn->pad[2] = 6; rn->pad[1] = rn->pad[3] = 10;
        rn->bg = li.hovered ? CUI_C(ui, HOVER) : cui__rgba(0, 0, 0, 0);
        cui__mark_interactive(ui, row, CUI_IF_FOCUSABLE);
        cui__focus_ring(ui, row, li.focus_visible ? 1.0f : 0.0f);
        if (li.hovered) cui__set_cursor(ui, CUI_CURSOR_HAND);
        cui__open(ui, row);
        cui__icon_node(ui, lib_open ? CUI_ICON_CHEVRON_DOWN : CUI_ICON_CHEVRON_RIGHT, 14, CUI_C(ui, TEXT_MUTED));
        cui__text_node(ui, cui__frame_printf(ui, j - i == 1 ? "1 library frame" : "%d library frames", j - i), -1, CUI_FONT_BODY,
                       cui__fs(ui, ui->theme.text_xs), CUI_C(ui, TEXT_MUTED), false);
        cui__close(ui);
        if (lib_open) for (int k = i; k < j; k++) cui__stk_frame_row(ui, s, &frames[k], k, o->lang, narrow, &res);
        i = j;
    }
    if (count == 0) {
        int e = cui__text_node(ui, "No frames", -1, CUI_FONT_BODY, cui__fs(ui, ui->theme.text_sm), CUI_C(ui, TEXT_MUTED), false);
        ui->nodes[e].pad[0] = 8; ui->nodes[e].pad[3] = 14;
    }
    cui__close(ui);
    cui_pop_id(ui);
    cui__close(ui);
    ui->last_id = id;
    return res;
}

/* ======================================================================== */
/* HTTP request bar                                                         */
/* ======================================================================== */

static const char *const cui__http_methods[7] = { "GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", "OPTIONS" };

const char *cui_http_method_name(int method) { return method >= 0 && method < 7 ? cui__http_methods[method] : ""; }

const char *cui_http_reason(int status) {
    static const struct { short code; const char *text; } reasons[] = {
        { 100, "Continue" }, { 101, "Switching Protocols" }, { 200, "OK" }, { 201, "Created" }, { 202, "Accepted" }, { 204, "No Content" },
        { 206, "Partial Content" }, { 301, "Moved Permanently" }, { 302, "Found" }, { 303, "See Other" }, { 304, "Not Modified" },
        { 307, "Temporary Redirect" }, { 308, "Permanent Redirect" }, { 400, "Bad Request" }, { 401, "Unauthorized" }, { 403, "Forbidden" },
        { 404, "Not Found" }, { 405, "Method Not Allowed" }, { 406, "Not Acceptable" }, { 408, "Request Timeout" }, { 409, "Conflict" },
        { 410, "Gone" }, { 412, "Precondition Failed" }, { 413, "Content Too Large" }, { 415, "Unsupported Media Type" },
        { 418, "I'm a teapot" }, { 422, "Unprocessable Content" }, { 429, "Too Many Requests" }, { 500, "Internal Server Error" },
        { 501, "Not Implemented" }, { 502, "Bad Gateway" }, { 503, "Service Unavailable" }, { 504, "Gateway Timeout" },
    };
    for (size_t i = 0; i < sizeof reasons / sizeof reasons[0]; i++) if (reasons[i].code == status) return reasons[i].text;
    return "";
}

cui_request_bar_result cui_request_bar_ex(cui_ctx *ui, const char *id_str, int *method, char *url, size_t cap, const cui_request_bar_opts *o) {
    cui_request_bar_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui_request_bar_result res = { false, false, false };
    if (!url || cap == 0) return res;
    int own = 0;
    if (!method) method = &own;
    if (*method < 0 || *method > 6) *method = 0;
    cui_id id = cui_make_id(ui, id_str);
    cui_rect pr = cui__prev_rect(ui, id);
    bool narrow = pr.w > 0 && pr.w < 420;
    int row = cui__node_new(ui, id);
    cui_node *rn = &ui->nodes[row];
    rn->dir = CUI_ROW; rn->gap = 8; rn->align = CUI_ALIGN_CENTER;
    rn->w = o->width.kind ? o->width : CUI_FILL;
    cui__open(ui, row);
    cui_push_id(ui, id_str);
    cui_select_opts so;
    memset(&so, 0, sizeof so);
    so.width = CUI_PX(narrow ? 96 : 112);
    if (cui_select_ex(ui, "method", cui__http_methods, 7, method, &so)) res.changed = true;
    cui_input_opts io;
    memset(&io, 0, sizeof io);
    io.width = CUI_FILL;
    io.placeholder = o->placeholder ? o->placeholder : "https://api.example.com/v1/items";
    int r = cui_input_ex(ui, "url", url, cap, &io);
    ui->nodes[ui->last_node].direction = CUI_DIRECTION_LTR;   /* a URL reads left to right */
    if (r & CUI_CHANGED) res.changed = true;
    if ((r & CUI_SUBMITTED) && !o->loading && url[0]) res.send = true;
    cui_button_opts bo;
    memset(&bo, 0, sizeof bo);
    bo.variant = o->loading ? CUI_BUTTON_OUTLINE : CUI_BUTTON_PRIMARY;
    bo.icon = o->loading ? CUI_ICON_X : CUI_ICON_SEND;
    bo.disabled = !o->loading && !url[0];
    bool pressed = narrow ? cui_icon_button_ex(ui, bo.icon, &bo) : cui_button_ex(ui, o->loading ? "Cancel" : "Send", &bo);
    if (pressed) { if (o->loading) res.cancel = true; else res.send = true; }
    cui_pop_id(ui);
    cui__close(ui);
    ui->last_id = id;
    return res;
}

void cui_response_status(cui_ctx *ui, int status, double seconds, double bytes) {
    int row = cui__node_new(ui, 0);
    cui_node *rn = &ui->nodes[row];
    rn->dir = CUI_ROW; rn->gap = 10; rn->align = CUI_ALIGN_CENTER; rn->flags |= CUI_NF_ROW_WRAP;
    cui__open(ui, row);
    cui_tone tone = status <= 0 ? CUI_TONE_NEUTRAL : status < 300 ? CUI_TONE_SUCCESS : status < 400 ? CUI_TONE_INFO : status < 500 ? CUI_TONE_WARNING : CUI_TONE_DANGER;
    cui_badge_opts bo;
    memset(&bo, 0, sizeof bo);
    bo.tone = tone; bo.dot = true;
    const char *reason = cui_http_reason(status);
    cui_badge_ex(ui, status <= 0 ? "No response" : reason[0] ? cui__frame_printf(ui, "%d %s", status, reason) : cui__frame_printf(ui, "%d", status), &bo);
    float fx = cui__fs(ui, ui->theme.text_xs);
    if (seconds >= 0) {
        int t = cui__text_node(ui, seconds < 1 ? cui__frame_printf(ui, "%.0f ms", seconds * 1000) : cui__frame_printf(ui, "%.2f s", seconds), -1,
                               CUI_FONT_BODY, fx, CUI_C(ui, TEXT_SECONDARY), false);
        ui->nodes[t].flags |= CUI_NF_TABULAR;
    }
    if (bytes >= 0) cui__text_node(ui, cui__size_text(ui, bytes), -1, CUI_FONT_BODY, fx, CUI_C(ui, TEXT_SECONDARY), false);
    cui__close(ui);
}

/* percent-decoding and encoding of query parts */
static int cui__dp_hex(int c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; }
static int cui__url_decode(const char *s, int n, char *out, int cap) {
    int o = 0;
    for (int i = 0; i < n && o < cap - 1; i++) {
        int c = (unsigned char)s[i];
        if (c == '+') c = ' ';
        else if (c == '%' && i + 2 < n) {
            int h = cui__dp_hex((unsigned char)s[i + 1]), l = cui__dp_hex((unsigned char)s[i + 2]);
            if (h >= 0 && l >= 0) { c = h * 16 + l; i += 2; }
        }
        out[o++] = (char)c;
    }
    out[o] = 0;
    return o;
}
static bool cui__url_encode(const char *s, char *out, size_t cap, size_t *len) {
    static const char hex[] = "0123456789ABCDEF";
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~' ||
                    c == '{' || c == '}' || c == ':' || c == '/' || c == ',' || c == '@';
        if (*len + 4 > cap) return false;
        if (keep) out[(*len)++] = (char)c;
        else { out[(*len)++] = '%'; out[(*len)++] = hex[c >> 4]; out[(*len)++] = hex[c & 15]; }
    }
    out[*len] = 0;
    return true;
}

int cui_url_params(const char *url, cui_kv_pair *pairs, int cap) {
    if (!url || !pairs || cap <= 0) return 0;
    const char *q = strchr(url, '?');
    if (!q) return 0;
    q++;
    const char *end = strchr(q, '#');
    if (!end) end = q + strlen(q);
    int n = 0;
    while (q < end && n < cap) {
        const char *amp = q;
        while (amp < end && *amp != '&') amp++;
        const char *eq = q;
        while (eq < amp && *eq != '=') eq++;
        if (amp > q) {
            cui_kv_pair *p = &pairs[n++];
            memset(p, 0, sizeof *p);
            cui__url_decode(q, (int)(eq - q), p->key, (int)sizeof p->key);
            if (eq < amp) cui__url_decode(eq + 1, (int)(amp - eq - 1), p->value, (int)sizeof p->value);
            p->enabled = true;
        }
        q = amp < end ? amp + 1 : end;
    }
    return n;
}

bool cui_url_set_params(char *url, size_t cap, const cui_kv_pair *pairs, int count) {
    if (!url || cap == 0) return false;
    size_t ul = strlen(url);
    const char *q = strchr(url, '?'), *hash = strchr(url, '#');
    size_t base = q ? (size_t)(q - url) : hash ? (size_t)(hash - url) : ul;
    char frag[256] = "";
    if (hash) snprintf(frag, sizeof frag, "%s", hash);
    char *out = malloc(cap);
    if (!out) return false;
    memcpy(out, url, base);
    size_t len = base;
    bool first = true, ok = true;
    for (int i = 0; i < count && ok; i++) {
        if (!pairs[i].enabled || !pairs[i].key[0]) continue;
        if (len + 2 > cap) { ok = false; break; }
        out[len++] = first ? '?' : '&';
        first = false;
        ok = cui__url_encode(pairs[i].key, out, cap, &len);
        if (ok && pairs[i].value[0]) {
            if (len + 2 > cap) { ok = false; break; }
            out[len++] = '=';
            ok = cui__url_encode(pairs[i].value, out, cap, &len);
        }
    }
    size_t fl = strlen(frag);
    if (ok && len + fl + 1 <= cap) { memcpy(out + len, frag, fl + 1); len += fl; memcpy(url, out, len + 1); }
    else ok = false;
    free(out);
    return ok;
}

/* ======================================================================== */
/* Feature flags                                                            */
/* ======================================================================== */

#define CUI__FF_NARROW 560.0f
#define CUI__FF_COL    128.0f

typedef struct { bool init; char query[96]; int env; float rollout; int pop_flag, pop_env; } cui__ff_state;

static int cui__ff_share(const cui_feature_flag *f, int e) { int r = f->rollout[e]; return r <= 0 || r >= 100 ? 100 : r; }

/* one environment's cell: the switch, and the share of users as a button that opens the slider */
static void cui__ff_cell(cui_ctx *ui, cui__ff_state *s, cui_feature_flag *fl, int i, int e, cui_feature_flags_result *res, bool narrow) {
    cui_push_id_int(ui, e);
    int cell = cui__node_new(ui, 0);
    cui_node *cn = &ui->nodes[cell];
    cn->dir = CUI_ROW; cn->gap = 6; cn->align = CUI_ALIGN_CENTER;
    cn->w = narrow ? CUI_FIT : CUI_PX(CUI__FF_COL);
    cui__open(ui, cell);
    cui_check_opts co;
    memset(&co, 0, sizeof co);
    if (cui_switch_ex(ui, "##on", &fl->on[e], &co)) { res->flag = i; res->env = e; }
    if (fl->on[e]) {
        int share = cui__ff_share(fl, e);
        cui_button_opts bo;
        memset(&bo, 0, sizeof bo);
        bo.size = CUI_SIZE_SM; bo.variant = CUI_BUTTON_GHOST; bo.tooltip = "Share of users";
        if (share < 100) bo.color = cui__tone_color(ui, CUI_TONE_WARNING);
        if (cui_button_ex(ui, cui__frame_printf(ui, "%d%%", share), &bo)) {
            s->pop_flag = i; s->pop_env = e; s->rollout = (float)share;
            cui_popover_open(ui, "share");
        }
        cui_popover_opts po;
        memset(&po, 0, sizeof po);
        po.width = 240; po.placement = CUI_PLACE_BOTTOM;
        if (cui_popover_begin(ui, "share", &po)) {
            cui_column(ui, .gap = 10, .width = CUI_FILL) {
                cui_text(ui, cui__frame_printf(ui, "%s users", fl->key ? fl->key : ""), .font = CUI_FONT_MEDIUM, .size = 13);
                cui_slider_opts sl;
                memset(&sl, 0, sizeof sl);
                sl.step = 1; sl.show_value = true; sl.format = "%.0f%%";
                if (cui_slider_ex(ui, "##share", &s->rollout, 1, 100, &sl)) { fl->rollout[e] = (int)(s->rollout + 0.5f); res->flag = i; res->env = e; }
                static const int presets[5] = { 1, 5, 25, 50, 100 };
                cui_row(ui, .gap = 6, .wrap = true) {
                    for (int k = 0; k < 5; k++) {
                        cui_push_id_int(ui, k);
                        cui_button_opts pb;
                        memset(&pb, 0, sizeof pb);
                        pb.size = CUI_SIZE_SM; pb.variant = CUI_BUTTON_OUTLINE; pb.selected = share == presets[k];
                        if (cui_button_ex(ui, cui__frame_printf(ui, "%d%%", presets[k]), &pb)) {
                            fl->rollout[e] = presets[k]; s->rollout = (float)presets[k]; res->flag = i; res->env = e;
                        }
                        cui_pop_id(ui);
                    }
                }
            }
            cui_popover_end(ui);
        }
    }
    cui__close(ui);
    cui_pop_id(ui);
}

cui_feature_flags_result cui_feature_flags_ex(cui_ctx *ui, const char *id_str, cui_feature_flag *flags, int count,
                                              const char *const *envs, int env_count, const cui_feature_flags_opts *o) {
    cui_feature_flags_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui_feature_flags_result res = { -1, -1 };
    if (!flags || count < 0) count = 0;
    env_count = envs ? CUI_CLAMP(env_count, 0, CUI_FLAG_ENVS) : 0;
    cui_id id = cui_make_id(ui, id_str);
    cui__ff_state *s = cui_state(ui, id ^ 0xF1A6u, sizeof *s);
    if (!s->init) { s->init = true; s->pop_flag = -1; }
    if (s->env >= env_count) s->env = 0;
    cui_rect pr = cui__prev_rect(ui, id);
    bool narrow = pr.w > 0 && (pr.w < CUI__FF_NARROW || pr.w < 260 + env_count * CUI__FF_COL);
    int outer = cui__node_new(ui, id);
    cui_node *on = &ui->nodes[outer];
    on->gap = 8; on->w = o->width.kind ? o->width : CUI_FILL;
    cui__open(ui, outer);
    cui_push_id(ui, id_str);
    /* filter, and on a phone the environment */
    int bar = cui__node_new(ui, 0);
    ui->nodes[bar].dir = CUI_ROW; ui->nodes[bar].gap = 8; ui->nodes[bar].w = CUI_FILL; ui->nodes[bar].align = CUI_ALIGN_CENTER;
    ui->nodes[bar].flags |= CUI_NF_ROW_WRAP;
    cui__open(ui, bar);
    cui_input_opts io;
    memset(&io, 0, sizeof io);
    io.size = CUI_SIZE_SM; io.width = CUI_FILL; io.placeholder = "Filter flags"; io.clearable = true;
    cui_search_ex(ui, "filter", s->query, sizeof s->query, &io);
    ui->nodes[ui->last_node].min_w = 140;
    if (narrow && env_count > 1) {
        cui_segmented_opts so;
        memset(&so, 0, sizeof so);
        so.size = CUI_SIZE_SM;
        cui_segmented_ex(ui, "env", (const char *const *)envs, env_count, &s->env, &so);
    }
    cui__close(ui);
    /* the table */
    int frame = cui__node_new(ui, 0);
    cui_node *fn = &ui->nodes[frame];
    fn->w = CUI_FILL;
    fn->radius = CUI_CORNERS(ui->theme.radius_control);
    fn->border = 1; fn->border_color = CUI_C(ui, BORDER);
    fn->bg = CUI_C(ui, SURFACE);
    fn->flags |= CUI_NF_CLIP;
    if (o->height > 0) fn->h = CUI_PX(o->height);
    cui__open(ui, frame);
    float fs = cui__fs(ui, ui->theme.text_sm), fx = cui__fs(ui, ui->theme.text_xs);
    if (!narrow && env_count > 0) {
        int h = cui__node_new(ui, 0);
        cui_node *hn = &ui->nodes[h];
        hn->dir = CUI_ROW; hn->gap = 12; hn->w = CUI_FILL; hn->align = CUI_ALIGN_CENTER;
        hn->pad[0] = hn->pad[2] = 7; hn->pad[1] = hn->pad[3] = 12;
        hn->bg = CUI_C(ui, SURFACE_MUTED);
        cui__open(ui, h);
        int t = cui__text_node(ui, "Flag", -1, CUI_FONT_MEDIUM, fx, CUI_C(ui, TEXT_MUTED), false);
        ui->nodes[t].w = CUI_FILL;
        for (int e = 0; e < env_count; e++) {
            int et = cui__text_node(ui, envs[e] ? envs[e] : "", -1, CUI_FONT_MEDIUM, fx, CUI_C(ui, TEXT_MUTED), false);
            ui->nodes[et].w = CUI_PX(CUI__FF_COL); ui->nodes[et].flags |= CUI_NF_CLIP;
        }
        cui__close(ui);
    }
    cui_scroll_opts sc;
    memset(&sc, 0, sizeof sc);
    sc.id = "rows"; sc.width = CUI_FILL; sc.height = CUI_FILL;
    bool scrolls = o->height > 0;
    if (scrolls) cui_scroll_begin(ui, &sc);
    int shown = 0;
    for (int i = 0; i < count; i++) {
        cui_feature_flag *fl = &flags[i];
        if (s->query[0] && !cui__contains_ci(fl->key ? fl->key : "", s->query) && !cui__contains_ci(fl->description ? fl->description : "", s->query)) continue;
        if (shown++ > 0) {
            int sep = cui__node_new(ui, 0);
            ui->nodes[sep].w = CUI_FILL; ui->nodes[sep].h = CUI_PX(1); ui->nodes[sep].bg = CUI_C(ui, BORDER_SUBTLE);
        }
        cui_push_id_int(ui, i);
        int row = cui__node_new(ui, 0);
        cui_node *rn = &ui->nodes[row];
        rn->dir = CUI_ROW; rn->gap = 12; rn->w = CUI_FILL; rn->align = CUI_ALIGN_CENTER;
        rn->pad[0] = rn->pad[2] = 8; rn->pad[1] = rn->pad[3] = 12;
        cui__open(ui, row);
        int txt = cui__node_new(ui, 0);
        ui->nodes[txt].gap = 2; ui->nodes[txt].w = CUI_FILL;
        cui__open(ui, txt);
        int k = cui__text_node(ui, fl->key ? fl->key : "", -1, CUI_FONT_MONO, fs, CUI_C(ui, FOREGROUND), false);
        ui->nodes[k].flags |= CUI_NF_CLIP; ui->nodes[k].w = CUI_FILL;
        if (fl->description && fl->description[0]) {
            int dn = cui__text_node(ui, fl->description, -1, CUI_FONT_BODY, fx, CUI_C(ui, TEXT_MUTED), true);
            ui->nodes[dn].w = CUI_FILL;
        }
        cui__close(ui);
        if (narrow) { if (env_count > 0) cui__ff_cell(ui, s, fl, i, s->env, &res, true); }
        else for (int e = 0; e < env_count; e++) cui__ff_cell(ui, s, fl, i, e, &res, false);
        cui__close(ui);
        cui_pop_id(ui);
    }
    if (shown == 0) {
        int e = cui__text_node(ui, count == 0 ? "No flags" : "No flags match", -1, CUI_FONT_BODY, fs, CUI_C(ui, TEXT_MUTED), false);
        ui->nodes[e].pad[0] = ui->nodes[e].pad[2] = 14; ui->nodes[e].pad[3] = 12;
    }
    if (scrolls) cui_scroll_end(ui);
    cui__close(ui);
    cui_pop_id(ui);
    cui__close(ui);
    ui->last_id = id;
    return res;
}

/* ======================================================================== */
/* API explorer                                                             */
/* ======================================================================== */

#define CUI__AX_ITEMS 512          /* endpoints that remember being open */

typedef struct {
    bool init;
    char query[96];
    int own_sel;
    uint8_t open[CUI__AX_ITEMS];
    uint32_t folded[32]; int nfolded;
    bool scroll_to_sel;
} cui__ax_state;

static const char *cui__ax_tag(const cui_api_endpoint *e) { return e->tag && e->tag[0] ? e->tag : "Other"; }

/* the method as a fixed-width colored tag */
static void cui__ax_method(cui_ctx *ui, const char *m, bool narrow) {
    cui_color c = cui__dp_method_color(ui, m);
    int b = cui__node_new(ui, 0);
    cui_node *bn = &ui->nodes[b];
    bn->w = CUI_PX(narrow ? 52 : 62); bn->align = CUI_ALIGN_CENTER; bn->justify = CUI_JUSTIFY_CENTER;
    bn->pad[0] = bn->pad[2] = 2;
    bn->radius = CUI_CORNERS(4);
    bn->bg = cui_alpha(c, ui->mode == CUI_THEME_DARK ? 0.18f : 0.12f);
    cui__open(ui, b);
    char up[12];
    int n = 0;
    for (const char *p = m ? m : "?"; *p && n < 7; p++) up[n++] = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
    up[n] = 0;
    cui__text_node(ui, up, n, CUI_FONT_MONO, cui__fs(ui, narrow ? 10.5f : 11.0f), cui__legible(c, CUI_C(ui, SURFACE), 4.5f), false);
    cui__close(ui);
}

/* the path, its {parameters} in the accent color */
typedef struct { const char *path; float size; cui_color fg, param; bool strike; } cui__ax_path_paint;
static void cui__paint_ax_path(cui_ctx *ui, cui_node *n, void *data) {
    const cui__ax_path_paint *p = data;
    cui_rect r = n->rect;
    float fh = cui__font_line_height(ui, CUI_FONT_MONO, p->size), y = r.y + (r.h - fh) * 0.5f, x = r.x;
    const char *s = p->path;
    cui_push_clip(ui, r);
    while (*s) {
        const char *e = s;
        bool param = *s == '{';
        if (param) { while (*e && *e != '}') e++; if (*e) e++; }
        else while (*e && *e != '{') e++;
        cui__draw_text_line(ui, CUI_FONT_MONO, p->size, x, y, param ? p->param : p->fg, s, (int)(e - s), true);
        x += cui__text_width(ui, CUI_FONT_MONO, p->size, s, (int)(e - s), true);
        s = e;
    }
    if (p->strike) cui_draw_rect(ui, (cui_rect){ r.x, y + fh * 0.55f, x - r.x, 1 }, CUI_CORNERS(0), p->fg);
    cui_pop_clip(ui);
}

static void cui__ax_params(cui_ctx *ui, const cui_api_endpoint *ep, int i, bool narrow, cui_api_explorer_result *res) {
    int box = cui__node_new(ui, 0);
    cui_node *bn = &ui->nodes[box];
    bn->gap = 8; bn->w = CUI_FILL;
    bn->pad[0] = 4; bn->pad[2] = 12; bn->pad[1] = 12; bn->pad[3] = narrow ? 12 : 82;
    cui__open(ui, box);
    float fs = cui__fs(ui, ui->theme.text_sm), fx = cui__fs(ui, ui->theme.text_xs);
    if (ep->param_count <= 0 || !ep->params)
        cui__text_node(ui, "No parameters", -1, CUI_FONT_BODY, fx, CUI_C(ui, TEXT_MUTED), false);
    for (int k = 0; k < ep->param_count && ep->params; k++) {
        const cui_api_param *pa = &ep->params[k];
        int row = cui__node_new(ui, 0);
        cui_node *rn = &ui->nodes[row];
        rn->dir = CUI_ROW; rn->gap = 10; rn->w = CUI_FILL; rn->align = CUI_ALIGN_START; rn->flags |= CUI_NF_ROW_WRAP;
        cui__open(ui, row);
        int nm = cui__node_new(ui, 0);
        ui->nodes[nm].dir = CUI_ROW; ui->nodes[nm].gap = 2; ui->nodes[nm].w = CUI_PX(narrow ? 110 : 150); ui->nodes[nm].direction = CUI_DIRECTION_LTR;
        cui__open(ui, nm);
        int t = cui__text_node(ui, pa->name ? pa->name : "", -1, CUI_FONT_MONO, fs, CUI_C(ui, FOREGROUND), false);
        ui->nodes[t].flags |= CUI_NF_CLIP;
        if (pa->required) cui__text_node(ui, "*", 1, CUI_FONT_MEDIUM, fs, CUI_C(ui, DANGER), false);
        cui__close(ui);
        const char *meta = pa->in && pa->type ? cui__frame_printf(ui, "%s \xc2\xb7 %s", pa->type, pa->in) : pa->type ? pa->type : pa->in ? pa->in : "";
        int mt = cui__text_node(ui, meta, -1, CUI_FONT_BODY, fx, CUI_C(ui, TEXT_MUTED), false);
        ui->nodes[mt].w = CUI_PX(110);
        if (pa->description) {
            int dn = cui__text_node(ui, pa->description, -1, CUI_FONT_BODY, fx, CUI_C(ui, TEXT_SECONDARY), true);
            ui->nodes[dn].w = CUI_FILL; ui->nodes[dn].min_w = 160;
        }
        cui__close(ui);
    }
    cui_button_opts bo;
    memset(&bo, 0, sizeof bo);
    bo.size = CUI_SIZE_SM; bo.icon = CUI_ICON_PLAY;
    if (cui_button_ex(ui, "Try it", &bo)) res->try_it = i;
    cui__close(ui);
}

cui_api_explorer_result cui_api_explorer_ex(cui_ctx *ui, const char *id_str, const cui_api_endpoint *eps, int count, const cui_api_explorer_opts *o) {
    cui_api_explorer_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui_api_explorer_result res = { -1, -1 };
    if (!eps || count < 0) count = 0;
    cui_id id = cui_make_id(ui, id_str);
    cui__ax_state *s = cui_state(ui, id ^ 0xA91Eu, sizeof *s);
    if (!s->init) { s->init = true; s->own_sel = -1; }
    int *sel = o->selected ? o->selected : &s->own_sel;
    if (*sel < -1 || *sel >= count) *sel = -1;
    cui_rect pr = cui__prev_rect(ui, id);
    bool narrow = pr.w > 0 && pr.w < 520;
    cui_id lid = id ^ 0xA91Fu;
    int outer = cui__node_new(ui, id);
    cui_node *on = &ui->nodes[outer];
    on->gap = 8; on->w = o->width.kind ? o->width : CUI_FILL;
    if (o->height != 0) on->h = o->height < 0 ? CUI_FILL : CUI_PX(o->height);
    cui__open(ui, outer);
    cui_push_id(ui, id_str);
    cui_input_opts io;
    memset(&io, 0, sizeof io);
    io.size = CUI_SIZE_SM; io.width = CUI_FILL; io.placeholder = "Filter endpoints"; io.clearable = true;
    cui_search_ex(ui, "filter", s->query, sizeof s->query, &io);
    /* the endpoints that pass the filter, in tag order of first appearance */
    size_t nn = (size_t)CUI_MAX(1, count);
    int *order = cui__frame_alloc(ui, sizeof(int) * nn), no = 0;
    bool *done = cui__frame_alloc(ui, sizeof(bool) * nn);
    memset(done, 0, sizeof(bool) * nn);
    for (int i = 0; i < count; i++) {
        if (done[i]) continue;
        const char *tag = cui__ax_tag(&eps[i]);
        for (int j = i; j < count; j++) {
            if (done[j] || strcmp(cui__ax_tag(&eps[j]), tag)) continue;
            done[j] = true;
            if (s->query[0] && !cui__contains_ci(eps[j].path ? eps[j].path : "", s->query) && !cui__contains_ci(eps[j].summary ? eps[j].summary : "", s->query)) continue;
            order[no++] = j;
        }
    }
    /* keys move through the endpoints in view */
    cui_interact lr = cui__interact(ui, lid, CUI_IF_FOCUSABLE);
    if (lr.focused && !ui->edit_prev && no > 0) {
        int at = -1;
        for (int k = 0; k < no; k++) if (order[k] == *sel) at = k;
        if (cui_key_pressed(ui, CUI_KEY_DOWN)) { at = at < 0 ? 0 : CUI_MIN(no - 1, at + 1); *sel = order[at]; s->scroll_to_sel = true; }
        if (cui_key_pressed(ui, CUI_KEY_UP)) { at = at < 0 ? 0 : CUI_MAX(0, at - 1); *sel = order[at]; s->scroll_to_sel = true; }
        if ((cui_key_pressed(ui, CUI_KEY_ENTER) || cui_key_pressed(ui, CUI_KEY_SPACE)) && *sel >= 0 && *sel < CUI__AX_ITEMS) s->open[*sel] = !s->open[*sel];
    }
    int frame = cui__node_new(ui, lid);
    cui_node *fn = &ui->nodes[frame];
    fn->w = CUI_FILL;
    if (o->height != 0) fn->h = CUI_FILL;
    fn->radius = CUI_CORNERS(ui->theme.radius_control);
    fn->border = 1; fn->border_color = CUI_C(ui, BORDER);
    fn->bg = CUI_C(ui, SURFACE);
    fn->flags |= CUI_NF_CLIP;
    cui__mark_interactive(ui, frame, CUI_IF_FOCUSABLE);
    cui__focus_ring(ui, frame, lr.focus_visible ? 1.0f : 0.0f);
    cui__open(ui, frame);
    cui_scroll_opts sc;
    memset(&sc, 0, sizeof sc);
    sc.id = "list"; sc.width = CUI_FILL; sc.height = CUI_FILL;
    bool scrolls = o->height != 0;
    if (scrolls) cui_scroll_begin(ui, &sc);
    cui_id scroll_id = cui_make_id(ui, "list");
    float fs = cui__fs(ui, ui->theme.text_sm), fx = cui__fs(ui, ui->theme.text_xs);
    const char *cur_tag = NULL;
    bool tag_folded = false;
    for (int k = 0; k < no; k++) {
        int i = order[k];
        const cui_api_endpoint *ep = &eps[i];
        const char *tag = cui__ax_tag(ep);
        if (!cur_tag || strcmp(cur_tag, tag)) {
            cur_tag = tag;
            uint32_t h = cui__hash_str(tag, 0xA9u);
            tag_folded = false;
            for (int f = 0; f < s->nfolded; f++) if (s->folded[f] == h) tag_folded = true;
            int count_in = 0;
            for (int j = k; j < no && !strcmp(cui__ax_tag(&eps[order[j]]), tag); j++) count_in++;
            cui_id hid = id ^ h ^ 0x7A6u;
            cui_interact hi = cui__interact(ui, hid, 0);
            if (hi.clicked) {
                if (tag_folded) { for (int f = 0; f < s->nfolded; f++) if (s->folded[f] == h) { s->folded[f] = s->folded[--s->nfolded]; break; } }
                else if (s->nfolded < 32) s->folded[s->nfolded++] = h;
                tag_folded = !tag_folded;
            }
            int hr = cui__node_new(ui, hid);
            cui_node *hn = &ui->nodes[hr];
            hn->dir = CUI_ROW; hn->gap = 6; hn->align = CUI_ALIGN_CENTER; hn->w = CUI_FILL;
            hn->pad[0] = hn->pad[2] = 7; hn->pad[1] = hn->pad[3] = 10;
            hn->bg = hi.hovered ? CUI_C(ui, HOVER) : CUI_C(ui, SURFACE_MUTED);
            cui__mark_interactive(ui, hr, 0);
            if (hi.hovered) cui__set_cursor(ui, CUI_CURSOR_HAND);
            cui__open(ui, hr);
            cui__icon_node(ui, tag_folded ? CUI_ICON_CHEVRON_RIGHT : CUI_ICON_CHEVRON_DOWN, 14, CUI_C(ui, TEXT_MUTED));
            int tt = cui__text_node(ui, tag, -1, CUI_FONT_MEDIUM, fs, CUI_C(ui, FOREGROUND), false);
            ui->nodes[tt].w = CUI_FILL;
            cui__text_node(ui, cui__frame_printf(ui, "%d", count_in), -1, CUI_FONT_BODY, fx, CUI_C(ui, TEXT_MUTED), false);
            cui__close(ui);
        }
        if (tag_folded) continue;
        cui_id rid = cui__hash(&i, sizeof i, id ^ 0xA9E0u);
        cui_interact ri = cui__interact(ui, rid, CUI_IF_KEEP_FOCUS);
        if (ri.clicked) {
            *sel = i; ui->focus = lid;
            if (i < CUI__AX_ITEMS) s->open[i] = !s->open[i];
        }
        bool is_sel = *sel == i, open = i < CUI__AX_ITEMS && s->open[i];
        int row = cui__node_new(ui, rid);
        cui_node *rn = &ui->nodes[row];
        rn->dir = CUI_ROW; rn->gap = 10; rn->align = CUI_ALIGN_CENTER; rn->w = CUI_FILL;
        rn->pad[0] = rn->pad[2] = 6; rn->pad[1] = rn->pad[3] = 10;
        rn->bg = is_sel ? (ui->focus == lid ? cui_mix(CUI_C(ui, SURFACE), cui__emphasis(ui), ui->accent == CUI_ACCENT_NEUTRAL ? 0.08f : 0.16f) : CUI_C(ui, ACTIVE))
                        : ri.hovered ? CUI_C(ui, HOVER) : cui__rgba(0, 0, 0, 0);
        rn->flags |= CUI_NF_ROW_WRAP;
        cui__mark_interactive(ui, row, 0);
        if (ri.hovered) cui__set_cursor(ui, CUI_CURSOR_HAND);
        cui__open(ui, row);
        cui__ax_method(ui, ep->method, narrow);
        cui__ax_path_paint *pp = cui__frame_alloc(ui, sizeof *pp);
        pp->path = cui__frame_strdup(ui, ep->path ? ep->path : "", -1);
        pp->size = fs;
        pp->fg = ep->deprecated ? CUI_C(ui, TEXT_MUTED) : CUI_C(ui, FOREGROUND);
        pp->param = cui__emphasis(ui);
        if (ui->accent == CUI_ACCENT_NEUTRAL) pp->param = cui__tone_color(ui, CUI_TONE_VIOLET);
        pp->strike = ep->deprecated;
        int pn = cui__node_new(ui, 0);
        float pw = cui__text_width(ui, CUI_FONT_MONO, fs, pp->path, -1, true);
        ui->nodes[pn].w = CUI_PX(pw + 1); ui->nodes[pn].h = CUI_PX(roundf(fs * 1.5f));
        ui->nodes[pn].direction = CUI_DIRECTION_LTR;
        ui->nodes[pn].paint = cui__paint_ax_path; ui->nodes[pn].paint_data = pp;
        if (ep->summary && ep->summary[0]) {
            int st = cui__text_node(ui, ep->summary, -1, CUI_FONT_BODY, fx, CUI_C(ui, TEXT_MUTED), false);
            ui->nodes[st].w = CUI_FILL; ui->nodes[st].flags |= CUI_NF_CLIP; ui->nodes[st].min_w = 80;
        }
        if (ep->deprecated) {
            cui_badge_opts bo;
            memset(&bo, 0, sizeof bo);
            bo.tone = CUI_TONE_WARNING; bo.size = CUI_SIZE_SM;
            cui_badge_ex(ui, "Deprecated", &bo);
        }
        cui__close(ui);
        if (is_sel && s->scroll_to_sel) { if (scrolls) cui__dp_reveal(ui, scroll_id, rid); s->scroll_to_sel = false; }
        if (open) { cui_push_id_int(ui, i); cui__ax_params(ui, ep, i, narrow, &res); cui_pop_id(ui); }
    }
    if (no == 0) {
        int e = cui__text_node(ui, count == 0 ? "No endpoints" : "No endpoints match", -1, CUI_FONT_BODY, fs, CUI_C(ui, TEXT_MUTED), false);
        ui->nodes[e].pad[0] = ui->nodes[e].pad[2] = 14; ui->nodes[e].pad[3] = 12;
    }
    if (scrolls) cui_scroll_end(ui);
    cui__close(ui);
    cui_pop_id(ui);
    cui__close(ui);
    res.selected = *sel;
    ui->last_id = id;
    return res;
}
