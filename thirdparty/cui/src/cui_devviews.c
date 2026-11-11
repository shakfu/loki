/* Developer views drawn on a grid of their own: uptime bars, the time
 * histogram, the coverage tree and blame. The time axes and the bars keep
 * their left-to-right order in a right-to-left interface; names, numbers and
 * columns around them mirror like any other layout. */
#include "cui_internal.h"
#include <time.h>

/* "now", "5 min ago", "3 hours ago", "2 days ago", "3 weeks ago", "4 months ago", "2 years ago" */
static const char *cui__dv_age(cui_ctx *ui, int64_t dt) {
    if (dt < 60) return "now";
    if (dt < 3600) return cui__frame_printf(ui, "%d min ago", (int)(dt / 60));
    if (dt < 86400) { int h = (int)(dt / 3600); return h == 1 ? "1 hour ago" : cui__frame_printf(ui, "%d hours ago", h); }
    if (dt < 86400 * 14) { int d = (int)(dt / 86400); return d == 1 ? "yesterday" : cui__frame_printf(ui, "%d days ago", d); }
    if (dt < 86400 * 60) return cui__frame_printf(ui, "%d weeks ago", (int)(dt / (86400 * 7)));
    if (dt < 86400 * 365) return cui__frame_printf(ui, "%d months ago", (int)(dt / (86400 * 30)));
    int y = (int)(dt / (86400 * 365));
    return y == 1 ? "1 year ago" : cui__frame_printf(ui, "%d years ago", y);
}

/* ======================================================================== */
/* Uptime bars                                                              */
/* ======================================================================== */

#define CUI__UP_BAR_H 30.0f

static cui_color cui__up_color(cui_ctx *ui, float v) {
    if (v < 0) return CUI_C(ui, BORDER);
    if (v >= 0.999f) return CUI_C(ui, SUCCESS);
    if (v >= 0.99f) return cui_mix(CUI_C(ui, SUCCESS), CUI_C(ui, WARNING), 0.55f);
    if (v >= 0.95f) return CUI_C(ui, WARNING);
    return CUI_C(ui, DANGER);
}

typedef struct { const float *days; int first, n; float bw, gap; int hover; } cui__up_paint;

static void cui__paint_uptime(cui_ctx *ui, cui_node *nd, void *data) {
    const cui__up_paint *p = data;
    cui_rect r = nd->rect;
    for (int k = 0; k < p->n; k++) {
        float v = p->days[p->first + k];
        float x = r.x + k * (p->bw + p->gap);
        cui_color c = cui__up_color(ui, v);
        if (p->hover >= 0 && k != p->hover) c = cui_alpha(c, 0.75f);
        float inset = k == p->hover ? 0 : 2;
        cui_draw_rect(ui, (cui_rect){ x, r.y + inset, p->bw, r.h - inset * 2 }, CUI_CORNERS(CUI_MIN(2.0f, p->bw * 0.5f)), c);
    }
}

/* how many of the latest days fit, and how wide each bar is */
static void cui__up_fit(float w, int days, int *n, float *bw, float *gap) {
    *gap = w < 500 ? 2 : 3;
    int fit = (int)((w + *gap) / (3 + *gap));
    *n = CUI_MIN(days, CUI_MAX(1, fit));
    *bw = *n > 0 ? (w - *gap * (*n - 1)) / *n : 0;
    if (*bw > 14) *bw = 14;
}

cui_uptime_result cui_uptime_ex(cui_ctx *ui, const char *id_str, const cui_uptime_service *services, int count, const cui_uptime_opts *o) {
    cui_uptime_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui_uptime_result res = { -1, -1 };
    if (!services || count < 0) count = 0;
    cui_id id = cui_make_id(ui, id_str);
    int col = cui__node_new(ui, id);
    cui_node *cn = &ui->nodes[col];
    cn->gap = 18; cn->w = o->width.kind ? o->width : CUI_FILL;
    cui__open(ui, col);
    cui_push_id(ui, id_str);
    float fs = cui__fs(ui, ui->theme.text_sm), fx = cui__fs(ui, ui->theme.text_xs);
    for (int s = 0; s < count; s++) {
        const cui_uptime_service *sv = &services[s];
        int nd = sv->days ? CUI_MAX(0, sv->day_count) : 0;
        cui_push_id_int(ui, s);
        /* name, state, uptime */
        double sum = 0;
        int with = 0;
        for (int k = 0; k < nd; k++) if (sv->days[k] >= 0) { sum += sv->days[k]; with++; }
        float last = nd > 0 ? sv->days[nd - 1] : -1;
        int box = cui__node_new(ui, 0);
        ui->nodes[box].gap = 8; ui->nodes[box].w = CUI_FILL;
        cui__open(ui, box);
        int head = cui__node_new(ui, 0);
        cui_node *hn = &ui->nodes[head];
        hn->dir = CUI_ROW; hn->gap = 8; hn->w = CUI_FILL; hn->align = CUI_ALIGN_CENTER; hn->flags |= CUI_NF_ROW_WRAP;
        cui__open(ui, head);
        int dot = cui__node_new(ui, 0);
        ui->nodes[dot].w = ui->nodes[dot].h = CUI_PX(8); ui->nodes[dot].radius = CUI_CORNERS(4); ui->nodes[dot].bg = cui__up_color(ui, last);
        int nm = cui__text_node(ui, sv->name ? sv->name : "", -1, CUI_FONT_MEDIUM, fs, CUI_C(ui, FOREGROUND), false);
        ui->nodes[nm].flags |= CUI_NF_CLIP;
        const char *state = last < 0 ? "No data" : last >= 0.999f ? "Operational" : last >= 0.95f ? "Degraded" : "Outage";
        cui__text_node(ui, state, -1, CUI_FONT_BODY, fx, last < 0 ? CUI_C(ui, TEXT_MUTED) : last >= 0.999f ? CUI_C(ui, TEXT_SECONDARY) : cui__up_color(ui, last), false);
        int sp = cui__node_new(ui, 0);
        ui->nodes[sp].w = CUI_FILL;
        int up = cui__text_node(ui, with ? cui__frame_printf(ui, "%.2f%% uptime", sum / with * 100.0) : "\xe2\x80\x94", -1, CUI_FONT_BODY, fx, CUI_C(ui, TEXT_MUTED), false);
        ui->nodes[up].flags |= CUI_NF_TABULAR;
        cui__close(ui);
        /* the bars: the latest days that fit, oldest on the left */
        cui_id bid = cui_make_id(ui, "bars");
        cui_rect br = cui__prev_rect(ui, bid);
        float w = br.w > 0 ? br.w : 600;
        cui__up_paint *p = cui__frame_alloc(ui, sizeof *p);
        int n;
        cui__up_fit(w, nd, &n, &p->bw, &p->gap);
        p->days = sv->days; p->n = nd > 0 ? n : 0; p->first = nd - p->n; p->hover = -1;
        cui_interact bi = cui__interact(ui, bid, 0);
        if (bi.hovered && p->n > 0 && cui__has_prev_rect(ui, bid)) {
            int k = (int)((ui->mx - br.x) / (p->bw + p->gap));
            if (k >= 0 && k < p->n) {
                p->hover = k;
                res.service = s; res.day = p->first + k;
            }
        }
        int bars = cui__node_new(ui, bid);
        cui_node *bn = &ui->nodes[bars];
        bn->w = CUI_FILL; bn->h = CUI_PX(CUI__UP_BAR_H);
        bn->direction = CUI_DIRECTION_LTR;   /* time runs left to right */
        bn->paint = cui__paint_uptime; bn->paint_data = p;
        cui__mark_interactive(ui, bars, 0);
        if (p->hover >= 0) {
            float v = sv->days[p->first + p->hover];
            int back = p->n - 1 - p->hover;
            const char *day;
            if (o->last_day.year > 0) {
                cui_date dd = cui_date_from_days(cui_date_to_days(o->last_day) - back);
                day = cui__frame_printf(ui, "%s %d, %d", cui__month_short(ui, dd.month - 1), dd.day, dd.year);
            } else day = back == 0 ? "Latest day" : cui__frame_printf(ui, back == 1 ? "1 day earlier" : "%d days earlier", back);
            const char *tip = v < 0 ? cui__frame_printf(ui, "%s\nNo data", day) : cui__frame_printf(ui, "%s\n%.2f%% up", day, v * 100.0);
            cui_id tid = bid ^ (uint32_t)(p->hover * 2654435761u);
            cui__absolute(ui, tid, p->hover * (p->bw + p->gap), 0, p->bw, CUI__UP_BAR_H);
            cui__tooltip_passive(ui, tid, tip);
        }
        /* the period under the last service */
        if (s == count - 1 && p->n > 0) {
            int ax = cui__node_new(ui, 0);
            cui_node *an = &ui->nodes[ax];
            an->dir = CUI_ROW; an->w = CUI_FILL; an->direction = CUI_DIRECTION_LTR;
            cui__open(ui, ax);
            int a0 = cui__text_node(ui, cui__frame_printf(ui, p->n == 1 ? "1 day" : "%d days ago", p->n), -1, CUI_FONT_BODY, fx, CUI_C(ui, TEXT_MUTED), false);
            ui->nodes[a0].w = CUI_FILL;
            cui__text_node(ui, "Today", -1, CUI_FONT_BODY, fx, CUI_C(ui, TEXT_MUTED), false);
            cui__close(ui);
        }
        cui__close(ui);
        cui_pop_id(ui);
    }
    cui_pop_id(ui);
    cui__close(ui);
    ui->last_id = id;
    return res;
}

/* ======================================================================== */
/* Time histogram                                                           */
/* ======================================================================== */

#define CUI__TH_AXIS_W 40.0f   /* counts on the left */
#define CUI__TH_AXIS_H 20.0f   /* times below */

typedef struct { int mode; float press_x; double press_t; } cui__th_state;

typedef struct {
    double start, bucket; int n;
    const float **counts; cui_color *colors; int ns;
    double sel0, sel1;
    int hover;
    double vmax, ystep;
    double tstep; int offset;   /* the time labels' step and the zone's offset in minutes */
    bool label_dates, label_secs;
} cui__th_paint;

/* label steps: one second to four weeks */
static const double cui__th_steps[] = { 1, 5, 15, 30, 60, 300, 900, 1800, 3600, 10800, 21600, 43200, 86400, 172800, 604800, 2419200 };

static const char *cui__th_label(cui_ctx *ui, const cui__th_paint *p, double t) {
    int64_t lt = (int64_t)floor(t) + (int64_t)p->offset * 60;
    int64_t days = lt >= 0 ? lt / 86400 : (lt - 86399) / 86400;
    int sec = (int)(lt - days * 86400);
    if (p->label_dates && sec == 0) {
        cui_date d = cui_date_from_days((int)days);
        return cui__frame_printf(ui, "%s %d", cui__month_short(ui, d.month - 1), d.day);
    }
    if (p->label_secs) return cui__frame_printf(ui, "%02d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60);
    return cui__frame_printf(ui, "%02d:%02d", sec / 3600, sec / 60 % 60);
}

static void cui__paint_thist(cui_ctx *ui, cui_node *nd, void *data) {
    const cui__th_paint *p = data;
    cui_rect r = nd->rect;
    cui_rect plot = { r.x + CUI__TH_AXIS_W, r.y + 6, CUI_MAX(1.0f, r.w - CUI__TH_AXIS_W - 4), CUI_MAX(1.0f, r.h - CUI__TH_AXIS_H - 6) };
    float fx = cui__fs(ui, ui->theme.text_xs), fh = cui__font_line_height(ui, CUI_FONT_BODY, fx);
    cui_color grid = cui_alpha(CUI_C(ui, BORDER_SUBTLE), ui->mode == CUI_THEME_DARK ? 0.8f : 1.0f);
    /* count lines */
    for (double v = 0; v <= p->vmax + 1e-9 && p->ystep > 0; v += p->ystep) {
        float y = floorf(plot.y + plot.h - (float)(v / p->vmax) * plot.h);
        cui_draw_rect(ui, (cui_rect){ plot.x, y, plot.w, 1 }, CUI_CORNERS(0), v == 0 ? CUI_C(ui, BORDER) : grid);
        const char *lbl = v >= 10000 ? cui__frame_printf(ui, "%.0fk", v / 1000) : v >= 1000 ? cui__frame_printf(ui, "%.1fk", v / 1000) : cui__frame_printf(ui, "%.0f", v);
        float lw = cui__text_width(ui, CUI_FONT_BODY, fx, lbl, -1, true);
        cui__draw_text_line(ui, CUI_FONT_BODY, fx, plot.x - 6 - lw, y - fh * 0.5f, CUI_C(ui, TEXT_MUTED), lbl, -1, true);
    }
    /* bars, stacked */
    float slot = plot.w / (float)CUI_MAX(1, p->n), gap = slot >= 6 ? 1.0f : 0.0f;
    for (int b = 0; b < p->n; b++) {
        float x = plot.x + b * slot, y = plot.y + plot.h;
        for (int s = 0; s < p->ns; s++) {
            float v = p->counts[s] ? p->counts[s][b] : 0;
            if (!(v > 0)) continue;
            float h = (float)(v / p->vmax) * plot.h;
            cui_color c = p->colors[s];
            if (p->hover >= 0 && b != p->hover) c = cui_alpha(c, 0.7f);
            cui_draw_rect(ui, (cui_rect){ x + gap * 0.5f, y - h, CUI_MAX(0.5f, slot - gap), h }, CUI_CORNERS(0), c);
            y -= h;
        }
    }
    /* time labels */
    double t0 = p->start, t1 = p->start + p->bucket * p->n;
    double first = ceil((t0 + p->offset * 60.0) / p->tstep) * p->tstep - p->offset * 60.0;
    float last_x = -1e9f;
    for (double t = first; t <= t1 + 1e-6 && p->tstep > 0; t += p->tstep) {
        float x = plot.x + (float)((t - t0) / (t1 - t0)) * plot.w;
        cui_draw_rect(ui, (cui_rect){ floorf(x), plot.y + plot.h, 1, 4 }, CUI_CORNERS(0), CUI_C(ui, BORDER_STRONG));
        const char *lbl = cui__th_label(ui, p, t);
        float lw = cui__text_width(ui, CUI_FONT_BODY, fx, lbl, -1, true);
        float lx = CUI_CLAMP(x - lw * 0.5f, r.x, r.x + r.w - lw);
        if (lx < last_x + 8) continue;
        cui__draw_text_line(ui, CUI_FONT_BODY, fx, lx, plot.y + plot.h + 5, CUI_C(ui, TEXT_MUTED), lbl, -1, true);
        last_x = lx + lw;
    }
    /* the selected range */
    if (p->sel1 > p->sel0) {
        float xa = plot.x + (float)((p->sel0 - t0) / (t1 - t0)) * plot.w, xb = plot.x + (float)((p->sel1 - t0) / (t1 - t0)) * plot.w;
        xa = CUI_CLAMP(xa, plot.x, plot.x + plot.w); xb = CUI_CLAMP(xb, plot.x, plot.x + plot.w);
        cui_color em = cui__emphasis(ui);
        cui_draw_rect(ui, (cui_rect){ xa, plot.y, xb - xa, plot.h }, CUI_CORNERS(0), cui_alpha(em, 0.10f));
        cui_draw_rect(ui, (cui_rect){ xa, plot.y, 1.5f, plot.h }, CUI_CORNERS(0), em);
        cui_draw_rect(ui, (cui_rect){ xb - 1.5f, plot.y, 1.5f, plot.h }, CUI_CORNERS(0), em);
    }
}

static double cui__th_nice(double raw) {
    if (!(raw > 0)) return 1;
    double p = pow(10.0, floor(log10(raw))), m = raw / p;
    return (m <= 1 ? 1 : m <= 2 ? 2 : m <= 5 ? 5 : 10) * p;
}

cui_time_histogram_result cui_time_histogram_ex(cui_ctx *ui, const char *id_str, double start, double bucket, int buckets,
                                                const cui_histogram_series *series, int series_count, double *sel_start, double *sel_end,
                                                const cui_time_histogram_opts *o) {
    cui_time_histogram_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui_time_histogram_result res = { false, -1 };
    if (!series || series_count < 0) series_count = 0;
    if (buckets < 0) buckets = 0;
    if (!(bucket > 0)) bucket = 60;
    double own0 = 0, own1 = 0;
    if (!sel_start || !sel_end) { sel_start = &own0; sel_end = &own1; }
    cui_id id = cui_make_id(ui, id_str);
    cui__th_state *st = cui_state(ui, id ^ 0x7415u, sizeof *st);
    cui_rect R = cui__prev_rect(ui, id);
    bool known = cui__has_prev_rect(ui, id);
    cui_rect plot = { R.x + CUI__TH_AXIS_W, R.y + 6, CUI_MAX(1.0f, R.w - CUI__TH_AXIS_W - 4), CUI_MAX(1.0f, R.h - CUI__TH_AXIS_H - 6) };
    double t0 = start, t1 = start + bucket * buckets;
    cui_interact bi = cui__interact(ui, id, CUI_IF_DRAG);
    double b0 = *sel_start, b1 = *sel_end;
    double tm = known && buckets > 0 ? t0 + CUI_CLAMP((ui->mx - plot.x) / plot.w, 0.0f, 1.0f) * (t1 - t0) : t0;
    /* drag selects a range of whole buckets; a click clears it */
    if (known && buckets > 0) {
        if (bi.hovered && ui->mpressed[0] && ui->mx >= plot.x - 2) { st->mode = 1; st->press_x = ui->mx; st->press_t = tm; }
        if (st->mode == 1 && ui->mdown[0] && fabsf(ui->mx - st->press_x) > 3) st->mode = 2;
        if (st->mode == 2) {
            double a = CUI_MIN(st->press_t, tm), b = CUI_MAX(st->press_t, tm);
            a = t0 + floor((a - t0) / bucket) * bucket;
            b = t0 + ceil((b - t0) / bucket) * bucket;
            *sel_start = CUI_MAX(a, t0); *sel_end = CUI_MIN(b, t1);
            cui__set_cursor(ui, CUI_CURSOR_RESIZE_EW);
        }
        if (!ui->mdown[0]) {
            if (st->mode == 1) *sel_start = *sel_end = 0;
            st->mode = 0;
        }
    }
    res.selection_changed = *sel_start != b0 || *sel_end != b1;
    cui__th_paint *p = cui__frame_alloc(ui, sizeof *p);
    memset(p, 0, sizeof *p);
    p->start = start; p->bucket = bucket; p->n = buckets; p->ns = series_count;
    p->counts = cui__frame_alloc(ui, sizeof(float *) * (size_t)CUI_MAX(1, series_count));
    p->colors = cui__frame_alloc(ui, sizeof(cui_color) * (size_t)CUI_MAX(1, series_count));
    double vmax = 0;
    for (int b = 0; b < buckets; b++) {
        double sum = 0;
        for (int s = 0; s < series_count; s++) if (series[s].counts && series[s].counts[b] > 0) sum += series[s].counts[b];
        vmax = CUI_MAX(vmax, sum);
    }
    for (int s = 0; s < series_count; s++) {
        float *copy = NULL;
        if (series[s].counts && buckets > 0) { copy = cui__frame_alloc(ui, sizeof(float) * (size_t)buckets); memcpy(copy, series[s].counts, sizeof(float) * (size_t)buckets); }
        p->counts[s] = copy;
        p->colors[s] = series[s].tone ? cui__tone_color(ui, series[s].tone) : ui->theme.colors[CUI_TOKEN_SERIES_1 + s % 6];
    }
    float ph = known ? plot.h : 100;
    p->ystep = cui__th_nice(CUI_MAX(vmax, 1.0) / CUI_MAX(1.0f, ph / 28.0f));
    p->vmax = CUI_MAX(p->ystep, ceil(CUI_MAX(vmax, 1.0) / p->ystep) * p->ystep);
    p->sel0 = *sel_start; p->sel1 = *sel_end;
    p->offset = o->utc ? 0 : cui__utc_offset((int64_t)start);
    double span = t1 - t0;
    float pw = known ? plot.w : 500;
    p->tstep = cui__th_steps[sizeof cui__th_steps / sizeof cui__th_steps[0] - 1];
    for (size_t k = 0; k < sizeof cui__th_steps / sizeof cui__th_steps[0]; k++)
        if (span > 0 && cui__th_steps[k] / span * pw >= 78) { p->tstep = cui__th_steps[k]; break; }
    p->label_dates = p->tstep >= 86400 || span > 86400;
    p->label_secs = p->tstep < 60;
    p->hover = -1;
    if (bi.hovered && known && buckets > 0 && st->mode != 2 && ui->mx >= plot.x && ui->mx <= plot.x + plot.w) {
        int b = CUI_CLAMP((int)((ui->mx - plot.x) / plot.w * buckets), 0, buckets - 1);
        p->hover = b; res.hovered = b;
    }
    int col = cui__node_new(ui, 0);
    ui->nodes[col].gap = 8; ui->nodes[col].w = o->width.kind ? o->width : CUI_FILL;
    cui__open(ui, col);
    int nd = cui__node_new(ui, id);
    cui_node *n = &ui->nodes[nd];
    n->w = CUI_FILL;
    n->h = CUI_PX(o->height > 0 ? o->height : 140);
    n->direction = CUI_DIRECTION_LTR;
    n->paint = cui__paint_thist; n->paint_data = p;
    cui__mark_interactive(ui, nd, CUI_IF_DRAG);
    if (p->hover >= 0 && ui->active == 0) {
        int b = p->hover;
        double tb = t0 + b * bucket;
        char buf[512];
        int len = snprintf(buf, sizeof buf, "%s \xe2\x80\x93 %s", cui__th_label(ui, p, tb), cui__th_label(ui, p, tb + bucket));
        for (int s = 0; s < series_count && len < (int)sizeof buf - 64; s++)
            len += snprintf(buf + len, sizeof buf - (size_t)len, "\n%s  %.0f", series[s].name ? series[s].name : "", series[s].counts ? (double)series[s].counts[b] : 0.0);
        cui_id tid = id ^ (uint32_t)(b * 2654435761u) ^ 0x7416u;
        float slot = plot.w / buckets;
        cui__open(ui, nd);
        cui__absolute(ui, tid, CUI__TH_AXIS_W + b * slot, 6, CUI_MAX(1.0f, slot), plot.h);
        cui__tooltip_passive(ui, tid, cui__frame_strdup(ui, buf, -1));
        cui__close(ui);
    }
    /* the series, with their totals */
    if (series_count > 1) {
        int lg = cui__node_new(ui, 0);
        cui_node *ln = &ui->nodes[lg];
        ln->dir = CUI_ROW; ln->gap = 16; ln->align = CUI_ALIGN_CENTER; ln->flags |= CUI_NF_ROW_WRAP;
        ln->pad[3] = CUI__TH_AXIS_W;
        cui__open(ui, lg);
        float fx = cui__fs(ui, ui->theme.text_xs);
        for (int s = 0; s < series_count; s++) {
            double tot = 0;
            for (int b = 0; b < buckets && series[s].counts; b++) tot += series[s].counts[b] > 0 ? series[s].counts[b] : 0;
            int it = cui__node_new(ui, 0);
            ui->nodes[it].dir = CUI_ROW; ui->nodes[it].gap = 6; ui->nodes[it].align = CUI_ALIGN_CENTER;
            cui__open(ui, it);
            int sw = cui__node_new(ui, 0);
            ui->nodes[sw].w = ui->nodes[sw].h = CUI_PX(8); ui->nodes[sw].radius = CUI_CORNERS(2); ui->nodes[sw].bg = p->colors[s];
            cui__text_node(ui, series[s].name ? series[s].name : "", -1, CUI_FONT_BODY, fx, CUI_C(ui, TEXT_SECONDARY), false);
            int tt = cui__text_node(ui, tot >= 10000 ? cui__frame_printf(ui, "%.1fk", tot / 1000) : cui__frame_printf(ui, "%.0f", tot), -1, CUI_FONT_BODY, fx, CUI_C(ui, TEXT_MUTED), false);
            ui->nodes[tt].flags |= CUI_NF_TABULAR;
            cui__close(ui);
        }
        cui__close(ui);
    }
    cui__close(ui);
    ui->last_id = id;
    return res;
}

/* ======================================================================== */
/* Coverage                                                                 */
/* ======================================================================== */

#define CUI__COV_ROW    30.0f
#define CUI__COV_NARROW 560.0f

typedef struct {
    const char *name; int nlen;     /* the folder or file name, inside the app's path */
    int parent, depth, file;        /* file: index into the files, -1 for a folder */
    int first_child, next;
    long lines, lines_hit, br, br_hit, fn, fn_hit;
} cui__cov_node;

typedef struct {
    bool init;
    uint32_t key;
    cui__cov_node *nodes; int nn, cap;
    uint32_t folded[64]; int nfolded;
    int sort;                       /* 0 name, 1 lines, 2 branches, 3 functions */
    bool desc;
    int sel;
} cui__cov_state;

static void cui__cov_dtor(cui_ctx *ui, void *data) {
    cui__cov_state *s = data;
    cui__free(ui, s->nodes);
    s->nodes = NULL; s->nn = s->cap = 0;
}

static int cui__cov_add(cui_ctx *ui, cui__cov_state *s, int parent, const char *name, int len, int file) {
    CUI__GROW(ui, s->nodes, s->nn, s->cap, 1);
    if (!s->nodes) { s->nn = s->cap = 0; return -1; }
    cui__cov_node *n = &s->nodes[s->nn];
    memset(n, 0, sizeof *n);
    n->name = name; n->nlen = len; n->parent = parent; n->file = file;
    n->depth = parent >= 0 ? s->nodes[parent].depth + 1 : 0;
    n->first_child = n->next = -1;
    if (parent >= 0) {   /* append to the parent's children */
        int *at = &s->nodes[parent].first_child;
        while (*at >= 0) at = &s->nodes[*at].next;
        *at = s->nn;
    }
    return s->nn++;
}

/* the folder tree from the paths, with each folder summing what it holds; rebuilt when they change */
static void cui__cov_build(cui_ctx *ui, cui__cov_state *s, const cui_coverage_file *files, int count) {
    uint32_t key = cui__hash(&count, sizeof count, 0xC0Fu);
    for (int i = 0; i < count; i++) {
        key = cui__hash_str(files[i].path ? files[i].path : "", key);
        key = cui__hash(&files[i].lines, sizeof(int) * 6, key);
    }
    if (s->init && s->key == key) return;
    s->init = true; s->key = key; s->nn = 0;
    int root = cui__cov_add(ui, s, -1, "All files", 9, -1);
    if (root < 0) return;
    for (int i = 0; i < count; i++) {
        const char *p = files[i].path ? files[i].path : "";
        int at = root;
        for (;;) {
            const char *e = p;
            while (*e && *e != '/' && *e != '\\') e++;
            int len = (int)(e - p);
            bool leaf = !*e;
            if (len > 0 || leaf) {
                int found = -1;
                if (!leaf)
                    for (int c = s->nodes[at].first_child; c >= 0; c = s->nodes[c].next)
                        if (s->nodes[c].file < 0 && s->nodes[c].nlen == len && !memcmp(s->nodes[c].name, p, (size_t)len)) { found = c; break; }
                if (found < 0) found = cui__cov_add(ui, s, at, p, len, leaf ? i : -1);
                if (found < 0) return;
                at = found;
            }
            if (leaf) break;
            p = e + 1;
        }
        const cui_coverage_file *f = &files[i];
        for (int k = at; k >= 0; k = s->nodes[k].parent) {
            cui__cov_node *n = &s->nodes[k];
            n->lines += f->lines; n->lines_hit += f->lines_hit; n->br += f->branches; n->br_hit += f->branches_hit;
            n->fn += f->functions; n->fn_hit += f->functions_hit;
        }
    }
}

static double cui__cov_pct(long hit, long all) { return all > 0 ? 100.0 * (double)hit / (double)all : -1; }
static double cui__cov_metric(const cui__cov_node *n, int m) {
    return m == 1 ? cui__cov_pct(n->lines_hit, n->lines) : m == 2 ? cui__cov_pct(n->br_hit, n->br) : cui__cov_pct(n->fn_hit, n->fn);
}
static uint32_t cui__cov_key(const cui__cov_state *s, int k) { return cui__hash(s->nodes[k].name, (size_t)s->nodes[k].nlen, (uint32_t)s->nodes[k].depth * 977u + (uint32_t)s->nodes[k].parent); }
static bool cui__cov_folded(const cui__cov_state *s, int k) {
    uint32_t h = cui__cov_key(s, k);
    for (int i = 0; i < s->nfolded; i++) if (s->folded[i] == h) return true;
    return false;
}
static void cui__cov_toggle(cui__cov_state *s, int k) {
    uint32_t h = cui__cov_key(s, k);
    for (int i = 0; i < s->nfolded; i++) if (s->folded[i] == h) { s->folded[i] = s->folded[--s->nfolded]; return; }
    if (s->nfolded < 64) s->folded[s->nfolded++] = h;
}

/* the rows in order: folders first, then by the sort column; children of folded folders left out */
static void cui__cov_flatten(cui_ctx *ui, const cui__cov_state *s, int k, int *rows, int *nr) {
    rows[(*nr)++] = k;
    if (s->nodes[k].file >= 0 || cui__cov_folded(s, k)) return;
    int kids[512], nk = 0;
    for (int c = s->nodes[k].first_child; c >= 0 && nk < 512; c = s->nodes[c].next) kids[nk++] = c;
    for (int a = 1; a < nk; a++) {   /* insertion sort: few children per folder */
        int x = kids[a], b = a - 1;
        for (; b >= 0; b--) {
            const cui__cov_node *p = &s->nodes[kids[b]], *q = &s->nodes[x];
            bool pf = p->file < 0, qf = q->file < 0;
            int c;
            if (pf != qf) c = pf ? -1 : 1;
            else if (s->sort == 0) { int m = memcmp(p->name, q->name, (size_t)CUI_MIN(p->nlen, q->nlen)); c = m ? m : p->nlen - q->nlen; }
            else { double u = cui__cov_metric(p, s->sort), v = cui__cov_metric(q, s->sort); c = u < v ? -1 : u > v ? 1 : 0; }
            if (s->desc && pf == qf) c = -c;
            if (c <= 0) break;
            kids[b + 1] = kids[b];
        }
        kids[b + 1] = x;
    }
    for (int i = 0; i < nk; i++) cui__cov_flatten(ui, s, kids[i], rows, nr);
}

typedef struct { float pct; cui_color c, track; } cui__cov_bar;
static void cui__paint_cv_bar(cui_ctx *ui, cui_node *n, void *data) {
    (void)ui;
    const cui__cov_bar *b = data;
    cui_rect r = n->rect;
    cui_rect t = { r.x, r.y + (r.h - 6) * 0.5f, r.w, 6 };
    cui_draw_rect(ui, t, CUI_CORNERS(3), b->track);
    float w = CUI_MAX(3.0f, t.w * b->pct / 100.0f);
    if (b->pct > 0) cui_draw_rect(ui, (cui_rect){ n->rtl ? t.x + t.w - w : t.x, t.y, w, t.h }, CUI_CORNERS(3), b->c);   /* fills from the start side */
}

static cui_color cui__cov_color(cui_ctx *ui, double pct, float lo, float hi) {
    return pct < 0 ? CUI_C(ui, TEXT_MUTED) : pct < lo ? CUI_C(ui, DANGER) : pct < hi ? CUI_C(ui, WARNING) : CUI_C(ui, SUCCESS);
}

static void cui__cov_pct_cell(cui_ctx *ui, double pct, float w, float lo, float hi) {
    int t = cui__text_node(ui, pct < 0 ? "\xe2\x80\x94" : cui__frame_printf(ui, "%.1f%%", pct), -1, CUI_FONT_BODY, cui__fs(ui, ui->theme.text_xs),
                           pct < 0 ? CUI_C(ui, TEXT_MUTED) : cui__cov_color(ui, pct, lo, hi), false);
    ui->nodes[t].w = CUI_PX(w); ui->nodes[t].text_align = CUI_TEXT_END; ui->nodes[t].flags |= CUI_NF_TABULAR;
}

cui_coverage_result cui_coverage_ex(cui_ctx *ui, const char *id_str, const cui_coverage_file *files, int count, const cui_coverage_opts *o) {
    cui_coverage_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui_coverage_result res = { -1, 0 };
    if (!files || count < 0) count = 0;
    float lo = o->low > 0 ? o->low : 50, hi = o->high > 0 ? o->high : 80;
    cui_id id = cui_make_id(ui, id_str);
    cui__cov_state *s = cui_state(ui, id ^ 0xC0E1u, sizeof *s);
    cui__state_set_dtor(ui, id ^ 0xC0E1u, cui__cov_dtor);
    if (!s->init) { s->sel = -1; s->sort = 0; }
    cui__cov_build(ui, s, files, count);
    if (s->nn == 0) return res;
    res.lines = (float)CUI_MAX(0.0, cui__cov_pct(s->nodes[0].lines_hit, s->nodes[0].lines));
    cui_rect pr = cui__prev_rect(ui, id);
    bool narrow = pr.w > 0 && pr.w < CUI__COV_NARROW;
    int *rows = cui__frame_alloc(ui, sizeof(int) * (size_t)s->nn), nr = 0;
    cui__cov_flatten(ui, s, 0, rows, &nr);
    if (s->sel >= nr) s->sel = -1;
    cui_id lid = id ^ 0xC0E2u;
    int outer = cui__node_new(ui, id);
    cui_node *on = &ui->nodes[outer];
    on->w = o->width.kind ? o->width : CUI_FILL;
    on->h = o->height < 0 ? CUI_FILL : CUI_PX(o->height > 0 ? o->height : CUI_MIN(360.0f, CUI__COV_ROW * (nr + 1) + 4));
    on->radius = CUI_CORNERS(ui->theme.radius_control);
    on->border = 1; on->border_color = CUI_C(ui, BORDER);
    on->bg = CUI_C(ui, SURFACE);
    on->flags |= CUI_NF_CLIP;
    cui__open(ui, outer);
    cui_push_id(ui, id_str);
    float fx = cui__fs(ui, ui->theme.text_xs), fs = cui__fs(ui, ui->theme.text_sm);
    float bar_w = narrow ? 56 : 90;
    /* header: click a title to sort by it, again to reverse */
    int h = cui__node_new(ui, 0);
    cui_node *hn = &ui->nodes[h];
    hn->dir = CUI_ROW; hn->gap = 12; hn->w = CUI_FILL; hn->h = CUI_PX(CUI__COV_ROW); hn->align = CUI_ALIGN_CENTER;
    hn->pad[1] = hn->pad[3] = 12;
    hn->bg = CUI_C(ui, SURFACE_MUTED);
    cui__open(ui, h);
    static const char *const titles[4] = { "File", "Lines", "Branches", "Functions" };
    int cols = narrow ? 2 : 4;
    for (int c = 0; c < cols; c++) {
        cui_id cid = cui_make_id(ui, titles[c]);
        cui_interact ci = cui__interact(ui, cid, 0);
        if (ci.clicked) { if (s->sort == c) s->desc = !s->desc; else { s->sort = c; s->desc = c != 0; } }
        int cell = cui__node_new(ui, cid);
        cui_node *cn = &ui->nodes[cell];
        cn->dir = CUI_ROW; cn->gap = 4; cn->align = CUI_ALIGN_CENTER;
        cn->w = c == 0 ? CUI_FILL : CUI_PX(c == 1 ? bar_w + 12 + 54 : 70);
        cn->justify = c == 0 ? CUI_JUSTIFY_START : CUI_JUSTIFY_END;
        cui__mark_interactive(ui, cell, 0);
        if (ci.hovered) cui__set_cursor(ui, CUI_CURSOR_HAND);
        cui__open(ui, cell);
        cui__text_node(ui, titles[c], -1, CUI_FONT_MEDIUM, fx, s->sort == c ? CUI_C(ui, FOREGROUND) : CUI_C(ui, TEXT_MUTED), false);
        if (s->sort == c) cui__icon_node(ui, s->desc ? CUI_ICON_ARROW_DOWN : CUI_ICON_ARROW_UP, 12, CUI_C(ui, TEXT_SECONDARY));
        cui__close(ui);
    }
    cui__close(ui);
    int line = cui__node_new(ui, 0);
    ui->nodes[line].w = CUI_FILL; ui->nodes[line].h = CUI_PX(1); ui->nodes[line].bg = CUI_C(ui, BORDER);
    /* keys */
    cui_interact lr = cui__interact(ui, lid, CUI_IF_FOCUSABLE);
    if (lr.focused && !ui->edit_prev) {
        if (cui_key_pressed(ui, CUI_KEY_DOWN)) s->sel = CUI_MIN(nr - 1, s->sel + 1);
        if (cui_key_pressed(ui, CUI_KEY_UP)) s->sel = CUI_MAX(0, s->sel - 1);
        if (s->sel >= 0) {
            const cui__cov_node *n = &s->nodes[rows[s->sel]];
            bool left = cui_key_pressed(ui, CUI_KEY_LEFT), right = cui_key_pressed(ui, CUI_KEY_RIGHT);
            if (n->file < 0 && ((left && !cui__cov_folded(s, rows[s->sel])) || (right && cui__cov_folded(s, rows[s->sel])))) cui__cov_toggle(s, rows[s->sel]);
            if (cui_key_pressed(ui, CUI_KEY_ENTER)) { if (n->file >= 0) res.activated = n->file; else cui__cov_toggle(s, rows[s->sel]); }
        }
    }
    int frame = cui__node_new(ui, lid);
    ui->nodes[frame].w = CUI_FILL; ui->nodes[frame].h = CUI_FILL;
    cui__mark_interactive(ui, frame, CUI_IF_FOCUSABLE);
    cui__focus_ring(ui, frame, lr.focus_visible ? 1.0f : 0.0f);
    cui__open(ui, frame);
    int first, last;
    cui_virtual_list_begin(ui, "rows", nr, CUI__COV_ROW, CUI_FILL, &first, &last);
    for (int r = first; r < last; r++) {
        int k = rows[r];
        const cui__cov_node *n = &s->nodes[k];
        cui_id rid = cui__hash(&k, sizeof k, id ^ 0xC0E3u);
        cui_interact ri = cui__interact(ui, rid, CUI_IF_KEEP_FOCUS);
        if (ri.clicked) {
            s->sel = r; ui->focus = lid;
            if (n->file < 0) cui__cov_toggle(s, k);
            else if (ui->click_count >= 2) res.activated = n->file;
        }
        bool sel = s->sel == r;
        int row = cui__node_new(ui, rid);
        cui_node *rn = &ui->nodes[row];
        rn->dir = CUI_ROW; rn->gap = 12; rn->w = CUI_FILL; rn->h = CUI_PX(CUI__COV_ROW); rn->align = CUI_ALIGN_CENTER;
        rn->pad[1] = 12; rn->pad[3] = 12;
        rn->bg = sel ? (ui->focus == lid ? cui_mix(CUI_C(ui, SURFACE), cui__emphasis(ui), ui->accent == CUI_ACCENT_NEUTRAL ? 0.08f : 0.16f) : CUI_C(ui, ACTIVE))
                     : ri.hovered ? CUI_C(ui, HOVER) : cui__rgba(0, 0, 0, 0);
        cui__mark_interactive(ui, row, 0);
        if (ri.hovered) cui__set_cursor(ui, CUI_CURSOR_HAND);
        cui__open(ui, row);
        int nm = cui__node_new(ui, 0);
        cui_node *nn = &ui->nodes[nm];
        nn->dir = CUI_ROW; nn->gap = 6; nn->align = CUI_ALIGN_CENTER; nn->w = CUI_FILL; nn->flags |= CUI_NF_CLIP;
        nn->pad[3] = (float)CUI_MIN(n->depth, 10) * (narrow ? 10 : 16);
        cui__open(ui, nm);
        if (n->file < 0) {
            bool f = cui__cov_folded(s, k);
            cui__icon_node(ui, f ? CUI_ICON_CHEVRON_RIGHT : CUI_ICON_CHEVRON_DOWN, 14, CUI_C(ui, TEXT_MUTED));
            cui__icon_node(ui, f ? CUI_ICON_FOLDER : CUI_ICON_FOLDER_OPEN, 14, CUI_C(ui, TEXT_SECONDARY));
        } else {
            int sp = cui__node_new(ui, 0);
            ui->nodes[sp].w = ui->nodes[sp].h = CUI_PX(14);
            cui__icon_node(ui, CUI_ICON_FILE, 14, CUI_C(ui, TEXT_MUTED));
        }
        int t = cui__text_node(ui, n->name, n->nlen, n->file < 0 ? CUI_FONT_MEDIUM : CUI_FONT_BODY, fs, CUI_C(ui, FOREGROUND), false);
        ui->nodes[t].direction = CUI_DIRECTION_LTR; ui->nodes[t].flags |= CUI_NF_CLIP;
        cui__close(ui);
        double lp = cui__cov_pct(n->lines_hit, n->lines);
        cui__cov_bar *bp = cui__frame_alloc(ui, sizeof *bp);
        bp->pct = (float)CUI_MAX(0.0, lp); bp->c = cui__cov_color(ui, lp, lo, hi); bp->track = CUI_C(ui, BORDER_SUBTLE);
        int bar = cui__node_new(ui, 0);
        ui->nodes[bar].w = CUI_PX(bar_w); ui->nodes[bar].h = CUI_PX(12);
        ui->nodes[bar].paint = cui__paint_cv_bar; ui->nodes[bar].paint_data = bp;
        cui__cov_pct_cell(ui, lp, 54, lo, hi);
        if (!narrow) { cui__cov_pct_cell(ui, cui__cov_pct(n->br_hit, n->br), 70, lo, hi); cui__cov_pct_cell(ui, cui__cov_pct(n->fn_hit, n->fn), 70, lo, hi); }
        cui__close(ui);
        if (ri.hovered && ui->active == 0)
            cui__tooltip_passive(ui, rid, cui__frame_printf(ui, "%ld of %ld lines%s", n->lines_hit, n->lines,
                                                            n->br > 0 ? cui__frame_printf(ui, "\n%ld of %ld branches", n->br_hit, n->br) : ""));
    }
    cui_virtual_list_end(ui);
    cui__close(ui);
    cui_pop_id(ui);
    cui__close(ui);
    ui->last_id = id;
    return res;
}

/* ======================================================================== */
/* Blame                                                                    */
/* ======================================================================== */

#define CUI__BLM_ROW    20.0f
#define CUI__BLM_NARROW 560.0f

typedef struct {
    bool init;
    const void *lines; int count; const char *lang;   /* what the comment flags are for */
    uint8_t *in_block; int cap;     /* per line: inside a block comment at its start */
    int hovered;
} cui__blm_state;

static void cui__blm_dtor(cui_ctx *ui, void *data) {
    cui__blm_state *s = data;
    cui__free(ui, s->in_block);
    s->in_block = NULL; s->cap = 0;
}

typedef struct {
    const char *text; int len;
    const char *lang; bool in_block;
    float size, gutter, num_w;
    int number;
    float heat;                     /* 0 oldest .. 1 newest */
    bool run_start, marked;
    const char *author, *age, *hash;
    cui_color colors[CUI__TK_COUNT];
} cui__blm_paint;

static void cui__paint_blm_row(cui_ctx *ui, cui_node *nd, void *data) {
    const cui__blm_paint *p = data;
    cui_rect r = nd->rect;
    float fh = cui__font_line_height(ui, CUI_FONT_MONO, p->size), ty = r.y + (r.h - fh) * 0.5f;
    float fx = cui__fs(ui, ui->theme.text_xs), bh = cui__font_line_height(ui, CUI_FONT_BODY, fx);
    cui_color em = ui->accent == CUI_ACCENT_NEUTRAL ? cui__tone_color(ui, CUI_TONE_INFO) : CUI_C(ui, ACCENT);
    if (p->marked) cui_draw_rect(ui, r, CUI_CORNERS(0), cui_alpha(em, ui->mode == CUI_THEME_DARK ? 0.12f : 0.07f));
    if (p->run_start) cui_draw_rect(ui, (cui_rect){ r.x, r.y, r.w, 1 }, CUI_CORNERS(0), CUI_C(ui, BORDER_SUBTLE));
    /* age shade at the edge, then the gutter */
    cui_draw_rect(ui, (cui_rect){ r.x, r.y, 3, r.h }, CUI_CORNERS(0), cui_alpha(em, 0.15f + 0.85f * p->heat));
    cui_push_clip(ui, (cui_rect){ r.x, r.y, p->gutter - 8, r.h });
    if (p->run_start) {
        float x = r.x + 10;
        if (p->author) {
            cui__draw_text_line(ui, CUI_FONT_MEDIUM, fx, x, r.y + (r.h - bh) * 0.5f, CUI_C(ui, TEXT_SECONDARY), p->author, -1, false);
            x += cui__text_width(ui, CUI_FONT_MEDIUM, fx, p->author, -1, false) + 8;
        }
        if (p->age) cui__draw_text_line(ui, CUI_FONT_BODY, fx, x, r.y + (r.h - bh) * 0.5f, CUI_C(ui, TEXT_MUTED), p->age, -1, false);
        if (p->hash) {
            float hw = cui__text_width(ui, CUI_FONT_MONO, fx, p->hash, -1, true);
            cui__draw_text_line(ui, CUI_FONT_MONO, fx, r.x + p->gutter - 14 - hw, r.y + (r.h - bh) * 0.5f, CUI_C(ui, TEXT_MUTED), p->hash, -1, true);
        }
    }
    cui_pop_clip(ui);
    cui_draw_rect(ui, (cui_rect){ r.x + p->gutter - 1, r.y, 1, r.h }, CUI_CORNERS(0), CUI_C(ui, BORDER_SUBTLE));
    /* number and code */
    const char *num = cui__frame_printf(ui, "%d", p->number);
    float nw = cui__text_width(ui, CUI_FONT_MONO, p->size, num, -1, true);
    cui__draw_text_line(ui, CUI_FONT_MONO, p->size, r.x + p->gutter + p->num_w - nw, ty, cui_alpha(CUI_C(ui, TEXT_MUTED), 0.8f), num, -1, true);
    float cx = r.x + p->gutter + p->num_w + 14;
    cui_push_clip(ui, (cui_rect){ cx, r.y, CUI_MAX(0.0f, r.x + r.w - cx), r.h });
    bool block = p->in_block;
    cui__draw_code_line(ui, p->lang, p->text, p->len, &block, cx, ty, p->size, p->colors, r.x + r.w);
    cui_pop_clip(ui);
}

cui_blame_result cui_blame_ex(cui_ctx *ui, const char *id_str, const char *const *lines, const int *line_commit, int line_count,
                              const cui_blame_commit *commits, int commit_count, const cui_blame_opts *o) {
    cui_blame_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui_blame_result res = { -1, -1 };
    if (!lines || line_count < 0) line_count = 0;
    if (!commits || commit_count < 0) commit_count = 0;
    cui_id id = cui_make_id(ui, id_str);
    cui__blm_state *s = cui_state(ui, id ^ 0xB1A3u, sizeof *s);
    cui__state_set_dtor(ui, id ^ 0xB1A3u, cui__blm_dtor);
    if (!s->init) { s->init = true; s->hovered = -1; }
    /* which lines start inside a block comment, once per change of the lines */
    if (o->lang && (s->lines != (const void *)lines || s->count != line_count || s->lang != o->lang)) {
        s->lines = lines; s->count = line_count; s->lang = o->lang;
        if (s->cap < line_count) {
            cui__free(ui, s->in_block);
            s->in_block = cui__alloc(ui, (size_t)CUI_MAX(1, line_count));
            s->cap = s->in_block ? line_count : 0;
        }
        if (s->in_block) {
            bool block = false;
            cui__tok tk[256];
            for (int i = 0; i < line_count; i++) {
                s->in_block[i] = block;
                const char *t = lines[i] ? lines[i] : "";
                cui__tokenize_lang(o->lang, t, (int)CUI_MIN(strlen(t), (size_t)2000), &block, tk, 256);
            }
        }
    }
    int64_t now = o->now ? o->now : (int64_t)time(NULL);
    int64_t tmin = 0, tmax = 0;
    for (int c = 0; c < commit_count; c++) {
        if (c == 0 || commits[c].time < tmin) tmin = commits[c].time;
        if (c == 0 || commits[c].time > tmax) tmax = commits[c].time;
    }
    cui_rect pr = cui__prev_rect(ui, id);
    bool narrow = pr.w > 0 && pr.w < CUI__BLM_NARROW;
    float size = cui__fs(ui, narrow ? 11.5f : 12.5f);
    float gutter = narrow ? 52 : CUI_CLAMP(pr.w * 0.32f, 200.0f, 300.0f);
    int first_no = o->first_line > 0 ? o->first_line : 1;
    float num_w = cui__text_width(ui, CUI_FONT_MONO, size, cui__frame_printf(ui, "%d", first_no + CUI_MAX(0, line_count - 1)), -1, true) + 10;
    int outer = cui__node_new(ui, id);
    cui_node *on = &ui->nodes[outer];
    on->w = o->width.kind ? o->width : CUI_FILL;
    on->h = o->height < 0 ? CUI_FILL : CUI_PX(o->height > 0 ? o->height : 360);
    on->radius = CUI_CORNERS(ui->theme.radius_control);
    on->border = 1; on->border_color = CUI_C(ui, BORDER);
    on->bg = CUI_C(ui, SURFACE);
    on->flags |= CUI_NF_CLIP;
    on->direction = CUI_DIRECTION_LTR;   /* code */
    cui__open(ui, outer);
    cui_push_id(ui, id_str);
    int hovered = -1;
    int first, last;
    cui_virtual_list_begin(ui, "lines", line_count, CUI__BLM_ROW, CUI_FILL, &first, &last);
    cui_color colors[CUI__TK_COUNT];
    cui__syntax_colors(ui, colors);
    for (int i = first; i < last; i++) {
        int c = line_commit ? line_commit[i] : -1;
        if (c < 0 || c >= commit_count) c = -1;
        int pc = i > 0 && line_commit ? line_commit[i - 1] : -2;
        bool run_start = i == 0 || pc != line_commit[i];
        cui_id rid = cui__hash(&i, sizeof i, id ^ 0xB1A4u);
        cui_id gid = rid ^ 0x6u;
        cui_interact gi = cui__interact(ui, gid, 0);
        if (gi.hovered && c >= 0) hovered = c;
        if (gi.clicked && c >= 0) res.clicked = c;
        cui__blm_paint *p = cui__frame_alloc(ui, sizeof *p);
        memset(p, 0, sizeof *p);
        const char *t = lines[i] ? lines[i] : "";
        p->len = (int)CUI_MIN(strlen(t), (size_t)2000);
        p->text = cui__frame_strdup(ui, t, p->len);
        p->lang = o->lang; p->in_block = o->lang && s->in_block && i < s->cap ? s->in_block[i] != 0 : false;
        p->size = size; p->gutter = gutter; p->num_w = num_w; p->number = first_no + i;
        p->run_start = run_start || i == first;
        p->marked = c >= 0 && c == s->hovered;
        memcpy(p->colors, colors, sizeof colors);
        if (c >= 0) {
            const cui_blame_commit *cm = &commits[c];
            p->heat = tmax > tmin ? (float)(cm->time - tmin) / (float)(tmax - tmin) : 1.0f;
            if (narrow) {   /* initials only */
                char ini[8];
                int n = 0;
                const char *a = cm->author ? cm->author : "";
                bool start = true;
                for (; *a && n < 2; a++) { if (*a == ' ') { start = true; continue; } if (start) { ini[n++] = (char)(*a >= 'a' && *a <= 'z' ? *a - 32 : *a); start = false; } }
                ini[n] = 0;
                p->author = cui__frame_strdup(ui, ini, n);
            } else {
                p->author = cm->author ? cui__frame_strdup(ui, cm->author, -1) : NULL;
                p->age = cm->time ? cui__dv_age(ui, now - cm->time) : NULL;
                p->hash = cm->hash ? cui__frame_strdup(ui, cm->hash, (int)CUI_MIN(strlen(cm->hash), (size_t)7)) : NULL;
            }
        }
        int row = cui__node_new(ui, rid);
        cui_node *rn = &ui->nodes[row];
        rn->w = CUI_FILL; rn->h = CUI_PX(CUI__BLM_ROW);
        rn->paint = cui__paint_blm_row; rn->paint_data = p;
        cui__open(ui, row);
        int g = cui__node_new(ui, gid);   /* the gutter, for the pointer */
        ui->nodes[g].w = CUI_PX(gutter); ui->nodes[g].h = CUI_FILL;
        cui__mark_interactive(ui, g, 0);
        if (gi.hovered && c >= 0) cui__set_cursor(ui, CUI_CURSOR_HAND);
        cui__close(ui);
        if (gi.hovered && c >= 0 && ui->active == 0) {
            const cui_blame_commit *cm = &commits[c];
            cui__tooltip_passive(ui, gid, cui__frame_printf(ui, "%s\n%s \xc2\xb7 %s \xc2\xb7 %.7s", cm->summary ? cm->summary : "", cm->author ? cm->author : "",
                                                            cm->time ? cui__dv_age(ui, now - cm->time) : "", cm->hash ? cm->hash : ""));
        }
    }
    cui_virtual_list_end(ui);
    cui_pop_id(ui);
    cui__close(ui);
    s->hovered = hovered;
    res.hovered = hovered;
    ui->last_id = id;
    return res;
}
