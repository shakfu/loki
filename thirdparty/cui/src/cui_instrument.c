/* Instruments: the dot-matrix chart, chart annotations, the event track, the readout and
 * the dot-matrix display. They share an axis (units, labels and ticks) and a quiet look:
 * values as dots, labels in the monospaced font, hairlines and one accent. */
#include "cui_internal.h"

/* the gutter on the right of a dot chart and an event track, where value labels go, so the
 * two stacked at one width share their x positions */
#define CUI__INS_GUTTER   34.0f
#define CUI__INS_AXIS_H   18.0f    /* the row of x labels under a dot chart */
#define CUI__EV_DOT_Y     12.0f    /* the event dots' center, from the top of the track */
#define CUI__EV_REACH     11.0f    /* how far from a dot a press still takes it */

/* ======================================================================== */
/* The axis                                                                 */
/* ======================================================================== */

/* hours as a time of day, "12:15"; whole hours alone as "12" when `short_hours` */
static const char *cui__ins_label(cui_ctx *ui, cui_axis_units units, const char *fmt, double x, bool short_hours) {
    if (units == CUI_AXIS_HOURS) {
        long m = lround(x * 60.0);
        long day = 24 * 60;
        m = ((m % day) + day) % day;
        if (short_hours && m % 60 == 0) return cui__frame_printf(ui, "%02ld", m / 60);
        return cui__frame_printf(ui, "%02ld:%02ld", m / 60, m % 60);
    }
    if (fabs(x) < 1e-9) x = 0;   /* never "-0" */
    return cui__frame_printf(ui, fmt && *fmt ? fmt : "%g", x);
}

/* label spacing: the smallest "nice" step whose labels sit at least `min_px` apart */
static double cui__ins_step(cui_axis_units units, double span, float width, float min_px) {
    if (!(span > 0) || width <= 0) return 0;
    if (units == CUI_AXIS_HOURS) {
        static const double hours[] = { 0.25, 0.5, 1, 2, 3, 6, 12, 24, 48, 168 };
        for (int k = 0; k < CUI_COUNTOF(hours); k++)
            if (hours[k] / span * width >= min_px) return hours[k];
        return 168;
    }
    double raw = span * min_px / width;
    double p = pow(10.0, floor(log10(raw))), m = raw / p;
    return (m <= 1 ? 1 : m <= 2 ? 2 : m <= 5 ? 5 : 10) * p;
}

typedef struct {
    double x0, x1, step;
    cui_axis_units units;
    const char *fmt;
} cui__ins_axis;

/* labels under a span of pixels, each centered on its value and kept inside `clip`;
 * one that would crowd the previous label is left out */
static void cui__ins_draw_axis(cui_ctx *ui, const cui__ins_axis *a, float px, float pw, float y, cui_rect clip, bool ticks) {
    if (!(a->x1 > a->x0)) return;
    double step = a->step > 0 ? a->step : cui__ins_step(a->units, a->x1 - a->x0, pw, a->units == CUI_AXIS_HOURS ? 34 : 56);
    if (!(step > 0)) return;
    float fs = cui__fs(ui, ui->theme.text_xs);
    double first = ceil(a->x0 / step - 1e-9) * step;
    float last_end = -1e9f;
    for (double v = first; v <= a->x1 + step * 1e-6; v += step) {
        float x = px + (float)((v - a->x0) / (a->x1 - a->x0)) * pw;
        if (ticks) cui_draw_rect(ui, (cui_rect){ floorf(x), y - 6, 1, 3 }, CUI_CORNERS(0), CUI_C(ui, BORDER_STRONG));
        const char *lbl = cui__ins_label(ui, a->units, a->fmt, v, true);
        float lw = cui__text_width(ui, CUI_FONT_MONO, fs, lbl, -1, true);
        float lx = CUI_CLAMP(x - lw * 0.5f, clip.x, clip.x + clip.w - lw);
        if (lx < last_end + 6) continue;
        cui__draw_text_line(ui, CUI_FONT_MONO, fs, lx, y, CUI_C(ui, TEXT_MUTED), lbl, -1, true);
        last_end = lx + lw;
    }
}

/* ======================================================================== */
/* Annotations, shared by every chart that takes them                       */
/* ======================================================================== */

static void cui__ann_dashes(cui_ctx *ui, float x0, float x1, float y, cui_color c) {
    for (float x = x0; x < x1; x += 6) cui_draw_rect(ui, (cui_rect){ x, y, CUI_MIN(3.0f, x1 - x), 1 }, CUI_CORNERS(0), c);
}

/* Draws `a` over `plot`, mapping x from x0..x1 and values from y0..y1. Labels of vertical
 * lines start at `label_top` (headroom above the plot, or the plot's own top); labels of
 * horizontal lines go right of the plot when `gutter` is set, else inside its right end.
 * A label that would cover one placed before it tries the other side of its line, then
 * moves down a step at a time. */
void cui__paint_annotations(cui_ctx *ui, const cui_chart_annotation *a, int n, cui_rect plot, double x0, double x1,
                            double y0, double y1, float label_top, bool gutter) {
    if (!a || n <= 0 || !(x1 > x0) || !(y1 > y0)) return;
    float fs = cui__fs(ui, ui->theme.text_xs), lh = cui__font_line_height(ui, CUI_FONT_MONO, fs);
    cui_color em = cui__emphasis(ui);
    /* first every label's place, in the order given: earlier annotations choose first */
    cui_rect *box = cui__frame_alloc(ui, sizeof(cui_rect) * (size_t)n);
    for (int i = 0; i < n; i++) {
        const cui_chart_annotation *an = &a[i];
        box[i] = (cui_rect){ 0, 0, 0, 0 };
        if (an->kind == CUI_ANNOTATION_Y || (!an->label && !an->detail) || an->x < x0 || an->x > x1) continue;
        float xx = floorf(plot.x + (float)((an->x - x0) / (x1 - x0)) * plot.w) + 0.5f;
        float y = plot.y + plot.h - (float)((an->y - y0) / (y1 - y0)) * plot.h;
        float bw = CUI_MAX(an->label ? cui__text_width(ui, CUI_FONT_MONO, fs, an->label, -1, true) : 0,
                           an->detail ? cui__text_width(ui, CUI_FONT_MONO, fs, an->detail, -1, true) : 0);
        float bh = (an->label ? lh : 0) + (an->detail ? lh : 0);
        bool point = an->kind == CUI_ANNOTATION_POINT;
        float by = point ? y - bh - 4 : label_top, bx = xx + 6;
        float cx0 = plot.x - (gutter ? 0 : 4), cx1 = plot.x + plot.w + (gutter ? CUI__INS_GUTTER : 4);
        /* right of the line, else left of it, else one label lower, up to eight times */
        bool ok = false;
        for (int tries = 0; tries < 8 && !ok; tries++) {
            for (int side = 0; side < 2 && !ok; side++) {
                float cx = side == 0 ? xx + 6 : xx - 6 - bw;
                if (cx < cx0 || cx + bw > cx1) continue;
                cui_rect r = { cx - 2, by, bw + 4, bh };
                bool hit = false;
                for (int k = 0; k < i && !hit; k++)
                    hit = box[k].w > 0 && r.x < box[k].x + box[k].w && box[k].x < r.x + r.w && r.y < box[k].y + box[k].h && box[k].y < r.y + r.h;
                if (!hit) { bx = cx; ok = true; }
            }
            if (!ok) by += bh + 2;
        }
        if (!ok) { by = point ? y - bh - 4 : label_top; bx = CUI_CLAMP(xx + 6, cx0, cx1 - bw); }
        box[i] = (cui_rect){ bx - 2, by, bw + 4, bh };
    }
    /* then the lines and dots, under every label */
    for (int i = 0; i < n; i++) {
        const cui_chart_annotation *an = &a[i];
        cui_color line = an->accent ? em : CUI_C(ui, TEXT_MUTED);
        if (an->kind == CUI_ANNOTATION_Y) {
            if (an->y < y0 || an->y > y1) continue;
            float yy = floorf(plot.y + plot.h - (float)((an->y - y0) / (y1 - y0)) * plot.h) + 0.5f;
            cui__ann_dashes(ui, plot.x, plot.x + plot.w, yy - 0.5f, an->accent ? em : CUI_C(ui, BORDER_STRONG));
            continue;
        }
        if (an->x < x0 || an->x > x1) continue;
        float x = plot.x + (float)((an->x - x0) / (x1 - x0)) * plot.w, xx = floorf(x) + 0.5f;
        if (an->kind == CUI_ANNOTATION_POINT) {
            cui_draw_circle(ui, (cui_vec2){ x, plot.y + plot.h - (float)((an->y - y0) / (y1 - y0)) * plot.h }, 3.5f, line);
        } else {
            float top = box[i].w > 0 ? box[i].y + box[i].h + 3 : plot.y;
            if (top < plot.y + plot.h) cui_draw_rect(ui, (cui_rect){ xx - 0.5f, top, 1, plot.y + plot.h - top }, CUI_CORNERS(0), line);
        }
    }
    /* last the labels: a label inside the plot, or beside a point, gets the page's color behind it */
    for (int i = 0; i < n; i++) {
        const cui_chart_annotation *an = &a[i];
        if (an->kind == CUI_ANNOTATION_Y) {
            if (!an->label || an->y < y0 || an->y > y1) continue;
            float yy = floorf(plot.y + plot.h - (float)((an->y - y0) / (y1 - y0)) * plot.h) + 0.5f;
            float w1 = cui__text_width(ui, CUI_FONT_MONO, fs, an->label, -1, true);
            bool out = gutter && w1 <= CUI__INS_GUTTER - 6;   /* a label too wide for the gutter goes inside, above its line */
            float lx = out ? plot.x + plot.w + 6 : plot.x + plot.w - w1 - 2;
            float ly = out ? yy - lh * 0.5f : yy - lh - 2;
            if (!out) cui_draw_rect(ui, (cui_rect){ lx - 3, ly, w1 + 6, lh }, CUI_CORNERS(3), cui_alpha(CUI_C(ui, BACKGROUND), 0.86f));
            cui__draw_text_line(ui, CUI_FONT_MONO, fs, lx, ly, an->accent ? em : CUI_C(ui, TEXT_MUTED), an->label, -1, true);
            continue;
        }
        if (box[i].w <= 0) continue;
        float bx = box[i].x + 2, by = box[i].y;
        if (an->kind == CUI_ANNOTATION_POINT || by + box[i].h > plot.y + 1)
            cui_draw_rect(ui, (cui_rect){ bx - 3, by, box[i].w + 2, box[i].h }, CUI_CORNERS(3), cui_alpha(CUI_C(ui, BACKGROUND), 0.86f));
        if (an->label) cui__draw_text_line(ui, CUI_FONT_MONO, fs, bx, by, an->accent ? em : CUI_C(ui, FOREGROUND), an->label, -1, true);
        if (an->detail) cui__draw_text_line(ui, CUI_FONT_MONO, fs, bx, by + (an->label ? lh : 0), CUI_C(ui, TEXT_MUTED), an->detail, -1, true);
    }
}

/* the annotations copied into frame memory, for a paint callback that runs after the caller's strings may be gone */
cui_chart_annotation *cui__copy_annotations(cui_ctx *ui, const cui_chart_annotation *a, int n) {
    if (!a || n <= 0) return NULL;
    cui_chart_annotation *c = cui__frame_alloc(ui, sizeof *c * (size_t)n);
    for (int i = 0; i < n; i++) {
        c[i] = a[i];
        c[i].label = a[i].label ? cui__frame_strdup(ui, a[i].label, -1) : NULL;
        c[i].detail = a[i].detail ? cui__frame_strdup(ui, a[i].detail, -1) : NULL;
    }
    return c;
}

/* label rows above the plot for vertical lines that carry text */
static float cui__ann_headroom(cui_ctx *ui, const cui_chart_annotation *a, int n) {
    int lines = 0;
    for (int i = 0; i < n; i++)
        if (a[i].kind == CUI_ANNOTATION_X) lines = CUI_MAX(lines, (a[i].label ? 1 : 0) + (a[i].detail ? 1 : 0));
    if (!lines) return 0;
    float fs = cui__fs(ui, ui->theme.text_xs);
    return lines * cui__font_line_height(ui, CUI_FONT_MONO, fs) + 6;
}

/* ======================================================================== */
/* Dot-matrix chart                                                         */
/* ======================================================================== */

typedef struct { cui_spring intro; int cursor; bool keyed; float focus; } cui__dm_state;

typedef struct {
    float **vals; bool *accent; const char **names; int ns, n;
    double x0, x1; float lo, hi, pitch, head;
    cui__ins_axis axis;
    const char *fmt, *unit;
    bool grid, axis_on, readout;
    int cursor; float intro, focus;
    cui_chart_annotation *ann; int nann;
} cui__dm_paint;

/* the geometry both the build and the paint use */
static cui_rect cui__dm_plot(cui_rect r, float head, bool axis_on) {
    cui_rect p = { r.x, r.y + head, r.w - CUI__INS_GUTTER, r.h - head - (axis_on ? CUI__INS_AXIS_H : 0) };
    p.w = CUI_MAX(p.w, 1.0f); p.h = CUI_MAX(p.h, 1.0f);
    return p;
}

/* a value as the chart counts it: negative, infinite and NaN values are 0 */
static float cui__dm_v(float v) { return v > 0 && v < 1e30f ? v : 0; }

/* a series' value at a fractional sample position */
static float cui__dm_at(const float *v, int n, double f) {
    if (!v || n <= 0) return 0;
    if (n == 1 || !(f > 0)) return cui__dm_v(v[0]);
    if (f >= n - 1) return cui__dm_v(v[n - 1]);
    int i = (int)f;
    float t = (float)(f - i), a = cui__dm_v(v[i]), b = cui__dm_v(v[i + 1]);
    return a + (b - a) * t;
}

static float cui__dm_total(float *const *vals, int ns, int n, double f) {
    float s = 0;
    for (int k = 0; k < ns; k++) s += cui__dm_at(vals[k], n, f);
    return s;
}

static void cui__paint_dot_chart(cui_ctx *ui, cui_node *nd, void *data) {
    const cui__dm_paint *p = data;
    cui_rect r = nd->rect;
    cui_rect plot = cui__dm_plot(r, p->head, p->axis_on);
    float pitch = p->pitch;
    int cols = (int)floorf(plot.w / pitch), rows = (int)floorf(plot.h / pitch);
    if (cols < 1 || rows < 1) return;
    /* center the grid in the plot, so the dots never touch its edges unevenly */
    float gx = plot.x + (plot.w - cols * pitch) * 0.5f + pitch * 0.5f;
    float gy = plot.y + plot.h - (plot.h - rows * pitch) * 0.5f - pitch * 0.5f;
    float lit_r = pitch * 0.34f, dim_r = pitch * 0.13f;
    cui_color fg = CUI_C(ui, FOREGROUND), em = cui__emphasis(ui);
    cui_color dim = cui_alpha(fg, ui->mode == CUI_THEME_DARK ? 0.16f : 0.12f);
    float range = p->hi - p->lo;
    int shown = (int)ceilf(cols * cui__clampf(p->intro, 0, 1));
    int cursor_col = -1;
    if (p->cursor >= 0 && p->n > 1) cursor_col = (int)lround((double)p->cursor / (p->n - 1) * (cols - 1));
    float cum[16];
    int ns = CUI_MIN(p->ns, 16);
    bool any_accent = false;
    for (int k = 0; k < ns; k++) any_accent |= p->accent[k];
    for (int c = 0; c < cols; c++) {
        double fx = cols > 1 ? (double)c / (cols - 1) : 0.5;
        double f = fx * (p->n - 1);
        float acc = 0;
        for (int k = 0; k < ns; k++) { acc += cui__dm_at(p->vals[k], p->n, f); cum[k] = acc; }
        float x = gx + c * pitch;
        for (int rr = 0; rr < rows; rr++) {
            float v0 = p->lo + range * (float)rr / rows, v1 = p->lo + range * (float)(rr + 1) / rows;
            float y = gy - rr * pitch;
            /* lit up to the stack's total; the top dot fades with how much of it is covered,
             * and a dot belongs to the band its middle falls in */
            int band = -1;
            float cover = 0, total = ns > 0 ? cum[ns - 1] : 0;
            if (c < shown && total > v0) {
                cover = cui__clampf((total - v0) / (v1 - v0), 0, 1);
                float mid = (v0 + v1) * 0.5f;
                /* a dot under the top belongs to the band its middle falls in; the top dot,
                 * partly covered, to the band that reaches the top */
                if (mid < total) { for (int k = 0; k < ns; k++) if (cum[k] > mid) { band = k; break; } }
                else for (int k = 0; k < ns; k++) if (cum[k] >= total) { band = k; break; }
                if (band < 0) band = ns - 1;
            }
            if (band < 0 || cover < 0.12f) {
                if (p->grid) cui_draw_circle(ui, (cui_vec2){ x, y }, dim_r, dim);
                continue;
            }
            /* bands alternate two strengths; beside an accent series they step back further */
            cui_color col = p->accent[band] ? em : cui_alpha(fg, any_accent ? (band % 2 ? 0.34f : 0.56f) : (band % 2 ? 0.55f : 0.9f));
            /* the column under the cursor stands out a little */
            if (cursor_col == c && !p->accent[band]) col = cui_alpha(fg, any_accent ? (band % 2 ? 0.6f : 0.8f) : 1.0f);
            if (cover < 1) col = cui_alpha(col, 0.35f + 0.65f * cover);
            cui_draw_circle(ui, (cui_vec2){ x, y }, lit_r, col);
        }
    }
    cui__paint_annotations(ui, p->ann, p->nann, plot, p->x0, p->x1, p->lo, p->hi, r.y, true);
    if (p->axis_on) cui__ins_draw_axis(ui, &p->axis, plot.x, plot.w, plot.y + plot.h + 5, (cui_rect){ r.x, r.y, r.w, r.h }, false);
    /* the readout: a dot on the top of the stack, with the time and the value beside it */
    if (p->cursor >= 0 && p->readout && p->n > 0) {
        double f = p->cursor;
        float v = cui__dm_total(p->vals, ns, p->n, f);
        double xv = p->n > 1 ? p->x0 + (p->x1 - p->x0) * f / (p->n - 1) : p->x0;
        float px = plot.x + (float)((xv - p->x0) / (p->x1 - p->x0)) * plot.w;
        float py = plot.y + plot.h - cui__clampf((v - p->lo) / range, 0, 1) * plot.h;
        cui_draw_circle(ui, (cui_vec2){ px, py }, 3.5f, fg);
        float fs = cui__fs(ui, ui->theme.text_xs), lh = cui__font_line_height(ui, CUI_FONT_MONO, fs);
        const char *val = cui__frame_printf(ui, p->fmt && *p->fmt ? p->fmt : "%.0f", (double)v);
        const char *lbl = cui__frame_printf(ui, "%s \xc2\xb7 %s%s%s", cui__ins_label(ui, p->axis.units, p->axis.fmt, xv, false), val,
                                            p->unit && *p->unit ? " " : "", p->unit ? p->unit : "");
        float lw = cui__text_width(ui, CUI_FONT_MONO, fs, lbl, -1, true);
        float lx = px + 9 + lw <= r.x + r.w ? px + 9 : px - 9 - lw;
        float ly = CUI_CLAMP(py - lh - 2, r.y, plot.y + plot.h - lh);
        cui_rect bg = { lx - 3, ly, lw + 6, lh };
        cui_draw_rect(ui, bg, CUI_CORNERS(3), cui_alpha(CUI_C(ui, BACKGROUND), 0.82f));
        cui__draw_text_line(ui, CUI_FONT_MONO, fs, lx, ly, fg, lbl, -1, true);
    }
    if (p->focus > 0.01f) cui__draw_focus_ring(ui, (cui_rect){ plot.x - 4, plot.y - 4, plot.w + 8, plot.h + 8 }, CUI_CORNERS(6), p->focus);
}

cui_dot_chart_result cui_dot_chart_ex(cui_ctx *ui, const char *id_str, const cui_dot_series *series, int series_count, int count, const cui_dot_chart_opts *o) {
    cui_dot_chart_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui_dot_chart_result res = { -1, 0, 0, false };
    if (!series || series_count < 0) series_count = 0;
    series_count = CUI_MIN(series_count, 16);
    if (count < 0) count = 0;
    cui_id id = cui_make_id(ui, id_str);
    cui_interact r = cui__interact(ui, id, CUI_IF_FOCUSABLE);
    cui__dm_state *st = cui_state(ui, id, sizeof *st);
    cui__spring(ui, &st->intro, 1.0f, 0.9f, 0.0f);
    cui__tween(ui, &st->focus, r.focus_visible ? 1.0f : 0.0f, CUI__T_FOCUS);
    double x0 = o->x_min, x1 = o->x_max;
    if (!(x1 > x0)) { x0 = 0; x1 = CUI_MAX(1, count - 1); }
    float head = cui__ann_headroom(ui, o->annotations, o->annotation_count);
    bool axis_on = !o->no_axis;
    float pitch = o->pitch > 0 ? CUI_CLAMP(o->pitch, 3.0f, 40.0f) : 6.0f;

    /* the cursor: the pointer's sample, or the one the arrow keys moved to */
    cui_rect R = cui__prev_rect(ui, id);
    bool known = cui__has_prev_rect(ui, id);
    cui_rect plot = cui__dm_plot(R, head, axis_on);
    int cursor = -1;
    if (count > 0) {
        if (r.focused) {
            int c = st->keyed ? st->cursor : count / 2;
            int big = (ui->mods & CUI_MOD_SHIFT) ? 10 : 1;
            bool moved = true;
            if (cui_key_pressed(ui, CUI_KEY_LEFT)) c -= big;
            else if (cui_key_pressed(ui, CUI_KEY_RIGHT)) c += big;
            else if (cui_key_pressed(ui, CUI_KEY_HOME)) c = 0;
            else if (cui_key_pressed(ui, CUI_KEY_END)) c = count - 1;
            else moved = false;
            if (moved) { st->keyed = true; st->cursor = CUI_CLAMP(c, 0, count - 1); }
            if (cui_key_pressed(ui, CUI_KEY_ESCAPE)) st->keyed = false;
        } else st->keyed = false;
        if (r.hovered && known && cui__rect_contains(plot, ui->mx, ui->my)) {
            float t = cui__clampf((ui->mx - plot.x) / plot.w, 0, 1);
            cursor = (int)lround(t * (count - 1));
            st->keyed = false;
        } else if (st->keyed) cursor = CUI_CLAMP(st->cursor, 0, count - 1);
    }
    /* copies of the data, for the paint callback, and the value range */
    cui__dm_paint *p = cui__frame_alloc(ui, sizeof *p);
    memset(p, 0, sizeof *p);
    p->ns = series_count; p->n = count;
    p->vals = cui__frame_alloc(ui, sizeof(float *) * (size_t)CUI_MAX(1, series_count));
    p->accent = cui__frame_alloc(ui, sizeof(bool) * (size_t)CUI_MAX(1, series_count));
    for (int k = 0; k < series_count; k++) {
        float *c = NULL;
        if (series[k].values && count > 0) { c = cui__frame_alloc(ui, sizeof(float) * (size_t)count); memcpy(c, series[k].values, sizeof(float) * (size_t)count); }
        p->vals[k] = c;
        p->accent[k] = series[k].accent;
    }
    float hi = 0;
    for (int i = 0; i < count; i++) hi = CUI_MAX(hi, cui__dm_total(p->vals, series_count, count, i));
    p->lo = 0; p->hi = hi > 0 ? hi * 1.08f : 1;
    if (o->min != o->max) { p->lo = CUI_MIN(o->min, o->max); p->hi = CUI_MAX(o->min, o->max); }
    p->x0 = x0; p->x1 = x1; p->pitch = pitch; p->head = head;
    p->axis.x0 = x0; p->axis.x1 = x1; p->axis.step = o->x_step; p->axis.units = o->x_units;
    p->axis.fmt = o->x_format ? cui__frame_strdup(ui, o->x_format, -1) : NULL;
    p->fmt = o->format ? cui__frame_strdup(ui, o->format, -1) : NULL;
    p->unit = o->unit ? cui__frame_strdup(ui, o->unit, -1) : NULL;
    p->grid = !o->no_grid; p->axis_on = axis_on; p->readout = !o->no_readout;
    p->cursor = cursor; p->intro = st->intro.x; p->focus = st->focus;
    p->ann = cui__copy_annotations(ui, o->annotations, o->annotation_count);
    p->nann = p->ann ? o->annotation_count : 0;
    if (cursor >= 0) {
        res.hovered = cursor;
        res.x = count > 1 ? x0 + (x1 - x0) * cursor / (count - 1) : x0;
        res.value = cui__dm_total(p->vals, series_count, count, cursor);
    }
    /* a click lands at the pointer's exact x, not the nearest sample */
    if (r.clicked && count > 0) {
        res.clicked = true;
        if (!r.keyboard_activate && known) res.x = x0 + (x1 - x0) * cui__clampf((ui->mx - plot.x) / plot.w, 0, 1);
    }
    int nd = cui__node_new(ui, id);
    cui_node *n = &ui->nodes[nd];
    n->w = o->width.kind ? o->width : CUI_FILL;
    n->h = CUI_PX(o->height > 0 ? o->height : 200);
    n->direction = CUI_DIRECTION_LTR;   /* a time axis runs left to right in every language */
    n->paint = cui__paint_dot_chart; n->paint_data = p;
    cui__mark_interactive(ui, nd, CUI_IF_FOCUSABLE);
    ui->last_id = id;
    return res;
}

/* ======================================================================== */
/* Event track                                                              */
/* ======================================================================== */

typedef struct { int drag, sel; float press_x; bool moved, own_init; float focus; } cui__ev_state;

typedef struct {
    double *t; int n; double lo, hi;
    int sel, hover, drag;
    cui__ins_axis axis;
    bool axis_on;
    float focus;
} cui__ev_paint;

static double cui__ev_time(double lo, double hi, double step, cui_rect R, float mx) {
    float w = CUI_MAX(1.0f, R.w - CUI__INS_GUTTER);
    double t = lo + (hi - lo) * cui__clampf((mx - R.x) / w, 0, 1);
    if (step > 0) t = lo + floor((t - lo) / step + 0.5) * step;
    return CUI_CLAMP(t, lo, hi);
}

/* keeps the array sorted after times[i] changed; returns where that event ended up */
static int cui__ev_settle(double *t, int n, int i) {
    while (i > 0 && t[i] < t[i - 1]) { double s = t[i]; t[i] = t[i - 1]; t[i - 1] = s; i--; }
    while (i + 1 < n && t[i] > t[i + 1]) { double s = t[i]; t[i] = t[i + 1]; t[i + 1] = s; i++; }
    return i;
}

static int cui__ev_insert(double *t, int *n, int cap, double v) {
    if (*n >= cap) return -1;
    t[*n] = v;
    (*n)++;
    return cui__ev_settle(t, *n, *n - 1);
}

static void cui__ev_remove(double *t, int *n, int i) {
    if (i < 0 || i >= *n) return;
    memmove(t + i, t + i + 1, sizeof(double) * (size_t)(*n - i - 1));
    (*n)--;
}

static void cui__paint_event_track(cui_ctx *ui, cui_node *nd, void *data) {
    const cui__ev_paint *p = data;
    cui_rect r = nd->rect;
    float w = CUI_MAX(1.0f, r.w - CUI__INS_GUTTER);
    float fs = cui__fs(ui, ui->theme.text_xs);
    float y = r.y + CUI__EV_DOT_Y;
    cui_color em = cui__emphasis(ui);
    /* the baseline the dots sit under, with a tick at each label */
    cui_draw_rect(ui, (cui_rect){ r.x, r.y, w, 1 }, CUI_CORNERS(0), CUI_C(ui, BORDER_STRONG));
    if (p->axis_on) cui__ins_draw_axis(ui, &p->axis, r.x, w, r.y + 36, (cui_rect){ r.x, r.y, r.w, r.h }, false);
    if (!(p->hi > p->lo)) return;
    int label = p->drag >= 0 ? p->drag : p->hover >= 0 ? p->hover : p->sel;
    for (int i = 0; i < p->n; i++) {
        float x = r.x + (float)((p->t[i] - p->lo) / (p->hi - p->lo)) * w;
        bool on = i == p->sel, hot = i == p->hover || i == p->drag;
        float rad = on || hot ? 5.0f : 4.0f;
        cui_color c = on ? em : hot ? CUI_C(ui, FOREGROUND) : cui_alpha(CUI_C(ui, FOREGROUND), 0.82f);
        /* a hairline up to the baseline marks where the event meets the chart above */
        if (on) cui_draw_rect(ui, (cui_rect){ floorf(x), r.y, 1, y - rad - r.y }, CUI_CORNERS(0), cui_alpha(em, 0.6f));
        cui_draw_circle(ui, (cui_vec2){ x, y }, rad, c);
        if (on && p->focus > 0.01f) cui__draw_focus_ring(ui, (cui_rect){ x - 8, y - 8, 16, 16 }, CUI_CORNERS(8), p->focus);
        if (i == label) {
            const char *lbl = cui__ins_label(ui, p->axis.units, p->axis.fmt, p->t[i], false);
            float lw = cui__text_width(ui, CUI_FONT_MONO, fs, lbl, -1, true);
            float lx = CUI_CLAMP(x - lw * 0.5f, r.x, r.x + r.w - lw);
            cui_color lc = on ? em : CUI_C(ui, FOREGROUND);
            cui__draw_text_line(ui, CUI_FONT_MONO, fs, lx, r.y + 20, lc, lbl, -1, true);
        }
    }
}

cui_event_track_result cui_event_track_ex(cui_ctx *ui, const char *id_str, double *times, int *count, int capacity,
                                          double lo, double hi, const cui_event_track_opts *o) {
    cui_event_track_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui_event_track_result res = { false, -1, -1, -1, false };
    int zero = 0;
    if (!count) count = &zero;
    if (!times || capacity < 0) capacity = 0;
    if (!times) *count = 0;
    *count = CUI_CLAMP(*count, 0, capacity);
    if (!(hi > lo)) hi = lo + 1;
    cui_id id = cui_make_id(ui, id_str);
    cui_interact r = cui__interact(ui, id, CUI_IF_FOCUSABLE | CUI_IF_DRAG);
    cui__ev_state *st = cui_state(ui, id, sizeof *st);
    if (!st->own_init) { st->own_init = true; st->drag = -1; st->sel = -1; }
    int *sel = o->selected ? o->selected : &st->sel;
    if (*sel >= *count || *sel < -1) *sel = -1;
    cui__tween(ui, &st->focus, r.focus_visible ? 1.0f : 0.0f, CUI__T_FOCUS);
    /* the caller's times may arrive unsorted: sort them once, keeping the selection on its event */
    bool sorted = true;
    for (int i = 0; i < *count; i++) {
        times[i] = CUI_CLAMP(times[i], lo, hi);
        if (i > 0 && times[i] < times[i - 1]) sorted = false;
    }
    if (!sorted) {
        double keep = *sel >= 0 ? times[*sel] : 0;
        for (int a = 1; a < *count; a++) cui__ev_settle(times, *count, a);
        if (*sel >= 0) for (int i = 0; i < *count; i++) if (times[i] == keep) { *sel = i; break; }
    }
    double step = o->step > 0 ? o->step : 0;
    cui_rect R = cui__prev_rect(ui, id);
    bool known = cui__has_prev_rect(ui, id);
    float w = CUI_MAX(1.0f, R.w - CUI__INS_GUTTER);
    /* the event nearest the pointer, within reach */
    int near_i = -1;
    if (known && r.hovered) {
        float best = CUI__EV_REACH;
        for (int i = 0; i < *count; i++) {
            float x = R.x + (float)((times[i] - lo) / (hi - lo)) * w;
            float dx = fabsf(ui->mx - x);
            if (dx <= best && fabsf(ui->my - (R.y + CUI__EV_DOT_Y)) <= 16) { best = dx; near_i = i; }
        }
    }
    /* the pointer: press takes the nearest event or adds one, drag moves it, double-click removes it */
    if (known && r.hovered && ui->mpressed[0]) {
        if (r.double_clicked && near_i >= 0 && !o->no_remove) {
            cui__ev_remove(times, count, near_i);
            res.removed = near_i; res.changed = true;
            *sel = -1; st->drag = -1;
        } else if (near_i >= 0) {
            st->drag = near_i; *sel = near_i; st->press_x = ui->mx; st->moved = false;
        } else if (!o->no_add && *count < capacity && ui->my <= R.y + CUI__EV_DOT_Y + 16) {
            int at = cui__ev_insert(times, count, capacity, cui__ev_time(lo, hi, step, R, ui->mx));
            if (at >= 0) { res.added = at; res.changed = true; *sel = at; st->drag = at; st->press_x = ui->mx; st->moved = true; }
        }
    }
    if (st->drag >= 0 && st->drag < *count && ui->mdown[0] && r.active) {
        if (fabsf(ui->mx - st->press_x) > 2) st->moved = true;
        if (st->moved) {
            double t = cui__ev_time(lo, hi, step, R, ui->mx);
            if (t != times[st->drag]) {
                times[st->drag] = t;
                st->drag = cui__ev_settle(times, *count, st->drag);
                *sel = st->drag;
                res.changed = true;
            }
            cui__set_cursor(ui, CUI_CURSOR_RESIZE_EW);
        }
    }
    if (!ui->mdown[0]) st->drag = -1;
    if (st->drag >= *count) st->drag = -1;
    if (near_i >= 0 && st->drag < 0) cui__set_cursor(ui, CUI_CURSOR_HAND);
    /* the keys */
    if (r.focused) {
        double k = step > 0 ? step : (hi - lo) / 100.0;
        double big = (ui->mods & CUI_MOD_SHIFT) ? 10 : 1;
        if (*sel >= 0 && *sel < *count) {
            double t = times[*sel];
            if (cui_key_pressed(ui, CUI_KEY_LEFT)) t -= k * big;
            if (cui_key_pressed(ui, CUI_KEY_RIGHT)) t += k * big;
            t = CUI_CLAMP(t, lo, hi);
            if (t != times[*sel]) { times[*sel] = t; *sel = cui__ev_settle(times, *count, *sel); res.changed = true; }
        }
        if (*count > 0) {
            if (cui_key_pressed(ui, CUI_KEY_DOWN)) *sel = *sel < 0 ? 0 : CUI_MIN(*sel + 1, *count - 1);
            if (cui_key_pressed(ui, CUI_KEY_UP)) *sel = *sel < 0 ? *count - 1 : CUI_MAX(*sel - 1, 0);
            if (cui_key_pressed(ui, CUI_KEY_HOME)) *sel = 0;
            if (cui_key_pressed(ui, CUI_KEY_END)) *sel = *count - 1;
        }
        if (cui_key_pressed(ui, CUI_KEY_ESCAPE)) *sel = -1;
        if (r.keyboard_activate && !o->no_add && *count < capacity) {
            double t = *sel >= 0 && *sel < *count ? times[*sel] + k : lo + (hi - lo) * 0.5;
            if (step > 0) t = lo + floor((t - lo) / step + 0.5) * step;
            int at = cui__ev_insert(times, count, capacity, CUI_CLAMP(t, lo, hi));
            if (at >= 0) { res.added = at; res.changed = true; *sel = at; }
        }
        if ((cui_key_pressed(ui, CUI_KEY_DELETE) || cui_key_pressed(ui, CUI_KEY_BACKSPACE)) && !o->no_remove && *sel >= 0 && *sel < *count) {
            res.removed = *sel;
            cui__ev_remove(times, count, *sel);
            res.changed = true;
            *sel = *count > 0 ? CUI_MIN(res.removed, *count - 1) : -1;
        }
    }
    res.selected = *sel;
    res.dragging = st->drag >= 0 && st->moved;
    cui__ev_paint *p = cui__frame_alloc(ui, sizeof *p);
    memset(p, 0, sizeof *p);
    p->n = *count; p->lo = lo; p->hi = hi;
    p->t = cui__frame_alloc(ui, sizeof(double) * (size_t)CUI_MAX(1, *count));
    if (*count > 0) memcpy(p->t, times, sizeof(double) * (size_t)*count);
    p->sel = *sel; p->drag = res.dragging ? st->drag : -1;
    p->hover = st->drag < 0 ? near_i : -1;
    p->axis.x0 = lo; p->axis.x1 = hi; p->axis.step = o->label_step; p->axis.units = o->units;
    p->axis.fmt = o->format ? cui__frame_strdup(ui, o->format, -1) : NULL;
    p->axis_on = !o->no_axis;
    p->focus = st->focus;
    int nd = cui__node_new(ui, id);
    cui_node *n = &ui->nodes[nd];
    n->w = o->width.kind ? o->width : CUI_FILL;
    n->h = CUI_PX(o->no_axis ? 36 : 54);
    n->direction = CUI_DIRECTION_LTR;
    n->paint = cui__paint_event_track; n->paint_data = p;
    cui__mark_interactive(ui, nd, CUI_IF_FOCUSABLE | CUI_IF_DRAG);
    ui->last_id = id;
    return res;
}

/* ======================================================================== */
/* Readout                                                                  */
/* ======================================================================== */

void cui_readout_ex(cui_ctx *ui, const char *label, const char *value, const cui_readout_opts *o) {
    cui_readout_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    float vs = o->size == CUI_SIZE_SM ? 17 : o->size == CUI_SIZE_LG ? 44 : 28;
    vs = cui__fs(ui, vs);
    float fx = cui__fs(ui, ui->theme.text_xs);
    cui_align cross = o->align == CUI_TEXT_CENTER ? CUI_ALIGN_CENTER
                    : (o->align == CUI_TEXT_END || o->align == CUI_TEXT_RIGHT) ? CUI_ALIGN_END : CUI_ALIGN_START;
    int col = cui__node_new(ui, 0);
    ui->nodes[col].gap = 4;
    ui->nodes[col].align = cross;
    if (o->width.kind) ui->nodes[col].w = o->width;
    cui__open(ui, col);
    if (label && *label) {
        int t = cui__text_node(ui, label, -1, CUI_FONT_MONO, fx, CUI_C(ui, TEXT_MUTED), false);
        ui->nodes[t].line_height = 1.3f;
    }
    int row = cui__node_new(ui, 0);
    ui->nodes[row].dir = CUI_ROW; ui->nodes[row].gap = CUI_MAX(4.0f, vs * 0.18f);
    ui->nodes[row].align = CUI_ALIGN_END;   /* the unit sits on the value's baseline */
    cui__open(ui, row);
    cui_color vc = o->accent ? cui__emphasis(ui) : o->tone ? cui__tone_color(ui, o->tone) : CUI_C(ui, FOREGROUND);
    int v = cui__text_node(ui, value ? value : "", -1, CUI_FONT_MONO, vs, vc, false);
    ui->nodes[v].line_height = 1.05f;
    ui->nodes[v].flags |= CUI_NF_TABULAR;
    if (o->unit && *o->unit) {
        float us = CUI_MAX(fx, vs * 0.42f);
        int u = cui__text_node(ui, o->unit, -1, CUI_FONT_MONO, us, CUI_C(ui, TEXT_SECONDARY), false);
        ui->nodes[u].line_height = 1.05f;
        /* lift the unit so its baseline meets the value's, not its descender */
        ui->nodes[u].pad[2] = (cui__font_line_height(ui, CUI_FONT_MONO, vs) * 1.05f - cui__font_ascent(ui, CUI_FONT_MONO, vs))
                            - (cui__font_line_height(ui, CUI_FONT_MONO, us) * 1.05f - cui__font_ascent(ui, CUI_FONT_MONO, us));
        if (ui->nodes[u].pad[2] < 0) ui->nodes[u].pad[2] = 0;
    }
    cui__close(ui);
    if (o->detail && *o->detail) {
        int t = cui__text_node(ui, o->detail, -1, CUI_FONT_MONO, fx, CUI_C(ui, TEXT_MUTED), true);
        ui->nodes[t].line_height = 1.4f;
        if (cross == CUI_ALIGN_CENTER) ui->nodes[t].text_align = CUI_TEXT_CENTER;
        else if (cross == CUI_ALIGN_END) ui->nodes[t].text_align = CUI_TEXT_END;
    }
    cui__close(ui);
}

/* ======================================================================== */
/* Dot-matrix display                                                       */
/* ======================================================================== */

/* 5 by 7 glyphs, one byte a row, bit 4 the leftmost dot. ASCII 32 to 126, then the degree
 * sign, the middle dot and a box for anything else. Drawn for CUI. */
static const uint8_t cui__dmf_glyphs[][7] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x04,0x04,0x04,0x04,0x04,0x00,0x04}, {0x0a,0x0a,0x0a,0x00,0x00,0x00,0x00}, {0x0a,0x0a,0x1f,0x0a,0x1f,0x0a,0x0a}, {0x04,0x0f,0x14,0x0e,0x05,0x1e,0x04}, {0x18,0x19,0x02,0x04,0x08,0x13,0x03},
    {0x0c,0x12,0x14,0x08,0x15,0x12,0x0d}, {0x04,0x04,0x00,0x00,0x00,0x00,0x00}, {0x02,0x04,0x08,0x08,0x08,0x04,0x02}, {0x08,0x04,0x02,0x02,0x02,0x04,0x08}, {0x00,0x04,0x15,0x0e,0x15,0x04,0x00}, {0x00,0x04,0x04,0x1f,0x04,0x04,0x00},
    {0x00,0x00,0x00,0x00,0x06,0x04,0x08}, {0x00,0x00,0x00,0x1f,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x0c,0x0c}, {0x00,0x01,0x02,0x04,0x08,0x10,0x00}, {0x0e,0x11,0x13,0x15,0x19,0x11,0x0e}, {0x04,0x0c,0x04,0x04,0x04,0x04,0x0e},
    {0x0e,0x11,0x01,0x02,0x04,0x08,0x1f}, {0x1f,0x02,0x04,0x02,0x01,0x11,0x0e}, {0x02,0x06,0x0a,0x12,0x1f,0x02,0x02}, {0x1f,0x10,0x1e,0x01,0x01,0x11,0x0e}, {0x06,0x08,0x10,0x1e,0x11,0x11,0x0e}, {0x1f,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e}, {0x0e,0x11,0x11,0x0f,0x01,0x02,0x0c}, {0x00,0x0c,0x0c,0x00,0x0c,0x0c,0x00}, {0x00,0x0c,0x0c,0x00,0x0c,0x04,0x08}, {0x02,0x04,0x08,0x10,0x08,0x04,0x02}, {0x00,0x00,0x1f,0x00,0x1f,0x00,0x00},
    {0x08,0x04,0x02,0x01,0x02,0x04,0x08}, {0x0e,0x11,0x01,0x02,0x04,0x00,0x04}, {0x0e,0x11,0x01,0x0d,0x15,0x15,0x0e}, {0x0e,0x11,0x11,0x1f,0x11,0x11,0x11}, {0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e}, {0x0e,0x11,0x10,0x10,0x10,0x11,0x0e},
    {0x1c,0x12,0x11,0x11,0x11,0x12,0x1c}, {0x1f,0x10,0x10,0x1e,0x10,0x10,0x1f}, {0x1f,0x10,0x10,0x1e,0x10,0x10,0x10}, {0x0e,0x11,0x10,0x17,0x11,0x11,0x0f}, {0x11,0x11,0x11,0x1f,0x11,0x11,0x11}, {0x0e,0x04,0x04,0x04,0x04,0x04,0x0e},
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0c}, {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, {0x10,0x10,0x10,0x10,0x10,0x10,0x1f}, {0x11,0x1b,0x15,0x15,0x11,0x11,0x11}, {0x11,0x11,0x19,0x15,0x13,0x11,0x11}, {0x0e,0x11,0x11,0x11,0x11,0x11,0x0e},
    {0x1e,0x11,0x11,0x1e,0x10,0x10,0x10}, {0x0e,0x11,0x11,0x11,0x15,0x12,0x0d}, {0x1e,0x11,0x11,0x1e,0x14,0x12,0x11}, {0x0f,0x10,0x10,0x0e,0x01,0x01,0x1e}, {0x1f,0x04,0x04,0x04,0x04,0x04,0x04}, {0x11,0x11,0x11,0x11,0x11,0x11,0x0e},
    {0x11,0x11,0x11,0x11,0x11,0x0a,0x04}, {0x11,0x11,0x11,0x15,0x15,0x15,0x0a}, {0x11,0x11,0x0a,0x04,0x0a,0x11,0x11}, {0x11,0x11,0x0a,0x04,0x04,0x04,0x04}, {0x1f,0x01,0x02,0x04,0x08,0x10,0x1f}, {0x0e,0x08,0x08,0x08,0x08,0x08,0x0e},
    {0x00,0x10,0x08,0x04,0x02,0x01,0x00}, {0x0e,0x02,0x02,0x02,0x02,0x02,0x0e}, {0x04,0x0a,0x11,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x1f}, {0x08,0x04,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x0e,0x01,0x0f,0x11,0x0f},
    {0x10,0x10,0x16,0x19,0x11,0x11,0x1e}, {0x00,0x00,0x0e,0x10,0x10,0x11,0x0e}, {0x01,0x01,0x0d,0x13,0x11,0x11,0x0f}, {0x00,0x00,0x0e,0x11,0x1f,0x10,0x0e}, {0x06,0x09,0x08,0x1c,0x08,0x08,0x08}, {0x00,0x0f,0x11,0x11,0x0f,0x01,0x0e},
    {0x10,0x10,0x16,0x19,0x11,0x11,0x11}, {0x04,0x00,0x0c,0x04,0x04,0x04,0x0e}, {0x02,0x00,0x06,0x02,0x02,0x12,0x0c}, {0x10,0x10,0x12,0x14,0x18,0x14,0x12}, {0x0c,0x04,0x04,0x04,0x04,0x04,0x0e}, {0x00,0x00,0x1a,0x15,0x15,0x11,0x11},
    {0x00,0x00,0x16,0x19,0x11,0x11,0x11}, {0x00,0x00,0x0e,0x11,0x11,0x11,0x0e}, {0x00,0x00,0x1e,0x11,0x1e,0x10,0x10}, {0x00,0x00,0x0d,0x13,0x0f,0x01,0x01}, {0x00,0x00,0x16,0x19,0x10,0x10,0x10}, {0x00,0x00,0x0f,0x10,0x0e,0x01,0x1e},
    {0x08,0x08,0x1c,0x08,0x08,0x09,0x06}, {0x00,0x00,0x11,0x11,0x11,0x13,0x0d}, {0x00,0x00,0x11,0x11,0x11,0x0a,0x04}, {0x00,0x00,0x11,0x11,0x15,0x15,0x0a}, {0x00,0x00,0x11,0x0a,0x04,0x0a,0x11}, {0x00,0x00,0x11,0x11,0x0f,0x01,0x0e},
    {0x00,0x00,0x1f,0x02,0x04,0x08,0x1f}, {0x02,0x04,0x04,0x08,0x04,0x04,0x02}, {0x04,0x04,0x04,0x04,0x04,0x04,0x04}, {0x08,0x04,0x04,0x02,0x04,0x04,0x08}, {0x00,0x00,0x08,0x15,0x02,0x00,0x00}, {0x0c,0x12,0x12,0x0c,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x04,0x00,0x00,0x00}, {0x1f,0x11,0x11,0x11,0x11,0x11,0x1f},
};
#define CUI__DMF_DEGREE 95
#define CUI__DMF_MIDDOT 96
#define CUI__DMF_BOX    97

static int cui__dmf_index(uint32_t cp) {
    if (cp >= 32 && cp <= 126) return (int)cp - 32;
    if (cp == 0xB0) return CUI__DMF_DEGREE;
    if (cp == 0xB7 || cp == 0x2022) return CUI__DMF_MIDDOT;
    return CUI__DMF_BOX;
}

typedef struct { uint8_t *glyph; int n; float pitch, scroll; cui_color lit, dim; bool dim_on; cui_text_align align; } cui__dmf_paint;

static void cui__paint_dot_display(cui_ctx *ui, cui_node *nd, void *data) {
    const cui__dmf_paint *p = data;
    cui_rect r = nd->rect;
    if (p->n <= 0) return;
    int cols = p->n * 6 - 1;
    float pitch = p->pitch;
    bool scroll = p->scroll >= 0 && cols * pitch > r.w + 0.5f;
    if (!scroll && cols * pitch > r.w) pitch = r.w / cols;   /* shrink to fit */
    float dot = pitch * 0.38f, dimr = pitch * 0.2f;
    float content = cols * pitch;
    /* the glyphs run left to right in every language, but where they sit in a wider box
     * follows the reading direction: start is the right edge in a right to left interface */
    float x0 = r.x;
    if (!scroll) {
        bool right = p->align == CUI_TEXT_RIGHT || (p->align == CUI_TEXT_END && !nd->rtl) || (p->align == CUI_TEXT_START && nd->rtl);
        if (p->align == CUI_TEXT_CENTER) x0 = r.x + (r.w - content) * 0.5f;
        else if (right) x0 = r.x + r.w - content;
    }
    float y0 = r.y + (r.h - 7 * pitch) * 0.5f;
    if (scroll) cui_push_clip(ui, r);
    /* scrolling text wraps around with a gap of two glyphs */
    float period = content + 12 * pitch;
    int copies = scroll ? 2 : 1;
    float off = scroll ? -fmodf(p->scroll, period) : 0;
    for (int copy = 0; copy < copies; copy++) {
        float bx = x0 + off + copy * period;
        for (int g = 0; g < p->n; g++) {
            const uint8_t *rows = cui__dmf_glyphs[p->glyph[g]];
            for (int row = 0; row < 7; row++)
                for (int c = 0; c < 5; c++) {
                    float x = bx + (g * 6 + c) * pitch + pitch * 0.5f, y = y0 + row * pitch + pitch * 0.5f;
                    if (x < r.x - pitch || x > r.x + r.w + pitch) continue;
                    if (rows[row] & (0x10 >> c)) cui_draw_circle(ui, (cui_vec2){ x, y }, dot, p->lit);
                    else if (p->dim_on) cui_draw_circle(ui, (cui_vec2){ x, y }, dimr, p->dim);
                }
            /* the column between glyphs is dim too, so the matrix reads as one grid */
            if (p->dim_on && g + 1 < p->n)
                for (int row = 0; row < 7; row++)
                    cui_draw_circle(ui, (cui_vec2){ bx + (g * 6 + 5) * pitch + pitch * 0.5f, y0 + row * pitch + pitch * 0.5f }, dimr, p->dim);
        }
    }
    if (scroll) cui_pop_clip(ui);
}

void cui_dot_display_ex(cui_ctx *ui, const char *text, const cui_dot_display_opts *o) {
    cui_dot_display_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    if (!text) text = "";
    float pitch = o->pitch > 0 ? CUI_CLAMP(o->pitch, 1.5f, 40.0f) : 4.0f;
    int len = (int)strlen(text);
    cui__dmf_paint *p = cui__frame_alloc(ui, sizeof *p);
    memset(p, 0, sizeof *p);
    p->glyph = cui__frame_alloc(ui, (size_t)CUI_MAX(1, len));
    for (int i = 0; i < len;) {
        uint32_t cp;
        i += cui__utf8_decode(text + i, len - i, &cp);
        p->glyph[p->n++] = (uint8_t)cui__dmf_index(cp);
    }
    cui_color fg = CUI_C(ui, FOREGROUND);
    p->lit = o->accent ? cui__emphasis(ui) : cui__color_set(o->color) ? cui_color_of(ui, o->color) : fg;
    p->dim = cui_alpha(p->lit, ui->mode == CUI_THEME_DARK ? 0.14f : 0.1f);
    p->dim_on = !o->no_dim;
    p->pitch = pitch;
    p->align = o->align;
    p->scroll = -1;
    int cols = CUI_MAX(1, p->n * 6 - 1);
    cui_id id = cui__position_id(ui, 0xD07Du);
    int nd = cui__node_new(ui, id);
    cui_node *n = &ui->nodes[nd];
    n->w = o->width.kind ? o->width : CUI_PX(cols * pitch);
    n->h = CUI_PX(7 * pitch);
    if (o->marquee && ui->theme.motion > 0) {
        /* 24 dots a second, asking for frames only while the text is wider than its box */
        p->scroll = (float)(cui_time(ui) * 24.0 * pitch);
        if (cui__has_prev_rect(ui, id) && cui__prev_rect(ui, id).w + 0.5f < cols * pitch) cui_request_wakeup(ui, 1.0f / 30.0f);
    }
    n->paint = cui__paint_dot_display; n->paint_data = p;
}
