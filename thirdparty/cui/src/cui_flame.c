/* Flame graph / profiler timeline. Time runs left to right and call depth top
 * to bottom; the caller owns the spans, the view and the selection.
 *
 * Spans are sorted by depth, then start, and spans of one depth do not
 * overlap, so within a row both starts and ends ascend. Every lookup is a
 * binary search on that order: the rows in view, the first span in view per
 * row, the span under the pointer, a span's parent and children. Spans
 * thinner than a pixel are merged into neutral runs, and a run skips ahead a
 * pixel at a time with a binary search, so the work per frame follows the
 * pixels on screen, not the number of spans.
 *
 * The trace viewer (lanes of such rows) and the network waterfall share the
 * ruler, the colors and the bars; they are at the end of the file. */
#include "cui_internal.h"

#define CUI__FL_RULER 24.0f    /* time ruler along the top */
#define CUI__FL_ROW   20.0f    /* default row height (bars are a point shorter) */
#define CUI__FL_PADX  8.0f     /* time axis inset from the panel edge */
#define CUI__FL_PADY  4.0f     /* rows below the ruler, and below the last row */
#define CUI__FL_DRAG  3.0f     /* travel before a press becomes a pan */
#define CUI__FL_LABEL 40.0f    /* narrowest bar that gets a label (three letters and an ellipsis) */
#define CUI__FL_RAD   3.0f

enum { FL_IDLE = 0, FL_PRESS, FL_PAN };
enum { FL_SPAN = 0, FL_HOVER, FL_RUN };

typedef struct {
    int mode;
    float press_x, press_y, last_x, last_y;
    int press_hit; bool press_double;
    cui_spring anim; bool anim_on;
    double a0, a1, b0, b1;      /* animated zoom: from [a0, a1] to [b0, b1] */
    float focus;
    cui_flame_view own_view; int own_sel; bool init;
    /* geometry of the latest paint, for cui_flame_span_rect */
    bool valid;
    float plot_x, plot_w, body_y, row_h, scroll;
    cui_rect body;
    double t0, t1;
} cui__fl_state;

typedef struct { double s, e; int row; uint8_t kind; cui_color fill, fg; const char *label; int label_len; } cui__fl_item;

typedef struct {
    cui__fl_state *st;
    cui_id sid;
    cui__fl_item *items; int n, cap;
    bool has_sel; cui__fl_item sel;
    double t0, t1, origin;
    float row_h, scroll, content_h, focus;
    bool empty;
} cui__fl_paint;

/* ---- searches over the sorted spans ---- */

static int cui__fl_depth_lb(const cui_flame_span *s, int n, int d) {   /* first index with depth >= d */
    int lo = 0, hi = n;
    while (lo < hi) { int m = lo + (hi - lo) / 2; if (s[m].depth < d) lo = m + 1; else hi = m; }
    return lo;
}
static int cui__fl_end_gt(const cui_flame_span *s, int lo, int hi, double t) {   /* first in [lo, hi) with end > t */
    while (lo < hi) { int m = lo + (hi - lo) / 2; if (s[m].end <= t) lo = m + 1; else hi = m; }
    return lo;
}
static int cui__fl_start_le(const cui_flame_span *s, int lo, int hi, double t) {   /* last in [lo, hi) with start <= t, or lo - 1 */
    int a = lo, b = hi;
    while (a < b) { int m = a + (b - a) / 2; if (s[m].start <= t) a = m + 1; else b = m; }
    return a - 1;
}
static void cui__fl_row(const cui_flame_span *s, int n, int d, int *lo, int *hi) {
    *lo = cui__fl_depth_lb(s, n, d);
    *hi = cui__fl_depth_lb(s, n, d + 1);
}

static int cui__fl_parent(const cui_flame_span *s, int n, int i) {
    if (i < 0 || i >= n || s[i].depth <= 0) return -1;
    int lo, hi;
    cui__fl_row(s, n, s[i].depth - 1, &lo, &hi);
    int k = cui__fl_start_le(s, lo, hi, s[i].start);
    return k >= lo && s[k].end >= s[i].end ? k : -1;
}
static int cui__fl_first_child(const cui_flame_span *s, int n, int i) {
    if (i < 0 || i >= n) return -1;
    int lo, hi;
    cui__fl_row(s, n, s[i].depth + 1, &lo, &hi);
    int k = cui__fl_end_gt(s, lo, hi, s[i].start);
    return k < hi && s[k].start >= s[i].start && s[k].start < s[i].end ? k : -1;
}

/* ---- colors: the series palette, hashed so a name keeps its color ---- */

static uint32_t cui__fl_name_key(const char *name, uint32_t color_key) {
    return color_key ? color_key : cui__hash_str(name ? name : "", 0xF1A3u);
}
static uint32_t cui__fl_key(const cui_flame_span *sp) { return cui__fl_name_key(sp->name, sp->color_key); }
static cui_color cui__fl_fill(cui_ctx *ui, uint32_t key, bool hover) {
    static const float light[3] = { 0.30f, 0.38f, 0.46f }, dark[3] = { 0.34f, 0.42f, 0.50f };
    cui_color tone = ui->theme.colors[CUI_TOKEN_SERIES_1 + key % 6];
    float k = (ui->mode == CUI_THEME_DARK ? dark : light)[(key / 6) % 3] + (hover ? 0.12f : 0.0f);
    return cui_mix(CUI_C(ui, SURFACE), tone, k);
}
static cui_color cui__fl_fg(cui_ctx *ui, uint32_t key, cui_color fill) {
    cui_color tone = ui->theme.colors[CUI_TOKEN_SERIES_1 + key % 6];
    return cui__legible(cui_mix(CUI_C(ui, FOREGROUND), tone, 0.3f), fill, 4.5f);
}

/* ---- number formatting ---- */

static const char *cui__fl_dur(cui_ctx *ui, double s) {
    const char *u = "s";
    double v = s;
    if (s < 1e-6) { v = s * 1e9; u = "ns"; }
    else if (s < 1e-3) { v = s * 1e6; u = "\xc2\xb5s"; }
    else if (s < 1) { v = s * 1e3; u = "ms"; }
    int dec = v >= 100 ? 0 : v >= 10 ? 1 : 2;
    return cui__frame_printf(ui, "%.*f %s", dec, v, u);
}

/* ruler step: 1, 2 or 5 times a power of ten, at least min_px apart */
static double cui__fl_nice(double raw) {
    if (!(raw > 0)) return 1;
    double p = pow(10.0, floor(log10(raw)));
    double m = raw / p;
    return (m <= 1 ? 1 : m <= 2 ? 2 : m <= 5 ? 5 : 10) * p;
}
static void cui__fl_unit(double step, double *unit, const char **name, int *dec) {
    if (step >= 0.1) { *unit = 1; *name = "s"; }
    else if (step >= 1e-5) { *unit = 1e-3; *name = "ms"; }
    else if (step >= 1e-8) { *unit = 1e-6; *name = "\xc2\xb5s"; }
    else { *unit = 1e-9; *name = "ns"; }
    double r = step / *unit;
    int d = (int)ceil(-log10(r) - 1e-9);
    *dec = CUI_CLAMP(d, 0, 3);
}

/* The time ruler: ticks a nice step apart with their labels, inside `rr` (its bottom edge is the
 * baseline), and grid lines `grid_h` tall below it. Time t sits at px0 + (t - t0) * pw / (t1 - t0);
 * labels count from `origin`. Returns the step. */
static double cui__fl_ruler(cui_ctx *ui, cui_rect rr, float px0, float pw, double t0, double t1, double origin, float grid_h) {
    double range = t1 - t0;
    float k = (float)(pw / range);
    float fs = cui__fs(ui, ui->theme.text_xs);
    double unit; const char *uname; int dec;
    float min_px = 72;
    double step = cui__fl_nice(range * min_px / pw);
    cui__fl_unit(step, &unit, &uname, &dec);
    /* wide labels ("12.42 ms") need more room: thin the ticks until they fit */
    for (int guard = 0; guard < 4; guard++) {
        const char *wl = cui__frame_printf(ui, "%.*f %s", dec, (t1 - origin) / unit, uname);
        float lw = cui__text_width(ui, CUI_FONT_BODY, fs, wl, -1, true);
        if ((float)(step * k) >= lw + 16) break;
        step = cui__fl_nice(step * 1.01 * 2);
        cui__fl_unit(step, &unit, &uname, &dec);
    }
    double first = ceil((t0 - origin) / step - 1e-9);
    double last = floor((t1 - origin) / step + 1e-9);
    float fh = cui__font_line_height(ui, CUI_FONT_BODY, fs);
    cui_color grid = cui_alpha(CUI_C(ui, BORDER_SUBTLE), ui->mode == CUI_THEME_DARK ? 0.8f : 1.0f);
    for (double q = first; q <= last && q - first < 400; q += 1) {
        double t = origin + q * step;
        float x = px0 + (float)((t - t0) * k);
        x = floorf(x);
        if (grid_h > 0) cui_draw_rect(ui, (cui_rect){ x, rr.y + rr.h + 1, 1, grid_h }, CUI_CORNERS(0), grid);
        cui_draw_rect(ui, (cui_rect){ x, rr.y + rr.h - 5, 1, 5 }, CUI_CORNERS(0), CUI_C(ui, BORDER_STRONG));
        double v = q * step / unit;
        if (fabs(v) < 1e-9) v = 0;
        const char *lbl = cui__frame_printf(ui, "%.*f %s", dec, v, uname);
        float lw = cui__text_width(ui, CUI_FONT_BODY, fs, lbl, -1, true);
        if (x + 4 + lw <= rr.x + rr.w - 6)
            cui__draw_text_line(ui, CUI_FONT_BODY, fs, x + 4, rr.y + (rr.h - 4 - fh) * 0.5f, CUI_C(ui, TEXT_MUTED), lbl, -1, true);
    }
    return step;
}

/* ---- painting ---- */

static void cui__fl_push(cui_ctx *ui, cui__fl_paint *p, const cui__fl_item *it) {
    if (p->n >= p->cap) {
        int nc = p->cap ? p->cap * 2 : 256;
        cui__fl_item *nv = cui__frame_alloc(ui, sizeof(cui__fl_item) * (size_t)nc);
        if (p->n) memcpy(nv, p->items, sizeof(cui__fl_item) * (size_t)p->n);
        p->items = nv; p->cap = nc;
    }
    p->items[p->n++] = *it;
}

static void cui__fl_bar(cui_ctx *ui, const cui__fl_paint *p, const cui__fl_item *it, float px0, float k, float body_y,
                        cui_rect body, bool selected) {
    float x0 = px0 + (float)((it->s - p->t0) * k), x1 = px0 + (float)((it->e - p->t0) * k);
    float y = body_y + it->row * p->row_h - p->scroll, h = p->row_h - 1;
    if (selected && x1 - x0 < 3) { float c = (x0 + x1) * 0.5f; x0 = c - 1.5f; x1 = c + 1.5f; }
    /* keep the bar within a few points of the view, so huge zooms stay in float range */
    float lim0 = body.x - 4, lim1 = body.x + body.w + 4;
    if (x0 < lim0) x0 = lim0;
    if (x1 > lim1) x1 = lim1;
    float w = x1 - x0;
    if (w >= 2) w -= 1;   /* a point between neighbours */
    if (w <= 0) return;
    cui_rect br = { x0, y, w, h };
    float rad = CUI_MIN(CUI__FL_RAD, CUI_MIN(w, h) * 0.5f);
    if (selected) {
        cui_draw_rect_border(ui, br, CUI_CORNERS(rad), it->fill, CUI_C(ui, ACCENT), 1.5f);
    } else {
        cui_draw_rect(ui, br, CUI_CORNERS(rad), it->fill);
    }
    if (it->label_len > 0 && it->kind != FL_RUN) {
        /* the label stays in the visible part of the bar */
        float vx0 = CUI_MAX(x0, body.x + 1), vx1 = CUI_MIN(x0 + w, body.x + body.w - 1);
        float avail = vx1 - vx0 - 12;
        if (avail >= CUI__FL_LABEL - 12) {
            float fs = cui__fs(ui, ui->theme.text_xs);
            float fh = cui__font_line_height(ui, CUI_FONT_BODY, fs);
            cui__draw_text_ellipsis(ui, CUI_FONT_BODY, fs, vx0 + 6, y + (h - fh) * 0.5f, avail, it->fg, it->label, it->label_len, false);
        }
    }
}

static void cui__paint_flame(cui_ctx *ui, cui_node *n, void *data) {
    cui__fl_paint *p = data;
    cui__fl_state *st = p->st;
    cui_rect r = n->rect;
    /* the wheel goes to the graph while the pointer is over its visible part */
    {
        cui_rect clip = ui->dl.clip_stack[ui->dl.clip_depth - 1];
        float s = ui->scale;
        cui_rect cp = { clip.x / s, clip.y / s, clip.w / s, clip.h / s };
        cui_scroll_state *ss = cui_state(ui, p->sid, sizeof *ss);
        ss->view_w = r.w; ss->view_h = r.h;
        ss->content_w = r.w + 1e6f; ss->content_h = r.h + 1e6f;
        cui__paint_add_hit(ui, p->sid, cui__rect_intersect(r, cp), CUI_NF_SCROLL_X | CUI_NF_SCROLL_Y);
    }
    float rad = ui->theme.radius_panel;
    float px0 = r.x + CUI__FL_PADX, pw = CUI_MAX(1.0f, r.w - 2 * CUI__FL_PADX);
    double range = p->t1 - p->t0;
    float k = range > 0 ? (float)(pw / range) : 0;
    float body_y = r.y + CUI__FL_RULER + 1 + CUI__FL_PADY;
    cui_rect body = { r.x + 1, r.y + CUI__FL_RULER + 1, CUI_MAX(0.0f, r.w - 2), CUI_MAX(0.0f, r.h - CUI__FL_RULER - 2) };
    float view_h = r.h - CUI__FL_RULER - 1 - 2 * CUI__FL_PADY;   /* rows area, as the build sees it */
    st->valid = !p->empty && k > 0;
    st->plot_x = px0; st->plot_w = pw; st->body_y = body_y; st->row_h = p->row_h; st->scroll = p->scroll;
    st->body = body; st->t0 = p->t0; st->t1 = p->t1;

    cui_push_clip(ui, cui__rect_inset(r, 1));
    /* ruler */
    cui_corners rc = { rad - 1, rad - 1, 0, 0 };
    cui_draw_rect(ui, (cui_rect){ r.x, r.y, r.w, CUI__FL_RULER }, rc, CUI_C(ui, SURFACE_MUTED));
    cui_draw_rect(ui, (cui_rect){ r.x, r.y + CUI__FL_RULER, r.w, 1 }, CUI_CORNERS(0), CUI_C(ui, BORDER));
    if (k > 0) cui__fl_ruler(ui, (cui_rect){ r.x, r.y, r.w, CUI__FL_RULER }, px0, pw, p->t0, p->t1, p->origin, r.h - CUI__FL_RULER - 2);
    /* rows */
    cui_push_clip(ui, body);
    for (int i = 0; i < p->n; i++) cui__fl_bar(ui, p, &p->items[i], px0, k, body_y, body, false);
    if (p->has_sel) cui__fl_bar(ui, p, &p->sel, px0, k, body_y, body, true);
    if (p->empty) {
        const char *msg = "No spans";
        float lw = cui__text_width(ui, CUI_FONT_BODY, cui__fs(ui, ui->theme.text_sm), msg, -1, false);
        float lh = cui__font_line_height(ui, CUI_FONT_BODY, cui__fs(ui, ui->theme.text_sm));
        cui__draw_text_line(ui, CUI_FONT_BODY, cui__fs(ui, ui->theme.text_sm), body.x + (body.w - lw) * 0.5f, body.y + (body.h - lh) * 0.5f,
                            CUI_C(ui, TEXT_MUTED), msg, -1, false);
    }
    /* a quiet scroll indicator when rows are hidden above or below */
    if (p->content_h > view_h + 0.5f && view_h > 8) {
        float th = CUI_MAX(16.0f, view_h * view_h / p->content_h);
        float ty = body.y + CUI__FL_PADY + (view_h - th) * (p->scroll / (p->content_h - view_h));
        cui_draw_rect(ui, (cui_rect){ body.x + body.w - 5, ty, 3, th }, CUI_CORNERS(1.5f), cui_alpha(CUI_C(ui, TEXT_MUTED), 0.4f));
    }
    cui_pop_clip(ui);
    if (p->focus > 0.01f)
        cui_draw_rect_border(ui, cui__rect_inset(r, 1), CUI_CORNERS(rad - 1), cui__rgba(0, 0, 0, 0), cui_alpha(CUI_C(ui, FOCUS_RING), p->focus), 2);
    cui_pop_clip(ui);
}

/* ---- the component ---- */

typedef struct {
    const cui_flame_span *s; int n;
    double dmin, dmax;
    int rows;                   /* deepest depth + 1 */
    float plot_x, plot_w, body_y, row_h, body_h;
} cui__fl_ctx;

static int cui__fl_hit(const cui__fl_ctx *c, const cui_flame_view *v, float mx, float my) {
    if (c->n == 0 || c->plot_w <= 0) return -1;
    float yy = my - c->body_y + v->scroll;
    if (yy < 0 || mx < c->plot_x - 1 || mx > c->plot_x + c->plot_w + 1) return -1;
    int d = (int)floorf(yy / c->row_h);
    if (d >= c->rows || yy - d * c->row_h > c->row_h - 1) return -1;   /* the gap between rows */
    int lo, hi;
    cui__fl_row(c->s, c->n, d, &lo, &hi);
    if (lo >= hi) return -1;
    double range = v->end - v->start;
    double t = v->start + (double)(mx - c->plot_x) / c->plot_w * range;
    double tol = range / c->plot_w;   /* a point either side, so a thin span can be found */
    int k = cui__fl_start_le(c->s, lo, hi, t);
    if (k >= lo && c->s[k].end >= t) return k;
    int best = -1;
    double bd = tol;
    if (k >= lo && t - c->s[k].end <= bd) { bd = t - c->s[k].end; best = k; }
    if (k + 1 < hi && c->s[k + 1].start - t <= bd) best = k + 1;
    return best;
}

static void cui__fl_clamp(const cui__fl_ctx *c, cui_flame_view *v, double min_range) {
    double ext = c->dmax - c->dmin;
    double range = v->end - v->start;
    if (!(range > 0)) range = ext;
    range = CUI_MAX(range, min_range);
    if (range > ext) range = ext;
    double s = v->start;
    if (s < c->dmin) s = c->dmin;
    if (s + range > c->dmax) s = c->dmax - range;
    v->start = s; v->end = s + range;
}

static void cui__fl_clamp_scroll(const cui__fl_ctx *c, cui_flame_view *v) {
    float max_s = CUI_MAX(0.0f, c->rows * c->row_h - c->body_h);
    if (!(v->scroll >= 0)) v->scroll = 0;   /* also NaN */
    if (v->scroll > max_s) v->scroll = max_s;
}

static void cui__fl_animate_to(cui__fl_state *st, const cui_flame_view *v, double s, double e) {
    st->a0 = v->start; st->a1 = v->end; st->b0 = s; st->b1 = e;
    st->anim.x = 0; st->anim.v = 0;
    st->anim_on = true;
}

/* Interpolates between two views around the point that stays put on screen,
 * with the range changing geometrically, so a zoom reads as one motion. */
static void cui__fl_lerp(const cui__fl_state *st, double p, double *s, double *e) {
    double r0 = st->a1 - st->a0, r1 = st->b1 - st->b0;
    if (!(r0 > 0) || !(r1 > 0) || fabs(r1 - r0) < 1e-6 * r0) {
        *s = st->a0 + (st->b0 - st->a0) * p;
        *e = st->a1 + (st->b1 - st->a1) * p;
        return;
    }
    double fx = (st->a0 * r1 - st->b0 * r0) / (r1 - r0);   /* same screen fraction in both views */
    double u = (fx - st->a0) / r0;
    double r = exp(log(r0) + (log(r1) - log(r0)) * p);
    *s = fx - u * r;
    *e = *s + r;
}

static void cui__fl_zoom_span(const cui__fl_ctx *c, cui__fl_state *st, cui_flame_view *v, int i, double min_range) {
    const cui_flame_span *sp = &c->s[i];
    double d = CUI_MAX(sp->end - sp->start, min_range);
    cui_flame_view t = { sp->start - d * 0.05, sp->start + d * 1.05, v->scroll };
    cui__fl_clamp(c, &t, min_range);
    cui__fl_animate_to(st, v, t.start, t.end);
}

static void cui__fl_reveal(const cui__fl_ctx *c, cui_flame_view *v, int i, double min_range) {
    const cui_flame_span *sp = &c->s[i];
    double range = v->end - v->start;
    if (sp->end < v->start || sp->start > v->end) {
        double mid = (sp->start + sp->end) * 0.5;
        v->start = mid - range * 0.5; v->end = mid + range * 0.5;
        cui__fl_clamp(c, v, min_range);
    }
    float top = sp->depth * c->row_h;
    if (top < v->scroll) v->scroll = top;
    if (top + c->row_h > v->scroll + c->body_h) v->scroll = top + c->row_h - c->body_h;
    cui__fl_clamp_scroll(c, v);
}

/* one frame of the flame graph: what its phases share */
typedef struct {
    cui_ctx *ui;
    const cui_flame_opts *o;
    cui_flame_result *res;
    cui_id id, sid;
    cui__fl_state *st;
    cui_flame_view *v;
    int *sel;
    const cui_flame_span *spans; int count;
    double min_range;
    cui__fl_ctx c;                      /* the data's extent and the plot's geometry */
    bool has_data, has_rect;
    cui_rect R;                         /* the graph, last frame */
    double range;                       /* the view's range before this frame's input */
    cui_interact bi;
    float mx, my;
    bool user_moved;                    /* input that should stop an animation */
    int hov;                            /* the span under the pointer, or -1 */
} cui__flf;

/* the extent of the data, a row at a time, and the plot's geometry from the last frame */
static void cui__flf_extent(cui__flf *f) {
    cui__fl_ctx *c = &f->c;
    const cui_flame_span *spans = f->spans;
    int count = f->count;
    memset(c, 0, sizeof *c);
    c->s = spans; c->n = count;
    c->row_h = f->o->row_height > 0 ? f->o->row_height : CUI__FL_ROW;
    c->dmin = 0; c->dmax = 0;
    bool any = false;
    for (int lo = cui__fl_depth_lb(spans, count, 0); lo < count;) {
        int dd = spans[lo].depth;
        int hi = cui__fl_depth_lb(spans, count, dd + 1);
        if (hi <= lo) break;   /* unsorted input: stop rather than loop */
        double a = spans[lo].start, b = spans[hi - 1].end;
        if (!any || a < c->dmin) c->dmin = a;
        if (!any || b > c->dmax) c->dmax = b;
        any = true;
        c->rows = dd + 1;
        lo = hi;
    }
    if (c->rows > 100000) c->rows = 100000;
    f->has_data = any && c->dmax >= c->dmin && isfinite(c->dmin) && isfinite(c->dmax);
    if (f->has_data && c->dmax - c->dmin < f->min_range) { double m = (c->dmin + c->dmax) * 0.5; c->dmin = m - f->min_range * 0.5; c->dmax = m + f->min_range * 0.5; }

    f->has_rect = cui__has_prev_rect(f->ui, f->id);
    f->R = cui__prev_rect(f->ui, f->id);
    cui_rect R = f->R;
    c->plot_x = R.x + CUI__FL_PADX;
    c->plot_w = f->has_rect ? CUI_MAX(1.0f, R.w - 2 * CUI__FL_PADX) : 600;
    c->body_y = R.y + CUI__FL_RULER + 1 + CUI__FL_PADY;
    c->body_h = f->has_rect ? CUI_MAX(1.0f, R.h - CUI__FL_RULER - 1 - 2 * CUI__FL_PADY) : 200;
}

/* wheel: zoom around the pointer; Shift or a sideways scroll pans */
static void cui__flf_wheel(cui__flf *f) {
    cui_ctx *ui = f->ui;
    cui_flame_view *v = f->v;
    if (!(ui->scroll_target == f->sid && ui->hot_layer == ui->current_layer && (ui->scroll_dx != 0 || ui->scroll_dy != 0))) return;
    double range = f->range;
    bool pan = ui->scroll_dx != 0 || (ui->mods & CUI_MOD_SHIFT);
    if (pan) {
        float dl = ui->scroll_dx != 0 ? ui->scroll_dx : ui->scroll_dy;
        double dt = (double)dl * 40.0 / f->c.plot_w * range;
        v->start -= dt; v->end -= dt;
    } else {
        double z = pow(1.12, (double)ui->scroll_dy);
        double nr = CUI_MAX(range / z, f->min_range);
        nr = CUI_MIN(nr, f->c.dmax - f->c.dmin);
        double u = (double)(f->mx - f->c.plot_x) / f->c.plot_w;
        double tm = v->start + u * range;
        v->start = tm - u * nr; v->end = v->start + nr;
    }
    ui->scroll_dx = ui->scroll_dy = 0;
    f->user_moved = true;
}

/* the pointer: a press that moves pans both ways; a click selects (not on the ruler); a
 * double click zooms to the span, or out to everything */
static void cui__flf_pointer(cui__flf *f) {
    cui_ctx *ui = f->ui;
    cui__fl_state *st = f->st;
    cui_flame_view *v = f->v;
    float mx = f->mx, my = f->my;
    if (f->bi.hovered && ui->mpressed[0] && st->mode == FL_IDLE) {
        st->mode = FL_PRESS;
        st->press_x = st->last_x = mx; st->press_y = st->last_y = my;
        st->press_hit = cui__fl_hit(&f->c, v, mx, my);
        st->press_double = f->bi.double_clicked;
    }
    if (st->mode == FL_PRESS && ui->mdown[0]) {
        float dx = mx - st->press_x, dy = my - st->press_y;
        if (dx * dx + dy * dy > CUI__FL_DRAG * CUI__FL_DRAG) st->mode = FL_PAN;
    }
    if (st->mode == FL_PAN) {
        double dt = (double)(mx - st->last_x) / f->c.plot_w * (v->end - v->start);
        v->start -= dt; v->end -= dt;
        v->scroll -= my - st->last_y;
        st->last_x = mx; st->last_y = my;
        cui__set_cursor(ui, CUI_CURSOR_MOVE);
        f->user_moved = true;
        if (!ui->mdown[0]) st->mode = FL_IDLE;
    } else if (st->mode == FL_PRESS && !ui->mdown[0]) {
        st->mode = FL_IDLE;
        int h = st->press_hit;
        bool in_rows = st->press_y >= f->c.body_y - CUI__FL_PADY;   /* a click on the ruler keeps the selection */
        if (in_rows && h != *f->sel) *f->sel = h;
        if (st->press_double) {
            if (h >= 0) { cui__fl_zoom_span(&f->c, st, v, h, f->min_range); f->res->zoomed = h; }
            else cui__fl_animate_to(st, v, f->c.dmin, f->c.dmax);
        }
    }
    if (st->mode != FL_IDLE && !ui->mdown[0]) st->mode = FL_IDLE;
}

/* arrows walk the tree: left and right along the row, up to the parent, down to the first
 * child; with nothing selected they start from the first span in view on the top row */
static void cui__flf_arrows(cui__flf *f) {
    cui_ctx *ui = f->ui;
    const cui_flame_span *spans = f->spans;
    int count = f->count;
    int s = *f->sel, ns = s;
    if (s < 0) {
        int lo, hi;
        cui__fl_row(spans, count, spans[cui__fl_depth_lb(spans, count, 0)].depth, &lo, &hi);
        int k = cui__fl_end_gt(spans, lo, hi, f->v->start);
        ns = k < hi ? k : (lo < hi ? lo : -1);
    } else if (cui_key_pressed(ui, CUI_KEY_LEFT)) {
        if (s > 0 && spans[s - 1].depth == spans[s].depth) ns = s - 1;
    } else if (cui_key_pressed(ui, CUI_KEY_RIGHT)) {
        if (s + 1 < count && spans[s + 1].depth == spans[s].depth) ns = s + 1;
    } else if (cui_key_pressed(ui, CUI_KEY_UP)) {
        int pk = cui__fl_parent(spans, count, s);
        if (pk >= 0) ns = pk;
    } else {
        int ck = cui__fl_first_child(spans, count, s);
        if (ck >= 0) ns = ck;
    }
    if (ns >= 0 && ns != s) { *f->sel = ns; cui__fl_reveal(&f->c, f->v, ns, f->min_range); f->user_moved = true; }
}

/* keys while focused: arrows (Shift pans), = and - zoom, Enter or Space zooms to the
 * selection, Home or F fits, Escape fits and then clears the selection */
static void cui__flf_keys(cui__flf *f) {
    cui_ctx *ui = f->ui;
    cui__fl_state *st = f->st;
    cui_flame_view *v = f->v;
    const cui__fl_ctx *c = &f->c;
    bool shift = (ui->mods & CUI_MOD_SHIFT) != 0;
    if (shift && (cui_key_pressed(ui, CUI_KEY_LEFT) || cui_key_pressed(ui, CUI_KEY_RIGHT))) {
        double dt = (cui_key_pressed(ui, CUI_KEY_LEFT) ? -0.1 : 0.1) * f->range;
        v->start += dt; v->end += dt;
        f->user_moved = true;
    } else if (cui_key_pressed(ui, CUI_KEY_LEFT) || cui_key_pressed(ui, CUI_KEY_RIGHT) ||
               cui_key_pressed(ui, CUI_KEY_UP) || cui_key_pressed(ui, CUI_KEY_DOWN)) {
        cui__flf_arrows(f);
    }
    ui->kpressed[CUI_KEY_LEFT] = ui->kpressed[CUI_KEY_RIGHT] = ui->kpressed[CUI_KEY_UP] = ui->kpressed[CUI_KEY_DOWN] = 0;
    double cx = (v->start + v->end) * 0.5;
    if (cui_key_pressed(ui, CUI_KEY_EQUAL) || cui_key_pressed(ui, CUI_KEY_MINUS)) {
        double z = cui_key_pressed(ui, CUI_KEY_EQUAL) ? 1 / 1.25 : 1.25;
        double nr = CUI_MIN(CUI_MAX((v->end - v->start) * z, f->min_range), c->dmax - c->dmin);
        v->start = cx - nr * 0.5; v->end = cx + nr * 0.5;
        f->user_moved = true;
        ui->kpressed[CUI_KEY_EQUAL] = ui->kpressed[CUI_KEY_MINUS] = 0;
    }
    if ((cui_key_pressed(ui, CUI_KEY_ENTER) || cui_key_pressed(ui, CUI_KEY_SPACE)) && *f->sel >= 0) {
        cui__fl_zoom_span(c, st, v, *f->sel, f->min_range);
        f->res->zoomed = *f->sel;
        ui->kpressed[CUI_KEY_ENTER] = ui->kpressed[CUI_KEY_SPACE] = 0;
    }
    bool fitted = fabs(v->start - c->dmin) <= 1e-9 * (c->dmax - c->dmin) + 1e-15 &&
                  fabs(v->end - c->dmax) <= 1e-9 * (c->dmax - c->dmin) + 1e-15;
    if (cui_key_pressed(ui, CUI_KEY_HOME) || (cui_key_pressed(ui, CUI_KEY_F) && !(ui->mods & CUI_MOD_PRIMARY))) {
        cui__fl_animate_to(st, v, c->dmin, c->dmax);
        ui->kpressed[CUI_KEY_HOME] = ui->kpressed[CUI_KEY_F] = 0;
    }
    if (cui_key_pressed(ui, CUI_KEY_ESCAPE)) {
        if (fitted && !st->anim_on) *f->sel = -1;
        else cui__fl_animate_to(st, v, c->dmin, c->dmax);
        ui->kpressed[CUI_KEY_ESCAPE] = 0;
    }
}

/* the view: glides to a zoom's target, or stays inside the data */
static void cui__flf_move_view(cui__flf *f) {
    cui__fl_state *st = f->st;
    cui_flame_view *v = f->v;
    if (f->user_moved) st->anim_on = false;
    if (st->anim_on) {
        float pr = cui__spring(f->ui, &st->anim, 1.0f, CUI__SPRING_EXPAND);
        double s0, s1;
        cui__fl_lerp(st, pr >= 1.0f ? 1.0 : (double)pr, &s0, &s1);
        v->start = s0; v->end = s1;
        if (pr >= 1.0f && st->anim.v == 0) { v->start = st->b0; v->end = st->b1; st->anim_on = false; }
    } else {
        cui__fl_clamp(&f->c, v, f->min_range);
    }
    cui__fl_clamp_scroll(&f->c, v);
}

/* the graph's node and its paint data, without the spans */
static cui__fl_paint *cui__flf_build_panel(cui__flf *f, int *node) {
    cui_ctx *ui = f->ui;
    const cui_flame_opts *o = f->o;
    int en = cui__node_new(ui, f->id);
    cui_node *n = &ui->nodes[en];
    n->w = o->width.kind == CUI_SIZE_FIT && o->width.value == 0 ? CUI_FILL : o->width;
    float def_h = CUI__FL_RULER + 1 + 2 * CUI__FL_PADY + CUI_MAX(3, f->c.rows) * f->c.row_h;
    n->h = CUI_PX(o->height > 0 ? o->height : CUI_MIN(360.0f, def_h));
    n->bg = CUI_C(ui, SURFACE);
    n->border = 1;
    n->border_color = CUI_C(ui, BORDER);
    n->radius = CUI_CORNERS(ui->theme.radius_panel);
    n->flags |= CUI_NF_CLIP;
    cui__mark_interactive(ui, en, CUI_IF_FOCUSABLE);
    cui__fl_paint *p = cui__frame_alloc(ui, sizeof *p);
    memset(p, 0, sizeof *p);
    p->st = f->st; p->sid = f->sid;
    p->t0 = f->v->start; p->t1 = f->v->end; p->origin = f->c.dmin;
    p->row_h = f->c.row_h; p->scroll = f->v->scroll; p->content_h = f->c.rows * f->c.row_h;
    p->empty = !f->has_data;
    cui__tween(ui, &f->st->focus, f->bi.focus_visible ? 1.0f : 0.0f, CUI__T_FOCUS);
    p->focus = f->st->focus;
    n->paint = cui__paint_flame;
    n->paint_data = p;
    *node = en;
    return p;
}

/* a span as drawn: its color, and its name when it is `w` points wide or more */
static cui__fl_item cui__flf_span_item(cui_ctx *ui, const cui_flame_span *sp, int row, bool hovered, double w) {
    uint32_t key = cui__fl_key(sp);
    cui__fl_item it;
    memset(&it, 0, sizeof it);
    it.row = row;
    it.s = sp->start; it.e = sp->end;
    it.kind = hovered ? FL_HOVER : FL_SPAN;
    it.fill = cui__fl_fill(ui, key, hovered);
    if (w >= CUI__FL_LABEL && sp->name && sp->name[0]) {
        it.fg = cui__fl_fg(ui, key, it.fill);
        it.label_len = (int)strlen(sp->name);
        it.label = cui__frame_strdup(ui, sp->name, it.label_len);
    }
    return it;
}

/* one row's spans in view, from its first visible span; spans thinner than a point merge
 * with their neighbours within a point into one run */
static void cui__flf_row_items(cui__flf *f, cui__fl_paint *p, int row, double t0, double t1, double k, cui_color run_fill) {
    cui_ctx *ui = f->ui;
    const cui_flame_span *spans = f->spans;
    int lo, hi;
    cui__fl_row(spans, f->count, row, &lo, &hi);
    int i = cui__fl_end_gt(spans, lo, hi, t0);
    while (i < hi && spans[i].start < t1) {
        const cui_flame_span *sp = &spans[i];
        double xs = (sp->start - t0) * k, xe = (sp->end - t0) * k;
        if (xe - xs >= 1.0) {
            cui__fl_item it = cui__flf_span_item(ui, sp, row, i == f->hov, xe - xs);
            cui__fl_push(ui, p, &it);
            i++;
            continue;
        }
        double rx0 = xs, rx1 = xe;
        i++;
        for (;;) {
            int j = cui__fl_end_gt(spans, i, hi, t0 + (rx1 + 1.0) / k);
            if (j > i) { double e2 = (spans[j - 1].end - t0) * k; if (e2 > rx1) rx1 = e2; i = j; }
            if (i < hi) {
                double ns = (spans[i].start - t0) * k, ne = (spans[i].end - t0) * k;
                if (ne - ns < 1.0 && ns <= rx1 + 1.0) { rx1 = ne; i++; continue; }
            }
            break;
        }
        if (rx1 - rx0 >= 1.0) {
            cui__fl_item it;
            memset(&it, 0, sizeof it);
            it.row = row;
            it.s = t0 + rx0 / k; it.e = t0 + rx1 / k;
            it.kind = FL_RUN;
            it.fill = run_fill;
            cui__fl_push(ui, p, &it);
        }
    }
}

/* the spans in view, then the selection on top */
static void cui__flf_build_spans(cui__flf *f, cui__fl_paint *p) {
    cui_ctx *ui = f->ui;
    const cui_flame_view *v = f->v;
    const cui__fl_ctx *c = &f->c;
    double t0 = v->start, t1 = v->end;
    double k = c->plot_w / (t1 - t0);
    int d0 = (int)floorf(v->scroll / c->row_h), d1 = (int)floorf((v->scroll + c->body_h) / c->row_h);
    d0 = CUI_MAX(0, d0); d1 = CUI_MIN(c->rows - 1, d1);
    cui_color run_fill = cui_mix(CUI_C(ui, SURFACE), CUI_C(ui, TEXT_MUTED), ui->mode == CUI_THEME_DARK ? 0.32f : 0.26f);
    for (int row = d0; row <= d1; row++) cui__flf_row_items(f, p, row, t0, t1, k, run_fill);
    int s = *f->sel;
    const cui_flame_span *spans = f->spans;
    if (s >= 0 && spans[s].depth >= d0 && spans[s].depth <= d1 && spans[s].end >= t0 && spans[s].start <= t1) {
        const cui_flame_span *sp = &spans[s];
        p->sel = cui__flf_span_item(ui, sp, sp->depth, s == f->hov, (sp->end - sp->start) * k);
        p->has_sel = true;
    }
}

/* the tooltip anchors on a small box at the pointer, on the hovered row */
static void cui__flf_tooltip(cui__flf *f) {
    cui_ctx *ui = f->ui;
    int hov = f->hov;
    if (!(hov >= 0 && ui->active == 0 && !ui->mdown[0])) return;
    const cui_flame_span *spans = f->spans;
    const cui_flame_span *sp = &spans[hov];
    cui_id tid = cui__hash(&hov, sizeof hov, f->id ^ 0x7111u);
    float ry = f->c.body_y + sp->depth * f->c.row_h - f->v->scroll;
    cui__absolute(ui, tid, f->mx - f->R.x - 1, ry - f->R.y, 2, f->c.row_h - 1);
    double dur = sp->end - sp->start;
    int pk = cui__fl_parent(spans, f->count, hov);
    double base = pk >= 0 ? spans[pk].end - spans[pk].start : f->v->end - f->v->start;
    double pct = base > 0 ? dur / base * 100.0 : 0;
    const char *of = pk >= 0 ? (spans[pk].name && spans[pk].name[0] ? spans[pk].name : "parent") : "view";
    const char *tip = cui__frame_printf(ui, "%s\n%s \xc2\xb7 %.*f%% of %s", sp->name && sp->name[0] ? sp->name : "Unnamed span",
                                        cui__fl_dur(ui, dur), pct >= 10 ? 0 : 1, pct, of);
    cui__tooltip_passive(ui, tid, tip);
}

cui_flame_result cui_flame_graph_ex(cui_ctx *ui, const char *id_str, const cui_flame_span *spans, int count,
                                    cui_flame_view *view, int *selected, const cui_flame_opts *o) {
    cui_flame_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui_flame_result res;
    res.selection_changed = res.view_changed = false;
    res.hovered = res.zoomed = -1;
    cui__flf f;
    memset(&f, 0, sizeof f);
    f.ui = ui; f.o = o; f.res = &res;
    f.id = cui_make_id(ui, id_str ? id_str : "flame");
    f.sid = f.id ^ 0xF1A5C011u;
    cui__fl_state *st = f.st = cui_state(ui, f.id ^ 0xF1A3u, sizeof *st);
    if (!st->init) { st->own_sel = -1; st->init = true; }
    cui_flame_view *v = f.v = view ? view : &st->own_view;
    int *sel = f.sel = selected ? selected : &st->own_sel;
    if (!spans || count < 0) count = 0;
    f.spans = spans; f.count = count;
    int sel_before = *sel;
    if (*sel < -1 || *sel >= count) *sel = -1;
    f.min_range = o->min_range > 0 ? o->min_range : 1e-8;
    cui__flf_extent(&f);

    double b0 = v->start, b1 = v->end;
    float bs = v->scroll;
    if (f.has_data && (!(v->end > v->start) || !isfinite(v->start) || !isfinite(v->end))) {
        v->start = f.c.dmin; v->end = f.c.dmax;
        st->anim_on = false;
    }

    f.bi = cui__interact(ui, f.id, CUI_IF_FOCUSABLE | CUI_IF_DRAG);
    f.mx = ui->mx; f.my = ui->my;

    /* input */
    if (f.has_data && f.has_rect) {
        f.range = v->end - v->start;
        cui__flf_wheel(&f);
        cui__flf_pointer(&f);
        if (f.bi.focused && !ui->edit_prev) cui__flf_keys(&f);
        cui__flf_move_view(&f);
    } else if (!f.has_data) {
        st->mode = FL_IDLE;
        st->anim_on = false;
        if (*sel != -1) *sel = -1;
    }

    /* hover */
    f.hov = -1;
    if (f.has_data && f.has_rect && f.bi.hovered && st->mode != FL_PAN && cui__rect_contains(f.R, f.mx, f.my)) f.hov = cui__fl_hit(&f.c, v, f.mx, f.my);
    res.hovered = f.hov;
    res.selection_changed = *sel != sel_before;
    res.view_changed = v->start != b0 || v->end != b1 || v->scroll != bs;

    /* build */
    int en;
    cui__fl_paint *p = cui__flf_build_panel(&f, &en);
    if (f.has_data && v->end > v->start) cui__flf_build_spans(&f, p);
    cui__open(ui, en);
    cui__flf_tooltip(&f);
    cui__close(ui);
    ui->last_id = f.id;
    return res;
}

static int cui__fl_cmp(const void *a, const void *b) {
    const cui_flame_span *x = a, *y = b;
    if (x->depth != y->depth) return x->depth < y->depth ? -1 : 1;
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    if (x->end != y->end) return x->end > y->end ? -1 : 1;   /* longer first */
    return 0;
}

void cui_flame_sort(cui_flame_span *spans, int count) {
    if (spans && count > 1) qsort(spans, (size_t)count, sizeof *spans, cui__fl_cmp);
}

cui_rect cui_flame_span_rect(cui_ctx *ui, cui_id flame, const cui_flame_span *span) {
    cui_rect none = { 0, 0, 0, 0 };
    if (!span || !cui__has_prev_rect(ui, flame)) return none;
    cui__fl_state *st = cui_state(ui, flame ^ 0xF1A3u, sizeof *st);
    if (!st->valid || !(st->t1 > st->t0)) return none;
    double k = st->plot_w / (st->t1 - st->t0);
    double x0 = st->plot_x + (span->start - st->t0) * k, x1 = st->plot_x + (span->end - st->t0) * k;
    float y = st->body_y + span->depth * st->row_h - st->scroll;
    cui_rect b = st->body;
    if (x1 < b.x || x0 > b.x + b.w || y + st->row_h - 1 < b.y || y > b.y + b.h || span->depth < 0) return none;
    cui_rect r = { (float)x0, y, (float)(x1 - x0), st->row_h - 1 };
    return r;
}

/* ======================================================================== */
/* Network waterfall                                                        */
/* ======================================================================== */

#define CUI__WF_ROW    26.0f
#define CUI__WF_HEAD   28.0f
#define CUI__WF_NARROW 600.0f
#define CUI__WF_PADX   10.0f     /* the time axis inset in the waterfall column */

typedef struct { int sel; bool init, scroll_to_sel; } cui__wf_state;

/* the time scale that every row and the header share */
typedef struct {
    double end;                  /* seconds at the right edge */
    double dom, load;            /* event lines, 0: none */
    double step;                 /* grid step: the header's ruler sets it, and is painted before the rows */
} cui__wf_scale;
typedef struct { const cui__wf_scale *sc; double start; float timing[6]; } cui__wf_bar;

static double cui__wf_total(const cui_net_request *q) {
    double t = 0;
    for (int k = 0; k < 6; k++) t += q->timing[k] > 0 ? q->timing[k] : 0;
    return t;
}

/* queued, DNS, connect, TLS, waiting, download */
static cui_color cui__wf_color(cui_ctx *ui, int k) {
    static const cui_tone tones[6] = { CUI_TONE_NEUTRAL, CUI_TONE_TEAL, CUI_TONE_WARNING, CUI_TONE_VIOLET, CUI_TONE_SUCCESS, CUI_TONE_INFO };
    if (k == 0) return CUI_C(ui, TEXT_MUTED);
    return cui__tone_color(ui, tones[k]);
}
static const char *const cui__wf_names[6] = { "Queued", "DNS", "Connect", "TLS", "Waiting", "Download" };

static void cui__wf_lines(cui_ctx *ui, const cui__wf_scale *b, cui_rect r, float px0, float k) {
    if (b->dom > 0) cui_draw_rect(ui, (cui_rect){ floorf(px0 + (float)(b->dom * k)), r.y, 1, r.h }, CUI_CORNERS(0), cui_alpha(cui__tone_color(ui, CUI_TONE_INFO), 0.8f));
    if (b->load > 0) cui_draw_rect(ui, (cui_rect){ floorf(px0 + (float)(b->load * k)), r.y, 1, r.h }, CUI_CORNERS(0), cui_alpha(CUI_C(ui, DANGER), 0.8f));
}

/* a row's bar: grid lines, then the phases, thin while queued, then the event lines */
static void cui__paint_wf_bar(cui_ctx *ui, cui_node *n, void *data) {
    const cui__wf_bar *bar = data;
    const cui__wf_scale *b = bar->sc;
    cui_rect r = n->rect;
    float px0 = r.x + CUI__WF_PADX, pw = CUI_MAX(1.0f, r.w - 2 * CUI__WF_PADX);
    float k = (float)(pw / b->end);
    cui_color grid = cui_alpha(CUI_C(ui, BORDER_SUBTLE), ui->mode == CUI_THEME_DARK ? 0.8f : 1.0f);
    for (int q = 1; b->step > 0 && q * b->step <= b->end + 1e-9 && q < 400; q++)
        cui_draw_rect(ui, (cui_rect){ floorf(px0 + (float)(q * b->step * k)), r.y, 1, r.h }, CUI_CORNERS(0), grid);
    float x = px0 + (float)(bar->start * k), cy = r.y + r.h * 0.5f, h = 10;
    for (int ph = 0; ph < 6; ph++) {
        float w = bar->timing[ph] > 0 ? (float)(bar->timing[ph] * k) : 0;
        if (w <= 0) continue;
        cui_color c = cui__wf_color(ui, ph);
        if (ph == 0) cui_draw_rect(ui, (cui_rect){ x, cy - 1, w, 2 }, CUI_CORNERS(1), cui_alpha(c, 0.6f));
        else {
            cui_color fill = ph == 4 ? cui_mix(CUI_C(ui, SURFACE), c, 0.55f) : c;
            cui_draw_rect(ui, (cui_rect){ x, cy - h * 0.5f, CUI_MAX(1.0f, w), h }, CUI_CORNERS(CUI_MIN(2.0f, w * 0.5f)), fill);
        }
        x += w;
    }
    cui__wf_lines(ui, b, r, px0, k);
}

/* the header's waterfall cell: the ruler */
static void cui__paint_wf_head(cui_ctx *ui, cui_node *n, void *data) {
    cui__wf_scale *b = data;
    cui_rect r = n->rect;
    float px0 = r.x + CUI__WF_PADX, pw = CUI_MAX(1.0f, r.w - 2 * CUI__WF_PADX);
    b->step = cui__fl_ruler(ui, r, px0, pw, 0, b->end, 0, 0);
    cui__wf_lines(ui, b, (cui_rect){ r.x, r.y + r.h - 6, r.w, 6 }, px0, (float)(pw / b->end));
}

/* one frame of the waterfall: what its parts share */
typedef struct {
    cui_ctx *ui;
    const cui_net_request *q; int n;
    cui_id id, lid;
    cui__wf_state *s;
    int *sel;
    bool narrow;
    float name_w;
    cui__wf_scale *head;
} cui__wff;

static void cui__wf_cell(cui_ctx *ui, const char *text, float w, cui_font font, cui_color c, bool end) {
    int t = cui__text_node(ui, text, -1, font, cui__fs(ui, ui->theme.text_xs), c, false);
    cui_node *tn = &ui->nodes[t];
    tn->w = CUI_PX(w); tn->flags |= CUI_NF_CLIP | CUI_NF_TABULAR;
    if (end) tn->text_align = CUI_TEXT_END;
}

static void cui__wf_head(cui__wff *f) {
    cui_ctx *ui = f->ui;
    int h = cui__node_new(ui, 0);
    cui_node *hn = &ui->nodes[h];
    hn->dir = CUI_ROW; hn->w = CUI_FILL; hn->h = CUI_PX(CUI__WF_HEAD); hn->align = CUI_ALIGN_CENTER; hn->gap = 8;
    hn->pad[3] = 10;
    hn->bg = CUI_C(ui, SURFACE_MUTED);
    hn->border_color = CUI_C(ui, BORDER);
    cui__open(ui, h);
    cui_color c = CUI_C(ui, TEXT_MUTED);
    cui__wf_cell(ui, "Name", f->name_w, CUI_FONT_MEDIUM, c, false);
    if (!f->narrow) cui__wf_cell(ui, "Method", 48, CUI_FONT_MEDIUM, c, false);
    cui__wf_cell(ui, "Status", 44, CUI_FONT_MEDIUM, c, false);
    if (!f->narrow) { cui__wf_cell(ui, "Type", 56, CUI_FONT_MEDIUM, c, false); cui__wf_cell(ui, "Size", 60, CUI_FONT_MEDIUM, c, true); }
    int w = cui__node_new(ui, 0);
    ui->nodes[w].w = CUI_FILL; ui->nodes[w].h = CUI_FILL;
    ui->nodes[w].direction = CUI_DIRECTION_LTR;
    ui->nodes[w].paint = cui__paint_wf_head; ui->nodes[w].paint_data = f->head;
    cui__close(ui);
    int line = cui__node_new(ui, 0);
    ui->nodes[line].w = CUI_FILL; ui->nodes[line].h = CUI_PX(1); ui->nodes[line].bg = CUI_C(ui, BORDER);
}

static const char *cui__wf_short(const char *name) {
    if (!name) return "";
    const char *q = name, *slash = NULL;
    for (const char *p = name; *p && *p != '?' && *p != '#'; p++) if (*p == '/' && p[1] && p[1] != '?' && p[1] != '#') slash = p;
    if (slash) q = slash + 1;
    return q;
}

static void cui__wf_row(cui__wff *f, int i, cui_waterfall_result *res) {
    cui_ctx *ui = f->ui;
    const cui_net_request *q = &f->q[i];
    cui_id rid = cui__hash(&i, sizeof i, f->id ^ 0x3EF1u);
    cui_interact ri = cui__interact(ui, rid, CUI_IF_KEEP_FOCUS);
    if (ri.clicked) {
        *f->sel = i; ui->focus = f->lid;
        res->clicked = i;
        if (ui->click_count >= 2) res->activated = i;
    }
    if (ri.hovered) res->hovered = i;
    bool sel = *f->sel == i, focused = ui->focus == f->lid;
    int row = cui__node_new(ui, rid);
    cui_node *rn = &ui->nodes[row];
    rn->dir = CUI_ROW; rn->gap = 8; rn->align = CUI_ALIGN_CENTER; rn->w = CUI_FILL; rn->h = CUI_PX(CUI__WF_ROW);
    rn->pad[3] = 10;
    rn->bg = sel ? (focused ? cui_mix(CUI_C(ui, SURFACE), cui__emphasis(ui), ui->accent == CUI_ACCENT_NEUTRAL ? 0.08f : 0.16f) : CUI_C(ui, ACTIVE))
                 : ri.hovered ? CUI_C(ui, HOVER) : cui__rgba(0, 0, 0, 0);
    cui__mark_interactive(ui, row, 0);
    cui__open(ui, row);
    bool bad = q->status >= 400;
    cui_color fg = bad ? CUI_C(ui, DANGER) : CUI_C(ui, FOREGROUND), mut = bad ? CUI_C(ui, DANGER) : CUI_C(ui, TEXT_SECONDARY);
    int t = cui__text_node(ui, cui__wf_short(q->name), -1, CUI_FONT_BODY, cui__fs(ui, ui->theme.text_sm), fg, false);
    ui->nodes[t].w = CUI_PX(f->name_w); ui->nodes[t].flags |= CUI_NF_CLIP;
    if (!f->narrow) cui__wf_cell(ui, q->method ? q->method : "GET", 48, CUI_FONT_MONO, mut, false);
    cui__wf_cell(ui, q->status > 0 ? cui__frame_printf(ui, "%d", q->status) : "pending", 44, q->status > 0 ? CUI_FONT_MONO : CUI_FONT_BODY,
                 q->status > 0 ? (q->status >= 300 && !bad ? CUI_C(ui, TEXT_MUTED) : mut) : CUI_C(ui, TEXT_MUTED), false);
    if (!f->narrow) {
        cui__wf_cell(ui, q->type ? q->type : "", 56, CUI_FONT_BODY, mut, false);
        cui__wf_cell(ui, q->size > 0 ? cui__size_text(ui, q->size) : "\xe2\x80\x94", 60, CUI_FONT_BODY, mut, true);
    }
    cui_id bid = rid ^ 0xBA2u;
    int b = cui__node_new(ui, bid);
    cui_node *bn = &ui->nodes[b];
    bn->w = CUI_FILL; bn->h = CUI_FILL; bn->direction = CUI_DIRECTION_LTR;
    cui__wf_bar *bar = cui__frame_alloc(ui, sizeof *bar);
    bar->sc = f->head;
    bar->start = q->start;
    memcpy(bar->timing, q->timing, sizeof bar->timing);
    bn->paint = cui__paint_wf_bar; bn->paint_data = bar;
    cui_interact bi = cui__interact(ui, bid, 0);
    cui__close(ui);
    if (bi.hovered && ui->active == 0) {
        /* the breakdown: each phase that took time, then the total */
        char buf[512];
        int len = snprintf(buf, sizeof buf, "%s", q->name ? q->name : "");
        for (int ph = 0; ph < 6 && len < (int)sizeof buf - 64; ph++)
            if (q->timing[ph] > 0) len += snprintf(buf + len, sizeof buf - (size_t)len, "\n%s  %s", cui__wf_names[ph], cui__fl_dur(ui, q->timing[ph]));
        if (len < (int)sizeof buf - 64)
            snprintf(buf + len, sizeof buf - (size_t)len, "\nTotal  %s, from %s", cui__fl_dur(ui, cui__wf_total(q)), cui__fl_dur(ui, q->start));
        cui__tooltip_passive(ui, bid, cui__frame_strdup(ui, buf, -1));
    }
}

static void cui__wf_keys(cui__wff *f, cui_waterfall_result *res) {
    cui_ctx *ui = f->ui;
    int s = *f->sel, ns = s;
    if (cui_key_pressed(ui, CUI_KEY_DOWN)) ns = s < 0 ? 0 : CUI_MIN(f->n - 1, s + 1);
    if (cui_key_pressed(ui, CUI_KEY_UP)) ns = s < 0 ? 0 : CUI_MAX(0, s - 1);
    if (cui_key_pressed(ui, CUI_KEY_HOME)) ns = 0;
    if (cui_key_pressed(ui, CUI_KEY_END)) ns = f->n - 1;
    if (ns != s && f->n > 0) { *f->sel = ns; f->s->scroll_to_sel = true; }
    if (cui_key_pressed(ui, CUI_KEY_ENTER) && *f->sel >= 0) res->activated = *f->sel;
    if (cui_key_pressed(ui, CUI_KEY_ESCAPE)) *f->sel = -1;
}

/* the summary under the table, then what the colors mean; items wrap with no gap between lines */
static void cui__wf_note(cui_ctx *ui, const char *text, cui_color c, const cui_color *swatch) {
    int g = cui__node_new(ui, 0);
    cui_node *gn = &ui->nodes[g];
    gn->dir = CUI_ROW; gn->gap = 5; gn->align = CUI_ALIGN_CENTER;
    gn->pad[0] = gn->pad[2] = 2; gn->pad[1] = swatch ? 10 : 14;
    cui__open(ui, g);
    if (swatch) {
        int sw = cui__node_new(ui, 0);
        ui->nodes[sw].w = ui->nodes[sw].h = CUI_PX(8); ui->nodes[sw].radius = CUI_CORNERS(2); ui->nodes[sw].bg = *swatch;
    }
    cui__text_node(ui, text, -1, CUI_FONT_BODY, cui__fs(ui, ui->theme.text_xs), c, false);
    cui__close(ui);
}

static void cui__wf_footer(cui__wff *f) {
    cui_ctx *ui = f->ui;
    double bytes = 0, fin = 0;
    for (int i = 0; i < f->n; i++) {
        bytes += f->q[i].size > 0 ? f->q[i].size : 0;
        fin = CUI_MAX(fin, f->q[i].start + cui__wf_total(&f->q[i]));
    }
    int line = cui__node_new(ui, 0);
    ui->nodes[line].w = CUI_FILL; ui->nodes[line].h = CUI_PX(1); ui->nodes[line].bg = CUI_C(ui, BORDER);
    int r = cui__node_new(ui, 0);
    cui_node *rn = &ui->nodes[r];
    rn->dir = CUI_ROW; rn->flags |= CUI_NF_ROW_WRAP; rn->w = CUI_FILL; rn->align = CUI_ALIGN_CENTER;
    rn->pad[0] = rn->pad[2] = 5; rn->pad[1] = 0; rn->pad[3] = 10;
    rn->bg = CUI_C(ui, SURFACE_MUTED);
    cui__open(ui, r);
    cui_color c = CUI_C(ui, TEXT_SECONDARY);
    cui__wf_note(ui, cui__frame_printf(ui, f->n == 1 ? "%d request" : "%d requests", f->n), c, NULL);
    cui__wf_note(ui, cui__size_text(ui, bytes), c, NULL);
    cui__wf_note(ui, cui__frame_printf(ui, "Finished in %s", cui__fl_dur(ui, fin)), c, NULL);
    if (f->head->dom > 0) cui__wf_note(ui, cui__frame_printf(ui, "DOMContentLoaded %s", cui__fl_dur(ui, f->head->dom)), cui__tone_color(ui, CUI_TONE_INFO), NULL);
    if (f->head->load > 0) cui__wf_note(ui, cui__frame_printf(ui, "Load %s", cui__fl_dur(ui, f->head->load)), CUI_C(ui, DANGER), NULL);
    for (int ph = 1; ph < 6; ph++) {
        cui_color sw = ph == 4 ? cui_mix(CUI_C(ui, SURFACE), cui__wf_color(ui, ph), 0.55f) : cui__wf_color(ui, ph);
        cui__wf_note(ui, cui__wf_names[ph], CUI_C(ui, TEXT_MUTED), &sw);
    }
    cui__close(ui);
}

cui_waterfall_result cui_waterfall_ex(cui_ctx *ui, const char *id_str, const cui_net_request *requests, int count, const cui_waterfall_opts *o) {
    cui_waterfall_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui_waterfall_result res = { -1, -1, -1 };
    if (!requests || count < 0) count = 0;
    cui__wff f;
    memset(&f, 0, sizeof f);
    f.ui = ui; f.q = requests; f.n = count;
    f.id = cui_make_id(ui, id_str ? id_str : "waterfall");
    f.lid = f.id ^ 0x3EF0u;
    cui__wf_state *s = f.s = cui_state(ui, f.id ^ 0x3EF2u, sizeof *s);
    if (!s->init) { s->init = true; s->sel = -1; }
    f.sel = o->selected ? o->selected : &s->sel;
    if (*f.sel < -1 || *f.sel >= count) *f.sel = -1;
    cui_rect pr = cui__prev_rect(ui, f.id);
    float W = pr.w > 0 ? pr.w : 800;
    f.narrow = W < CUI__WF_NARROW;
    f.name_w = f.narrow ? CUI_CLAMP(W * 0.36f, 90.0f, 170.0f) : CUI_CLAMP(W * 0.26f, 140.0f, 300.0f);
    /* the scale: the last byte or event, with a little room */
    double end = 0;
    for (int i = 0; i < count; i++) end = CUI_MAX(end, requests[i].start + cui__wf_total(&requests[i]));
    end = CUI_MAX(end, CUI_MAX(o->dom_loaded, o->loaded));
    end = end > 0 ? end * 1.04 : 1;
    f.head = cui__frame_alloc(ui, sizeof *f.head);
    memset(f.head, 0, sizeof *f.head);
    f.head->end = end; f.head->dom = o->dom_loaded; f.head->load = o->loaded;

    int outer = cui__node_new(ui, f.id);
    cui_node *on = &ui->nodes[outer];
    on->w = o->width.kind ? o->width : CUI_FILL;
    on->h = o->height < 0 ? CUI_FILL : CUI_PX(o->height > 0 ? o->height : 320);
    on->radius = CUI_CORNERS(ui->theme.radius_panel);
    on->border = 1; on->border_color = CUI_C(ui, BORDER);
    on->bg = CUI_C(ui, SURFACE);
    on->flags |= CUI_NF_CLIP;
    cui__open(ui, outer);
    cui_push_id(ui, id_str ? id_str : "waterfall");
    cui__wf_head(&f);
    cui_interact lr = cui__interact(ui, f.lid, CUI_IF_FOCUSABLE);
    if (lr.focused && !ui->edit_prev) cui__wf_keys(&f, &res);
    int frame = cui__node_new(ui, f.lid);
    cui_node *fn = &ui->nodes[frame];
    fn->w = CUI_FILL; fn->h = CUI_FILL; fn->min_h = 60;
    cui__mark_interactive(ui, frame, CUI_IF_FOCUSABLE);
    cui__focus_ring(ui, frame, lr.focus_visible ? 1.0f : 0.0f);
    cui__open(ui, frame);
    if (s->scroll_to_sel) {
        cui_scroll_state *ss = cui_state(ui, cui_make_id(ui, "rows"), sizeof *ss);
        float y = *f.sel * CUI__WF_ROW, vh = ss->view_h > 0 ? ss->view_h : 240;
        if (*f.sel >= 0) {
            if (y < ss->ty) ss->ty = y;
            else if (y + CUI__WF_ROW > ss->ty + vh) ss->ty = y + CUI__WF_ROW - vh;
        }
        s->scroll_to_sel = false;
    }
    if (count == 0) {
        int e = cui__node_new(ui, 0);
        ui->nodes[e].w = CUI_FILL; ui->nodes[e].h = CUI_FILL; ui->nodes[e].align = CUI_ALIGN_CENTER; ui->nodes[e].justify = CUI_JUSTIFY_CENTER;
        cui__open(ui, e);
        cui__text_node(ui, "No requests", -1, CUI_FONT_BODY, cui__fs(ui, ui->theme.text_sm), CUI_C(ui, TEXT_MUTED), false);
        cui__close(ui);
    } else {
        int first, last;
        cui_virtual_list_begin(ui, "rows", count, CUI__WF_ROW, CUI_FILL, &first, &last);
        for (int i = first; i < last; i++) cui__wf_row(&f, i, &res);
        cui_virtual_list_end(ui);
    }
    cui__close(ui);   /* frame */
    cui__wf_footer(&f);
    cui_pop_id(ui);
    cui__close(ui);   /* outer */
    ui->last_id = f.id;
    return res;
}

/* ======================================================================== */
/* Trace viewer                                                             */
/* ======================================================================== */

/* Lanes of nested spans on one ruler. Spans are sorted by track, depth and start, so a
 * (track, depth) row is a run of the array with starts and ends ascending, found by binary
 * search, and drawn like a flame graph row. Rows are numbered down the panel: a track takes
 * one row per depth, or one row folded. */

#define CUI__TR_GUTTER 150.0f
#define CUI__TR_GUTTER_NARROW 96.0f
#define CUI__TR_NARROW 560.0f
#define CUI__TR_MARKS  18.0f     /* the markers' strip under the ruler */

enum { TR_IDLE = 0, TR_PRESS, TR_PAN, TR_MEASURE };

typedef struct {
    cui__fl_state fl;            /* the zoom animation, as the flame graph's */
    int mode;
    float press_x, press_y, last_x, last_y;
    int press_hit, press_track;
    bool press_double;
    cui_trace_view own_view;
    int own_sel;
    bool init;
} cui__tr_state;

typedef struct { int lo, hi, maxd, row0, rows; } cui__tr_track;

typedef struct {
    const cui_trace_span *s; int n;
    cui_trace_track *tracks; int nt;
    cui__tr_track *tk;
    int rows;
    double dmin, dmax;
    float gutter, plot_x, plot_w, body_y, body_h, row_h;
} cui__tr_ctx;

/* first index in [lo, hi) at or after (track, depth) */
static int cui__tr_lb(const cui_trace_span *s, int lo, int hi, int track, int depth) {
    while (lo < hi) {
        int m = lo + (hi - lo) / 2;
        if (s[m].track < track || (s[m].track == track && s[m].depth < depth)) lo = m + 1; else hi = m;
    }
    return lo;
}
static void cui__tr_row(const cui__tr_ctx *c, int t, int d, int *lo, int *hi) {
    *lo = cui__tr_lb(c->s, c->tk[t].lo, c->tk[t].hi, t, d);
    *hi = cui__tr_lb(c->s, *lo, c->tk[t].hi, t, d + 1);
}
static int cui__tr_end_gt(const cui_trace_span *s, int lo, int hi, double t) {
    while (lo < hi) { int m = lo + (hi - lo) / 2; if (s[m].end <= t) lo = m + 1; else hi = m; }
    return lo;
}
static int cui__tr_start_le(const cui_trace_span *s, int lo, int hi, double t) {
    int a = lo, b = hi;
    while (a < b) { int m = a + (b - a) / 2; if (s[m].start <= t) a = m + 1; else b = m; }
    return a - 1;
}
static int cui__tr_parent(const cui__tr_ctx *c, int i) {
    const cui_trace_span *sp = &c->s[i];
    if (sp->depth <= 0) return -1;
    int lo, hi;
    cui__tr_row(c, sp->track, sp->depth - 1, &lo, &hi);
    int k = cui__tr_start_le(c->s, lo, hi, sp->start);
    return k >= lo && c->s[k].end >= sp->end ? k : -1;
}
static int cui__tr_child(const cui__tr_ctx *c, int i) {
    const cui_trace_span *sp = &c->s[i];
    if (c->tracks[sp->track].collapsed) return -1;
    int lo, hi;
    cui__tr_row(c, sp->track, sp->depth + 1, &lo, &hi);
    int k = cui__tr_end_gt(c->s, lo, hi, sp->start);
    return k < hi && c->s[k].start >= sp->start && c->s[k].start < sp->end ? k : -1;
}

/* the tracks' runs, depths and rows, and the time extent of everything */
static void cui__tr_layout(cui_ctx *ui, cui__tr_ctx *c, const cui_trace_opts *o) {
    size_t nt = (size_t)CUI_MAX(1, c->nt);
    c->tk = cui__frame_alloc(ui, sizeof(cui__tr_track) * nt);
    bool any = false;
    int row = 0, at = 0;
    for (int t = 0; t < c->nt; t++) {
        cui__tr_track *k = &c->tk[t];
        k->lo = cui__tr_lb(c->s, at, c->n, t, 0);
        k->hi = cui__tr_lb(c->s, k->lo, c->n, t + 1, 0);
        at = k->hi;
        k->maxd = k->hi > k->lo ? CUI_CLAMP(c->s[k->hi - 1].depth, 0, 4096) : 0;
        k->row0 = row;
        k->rows = c->tracks[t].collapsed ? 1 : k->maxd + 1;
        row += k->rows;
        for (int d = 0; d <= k->maxd && k->hi > k->lo; d++) {
            int lo, hi;
            cui__tr_row(c, t, d, &lo, &hi);
            if (lo >= hi) continue;
            double a = c->s[lo].start, b = c->s[hi - 1].end;
            if (!any || a < c->dmin) c->dmin = a;
            if (!any || b > c->dmax) c->dmax = b;
            any = true;
        }
    }
    for (int m = 0; m < o->marker_count && o->markers; m++) {
        double tm = o->markers[m].time;
        if (!isfinite(tm)) continue;
        if (!any || tm < c->dmin) c->dmin = tm;
        if (!any || tm > c->dmax) c->dmax = tm;
        any = true;
    }
    c->rows = row;
    if (!any || !(c->dmax > c->dmin) || !isfinite(c->dmin) || !isfinite(c->dmax)) {
        double m = any && isfinite(c->dmin) ? c->dmin : 0;
        c->dmin = m; c->dmax = m + 1;
    }
}

/* the track and depth of a row, or -1 */
static int cui__tr_track_of(const cui__tr_ctx *c, int row, int *depth) {
    for (int t = 0; t < c->nt; t++)
        if (row >= c->tk[t].row0 && row < c->tk[t].row0 + c->tk[t].rows) { *depth = row - c->tk[t].row0; return t; }
    return -1;
}

/* the span under a point in the plot, or -1; *track gets the track under the point */
static int cui__tr_hit(const cui__tr_ctx *c, const cui_trace_view *v, float mx, float my, int *track) {
    *track = -1;
    float yy = my - c->body_y + v->scroll;
    if (yy < 0) return -1;
    int row = (int)floorf(yy / c->row_h), d;
    int t = cui__tr_track_of(c, row, &d);
    *track = t;
    if (t < 0 || mx < c->plot_x - 1 || mx > c->plot_x + c->plot_w + 1 || yy - row * c->row_h > c->row_h - 1) return -1;
    int lo, hi;
    cui__tr_row(c, t, d, &lo, &hi);
    if (lo >= hi) return -1;
    double range = v->end - v->start;
    double tm = v->start + (double)(mx - c->plot_x) / c->plot_w * range, tol = range / c->plot_w;
    int k = cui__tr_start_le(c->s, lo, hi, tm);
    if (k >= lo && c->s[k].end >= tm) return k;
    int best = -1;
    double bd = tol;
    if (k >= lo && tm - c->s[k].end <= bd) { bd = tm - c->s[k].end; best = k; }
    if (k + 1 < hi && c->s[k + 1].start - tm <= bd) best = k + 1;
    return best;
}

static void cui__tr_clamp(const cui__tr_ctx *c, cui_trace_view *v) {
    double ext = c->dmax - c->dmin, range = v->end - v->start;
    if (!(range > 0) || range > ext) range = ext;
    range = CUI_MAX(range, ext * 1e-9);
    double st = v->start;
    if (!(st >= c->dmin)) st = c->dmin;
    if (st + range > c->dmax) st = c->dmax - range;
    v->start = st; v->end = st + range;
    float max_s = CUI_MAX(0.0f, c->rows * c->row_h - c->body_h);
    if (!(v->scroll >= 0)) v->scroll = 0;
    if (v->scroll > max_s) v->scroll = max_s;
}

static void cui__tr_animate(cui__tr_state *st, const cui_trace_view *v, double a, double b) {
    cui_flame_view fv = { v->start, v->end, v->scroll };
    cui__fl_animate_to(&st->fl, &fv, a, b);
}

static void cui__tr_reveal(const cui__tr_ctx *c, cui_trace_view *v, int i) {
    const cui_trace_span *sp = &c->s[i];
    double range = v->end - v->start;
    if (sp->end < v->start || sp->start > v->end) { double mid = (sp->start + sp->end) * 0.5; v->start = mid - range * 0.5; v->end = mid + range * 0.5; }
    float top = (c->tk[sp->track].row0 + sp->depth) * c->row_h;
    if (top < v->scroll) v->scroll = top;
    if (top + c->row_h > v->scroll + c->body_h) v->scroll = top + c->row_h - c->body_h;
    cui__tr_clamp(c, v);
}

/* ---- painting ---- */

typedef struct { const char *name; int row0, rows; bool foldable, folded; } cui__tr_lane;
typedef struct { const char *name; double time; cui_tone tone; } cui__tr_mark;

typedef struct {
    cui__fl_paint fp;            /* bars, view, rows, as the flame graph paints them */
    cui_id wid;
    float gutter, marks_h;
    cui__tr_lane *lanes; int nl;
    cui__tr_mark *marks; int nm;
    double mark0, mark1;
    int hover_lane;
} cui__tr_paint;

static void cui__tr_paint_marks(cui_ctx *ui, const cui__tr_paint *p, cui_rect plot, float px0, float k, float strip_y, float bottom) {
    float fs = cui__fs(ui, ui->theme.text_xs), fh = cui__font_line_height(ui, CUI_FONT_BODY, fs);
    for (int m = 0; m < p->nm; m++) {
        float x = floorf(px0 + (float)((p->marks[m].time - p->fp.t0) * k));
        if (x < plot.x - 40 || x > plot.x + plot.w) continue;
        cui_color c = p->marks[m].tone ? cui__tone_color(ui, p->marks[m].tone) : CUI_C(ui, TEXT_MUTED);
        cui_draw_rect(ui, (cui_rect){ x, strip_y, 1, bottom - strip_y }, CUI_CORNERS(0), cui_alpha(c, 0.55f));
        cui_vec2 tri[3] = { { x - 4, strip_y + 3 }, { x + 5, strip_y + 3 }, { x + 0.5f, strip_y + 9 } };
        cui_draw_polygon(ui, tri, 3, c);
        if (p->marks[m].name && x >= plot.x) {
            float next = plot.x + plot.w;
            for (int j = 0; j < p->nm; j++) {
                float xj = px0 + (float)((p->marks[j].time - p->fp.t0) * k);
                if (xj > x + 1 && xj < next) next = xj;
            }
            float avail = next - (x + 8) - 6;
            if (avail >= 24)
                cui__draw_text_ellipsis(ui, CUI_FONT_BODY, fs, x + 8, strip_y + (CUI__TR_MARKS - fh) * 0.5f, avail, CUI_C(ui, TEXT_SECONDARY), p->marks[m].name, -1, false);
        }
    }
}

static void cui__tr_paint_measure(cui_ctx *ui, const cui__tr_paint *p, cui_rect plot, float px0, float k, cui_rect ruler) {
    if (!(p->mark1 > p->mark0)) return;
    float x0 = px0 + (float)((p->mark0 - p->fp.t0) * k), x1 = px0 + (float)((p->mark1 - p->fp.t0) * k);
    x0 = CUI_MAX(x0, plot.x - 2); x1 = CUI_MIN(x1, plot.x + plot.w + 2);
    if (x1 <= x0) return;
    cui_color em = cui__emphasis(ui);
    cui_draw_rect(ui, (cui_rect){ x0, plot.y, x1 - x0, plot.h }, CUI_CORNERS(0), cui_alpha(em, 0.08f));
    cui_draw_rect(ui, (cui_rect){ x0, plot.y, 1, plot.h }, CUI_CORNERS(0), cui_alpha(em, 0.7f));
    cui_draw_rect(ui, (cui_rect){ x1 - 1, plot.y, 1, plot.h }, CUI_CORNERS(0), cui_alpha(em, 0.7f));
    /* the length, in a pill on the ruler */
    const char *lbl = cui__fl_dur(ui, p->mark1 - p->mark0);
    float fs = cui__fs(ui, ui->theme.text_xs), fh = cui__font_line_height(ui, CUI_FONT_MEDIUM, fs);
    float lw = cui__text_width(ui, CUI_FONT_MEDIUM, fs, lbl, -1, true) + 12;
    float cx = CUI_CLAMP((x0 + x1) * 0.5f, plot.x + lw * 0.5f + 2, plot.x + plot.w - lw * 0.5f - 2);
    cui_rect pill = { cx - lw * 0.5f, ruler.y + 3, lw, ruler.h - 6 };
    cui_draw_rect(ui, pill, CUI_CORNERS(pill.h * 0.5f), em);
    cui__draw_text_line(ui, CUI_FONT_MEDIUM, fs, pill.x + 6, pill.y + (pill.h - fh) * 0.5f, cui__legible(CUI_C(ui, SURFACE), em, 4.5f), lbl, -1, true);
}

/* the lanes' names, with a chevron when a lane folds */
static void cui__tr_paint_gutter(cui_ctx *ui, const cui__tr_paint *p, cui_rect g, float body_y) {
    float fs = cui__fs(ui, ui->theme.text_xs), fh = cui__font_line_height(ui, CUI_FONT_MEDIUM, fs);
    float rh = p->fp.row_h;
    for (int t = 0; t < p->nl; t++) {
        const cui__tr_lane *l = &p->lanes[t];
        float y = body_y + l->row0 * rh - p->fp.scroll;
        if (y > g.y + g.h || y + l->rows * rh < g.y) continue;
        float x = g.x + 8;
        cui_color tc = t == p->hover_lane && l->foldable ? CUI_C(ui, FOREGROUND) : CUI_C(ui, TEXT_SECONDARY);
        if (l->foldable) {
            float cy = y + (rh - 1) * 0.5f;
            cui_vec2 tri[3];
            if (l->folded) { tri[0] = (cui_vec2){ x + 1, cy - 4 }; tri[1] = (cui_vec2){ x + 1, cy + 4 }; tri[2] = (cui_vec2){ x + 6, cy }; }
            else { tri[0] = (cui_vec2){ x - 1, cy - 2 }; tri[1] = (cui_vec2){ x + 7, cy - 2 }; tri[2] = (cui_vec2){ x + 3, cy + 3 }; }
            cui_draw_polygon(ui, tri, 3, cui_alpha(tc, 0.8f));
        }
        x += 12;
        cui__draw_text_ellipsis(ui, CUI_FONT_MEDIUM, fs, x, y + (rh - 1 - fh) * 0.5f, g.x + g.w - x - 6, tc, l->name, -1, false);
    }
}

static void cui__paint_trace(cui_ctx *ui, cui_node *n, void *data) {
    cui__tr_paint *p = data;
    cui__fl_paint *fp = &p->fp;
    cui_rect r = n->rect;
    float G = p->gutter, rad = ui->theme.radius_panel;
    float head = CUI__FL_RULER + p->marks_h;
    cui_rect plot = { r.x + G, r.y + head + 1, CUI_MAX(0.0f, r.w - G - 1), CUI_MAX(0.0f, r.h - head - 2) };
    cui__paint_wheel_hit(ui, p->wid, (cui_rect){ r.x, r.y + head, r.w, r.h - head });
    float px0 = plot.x + CUI__FL_PADX, pw = CUI_MAX(1.0f, plot.w - 2 * CUI__FL_PADX);
    double range = fp->t1 - fp->t0;
    float k = range > 0 ? (float)(pw / range) : 0;
    float body_y = plot.y + CUI__FL_PADY;
    cui_push_clip(ui, cui__rect_inset(r, 1));
    cui_draw_rect(ui, (cui_rect){ r.x, r.y, r.w, head }, (cui_corners){ rad - 1, rad - 1, 0, 0 }, CUI_C(ui, SURFACE_MUTED));
    cui_draw_rect(ui, (cui_rect){ r.x, r.y + head, r.w, 1 }, CUI_CORNERS(0), CUI_C(ui, BORDER));
    cui_draw_rect(ui, (cui_rect){ r.x + G - 1, r.y + head + 1, 1, r.h - head - 1 }, CUI_CORNERS(0), CUI_C(ui, BORDER_SUBTLE));
    /* lanes: every other one a shade darker, a line between them */
    cui_push_clip(ui, (cui_rect){ r.x, plot.y, r.w, plot.h });
    for (int t = 0; t < p->nl; t++) {
        float y = body_y + p->lanes[t].row0 * fp->row_h - fp->scroll, h = p->lanes[t].rows * fp->row_h;
        if (y > plot.y + plot.h || y + h < plot.y) continue;
        if (t & 1) cui_draw_rect(ui, (cui_rect){ r.x, y - 1, r.w, h + 1 }, CUI_CORNERS(0), cui_mix(CUI_C(ui, SURFACE), CUI_C(ui, TEXT_MUTED), 0.04f));
        if (t > 0) cui_draw_rect(ui, (cui_rect){ r.x, y - 1, r.w, 1 }, CUI_CORNERS(0), CUI_C(ui, BORDER_SUBTLE));
    }
    cui__tr_paint_gutter(ui, p, (cui_rect){ r.x, plot.y, G - 1, plot.h }, body_y);
    cui_pop_clip(ui);
    cui_push_clip(ui, (cui_rect){ plot.x, r.y, plot.w, r.h });
    if (k > 0) cui__fl_ruler(ui, (cui_rect){ plot.x, r.y, plot.w, CUI__FL_RULER }, px0, pw, fp->t0, fp->t1, fp->origin, r.h - CUI__FL_RULER - 2);
    cui_push_clip(ui, plot);
    for (int i = 0; i < fp->n; i++) cui__fl_bar(ui, fp, &fp->items[i], px0, k, body_y, plot, false);
    if (fp->has_sel) cui__fl_bar(ui, fp, &fp->sel, px0, k, body_y, plot, true);
    cui_pop_clip(ui);
    if (k > 0) cui__tr_paint_marks(ui, p, plot, px0, k, r.y + CUI__FL_RULER + 1, plot.y + plot.h);
    if (k > 0) cui__tr_paint_measure(ui, p, plot, px0, k, (cui_rect){ plot.x, r.y, plot.w, CUI__FL_RULER });
    if (fp->empty) {
        float fs = cui__fs(ui, ui->theme.text_sm);
        float lw = cui__text_width(ui, CUI_FONT_BODY, fs, "No spans", -1, false);
        cui__draw_text_line(ui, CUI_FONT_BODY, fs, plot.x + (plot.w - lw) * 0.5f, plot.y + plot.h * 0.5f - 8, CUI_C(ui, TEXT_MUTED), "No spans", -1, false);
    }
    float view_h = plot.h - 2 * CUI__FL_PADY;
    if (fp->content_h > view_h + 0.5f && view_h > 8) {
        float th = CUI_MAX(16.0f, view_h * view_h / fp->content_h);
        float ty = plot.y + CUI__FL_PADY + (view_h - th) * (fp->scroll / (fp->content_h - view_h));
        cui_draw_rect(ui, (cui_rect){ plot.x + plot.w - 5, ty, 3, th }, CUI_CORNERS(1.5f), cui_alpha(CUI_C(ui, TEXT_MUTED), 0.4f));
    }
    cui_pop_clip(ui);
    if (fp->focus > 0.01f) cui__draw_focus_ring(ui, r, CUI_CORNERS(rad), fp->focus);
    cui_pop_clip(ui);
}

/* ---- the component ---- */

typedef struct {
    cui_ctx *ui;
    const cui_trace_opts *o;
    cui_trace_result *res;
    cui_id id, wid;
    cui__tr_state *st;
    cui_trace_view *v;
    int *sel;
    cui__tr_ctx c;
    cui_rect R;
    bool has_rect;
    cui_interact bi;
    float mx, my;
    bool user_moved;
    int hov, hov_track;
} cui__trf;

static double cui__tr_time_at(const cui__trf *f, float x) {
    return f->v->start + (double)(x - f->c.plot_x) / f->c.plot_w * (f->v->end - f->v->start);
}

/* wheel over the plot zooms around the pointer, Shift or a sideways scroll pans; over the
 * lane names it scrolls the lanes */
static void cui__trf_wheel(cui__trf *f) {
    cui_ctx *ui = f->ui;
    cui_trace_view *v = f->v;
    if (!(ui->scroll_target == f->wid && ui->hot_layer == ui->current_layer && (ui->scroll_dx != 0 || ui->scroll_dy != 0))) return;
    double range = v->end - v->start;
    if (f->mx < f->R.x + f->c.gutter) v->scroll -= ui->scroll_dy * 40;
    else if (ui->scroll_dx != 0 || (ui->mods & CUI_MOD_SHIFT)) {
        float dl = ui->scroll_dx != 0 ? ui->scroll_dx : ui->scroll_dy;
        double dt = (double)dl * 40.0 / f->c.plot_w * range;
        v->start -= dt; v->end -= dt;
    } else {
        double z = pow(1.12, (double)ui->scroll_dy);
        double nr = CUI_MIN(range / z, f->c.dmax - f->c.dmin);
        double u = CUI_CLAMP((double)(f->mx - f->c.plot_x) / f->c.plot_w, 0.0, 1.0);
        double tm = v->start + u * range;
        v->start = tm - u * nr; v->end = v->start + nr;
    }
    ui->scroll_dx = ui->scroll_dy = 0;
    f->user_moved = true;
}

/* a press on a lane's name folds it; Shift+drag measures; a drag pans; a click selects; a
 * double click zooms to the span, or out to everything */
static void cui__trf_pointer(cui__trf *f) {
    cui_ctx *ui = f->ui;
    cui__tr_state *st = f->st;
    cui_trace_view *v = f->v;
    float mx = f->mx, my = f->my;
    if (f->bi.hovered && ui->mpressed[0] && st->mode == TR_IDLE && my >= f->c.body_y - CUI__FL_PADY) {
        st->press_x = st->last_x = mx; st->press_y = st->last_y = my;
        st->press_hit = cui__tr_hit(&f->c, v, mx, my, &st->press_track);
        st->press_double = f->bi.double_clicked;
        if (mx < f->R.x + f->c.gutter) {
            int t = st->press_track;
            if (t >= 0 && f->c.tk[t].maxd > 0) { f->c.tracks[t].collapsed = !f->c.tracks[t].collapsed; f->user_moved = true; }
        } else if (ui->mods & CUI_MOD_SHIFT) {
            st->mode = TR_MEASURE;
            v->mark_start = v->mark_end = cui__tr_time_at(f, mx);
        } else st->mode = TR_PRESS;
    }
    if (st->mode == TR_MEASURE) {
        double a = cui__tr_time_at(f, st->press_x), b = cui__tr_time_at(f, mx);
        v->mark_start = CUI_MIN(a, b); v->mark_end = CUI_MAX(a, b);
        cui__set_cursor(ui, CUI_CURSOR_RESIZE_EW);
        if (!ui->mdown[0]) st->mode = TR_IDLE;
        return;
    }
    if (st->mode == TR_PRESS && ui->mdown[0]) {
        float dx = mx - st->press_x, dy = my - st->press_y;
        if (dx * dx + dy * dy > CUI__FL_DRAG * CUI__FL_DRAG) st->mode = TR_PAN;
    }
    if (st->mode == TR_PAN) {
        double dt = (double)(mx - st->last_x) / f->c.plot_w * (v->end - v->start);
        v->start -= dt; v->end -= dt;
        v->scroll -= my - st->last_y;
        st->last_x = mx; st->last_y = my;
        cui__set_cursor(ui, CUI_CURSOR_MOVE);
        f->user_moved = true;
        if (!ui->mdown[0]) st->mode = TR_IDLE;
    } else if (st->mode == TR_PRESS && !ui->mdown[0]) {
        st->mode = TR_IDLE;
        int h = st->press_hit;
        *f->sel = h;
        if (st->press_double) {
            if (h >= 0) {
                const cui_trace_span *sp = &f->c.s[h];
                double d = CUI_MAX(sp->end - sp->start, (f->c.dmax - f->c.dmin) * 1e-9);
                cui__tr_animate(st, v, sp->start - d * 0.05, sp->start + d * 1.05);
            } else cui__tr_animate(st, v, f->c.dmin, f->c.dmax);
        }
    }
    if (st->mode != TR_IDLE && !ui->mdown[0]) st->mode = TR_IDLE;
}

/* keys: Left and Right along the row, Up to the parent, Down to the first child; + and -
 * zoom; Enter zooms to the selection; F fits; Esc clears the measurement, then the selection */
static void cui__trf_keys(cui__trf *f) {
    cui_ctx *ui = f->ui;
    cui_trace_view *v = f->v;
    const cui__tr_ctx *c = &f->c;
    int s = *f->sel, ns = s;
    bool L = cui_key_pressed(ui, CUI_KEY_LEFT), Rk = cui_key_pressed(ui, CUI_KEY_RIGHT), U = cui_key_pressed(ui, CUI_KEY_UP), D = cui_key_pressed(ui, CUI_KEY_DOWN);
    if (L || Rk || U || D) {
        if (s < 0) {
            for (int t = 0; t < c->nt && ns < 0; t++) {
                int lo, hi;
                cui__tr_row(c, t, 0, &lo, &hi);
                int k = cui__tr_end_gt(c->s, lo, hi, v->start);
                if (k < hi) ns = k;
            }
        } else if (L) { if (s > 0 && c->s[s - 1].track == c->s[s].track && c->s[s - 1].depth == c->s[s].depth) ns = s - 1; }
        else if (Rk) { if (s + 1 < c->n && c->s[s + 1].track == c->s[s].track && c->s[s + 1].depth == c->s[s].depth) ns = s + 1; }
        else if (U) { int pk = cui__tr_parent(c, s); if (pk >= 0) ns = pk; }
        else { int ck = cui__tr_child(c, s); if (ck >= 0) ns = ck; }
        if (ns >= 0 && ns != s) { *f->sel = ns; cui__tr_reveal(c, v, ns); f->user_moved = true; }
        ui->kpressed[CUI_KEY_LEFT] = ui->kpressed[CUI_KEY_RIGHT] = ui->kpressed[CUI_KEY_UP] = ui->kpressed[CUI_KEY_DOWN] = 0;
    }
    if (cui_key_pressed(ui, CUI_KEY_EQUAL) || cui_key_pressed(ui, CUI_KEY_MINUS)) {
        double cx = (v->start + v->end) * 0.5, z = cui_key_pressed(ui, CUI_KEY_EQUAL) ? 1 / 1.25 : 1.25;
        double nr = CUI_MIN((v->end - v->start) * z, c->dmax - c->dmin);
        v->start = cx - nr * 0.5; v->end = cx + nr * 0.5;
        f->user_moved = true;
        ui->kpressed[CUI_KEY_EQUAL] = ui->kpressed[CUI_KEY_MINUS] = 0;
    }
    if (cui_key_pressed(ui, CUI_KEY_ENTER) && *f->sel >= 0) {
        const cui_trace_span *sp = &c->s[*f->sel];
        double d = CUI_MAX(sp->end - sp->start, (c->dmax - c->dmin) * 1e-9);
        cui__tr_animate(f->st, v, sp->start - d * 0.05, sp->start + d * 1.05);
        ui->kpressed[CUI_KEY_ENTER] = 0;
    }
    if (cui_key_pressed(ui, CUI_KEY_F) && !(ui->mods & CUI_MOD_PRIMARY)) { cui__tr_animate(f->st, v, c->dmin, c->dmax); ui->kpressed[CUI_KEY_F] = 0; }
    if (cui_key_pressed(ui, CUI_KEY_ESCAPE)) {
        if (v->mark_end > v->mark_start) v->mark_start = v->mark_end = 0;
        else *f->sel = -1;
        ui->kpressed[CUI_KEY_ESCAPE] = 0;
    }
}

static void cui__trf_move_view(cui__trf *f) {
    cui__tr_state *st = f->st;
    cui_trace_view *v = f->v;
    if (f->user_moved) st->fl.anim_on = false;
    if (st->fl.anim_on) {
        float pr = cui__spring(f->ui, &st->fl.anim, 1.0f, CUI__SPRING_EXPAND);
        double a, b;
        cui__fl_lerp(&st->fl, pr >= 1.0f ? 1.0 : (double)pr, &a, &b);
        v->start = a; v->end = b;
        if (pr >= 1.0f && st->fl.anim.v == 0) { v->start = st->fl.b0; v->end = st->fl.b1; st->fl.anim_on = false; }
    }
    cui__tr_clamp(&f->c, v);
}

/* one row's spans in view; spans thinner than a point merge into runs, as in the flame graph */
static void cui__trf_row_items(cui__trf *f, cui__fl_paint *p, int t, int d, int row, double k, cui_color run_fill) {
    cui_ctx *ui = f->ui;
    const cui_trace_span *s = f->c.s;
    double t0 = f->v->start, t1 = f->v->end;
    int lo, hi;
    cui__tr_row(&f->c, t, d, &lo, &hi);
    int i = cui__tr_end_gt(s, lo, hi, t0);
    while (i < hi && s[i].start < t1) {
        double xs = (s[i].start - t0) * k, xe = (s[i].end - t0) * k;
        cui__fl_item it;
        memset(&it, 0, sizeof it);
        it.row = row;
        if (xe - xs >= 1.0) {
            uint32_t key = cui__fl_name_key(s[i].name, s[i].color_key);
            it.s = s[i].start; it.e = s[i].end;
            it.kind = i == f->hov ? FL_HOVER : FL_SPAN;
            it.fill = cui__fl_fill(ui, key, i == f->hov);
            if (xe - xs >= CUI__FL_LABEL && s[i].name && s[i].name[0]) {
                it.fg = cui__fl_fg(ui, key, it.fill);
                it.label_len = (int)strlen(s[i].name);
                it.label = cui__frame_strdup(ui, s[i].name, it.label_len);
            }
            cui__fl_push(ui, p, &it);
            i++;
            continue;
        }
        double rx0 = xs, rx1 = xe;
        i++;
        for (;;) {
            int j = cui__tr_end_gt(s, i, hi, t0 + (rx1 + 1.0) / k);
            if (j > i) { double e2 = (s[j - 1].end - t0) * k; if (e2 > rx1) rx1 = e2; i = j; }
            if (i < hi) {
                double ns = (s[i].start - t0) * k, ne = (s[i].end - t0) * k;
                if (ne - ns < 1.0 && ns <= rx1 + 1.0) { rx1 = ne; i++; continue; }
            }
            break;
        }
        if (rx1 - rx0 >= 1.0) {
            it.s = t0 + rx0 / k; it.e = t0 + rx1 / k;
            it.kind = FL_RUN;
            it.fill = run_fill;
            cui__fl_push(ui, p, &it);
        }
    }
}

static void cui__trf_build(cui__trf *f, int *node, cui__tr_paint **out) {
    cui_ctx *ui = f->ui;
    const cui_trace_opts *o = f->o;
    const cui__tr_ctx *c = &f->c;
    cui_trace_view *v = f->v;
    int en = cui__node_new(ui, f->id);
    cui_node *n = &ui->nodes[en];
    n->w = o->width.kind ? o->width : CUI_FILL;
    float head = CUI__FL_RULER + (o->marker_count > 0 ? CUI__TR_MARKS : 0);
    float def_h = head + 1 + 2 * CUI__FL_PADY + CUI_MAX(4, c->rows) * c->row_h + 1;
    n->h = CUI_PX(o->height > 0 ? o->height : CUI_MIN(320.0f, def_h));
    n->bg = CUI_C(ui, SURFACE);
    n->border = 1; n->border_color = CUI_C(ui, BORDER);
    n->radius = CUI_CORNERS(ui->theme.radius_panel);
    n->flags |= CUI_NF_CLIP;
    n->direction = CUI_DIRECTION_LTR;   /* time runs left to right */
    cui__mark_interactive(ui, en, CUI_IF_FOCUSABLE);
    cui__tr_paint *p = cui__frame_alloc(ui, sizeof *p);
    memset(p, 0, sizeof *p);
    cui__fl_paint *fp = &p->fp;
    fp->t0 = v->start; fp->t1 = v->end; fp->origin = c->dmin;
    fp->row_h = c->row_h; fp->scroll = v->scroll; fp->content_h = c->rows * c->row_h;
    fp->empty = c->n == 0;
    cui__tween(ui, &f->st->fl.focus, f->bi.focus_visible ? 1.0f : 0.0f, CUI__T_FOCUS);
    fp->focus = f->st->fl.focus;
    p->wid = f->wid; p->gutter = c->gutter; p->marks_h = head - CUI__FL_RULER;
    p->mark0 = v->mark_start; p->mark1 = v->mark_end;
    p->hover_lane = f->bi.hovered && f->mx < f->R.x + c->gutter ? f->hov_track : -1;
    p->nl = c->nt;
    p->lanes = cui__frame_alloc(ui, sizeof(cui__tr_lane) * (size_t)CUI_MAX(1, c->nt));
    for (int t = 0; t < c->nt; t++) {
        cui__tr_lane *l = &p->lanes[t];
        l->name = cui__frame_strdup(ui, c->tracks[t].name ? c->tracks[t].name : "", -1);
        l->row0 = c->tk[t].row0; l->rows = c->tk[t].rows;
        l->foldable = c->tk[t].maxd > 0; l->folded = c->tracks[t].collapsed;
    }
    p->nm = o->markers ? CUI_MAX(0, o->marker_count) : 0;
    p->marks = cui__frame_alloc(ui, sizeof(cui__tr_mark) * (size_t)CUI_MAX(1, p->nm));
    for (int m = 0; m < p->nm; m++) {
        p->marks[m].time = o->markers[m].time; p->marks[m].tone = o->markers[m].tone;
        p->marks[m].name = o->markers[m].name ? cui__frame_strdup(ui, o->markers[m].name, -1) : NULL;
    }
    /* the rows in view */
    if (c->n > 0 && v->end > v->start) {
        double k = c->plot_w / (v->end - v->start);
        int r0 = CUI_MAX(0, (int)floorf(v->scroll / c->row_h)), r1 = CUI_MIN(c->rows - 1, (int)floorf((v->scroll + c->body_h) / c->row_h));
        cui_color run_fill = cui_mix(CUI_C(ui, SURFACE), CUI_C(ui, TEXT_MUTED), ui->mode == CUI_THEME_DARK ? 0.32f : 0.26f);
        for (int row = r0; row <= r1; row++) {
            int d, t = cui__tr_track_of(c, row, &d);
            if (t >= 0) cui__trf_row_items(f, fp, t, d, row, k, run_fill);
        }
        int s = *f->sel;
        if (s >= 0) {
            const cui_trace_span *sp = &c->s[s];
            int row = c->tk[sp->track].row0 + sp->depth;
            if ((!c->tracks[sp->track].collapsed || sp->depth == 0) && row >= r0 && row <= r1 && sp->end >= v->start && sp->start <= v->end) {
                uint32_t key = cui__fl_name_key(sp->name, sp->color_key);
                cui__fl_item it;
                memset(&it, 0, sizeof it);
                it.row = row; it.s = sp->start; it.e = sp->end; it.kind = FL_SPAN;
                it.fill = cui__fl_fill(ui, key, s == f->hov);
                if ((sp->end - sp->start) * k >= CUI__FL_LABEL && sp->name && sp->name[0]) {
                    it.fg = cui__fl_fg(ui, key, it.fill);
                    it.label_len = (int)strlen(sp->name);
                    it.label = cui__frame_strdup(ui, sp->name, it.label_len);
                }
                fp->sel = it; fp->has_sel = true;
            }
        }
    }
    n->paint = cui__paint_trace;
    n->paint_data = p;
    *node = en; *out = p;
}

static void cui__trf_tooltip(cui__trf *f) {
    cui_ctx *ui = f->ui;
    int hov = f->hov;
    if (!(hov >= 0 && ui->active == 0 && !ui->mdown[0])) return;
    const cui_trace_span *sp = &f->c.s[hov];
    cui_id tid = cui__hash(&hov, sizeof hov, f->id ^ 0x7A1Cu);
    float ry = f->c.body_y + (f->c.tk[sp->track].row0 + sp->depth) * f->c.row_h - f->v->scroll;
    cui__absolute(ui, tid, f->mx - f->R.x - 1, ry - f->R.y, 2, f->c.row_h - 1);
    const char *track = f->c.tracks[sp->track].name;
    const char *tip = cui__frame_printf(ui, "%s\n%s \xc2\xb7 at %s%s%s", sp->name && sp->name[0] ? sp->name : "Unnamed span",
                                        cui__fl_dur(ui, sp->end - sp->start), cui__fl_dur(ui, sp->start - f->c.dmin),
                                        track && track[0] ? " \xc2\xb7 " : "", track ? track : "");
    cui__tooltip_passive(ui, tid, tip);
}

cui_trace_result cui_trace_view_ex(cui_ctx *ui, const char *id_str, cui_trace_track *tracks, int track_count,
                                   const cui_trace_span *spans, int span_count, cui_trace_view *view, int *selected, const cui_trace_opts *o) {
    cui_trace_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui_trace_result res = { false, false, -1 };
    cui__trf f;
    memset(&f, 0, sizeof f);
    f.ui = ui; f.o = o; f.res = &res;
    f.id = cui_make_id(ui, id_str ? id_str : "trace");
    f.wid = f.id ^ 0x7A1Du;
    cui__tr_state *st = f.st = cui_state(ui, f.id ^ 0x7A1Eu, sizeof *st);
    if (!st->init) { st->own_sel = -1; st->init = true; }
    cui_trace_view *v = f.v = view ? view : &st->own_view;
    int *sel = f.sel = selected ? selected : &st->own_sel;
    if (!tracks || track_count < 0) track_count = 0;
    if (!spans || span_count < 0 || track_count == 0) span_count = 0;
    /* spans past the last track are ignored */
    while (span_count > 0 && (spans[span_count - 1].track >= track_count || spans[span_count - 1].track < 0)) span_count--;
    int sel_before = *sel;
    if (*sel < -1 || *sel >= span_count) *sel = -1;
    cui__tr_ctx *c = &f.c;
    c->s = spans; c->n = span_count; c->tracks = tracks; c->nt = track_count;
    c->row_h = o->row_height > 0 ? o->row_height : CUI__FL_ROW;
    cui__tr_layout(ui, c, o);
    f.has_rect = cui__has_prev_rect(ui, f.id);
    f.R = cui__prev_rect(ui, f.id);
    c->gutter = f.has_rect && f.R.w < CUI__TR_NARROW ? CUI__TR_GUTTER_NARROW : CUI__TR_GUTTER;
    float head = CUI__FL_RULER + (o->marker_count > 0 ? CUI__TR_MARKS : 0);
    c->plot_x = f.R.x + c->gutter + CUI__FL_PADX;
    c->plot_w = f.has_rect ? CUI_MAX(1.0f, f.R.w - c->gutter - 1 - 2 * CUI__FL_PADX) : 600;
    c->body_y = f.R.y + head + 1 + CUI__FL_PADY;
    c->body_h = f.has_rect ? CUI_MAX(1.0f, f.R.h - head - 2 - 2 * CUI__FL_PADY) : 200;

    double b0 = v->start, b1 = v->end;
    float bs = v->scroll;
    if (!(v->end > v->start) || !isfinite(v->start) || !isfinite(v->end)) { v->start = c->dmin; v->end = c->dmax; st->fl.anim_on = false; }
    f.bi = cui__interact(ui, f.id, CUI_IF_FOCUSABLE | CUI_IF_DRAG);
    f.mx = ui->mx; f.my = ui->my;
    if (f.has_rect) {
        cui__trf_wheel(&f);
        cui__trf_pointer(&f);
        if (f.bi.focused && !ui->edit_prev) cui__trf_keys(&f);
        if (f.user_moved) cui__tr_layout(ui, c, o);   /* a lane folded or unfolded */
        cui__trf_move_view(&f);
    }
    if (*sel >= c->n) *sel = -1;
    f.hov = -1; f.hov_track = -1;
    if (f.has_rect && f.bi.hovered && st->mode != TR_PAN && cui__rect_contains(f.R, f.mx, f.my))
        f.hov = cui__tr_hit(c, v, f.mx, f.my, &f.hov_track);
    if (f.hov >= 0 && c->tracks[c->s[f.hov].track].collapsed && c->s[f.hov].depth > 0) f.hov = -1;
    res.hovered = f.hov;
    res.selection_changed = *sel != sel_before;
    res.view_changed = v->start != b0 || v->end != b1 || v->scroll != bs;

    int en;
    cui__tr_paint *p;
    cui__trf_build(&f, &en, &p);
    cui__open(ui, en);
    cui__trf_tooltip(&f);
    cui__close(ui);
    ui->last_id = f.id;
    return res;
}

static int cui__tr_cmp(const void *a, const void *b) {
    const cui_trace_span *x = a, *y = b;
    if (x->track != y->track) return x->track < y->track ? -1 : 1;
    if (x->depth != y->depth) return x->depth < y->depth ? -1 : 1;
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    if (x->end != y->end) return x->end > y->end ? -1 : 1;
    return 0;
}

void cui_trace_sort(cui_trace_span *spans, int count) {
    if (spans && count > 1) qsort(spans, (size_t)count, sizeof *spans, cui__tr_cmp);
}
