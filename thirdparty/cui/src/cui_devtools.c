/* Developer tool panels: the key-value editor, the problems panel, the
 * pipeline, the query builder, the schedule picker and the keymap. They are
 * built from the regular widgets and layout boxes, so they follow the theme,
 * the reading direction and the width they get like any other screen. */
#include "cui_internal.h"
#include <time.h>

/* "48s", "1m 24s", "2h 05m" */
static const char *cui__dt_duration(cui_ctx *ui, float s) {
    if (!(s >= 0)) return "";
    int t = (int)(s + 0.5f);
    if (t < 60) return cui__frame_printf(ui, "%ds", t);
    if (t < 3600) return cui__frame_printf(ui, "%dm %02ds", t / 60, t % 60);
    return cui__frame_printf(ui, "%dh %02dm", t / 3600, (t / 60) % 60);
}

/* ======================================================================== */
/* Key-value editor                                                         */
/* ======================================================================== */

#define CUI__KV_NARROW 480.0f   /* below this width a key sits above its value */

/* why a key would be wrong: used twice by an enabled pair before it, or holding a space */
static const char *cui__kv_problem(const cui_kv_pair *pairs, int n, int i) {
    const cui_kv_pair *p = &pairs[i];
    if (!p->key[0]) return NULL;
    for (const char *c = p->key; *c; c++)
        if (*c == ' ' || *c == '\t') return "Has a space";
    if (!p->enabled) return NULL;
    for (int j = 0; j < i && j < n; j++)
        if (pairs[j].enabled && !strcmp(pairs[j].key, p->key)) return "Used twice";
    return NULL;
}

/* one pair: on/off, key, value, show or hide, remove. `fresh` is the empty row that adds a pair. */
static bool cui__kv_row(cui_ctx *ui, const cui_kv_editor_opts *o, cui_kv_pair *pairs, int *n, int i, bool fresh, bool narrow, bool *removed) {
    cui_kv_pair *p = &pairs[i];
    bool changed = false;
    cui_push_id_int(ui, i);
    int row = cui__node_new(ui, 0);
    cui_node *rn = &ui->nodes[row];
    rn->dir = CUI_ROW; rn->gap = 8; rn->w = CUI_FILL;
    rn->align = CUI_ALIGN_START;   /* a key's warning below its field leaves the row's top in line */
    cui__open(ui, row);
    if (!o->no_toggles) {
        cui_check_opts co;
        memset(&co, 0, sizeof co);
        co.disabled = fresh || o->read_only;
        bool on = fresh ? false : p->enabled;
        if (cui_checkbox_ex(ui, "##on", &on, &co) && !fresh) { p->enabled = on; changed = true; }
        ui->nodes[ui->last_node].pad[0] = 6;   /* level with the middle of the fields */
    }
    int fields = row;
    if (narrow) {
        fields = cui__node_new(ui, 0);
        ui->nodes[fields].gap = 4; ui->nodes[fields].w = CUI_FILL;
        cui__open(ui, fields);
    }
    cui_input_opts io;
    memset(&io, 0, sizeof io);
    io.size = CUI_SIZE_SM;
    io.read_only = o->read_only;
    io.placeholder = o->key_placeholder ? o->key_placeholder : "Key";
    io.width = narrow ? CUI_FILL : CUI_GROW(2);
    io.error = fresh ? NULL : cui__kv_problem(pairs, *n, i);
    if (cui_input_ex(ui, "key", p->key, sizeof p->key, &io) & CUI_CHANGED) changed = true;
    io.placeholder = o->value_placeholder ? o->value_placeholder : "Value";
    io.width = narrow ? CUI_FILL : CUI_GROW(3);
    io.error = NULL;
    io.password = p->secret;
    if (cui_input_ex(ui, "value", p->value, sizeof p->value, &io) & CUI_CHANGED) changed = true;
    if (narrow) cui__close(ui);
    cui_button_opts bo;
    memset(&bo, 0, sizeof bo);
    bo.size = CUI_SIZE_SM; bo.variant = CUI_BUTTON_GHOST;
    if (!o->no_secrets) {   /* a secret value shows as dots; the field's own eye reveals it for a moment */
        bo.tooltip = p->secret ? "Secret: shown as dots" : "Mark as secret";
        bo.disabled = o->read_only || fresh;
        bo.color = p->secret ? CUI_COLOR_FOREGROUND : CUI_COLOR_TEXT_MUTED;
        if (cui_icon_button_ex(ui, p->secret ? CUI_ICON_LOCK : CUI_ICON_UNLOCK, &bo)) { p->secret = !p->secret; changed = true; }
        bo.color = (cui_color){ 0 };
    }
    if (!o->read_only) {
        bo.tooltip = "Remove"; bo.disabled = fresh;
        if (cui_icon_button_ex(ui, CUI_ICON_X, &bo) && !fresh) *removed = true;
    }
    cui__close(ui);
    cui_pop_id(ui);
    /* typing in the empty row makes it a pair */
    if (fresh && (p->key[0] || p->value[0])) { p->enabled = true; (*n)++; changed = true; }
    return changed;
}

bool cui_kv_editor_ex(cui_ctx *ui, const char *id_str, cui_kv_pair *pairs, int *count, int capacity, const cui_kv_editor_opts *o) {
    cui_kv_editor_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    if (!pairs || !count || capacity <= 0) return false;
    int n = CUI_CLAMP(*count, 0, capacity);
    cui_id id = cui_make_id(ui, id_str);
    cui_rect pr = cui__prev_rect(ui, id);
    bool narrow = pr.w > 0 && pr.w < CUI__KV_NARROW;
    cui_push_id(ui, id_str);
    /* the empty row at the end starts clean unless it is being typed in */
    bool fresh_row = !o->read_only && n < capacity;
    if (fresh_row) {
        cui_push_id_int(ui, n);
        cui_id k = cui_make_id(ui, "key"), v = cui_make_id(ui, "value");
        cui_pop_id(ui);
        if (ui->focus != k && ui->focus != v) memset(&pairs[n], 0, sizeof pairs[n]);
    }
    int col = cui__node_new(ui, id);
    cui_node *cn = &ui->nodes[col];
    cn->gap = narrow ? 12 : 6;
    cn->w = o->width.kind ? o->width : CUI_FILL;
    cui__open(ui, col);
    bool changed = false;
    int remove = -1;
    int rows = n + (fresh_row ? 1 : 0);
    for (int i = 0; i < rows; i++) {
        bool removed = false, fresh = i == n && fresh_row;
        int before = n;
        if (cui__kv_row(ui, o, pairs, &n, i, fresh, narrow, &removed)) changed = true;
        if (removed) remove = i;
        if (n != before) break;   /* a pair was added: the next empty row comes next frame */
    }
    cui__close(ui);
    cui_pop_id(ui);
    if (remove >= 0 && remove < n) {
        memmove(&pairs[remove], &pairs[remove + 1], sizeof *pairs * (size_t)(n - remove - 1));
        n--;
        memset(&pairs[n], 0, sizeof pairs[n]);
        changed = true;
    }
    *count = n;
    ui->last_id = id;
    return changed;
}

/* ======================================================================== */
/* Problems panel                                                           */
/* ======================================================================== */

#define CUI__PRB_ROW     28.0f
#define CUI__PRB_NARROW  520.0f   /* below this width the code and source are left out */
#define CUI__PRB_FOLDS   64       /* files that can be folded at once */

typedef struct {
    bool init;
    bool show[4];               /* severities shown */
    char query[96];
    int sel;                    /* diagnostic selected, or -1 */
    uint32_t sel_file;          /* or the file header selected (its hash), when sel is -1 */
    uint32_t folded[CUI__PRB_FOLDS]; int nfolded;
    bool scroll_to_sel;
    bool rtl;                   /* the list read right to left last frame: Left and Right swap */
} cui__prb_state;

typedef struct { const char *file; uint32_t hash; int count, first; } cui__prb_group;

/* one frame of the panel: the groups and the rows that pass the filters */
typedef struct {
    cui_ctx *ui;
    const cui_diagnostic *d; int n;
    cui__prb_state *s;
    cui_id id, lid;
    int total[4];               /* per severity, before filtering */
    cui__prb_group *groups; int ng;
    int *order;                 /* diagnostic indices grouped by file */
    int *rows; int nrows;       /* rows: a diagnostic index, or -(group + 1) for a file header */
    int shown;
    bool narrow;
} cui__prbf;

static const char *const cui__prb_icons[4] = { CUI_ICON_CIRCLE_X, CUI_ICON_TRIANGLE_ALERT, CUI_ICON_INFO, CUI_ICON_CIRCLE_HELP };
static cui_color cui__prb_color(cui_ctx *ui, cui_severity s) {
    switch (s) {
    case CUI_SEVERITY_ERROR: return CUI_C(ui, DANGER);
    case CUI_SEVERITY_WARNING: return CUI_C(ui, WARNING);
    case CUI_SEVERITY_INFO: return CUI_C(ui, INFO);
    default: return CUI_C(ui, TEXT_MUTED);
    }
}
static const char *cui__prb_file(const cui_diagnostic *g) { return g->file && g->file[0] ? g->file : "No file"; }
static bool cui__prb_folded(const cui__prb_state *s, uint32_t h) {
    for (int i = 0; i < s->nfolded; i++) if (s->folded[i] == h) return true;
    return false;
}
static void cui__prb_fold(cui__prb_state *s, uint32_t h, bool fold) {
    for (int i = 0; i < s->nfolded; i++)
        if (s->folded[i] == h) { if (!fold) s->folded[i] = s->folded[--s->nfolded]; return; }
    if (fold && s->nfolded < CUI__PRB_FOLDS) s->folded[s->nfolded++] = h;
}

/* records the list's resolved reading direction for the next frame's keys */
static void cui__paint_pb_dir(cui_ctx *ui, cui_node *n, void *data) { (void)ui; *(bool *)data = n->rtl; }

static bool cui__prb_passes(const cui__prbf *f, const cui_diagnostic *g) {
    int sev = CUI_CLAMP((int)g->severity, 0, 3);
    if (!f->s->show[sev]) return false;
    const char *q = f->s->query;
    if (!q[0]) return true;
    return cui__contains_ci(g->message ? g->message : "", q) || cui__contains_ci(cui__prb_file(g), q) ||
           (g->code && cui__contains_ci(g->code, q)) || (g->source && cui__contains_ci(g->source, q));
}

/* group the diagnostics that pass by file, in the order files first appear, and list the rows */
static void cui__prb_rows(cui__prbf *f) {
    cui_ctx *ui = f->ui;
    int n = f->n;
    size_t nn = (size_t)CUI_MAX(1, n);
    int *gid = cui__frame_alloc(ui, sizeof(int) * nn);
    f->groups = cui__frame_alloc(ui, sizeof(cui__prb_group) * nn);
    int cap = 16;
    while (cap < n * 2) cap *= 2;
    int *table = cui__frame_alloc(ui, sizeof(int) * (size_t)cap);
    for (int i = 0; i < cap; i++) table[i] = -1;
    for (int i = 0; i < n; i++) {
        const cui_diagnostic *g = &f->d[i];
        f->total[CUI_CLAMP((int)g->severity, 0, 3)]++;
        if (!cui__prb_passes(f, g)) { gid[i] = -1; continue; }
        const char *file = cui__prb_file(g);
        uint32_t h = cui__hash_str(file, 0x9B0Bu);
        int slot = (int)(h & (uint32_t)(cap - 1));
        while (table[slot] >= 0 && strcmp(f->groups[table[slot]].file, file) != 0) slot = (slot + 1) & (cap - 1);
        if (table[slot] < 0) {
            table[slot] = f->ng;
            f->groups[f->ng] = (cui__prb_group){ file, h, 0, 0 };
            f->ng++;
        }
        gid[i] = table[slot];
        f->groups[gid[i]].count++;
        f->shown++;
    }
    for (int g = 0, at = 0; g < f->ng; g++) { f->groups[g].first = at; at += f->groups[g].count; }
    f->order = cui__frame_alloc(ui, sizeof(int) * nn);
    int *fill = cui__frame_alloc(ui, sizeof(int) * (size_t)CUI_MAX(1, f->ng));
    for (int g = 0; g < f->ng; g++) fill[g] = f->groups[g].first;
    for (int i = 0; i < n; i++) if (gid[i] >= 0) f->order[fill[gid[i]]++] = i;
    f->rows = cui__frame_alloc(ui, sizeof(int) * (nn + (size_t)f->ng));
    for (int g = 0; g < f->ng; g++) {
        f->rows[f->nrows++] = -(g + 1);
        if (cui__prb_folded(f->s, f->groups[g].hash)) continue;
        for (int k = 0; k < f->groups[g].count; k++) f->rows[f->nrows++] = f->order[f->groups[g].first + k];
    }
}

static int cui__prb_selected_row(const cui__prbf *f) {
    for (int r = 0; r < f->nrows; r++) {
        int v = f->rows[r];
        if (v >= 0 ? v == f->s->sel : (f->s->sel < 0 && f->groups[-v - 1].hash == f->s->sel_file)) return r;
    }
    return -1;
}
static void cui__prb_select_row(cui__prbf *f, int r) {
    int v = f->rows[r];
    if (v >= 0) f->s->sel = v;
    else { f->s->sel = -1; f->s->sel_file = f->groups[-v - 1].hash; }
}

/* keys on the list: Up and Down move, Home and End jump, Left and Right fold, Enter opens */
static void cui__prb_keys(cui__prbf *f, cui_problems_result *res) {
    cui_ctx *ui = f->ui;
    cui__prb_state *s = f->s;
    if (f->nrows == 0) return;
    int r = cui__prb_selected_row(f), to = r;
    if (cui_key_pressed(ui, CUI_KEY_DOWN)) to = r < 0 ? 0 : r + 1;
    if (cui_key_pressed(ui, CUI_KEY_UP)) to = r < 0 ? 0 : r - 1;
    if (cui_key_pressed(ui, CUI_KEY_HOME)) to = 0;
    if (cui_key_pressed(ui, CUI_KEY_END)) to = f->nrows - 1;
    if (to != r) { cui__prb_select_row(f, CUI_CLAMP(to, 0, f->nrows - 1)); s->scroll_to_sel = true; }
    r = cui__prb_selected_row(f);
    if (r < 0) return;
    int v = f->rows[r];
    /* the group of the selected row */
    uint32_t gh = 0;
    if (v < 0) gh = f->groups[-v - 1].hash;
    else for (int g = 0; g < f->ng; g++) if (!strcmp(f->groups[g].file, cui__prb_file(&f->d[v]))) { gh = f->groups[g].hash; break; }
    bool fold_key = cui_key_pressed(ui, CUI_KEY_LEFT) != 0, unfold_key = cui_key_pressed(ui, CUI_KEY_RIGHT) != 0;
    if (s->rtl) { bool t = fold_key; fold_key = unfold_key; unfold_key = t; }
    if (fold_key) { cui__prb_fold(s, gh, true); s->sel = -1; s->sel_file = gh; }
    if (unfold_key) cui__prb_fold(s, gh, false);
    if (cui_key_pressed(ui, CUI_KEY_ENTER)) {
        if (v >= 0) res->activated = v;
        else cui__prb_fold(s, gh, !cui__prb_folded(s, gh));
    }
}

/* severity toggles with their counts, and the text filter */
static void cui__prb_toolbar(cui__prbf *f) {
    cui_ctx *ui = f->ui;
    cui__prb_state *s = f->s;
    int bar = cui__node_new(ui, 0);
    cui_node *bn = &ui->nodes[bar];
    bn->dir = CUI_ROW; bn->gap = 8; bn->w = CUI_FILL; bn->align = CUI_ALIGN_CENTER; bn->flags |= CUI_NF_ROW_WRAP;
    cui__open(ui, bar);
    static const char *const names[4] = { "Errors", "Warnings", "Info", "Hints" };
    const char *labels[4];
    for (int k = 0; k < 4; k++)   /* on a phone the icon names the severity */
        labels[k] = f->narrow ? cui__frame_printf(ui, "%d", f->total[k]) : cui__frame_printf(ui, "%s %d", names[k], f->total[k]);
    cui_toggle_group_opts go;
    memset(&go, 0, sizeof go);
    go.size = CUI_SIZE_SM;
    go.icons = cui__prb_icons;
    cui_toggle_group_ex(ui, "severity", labels, 4, s->show, &go);
    cui_input_opts io;
    memset(&io, 0, sizeof io);
    io.size = CUI_SIZE_SM; io.width = CUI_FILL; io.placeholder = "Filter problems"; io.clearable = true;
    if (s->query[0]) io.suffix = cui__frame_printf(ui, "%d", f->shown);
    cui_search_ex(ui, "filter", s->query, sizeof s->query, &io);
    ui->nodes[ui->last_node].min_w = 140;
    cui__close(ui);
}

/* a file's header: fold chevron, the file name, its folder, the count */
static void cui__prb_header_row(cui__prbf *f, int g, int r, cui_problems_result *res) {
    cui_ctx *ui = f->ui;
    cui__prb_state *s = f->s;
    const cui__prb_group *gr = &f->groups[g];
    cui_id rid = f->id ^ (gr->hash * 2654435761u) ^ 0x9E01u;
    cui_interact ri = cui__interact(ui, rid, CUI_IF_KEEP_FOCUS);
    bool folded = cui__prb_folded(s, gr->hash);
    if (ri.clicked) { cui__prb_fold(s, gr->hash, !folded); s->sel = -1; s->sel_file = gr->hash; ui->focus = f->lid; folded = !folded; }
    bool sel = s->sel < 0 && s->sel_file == gr->hash;
    int row = cui__node_new(ui, rid);
    cui_node *rn = &ui->nodes[row];
    rn->dir = CUI_ROW; rn->gap = 6; rn->align = CUI_ALIGN_CENTER; rn->w = CUI_FILL; rn->h = CUI_PX(CUI__PRB_ROW);
    rn->pad[1] = rn->pad[3] = 8;
    rn->bg = sel ? CUI_C(ui, ACTIVE) : ri.hovered ? CUI_C(ui, HOVER) : cui__rgba(0, 0, 0, 0);
    cui__mark_interactive(ui, row, 0);
    if (ri.hovered) cui__set_cursor(ui, CUI_CURSOR_HAND);
    cui__open(ui, row);
    cui__icon_node(ui, folded ? CUI_ICON_CHEVRON_RIGHT : CUI_ICON_CHEVRON_DOWN, 14, CUI_C(ui, TEXT_MUTED));
    cui__icon_node(ui, CUI_ICON_FILE, 14, CUI_C(ui, TEXT_SECONDARY));
    const char *base = gr->file, *c;
    for (c = gr->file; *c; c++) if (*c == '/' || *c == '\\') base = c + 1;
    float fs = cui__fs(ui, ui->theme.text_sm);
    int t = cui__text_node(ui, base, -1, CUI_FONT_MEDIUM, fs, CUI_C(ui, FOREGROUND), false);
    ui->nodes[t].line_height = 1.3f;
    if (base > gr->file && !f->narrow) {
        int dt = cui__text_node(ui, gr->file, (int)(base - gr->file - 1), CUI_FONT_BODY, cui__fs(ui, ui->theme.text_xs), CUI_C(ui, TEXT_MUTED), false);
        ui->nodes[dt].line_height = 1.3f; ui->nodes[dt].w = CUI_FILL; ui->nodes[dt].flags |= CUI_NF_CLIP;
    } else {
        int sp = cui__leaf(ui, 0, CUI_FILL, CUI_PX(1));
        (void)sp;
    }
    cui_badge_opts bo;
    memset(&bo, 0, sizeof bo);
    bo.size = CUI_SIZE_SM;
    cui_badge_ex(ui, cui__frame_printf(ui, "%d", gr->count), &bo);
    cui__close(ui);
    (void)r; (void)res;
}

/* a diagnostic: severity, message, code and source, position */
static void cui__prb_diag_row(cui__prbf *f, int i, cui_problems_result *res) {
    cui_ctx *ui = f->ui;
    cui__prb_state *s = f->s;
    const cui_diagnostic *g = &f->d[i];
    cui_id rid = cui__hash(&i, sizeof i, f->id ^ 0x9D1Au);
    cui_interact ri = cui__interact(ui, rid, CUI_IF_KEEP_FOCUS);
    if (ri.clicked) {
        s->sel = i; ui->focus = f->lid;
        if (ui->click_count >= 2) res->activated = i;
    }
    bool sel = s->sel == i;
    int row = cui__node_new(ui, rid);
    cui_node *rn = &ui->nodes[row];
    rn->dir = CUI_ROW; rn->gap = 8; rn->align = CUI_ALIGN_CENTER; rn->w = CUI_FILL; rn->h = CUI_PX(CUI__PRB_ROW);
    rn->pad[1] = 8; rn->pad[3] = 30;   /* indented under the file's chevron */
    bool focused = ui->focus == f->lid;
    rn->bg = sel ? (focused ? cui_mix(CUI_C(ui, SURFACE), cui__emphasis(ui), ui->accent == CUI_ACCENT_NEUTRAL ? 0.08f : 0.16f) : CUI_C(ui, ACTIVE))
                 : ri.hovered ? CUI_C(ui, HOVER) : cui__rgba(0, 0, 0, 0);
    cui__mark_interactive(ui, row, 0);
    cui__open(ui, row);
    int sev = CUI_CLAMP((int)g->severity, 0, 3);
    cui__icon_node(ui, cui__prb_icons[sev], 14, cui__prb_color(ui, (cui_severity)sev));
    float fs = cui__fs(ui, ui->theme.text_sm), fx = cui__fs(ui, ui->theme.text_xs);
    int t = cui__text_node(ui, g->message ? g->message : "", -1, CUI_FONT_BODY, fs, CUI_C(ui, FOREGROUND), false);
    ui->nodes[t].w = CUI_FILL; ui->nodes[t].line_height = 1.3f; ui->nodes[t].flags |= CUI_NF_CLIP;
    if (!f->narrow && (g->code || g->source)) {
        const char *cs = g->code && g->source ? cui__frame_printf(ui, "%s(%s)", g->source, g->code) : g->code ? g->code : g->source;
        int ct = cui__text_node(ui, cs, -1, CUI_FONT_MONO, fx, CUI_C(ui, TEXT_MUTED), false);
        ui->nodes[ct].line_height = 1.3f;
    }
    if (g->line > 0) {
        const char *pos = g->column > 0 ? cui__frame_printf(ui, "%d:%d", g->line, g->column) : cui__frame_printf(ui, "%d", g->line);
        int pt = cui__text_node(ui, pos, -1, CUI_FONT_MONO, fx, CUI_C(ui, TEXT_SECONDARY), false);
        ui->nodes[pt].line_height = 1.3f; ui->nodes[pt].flags |= CUI_NF_TABULAR;
        ui->nodes[pt].direction = CUI_DIRECTION_LTR;
    }
    cui__close(ui);
    if (ri.hovered && g->message && ui->active == 0) {
        const char *tip = g->line > 0 ? cui__frame_printf(ui, "%s\n%s:%d:%d", g->message, cui__prb_file(g), g->line, g->column) : g->message;
        cui__tooltip_passive(ui, rid, tip);
    }
}

cui_problems_result cui_problems_ex(cui_ctx *ui, const char *id_str, const cui_diagnostic *diags, int count, const cui_problems_opts *o) {
    cui_problems_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui_problems_result res = { -1, -1, 0 };
    if (!diags || count < 0) count = 0;
    cui__prbf f;
    memset(&f, 0, sizeof f);
    f.ui = ui; f.d = diags; f.n = count;
    f.id = cui_make_id(ui, id_str);
    f.lid = f.id ^ 0xB0B5u;
    cui__prb_state *s = f.s = cui_state(ui, f.id ^ 0x9B0Bu, sizeof *s);
    if (!s->init) { s->init = true; for (int k = 0; k < 4; k++) s->show[k] = true; s->sel = -1; }
    if (s->sel >= count) s->sel = -1;
    cui_rect pr = cui__prev_rect(ui, f.id);
    f.narrow = pr.w > 0 && pr.w < CUI__PRB_NARROW;
    cui__prb_rows(&f);

    int outer = cui__node_new(ui, f.id);
    cui_node *on = &ui->nodes[outer];
    on->gap = 8;
    on->w = o->width.kind ? o->width : CUI_FILL;
    on->h = o->height < 0 ? CUI_FILL : CUI_PX(o->height > 0 ? o->height : 300);
    cui__open(ui, outer);
    cui_push_id(ui, id_str);
    if (!o->no_toolbar) cui__prb_toolbar(&f);

    cui_interact lr = cui__interact(ui, f.lid, CUI_IF_FOCUSABLE);
    if (lr.focused) cui__prb_keys(&f, &res);
    int frame = cui__node_new(ui, f.lid);
    cui_node *fn = &ui->nodes[frame];
    fn->w = CUI_FILL; fn->h = CUI_FILL; fn->min_h = 80;
    fn->radius = CUI_CORNERS(ui->theme.radius_control);
    fn->border = 1; fn->border_color = CUI_C(ui, BORDER);
    fn->bg = CUI_C(ui, SURFACE);
    fn->flags |= CUI_NF_CLIP;
    fn->paint = cui__paint_pb_dir; fn->paint_data = &s->rtl;
    cui__mark_interactive(ui, frame, CUI_IF_FOCUSABLE);
    cui__focus_ring(ui, frame, lr.focus_visible ? 1.0f : 0.0f);
    cui__open(ui, frame);
    /* keep the selection in view */
    if (s->scroll_to_sel) {
        int r = cui__prb_selected_row(&f);
        cui_scroll_state *ss = cui_state(ui, cui_make_id(ui, "rows"), sizeof *ss);
        float y = r * CUI__PRB_ROW, vh = ss->view_h > 0 ? ss->view_h : 240;
        if (r >= 0) {
            if (y < ss->ty) ss->ty = y;
            else if (y + CUI__PRB_ROW > ss->ty + vh) ss->ty = y + CUI__PRB_ROW - vh;
        }
        s->scroll_to_sel = false;
    }
    if (f.nrows == 0) {
        int e = cui__node_new(ui, 0);
        ui->nodes[e].w = CUI_FILL; ui->nodes[e].h = CUI_FILL; ui->nodes[e].align = CUI_ALIGN_CENTER; ui->nodes[e].justify = CUI_JUSTIFY_CENTER;
        ui->nodes[e].gap = 6;
        cui__open(ui, e);
        cui__icon_node(ui, count == 0 ? CUI_ICON_CIRCLE_CHECK : CUI_ICON_FILTER, 20, count == 0 ? CUI_C(ui, SUCCESS) : CUI_C(ui, TEXT_MUTED));
        cui__text_node(ui, count == 0 ? "No problems" : "No problems match the filters", -1, CUI_FONT_BODY,
                       cui__fs(ui, ui->theme.text_sm), CUI_C(ui, TEXT_MUTED), false);
        cui__close(ui);
    } else {
        int first, last;
        cui_virtual_list_begin(ui, "rows", f.nrows, CUI__PRB_ROW, CUI_FILL, &first, &last);
        for (int r = first; r < last; r++) {
            int v = f.rows[r];
            if (v < 0) cui__prb_header_row(&f, -v - 1, r, &res);
            else cui__prb_diag_row(&f, v, &res);
        }
        cui_virtual_list_end(ui);
    }
    cui__close(ui);   /* frame */
    cui_pop_id(ui);
    cui__close(ui);   /* outer */
    res.selected = s->sel;
    res.shown = f.shown;
    ui->last_id = f.id;
    return res;
}

/* ======================================================================== */
/* Pipeline                                                                 */
/* ======================================================================== */

#define CUI__PL_NARROW 560.0f
#define CUI__PL_JOB_H  34.0f

/* a stage sums up its jobs: failed, then running, then pending, then canceled; passed when all passed or skipped */
static cui_job_status cui__pl_stage_status(const cui_pipeline_job *jobs, int n, int stage, bool *soft_fail) {
    bool any = false, run = false, pend = false, fail = false, canc = false, allpass = true;
    *soft_fail = false;
    for (int i = 0; i < n; i++) {
        if (jobs[i].stage != stage) continue;
        any = true;
        switch (jobs[i].status) {
        case CUI_JOB_FAILED: if (jobs[i].allow_failure) *soft_fail = true; else fail = true; break;
        case CUI_JOB_RUNNING: run = true; allpass = false; break;
        case CUI_JOB_PENDING: pend = true; allpass = false; break;
        case CUI_JOB_CANCELED: canc = true; allpass = false; break;
        default: break;
        }
    }
    if (!any) return CUI_JOB_SKIPPED;
    if (fail) return CUI_JOB_FAILED;
    if (run) return CUI_JOB_RUNNING;
    if (canc) return CUI_JOB_CANCELED;
    if (pend) return CUI_JOB_PENDING;
    return allpass ? CUI_JOB_PASSED : CUI_JOB_PENDING;
}

static const char *cui__pl_status_name(cui_job_status st) {
    static const char *const n[] = { "Pending", "Running", "Passed", "Failed", "Skipped", "Canceled" };
    return st >= 0 && st <= CUI_JOB_CANCELED ? n[st] : "";
}

/* the status as an icon (a spinner while running) */
static void cui__pl_status_icon(cui_ctx *ui, cui_job_status st, bool soft, float size) {
    switch (st) {
    case CUI_JOB_RUNNING: cui_spinner(ui, size); return;
    case CUI_JOB_PASSED: cui__icon_node(ui, soft ? CUI_ICON_TRIANGLE_ALERT : CUI_ICON_CIRCLE_CHECK, size, soft ? CUI_C(ui, WARNING) : CUI_C(ui, SUCCESS)); return;
    case CUI_JOB_FAILED: cui__icon_node(ui, soft ? CUI_ICON_TRIANGLE_ALERT : CUI_ICON_CIRCLE_X, size, soft ? CUI_C(ui, WARNING) : CUI_C(ui, DANGER)); return;
    case CUI_JOB_SKIPPED: cui__icon_node(ui, CUI_ICON_CHEVRONS_RIGHT, size, CUI_C(ui, TEXT_MUTED)); return;
    case CUI_JOB_CANCELED: cui__icon_node(ui, CUI_ICON_CIRCLE_X, size, CUI_C(ui, TEXT_MUTED)); return;
    default: cui__icon_node(ui, CUI_ICON_CLOCK, size, CUI_C(ui, TEXT_MUTED)); return;
    }
}

/* the line between two stages, with a chevron pointing on (down when the stages stack) */
typedef struct { cui_color c; bool vertical; } cui__pl_link_paint;
static void cui__paint_pl_link(cui_ctx *ui, cui_node *n, void *data) {
    cui__pl_link_paint *p = data;
    cui_rect r = n->rect;
    if (p->vertical) cui_draw_rect(ui, (cui_rect){ r.x + r.w * 0.5f - 0.5f, r.y, 1, r.h }, CUI_CORNERS(0), p->c);
    else cui_draw_rect(ui, (cui_rect){ r.x, r.y + n->pad[0] + (r.h - n->pad[0]) * 0.5f - 0.5f, r.w, 1 }, CUI_CORNERS(0), p->c);
}
static void cui__pl_link(cui_ctx *ui, bool vertical) {
    int l = cui__node_new(ui, 0);
    cui_node *ln = &ui->nodes[l];
    ln->w = vertical ? CUI_FILL : CUI_PX(28);
    ln->h = vertical ? CUI_PX(22) : CUI_PX(30 + CUI__PL_JOB_H);
    ln->self_align = vertical ? CUI_ALIGN_STRETCH : CUI_ALIGN_START;
    ln->align = CUI_ALIGN_CENTER; ln->justify = CUI_JUSTIFY_CENTER;
    ln->dir = CUI_STACK;
    if (!vertical) ln->pad[0] = 30;   /* level with the first job, under the stage's header */
    cui__pl_link_paint *p = cui__frame_alloc(ui, sizeof *p);
    p->c = CUI_C(ui, BORDER_STRONG); p->vertical = vertical;
    ln->paint = cui__paint_pl_link; ln->paint_data = p;
    cui__open(ui, l);
    int bg = cui__node_new(ui, 0);
    ui->nodes[bg].bg = CUI_C(ui, BACKGROUND);
    ui->nodes[bg].radius = CUI_CORNERS(8);
    ui->nodes[bg].pad[0] = ui->nodes[bg].pad[1] = ui->nodes[bg].pad[2] = ui->nodes[bg].pad[3] = 1;
    cui__open(ui, bg);
    cui__icon_node(ui, vertical ? CUI_ICON_CHEVRON_DOWN : CUI_ICON_CHEVRON_RIGHT, 14, CUI_C(ui, TEXT_MUTED));
    cui__close(ui);
    cui__close(ui);
}

static void cui__pl_job(cui_ctx *ui, cui_id base, const cui_pipeline_job *j, int i, int *selected, cui_pipeline_result *res) {
    cui_id jid = cui__hash(&i, sizeof i, base ^ 0x70B5u);
    cui_interact r = cui__interact(ui, jid, CUI_IF_FOCUSABLE);
    if (r.clicked || r.keyboard_activate) { res->clicked = i; if (selected) *selected = i; }
    if (r.hovered) res->hovered = i;
    bool sel = selected && *selected == i;
    int jn = cui__node_new(ui, jid);
    cui_node *n = &ui->nodes[jn];
    n->dir = CUI_ROW; n->gap = 8; n->align = CUI_ALIGN_CENTER; n->w = CUI_FILL; n->h = CUI_PX(CUI__PL_JOB_H);
    n->pad[1] = n->pad[3] = 10;
    n->radius = CUI_CORNERS(ui->theme.radius_control);
    n->bg = r.hovered ? cui_mix(CUI_C(ui, SURFACE), CUI_C(ui, HOVER), 1.0f) : CUI_C(ui, SURFACE);
    n->border = sel ? 1.5f : 1;
    n->border_color = sel ? cui__emphasis(ui) : CUI_C(ui, BORDER);
    cui__mark_interactive(ui, jn, CUI_IF_FOCUSABLE);
    cui__focus_ring(ui, jn, r.focus_visible ? 1.0f : 0.0f);
    if (r.hovered) cui__set_cursor(ui, CUI_CURSOR_HAND);
    cui__open(ui, jn);
    cui__pl_status_icon(ui, j->status, j->allow_failure && j->status == CUI_JOB_FAILED, 14);
    float fs = cui__fs(ui, ui->theme.text_sm);
    int t = cui__text_node(ui, j->name ? j->name : "", -1, CUI_FONT_BODY, fs,
                           j->status == CUI_JOB_SKIPPED || j->status == CUI_JOB_CANCELED ? CUI_C(ui, TEXT_MUTED) : CUI_C(ui, FOREGROUND), false);
    ui->nodes[t].w = CUI_FILL; ui->nodes[t].line_height = 1.3f; ui->nodes[t].flags |= CUI_NF_CLIP;
    if (j->duration >= 0) {
        int dt = cui__text_node(ui, cui__dt_duration(ui, j->duration), -1, CUI_FONT_BODY, cui__fs(ui, ui->theme.text_xs), CUI_C(ui, TEXT_MUTED), false);
        ui->nodes[dt].flags |= CUI_NF_TABULAR; ui->nodes[dt].line_height = 1.3f;
    }
    cui__close(ui);
    if (r.hovered && ui->active == 0) {
        const char *tip = j->duration >= 0 ? cui__frame_printf(ui, "%s\n%s \xc2\xb7 %s%s", j->name ? j->name : "", cui__pl_status_name(j->status),
                                                               cui__dt_duration(ui, j->duration), j->allow_failure ? " \xc2\xb7 allowed to fail" : "")
                                           : cui__frame_printf(ui, "%s\n%s", j->name ? j->name : "", cui__pl_status_name(j->status));
        cui__tooltip_passive(ui, jid, tip);
    }
}

cui_pipeline_result cui_pipeline_ex(cui_ctx *ui, const char *id_str, const char *const *stages, int stage_count,
                                    const cui_pipeline_job *jobs, int job_count, const cui_pipeline_opts *o) {
    cui_pipeline_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui_pipeline_result res = { -1, -1 };
    if (!jobs || job_count < 0) job_count = 0;
    if (stage_count < 0) stage_count = 0;
    cui_id id = cui_make_id(ui, id_str);
    cui_rect pr = cui__prev_rect(ui, id);
    bool vertical = pr.w > 0 && pr.w < CUI__PL_NARROW;
    int outer = cui__node_new(ui, id);
    cui_node *on = &ui->nodes[outer];
    on->dir = vertical ? CUI_COLUMN : CUI_ROW;
    on->w = o->width.kind ? o->width : CUI_FILL;
    on->align = vertical ? CUI_ALIGN_STRETCH : CUI_ALIGN_START;
    cui__open(ui, outer);
    float fs = cui__fs(ui, ui->theme.text_sm), fx = cui__fs(ui, ui->theme.text_xs);
    for (int s = 0; s < stage_count; s++) {
        if (s > 0) cui__pl_link(ui, vertical);
        bool soft;
        cui_job_status st = cui__pl_stage_status(jobs, job_count, s, &soft);
        float total = 0;
        bool timed = false;
        for (int i = 0; i < job_count; i++) if (jobs[i].stage == s && jobs[i].duration >= 0) { total = CUI_MAX(total, jobs[i].duration); timed = true; }
        int col = cui__node_new(ui, 0);
        cui_node *cn = &ui->nodes[col];
        cn->gap = 6;
        cn->w = vertical ? CUI_FILL : CUI_GROW(1);
        cn->min_w = vertical ? 0 : 120;
        cui__open(ui, col);
        int head = cui__node_new(ui, 0);
        cui_node *hn = &ui->nodes[head];
        hn->dir = CUI_ROW; hn->gap = 6; hn->align = CUI_ALIGN_CENTER; hn->w = CUI_FILL; hn->h = CUI_PX(24);
        cui__open(ui, head);
        cui__pl_status_icon(ui, st, soft && st == CUI_JOB_PASSED, 14);
        int t = cui__text_node(ui, stages && stages[s] ? stages[s] : "", -1, CUI_FONT_MEDIUM, fs, CUI_C(ui, FOREGROUND), false);
        ui->nodes[t].w = CUI_FILL; ui->nodes[t].line_height = 1.3f; ui->nodes[t].flags |= CUI_NF_CLIP;
        if (timed) {
            int dt = cui__text_node(ui, cui__dt_duration(ui, total), -1, CUI_FONT_BODY, fx, CUI_C(ui, TEXT_MUTED), false);
            ui->nodes[dt].flags |= CUI_NF_TABULAR; ui->nodes[dt].line_height = 1.3f;
        }
        cui__close(ui);
        for (int i = 0; i < job_count; i++)
            if (jobs[i].stage == s) cui__pl_job(ui, id, &jobs[i], i, o->selected, &res);
        cui__close(ui);
    }
    cui__close(ui);
    ui->last_id = id;
    return res;
}

/* ======================================================================== */
/* Test explorer                                                            */
/* ======================================================================== */

#define CUI__TX_ROW    28.0f
#define CUI__TX_NARROW 480.0f
#define CUI__TX_FOLDS  64

typedef struct {
    bool init;
    bool show[3];               /* passed, failed, skipped shown */
    char query[96];
    int sel;
    uint32_t folded[CUI__TX_FOLDS]; int nfolded;
    bool scroll_to_sel;
    bool rtl;                   /* the list read right to left last frame: Left and Right swap */
} cui__tx_state;

/* a suite's results, from the tests under it */
typedef struct { int pass, fail, skip, run, pend, shown; float time; bool suite; } cui__tx_sum;

typedef struct {
    cui_ctx *ui;
    const cui_test_item *it; int n;
    cui__tx_state *s;
    cui_id id, lid;
    cui__tx_sum *sum;
    int *rows; int nrows;       /* an item, or -(item + 1) for a failure's message under it */
    bool narrow;
    cui_test_explorer_result *res;
} cui__txf;

static uint32_t cui__tx_key(const cui_test_item *it, int i) { return cui__hash_str(it[i].name ? it[i].name : "", (uint32_t)i * 2654435761u + 0x7E57u); }
static bool cui__tx_folded(const cui__tx_state *s, uint32_t h) {
    for (int i = 0; i < s->nfolded; i++) if (s->folded[i] == h) return true;
    return false;
}
static void cui__tx_fold(cui__tx_state *s, uint32_t h, bool fold) {
    for (int i = 0; i < s->nfolded; i++)
        if (s->folded[i] == h) { if (!fold) s->folded[i] = s->folded[--s->nfolded]; return; }
    if (fold && s->nfolded < CUI__TX_FOLDS) s->folded[s->nfolded++] = h;
}
static cui_job_status cui__tx_status(const cui__txf *f, int i) {
    const cui__tx_sum *m = &f->sum[i];
    if (!m->suite) return f->it[i].status;
    if (m->fail) return CUI_JOB_FAILED;
    if (m->run) return CUI_JOB_RUNNING;
    if (m->pend) return CUI_JOB_PENDING;
    if (m->pass) return CUI_JOB_PASSED;
    return m->skip ? CUI_JOB_SKIPPED : CUI_JOB_PENDING;
}
/* does a test pass the status toggles and the text filter */
static bool cui__tx_passes(const cui__txf *f, int i) {
    cui_job_status st = f->it[i].status;
    int b = st == CUI_JOB_PASSED ? 0 : st == CUI_JOB_FAILED ? 1 : st == CUI_JOB_SKIPPED || st == CUI_JOB_CANCELED ? 2 : -1;
    if (b >= 0 && !f->s->show[b]) return false;
    return cui__contains_ci(f->it[i].name ? f->it[i].name : "", f->s->query);
}

/* sums up each suite and lists the rows: a suite shows when a test under it passes the filters */
static void cui__tx_rows(cui__txf *f) {
    cui_ctx *ui = f->ui;
    int n = f->n;
    size_t nn = (size_t)CUI_MAX(1, n);
    f->res->passed = f->res->failed = f->res->skipped = f->res->total = 0;
    f->sum = cui__frame_alloc(ui, sizeof(cui__tx_sum) * nn);
    memset(f->sum, 0, sizeof(cui__tx_sum) * nn);
    int stack[64], sp = 0;
    for (int i = 0; i < n; i++) {
        int d = CUI_CLAMP(f->it[i].depth, 0, 63);
        while (sp > 0 && CUI_CLAMP(f->it[stack[sp - 1]].depth, 0, 63) >= d) sp--;
        bool suite = i + 1 < n && f->it[i + 1].depth > f->it[i].depth;
        f->sum[i].suite = suite;
        if (!suite) {
            cui_job_status st = f->it[i].status;
            bool shown = cui__tx_passes(f, i);
            float t = f->it[i].duration > 0 ? f->it[i].duration : 0;
            switch (st) {
            case CUI_JOB_PASSED: f->res->passed++; break;
            case CUI_JOB_FAILED: f->res->failed++; break;
            case CUI_JOB_SKIPPED: case CUI_JOB_CANCELED: f->res->skipped++; break;
            default: break;
            }
            f->res->total++;
            for (int k = 0; k < sp; k++) {
                cui__tx_sum *m = &f->sum[stack[k]];
                switch (st) {
                case CUI_JOB_PASSED: m->pass++; break;
                case CUI_JOB_FAILED: m->fail++; break;
                case CUI_JOB_SKIPPED: case CUI_JOB_CANCELED: m->skip++; break;
                case CUI_JOB_RUNNING: m->run++; break;
                default: m->pend++; break;
                }
                m->time += t;
                if (shown) m->shown++;
            }
            if (shown) f->sum[i].shown = 1;
        }
        if (suite && sp < 64) stack[sp++] = i;
    }
    f->rows = cui__frame_alloc(ui, sizeof(int) * nn * 2);
    f->nrows = 0;
    int hide_below = -1;   /* depth under a folded suite */
    for (int i = 0; i < n; i++) {
        int d = f->it[i].depth;
        if (hide_below >= 0) { if (d > hide_below) continue; hide_below = -1; }
        if (!f->sum[i].shown) continue;
        f->rows[f->nrows++] = i;
        if (f->sum[i].suite && cui__tx_folded(f->s, cui__tx_key(f->it, i))) hide_below = d;
        else if (!f->sum[i].suite && f->it[i].status == CUI_JOB_FAILED && f->it[i].message && f->it[i].message[0]) f->rows[f->nrows++] = -(i + 1);
    }
}

static int cui__tx_row_of(const cui__txf *f, int item) {
    for (int r = 0; r < f->nrows; r++) if (f->rows[r] == item) return r;
    return -1;
}

static void cui__tx_keys(cui__txf *f) {
    cui_ctx *ui = f->ui;
    cui__tx_state *s = f->s;
    int r = cui__tx_row_of(f, s->sel), nr = r;
    bool left = cui_key_pressed(ui, s->rtl ? CUI_KEY_RIGHT : CUI_KEY_LEFT), right = cui_key_pressed(ui, s->rtl ? CUI_KEY_LEFT : CUI_KEY_RIGHT);
    if (cui_key_pressed(ui, CUI_KEY_DOWN)) { nr = r + 1; while (nr < f->nrows && f->rows[nr] < 0) nr++; if (nr >= f->nrows) nr = r; }
    if (cui_key_pressed(ui, CUI_KEY_UP)) { nr = r < 0 ? 0 : r - 1; while (nr > 0 && f->rows[nr] < 0) nr--; if (nr < 0) nr = 0; }
    if (cui_key_pressed(ui, CUI_KEY_HOME)) nr = 0;
    if (cui_key_pressed(ui, CUI_KEY_END)) { nr = f->nrows - 1; while (nr > 0 && f->rows[nr] < 0) nr--; }
    if (r < 0 && nr < 0 && f->nrows > 0 && (cui_key_pressed(ui, CUI_KEY_DOWN) || cui_key_pressed(ui, CUI_KEY_UP))) nr = 0;
    if (nr != r && nr >= 0 && nr < f->nrows && f->rows[nr] >= 0) { s->sel = f->rows[nr]; s->scroll_to_sel = true; }
    int i = s->sel;
    if (i >= 0 && i < f->n) {
        bool suite = f->sum[i].suite;
        uint32_t h = cui__tx_key(f->it, i);
        if (left) {
            if (suite && !cui__tx_folded(s, h)) cui__tx_fold(s, h, true);
            else for (int k = i - 1; k >= 0; k--) if (f->it[k].depth < f->it[i].depth) { s->sel = k; s->scroll_to_sel = true; break; }
        }
        if (right && suite) cui__tx_fold(s, h, false);
        if (cui_key_pressed(ui, CUI_KEY_ENTER)) f->res->activated = i;
    }
}

static void cui__tx_toolbar(cui__txf *f) {
    cui_ctx *ui = f->ui;
    cui__tx_state *s = f->s;
    int bar = cui__node_new(ui, 0);
    cui_node *bn = &ui->nodes[bar];
    bn->dir = CUI_ROW; bn->gap = 8; bn->w = CUI_FILL; bn->align = CUI_ALIGN_CENTER; bn->flags |= CUI_NF_ROW_WRAP;
    cui__open(ui, bar);
    cui_button_opts bo;
    memset(&bo, 0, sizeof bo);
    bo.size = CUI_SIZE_SM; bo.icon = CUI_ICON_PLAY;
    if (cui_button_ex(ui, f->narrow ? "Run" : "Run all", &bo)) f->res->run_all = true;
    static const char *const icons[3] = { CUI_ICON_CIRCLE_CHECK, CUI_ICON_CIRCLE_X, CUI_ICON_CHEVRONS_RIGHT };
    const char *labels[3] = { cui__frame_printf(ui, "%d", f->res->passed), cui__frame_printf(ui, "%d", f->res->failed), cui__frame_printf(ui, "%d", f->res->skipped) };
    cui_toggle_group_opts go;
    memset(&go, 0, sizeof go);
    go.size = CUI_SIZE_SM; go.icons = icons;
    cui_toggle_group_ex(ui, "status", labels, 3, s->show, &go);
    cui_input_opts io;
    memset(&io, 0, sizeof io);
    io.size = CUI_SIZE_SM; io.width = CUI_FILL; io.placeholder = "Filter tests"; io.clearable = true;
    cui_search_ex(ui, "filter", s->query, sizeof s->query, &io);
    ui->nodes[ui->last_node].min_w = 120;
    cui__close(ui);
}

static void cui__tx_row(cui__txf *f, int i) {
    cui_ctx *ui = f->ui;
    cui__tx_state *s = f->s;
    const cui_test_item *t = &f->it[i];
    bool suite = f->sum[i].suite;
    cui_id rid = f->id ^ (cui__tx_key(f->it, i) * 0x9E3779B1u);
    cui_interact ri = cui__interact(ui, rid, CUI_IF_KEEP_FOCUS);
    uint32_t h = cui__tx_key(f->it, i);
    bool folded = suite && cui__tx_folded(s, h);
    if (ri.clicked) {
        s->sel = i; ui->focus = f->lid;
        if (ui->click_count >= 2) { if (suite) cui__tx_fold(s, h, !folded); else f->res->activated = i; }
    }
    bool sel = s->sel == i, focused = ui->focus == f->lid;
    int row = cui__node_new(ui, rid);
    cui_node *rn = &ui->nodes[row];
    rn->dir = CUI_ROW; rn->gap = 6; rn->align = CUI_ALIGN_CENTER; rn->w = CUI_FILL; rn->h = CUI_PX(CUI__TX_ROW);
    rn->pad[1] = 6; rn->pad[3] = 8 + (float)CUI_CLAMP(t->depth, 0, 12) * (f->narrow ? 12 : 16);
    rn->bg = sel ? (focused ? cui_mix(CUI_C(ui, SURFACE), cui__emphasis(ui), ui->accent == CUI_ACCENT_NEUTRAL ? 0.08f : 0.16f) : CUI_C(ui, ACTIVE))
                 : ri.hovered ? CUI_C(ui, HOVER) : cui__rgba(0, 0, 0, 0);
    cui__mark_interactive(ui, row, 0);
    cui__open(ui, row);
    if (suite) {
        cui_id cid = rid ^ 0xC4u;
        cui_interact ci = cui__interact(ui, cid, 0);
        if (ci.clicked) cui__tx_fold(s, h, !folded);
        cui__icon_node(ui, folded ? CUI_ICON_CHEVRON_RIGHT : CUI_ICON_CHEVRON_DOWN, 14, ci.hovered ? CUI_C(ui, FOREGROUND) : CUI_C(ui, TEXT_MUTED));
        int c = ui->last_node;
        ui->nodes[c].id = cid;
        cui__mark_interactive(ui, c, 0);
    } else {
        int sp = cui__node_new(ui, 0);
        ui->nodes[sp].w = ui->nodes[sp].h = CUI_PX(14);
    }
    cui__pl_status_icon(ui, cui__tx_status(f, i), false, 14);
    float fs = cui__fs(ui, ui->theme.text_sm), fx = cui__fs(ui, ui->theme.text_xs);
    int tn = cui__text_node(ui, t->name ? t->name : "", -1, suite ? CUI_FONT_MEDIUM : CUI_FONT_BODY, fs, CUI_C(ui, FOREGROUND), false);
    ui->nodes[tn].w = CUI_FILL; ui->nodes[tn].flags |= CUI_NF_CLIP; ui->nodes[tn].line_height = 1.3f;
    if (suite && !f->narrow) {
        const cui__tx_sum *m = &f->sum[i];
        int all = m->pass + m->fail + m->skip + m->run + m->pend;
        int ct = cui__text_node(ui, cui__frame_printf(ui, "%d/%d", m->pass, all), -1, CUI_FONT_BODY, fx, CUI_C(ui, TEXT_MUTED), false);
        ui->nodes[ct].flags |= CUI_NF_TABULAR;
    }
    float dur = suite ? f->sum[i].time : t->duration;
    if (dur > 0 || (!suite && t->duration >= 0 && t->status != CUI_JOB_PENDING && t->status != CUI_JOB_SKIPPED)) {
        const char *ds = dur < 1 ? cui__frame_printf(ui, "%.0f ms", dur * 1000) : cui__frame_printf(ui, "%.2f s", dur);
        int dt = cui__text_node(ui, ds, -1, CUI_FONT_BODY, fx, dur >= 1 ? CUI_C(ui, WARNING) : CUI_C(ui, TEXT_MUTED), false);
        ui->nodes[dt].flags |= CUI_NF_TABULAR; ui->nodes[dt].direction = CUI_DIRECTION_LTR;
    }
    /* the run button, on hover and on the selection; a space of its size otherwise */
    bool show_run = ri.hovered || sel;
    if (show_run) {
        cui_button_opts bo;
        memset(&bo, 0, sizeof bo);
        bo.size = CUI_SIZE_SM; bo.variant = CUI_BUTTON_GHOST; bo.tooltip = suite ? "Run these tests" : "Run this test";
        cui_push_id_int(ui, i);
        if (cui_icon_button_ex(ui, CUI_ICON_PLAY, &bo)) f->res->run = i;
        cui_pop_id(ui);
    } else {
        int sp = cui__node_new(ui, 0);
        ui->nodes[sp].w = ui->nodes[sp].h = CUI_PX(28);
    }
    cui__close(ui);
    if (ri.hovered && ui->active == 0 && t->message && t->message[0] && !suite) cui__tooltip_passive(ui, rid, t->message);
}

static void cui__tx_message_row(cui__txf *f, int i) {
    cui_ctx *ui = f->ui;
    const cui_test_item *t = &f->it[i];
    int row = cui__node_new(ui, 0);
    cui_node *rn = &ui->nodes[row];
    rn->dir = CUI_ROW; rn->align = CUI_ALIGN_CENTER; rn->w = CUI_FILL; rn->h = CUI_PX(CUI__TX_ROW);
    rn->pad[1] = 8; rn->pad[3] = 8 + (float)CUI_CLAMP(t->depth, 0, 12) * (f->narrow ? 12 : 16) + 40;
    cui__open(ui, row);
    const char *msg = t->message, *nl = strchr(msg, '\n');
    int tn = cui__text_node(ui, msg, nl ? (int)(nl - msg) : -1, CUI_FONT_MONO, cui__fs(ui, ui->theme.text_xs), CUI_C(ui, DANGER), false);
    ui->nodes[tn].w = CUI_FILL; ui->nodes[tn].flags |= CUI_NF_CLIP;
    cui__close(ui);
}

cui_test_explorer_result cui_test_explorer_ex(cui_ctx *ui, const char *id_str, const cui_test_item *items, int count, const cui_test_explorer_opts *o) {
    cui_test_explorer_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui_test_explorer_result res;
    memset(&res, 0, sizeof res);
    res.selected = res.activated = res.run = -1;
    if (!items || count < 0) count = 0;
    cui__txf f;
    memset(&f, 0, sizeof f);
    f.ui = ui; f.it = items; f.n = count; f.res = &res;
    f.id = cui_make_id(ui, id_str);
    f.lid = f.id ^ 0x7E50u;
    cui__tx_state *s = f.s = cui_state(ui, f.id ^ 0x7E51u, sizeof *s);
    if (!s->init) { s->init = true; s->show[0] = s->show[1] = s->show[2] = true; s->sel = -1; }
    if (s->sel >= count) s->sel = -1;
    cui_rect pr = cui__prev_rect(ui, f.id);
    f.narrow = pr.w > 0 && pr.w < CUI__TX_NARROW;
    cui__tx_rows(&f);

    int outer = cui__node_new(ui, f.id);
    cui_node *on = &ui->nodes[outer];
    on->gap = 8;
    on->w = o->width.kind ? o->width : CUI_FILL;
    on->h = o->height < 0 ? CUI_FILL : CUI_PX(o->height > 0 ? o->height : 320);
    cui__open(ui, outer);
    cui_push_id(ui, id_str);
    cui__tx_toolbar(&f);
    cui__tx_rows(&f);   /* the toggles and the filter may have changed */
    cui_interact lr = cui__interact(ui, f.lid, CUI_IF_FOCUSABLE);
    if (lr.focused && !ui->edit_prev) cui__tx_keys(&f);
    int frame = cui__node_new(ui, f.lid);
    cui_node *fn = &ui->nodes[frame];
    fn->w = CUI_FILL; fn->h = CUI_FILL; fn->min_h = 80;
    fn->radius = CUI_CORNERS(ui->theme.radius_control);
    fn->border = 1; fn->border_color = CUI_C(ui, BORDER);
    fn->bg = CUI_C(ui, SURFACE);
    fn->flags |= CUI_NF_CLIP;
    fn->paint = cui__paint_pb_dir; fn->paint_data = &s->rtl;
    cui__mark_interactive(ui, frame, CUI_IF_FOCUSABLE);
    cui__focus_ring(ui, frame, lr.focus_visible ? 1.0f : 0.0f);
    cui__open(ui, frame);
    if (s->scroll_to_sel) {
        int r = cui__tx_row_of(&f, s->sel);
        cui_scroll_state *ss = cui_state(ui, cui_make_id(ui, "rows"), sizeof *ss);
        float y = r * CUI__TX_ROW, vh = ss->view_h > 0 ? ss->view_h : 240;
        if (r >= 0) {
            if (y < ss->ty) ss->ty = y;
            else if (y + CUI__TX_ROW * 2 > ss->ty + vh) ss->ty = y + CUI__TX_ROW * 2 - vh;   /* with its message */
        }
        s->scroll_to_sel = false;
    }
    if (f.nrows == 0) {
        int e = cui__node_new(ui, 0);
        ui->nodes[e].w = CUI_FILL; ui->nodes[e].h = CUI_FILL; ui->nodes[e].align = CUI_ALIGN_CENTER; ui->nodes[e].justify = CUI_JUSTIFY_CENTER;
        cui__open(ui, e);
        cui__text_node(ui, count == 0 ? "No tests" : "No tests match the filters", -1, CUI_FONT_BODY, cui__fs(ui, ui->theme.text_sm), CUI_C(ui, TEXT_MUTED), false);
        cui__close(ui);
    } else {
        int first, last;
        cui_virtual_list_begin(ui, "rows", f.nrows, CUI__TX_ROW, CUI_FILL, &first, &last);
        for (int r = first; r < last; r++) {
            int v = f.rows[r];
            if (v >= 0) cui__tx_row(&f, v); else cui__tx_message_row(&f, -v - 1);
        }
        cui_virtual_list_end(ui);
    }
    cui__close(ui);   /* frame */
    cui_pop_id(ui);
    cui__close(ui);   /* outer */
    res.selected = s->sel;
    ui->last_id = f.id;
    return res;
}

/* ======================================================================== */
/* Query builder                                                            */
/* ======================================================================== */

#define CUI__QB_NARROW 520.0f

static const char *const cui__qb_ops_text[] = { "is", "is not", "contains", "does not contain", "starts with", "ends with" };
static const char *const cui__qb_ops_num[] = { "=", "!=", ">", ">=", "<", "<=" };
static const char *const cui__qb_ops_choice[] = { "is", "is not" };
static const char *const cui__qb_ops_bool[] = { "is true", "is false" };

static const char *const *cui__qb_ops(cui_query_type t, int *n) {
    switch (t) {
    case CUI_QUERY_NUMBER: *n = 6; return cui__qb_ops_num;
    case CUI_QUERY_CHOICE: *n = 2; return cui__qb_ops_choice;
    case CUI_QUERY_BOOL: *n = 2; return cui__qb_ops_bool;
    default: *n = 6; return cui__qb_ops_text;
    }
}

const char *cui_query_op_name(cui_query_type type, int op) {
    int n;
    const char *const *ops = cui__qb_ops(type, &n);
    return op >= 0 && op < n ? ops[op] : NULL;
}

/* the end of node i's subtree: the first node after it that is not deeper */
static int cui__qb_end(const cui_query_node *nodes, int count, int i) {
    int j = i + 1;
    if (nodes[i].group) while (j < count && nodes[j].depth > nodes[i].depth) j++;
    return j;
}

static const cui_query_field *cui__qb_field(const cui_query_field *fields, int field_count, int f) {
    return fields && f >= 0 && f < field_count ? &fields[f] : NULL;
}

/* ---- as text ---- */

typedef struct { char *out; size_t cap, len; } cui__qb_buf;
static void cui__qb_put(cui__qb_buf *b, const char *s) {
    size_t n = strlen(s);
    for (size_t i = 0; i < n; i++) { if (b->len + 1 < b->cap) b->out[b->len] = s[i]; b->len++; }
    if (b->cap) b->out[b->len < b->cap ? b->len : b->cap - 1] = 0;
}
static int cui__qb_text_node(const cui_query_field *fields, int field_count, const cui_query_node *nodes, int count, int i, cui__qb_buf *b, bool nested) {
    const cui_query_node *nd = &nodes[i];
    if (!nd->group) {
        const cui_query_field *f = cui__qb_field(fields, field_count, nd->field);
        if (!f) return i + 1;
        const char *op = cui_query_op_name(f->type, nd->op);
        cui__qb_put(b, f->name ? f->name : "?");
        cui__qb_put(b, " ");
        cui__qb_put(b, op ? op : "?");
        if (f->type != CUI_QUERY_BOOL) {
            cui__qb_put(b, f->type == CUI_QUERY_TEXT ? " \"" : " ");
            cui__qb_put(b, nd->value);
            if (f->type == CUI_QUERY_TEXT) cui__qb_put(b, "\"");
        }
        return i + 1;
    }
    int end = cui__qb_end(nodes, count, i), kids = 0;
    for (int j = i + 1; j < end; j = cui__qb_end(nodes, count, j)) kids++;
    bool paren = nested && kids > 1;
    if (paren) cui__qb_put(b, "(");
    int k = 0;
    for (int j = i + 1; j < end; k++) {
        if (k) cui__qb_put(b, nd->any ? " or " : " and ");
        j = cui__qb_text_node(fields, field_count, nodes, count, j, b, true);
        if (j <= i) break;
    }
    if (paren) cui__qb_put(b, ")");
    return end;
}

size_t cui_query_text(const cui_query_field *fields, int field_count, const cui_query_node *nodes, int count, char *out, size_t cap) {
    cui__qb_buf b = { out, cap, 0 };
    if (cap) out[0] = 0;
    if (!nodes || count <= 0) return 0;
    cui__qb_text_node(fields, field_count, nodes, count, 0, &b, false);
    return b.len;
}

/* ---- matching ---- */

static bool cui__qb_ieq_prefix(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) if (cui__ascii_lower((unsigned char)a[i]) != cui__ascii_lower((unsigned char)b[i])) return false;
    return true;
}
static bool cui__qb_rule(const cui_query_field *f, const cui_query_node *nd, const char *v) {
    if (!v) v = "";
    const char *q = nd->value;
    size_t lv = strlen(v), lq = strlen(q);
    switch (f->type) {
    case CUI_QUERY_NUMBER: {
        double a = strtod(v, NULL), c = strtod(q, NULL);
        switch (nd->op) { case 0: return a == c; case 1: return a != c; case 2: return a > c; case 3: return a >= c; case 4: return a < c; default: return a <= c; }
    }
    case CUI_QUERY_CHOICE: return (strcmp(v, q) == 0) == (nd->op == 0);
    case CUI_QUERY_BOOL: {
        bool t = !strcmp(v, "1") || cui__qb_ieq_prefix(v, "true", 5) || cui__qb_ieq_prefix(v, "yes", 4);
        return t == (nd->op == 0);
    }
    default:
        switch (nd->op) {
        case 0: return lv == lq && cui__qb_ieq_prefix(v, q, lq);
        case 1: return !(lv == lq && cui__qb_ieq_prefix(v, q, lq));
        case 2: return cui__contains_ci(v, q);
        case 3: return !cui__contains_ci(v, q);
        case 4: return lv >= lq && cui__qb_ieq_prefix(v, q, lq);
        default: return lv >= lq && cui__qb_ieq_prefix(v + lv - lq, q, lq);
        }
    }
}
static bool cui__qb_match(const cui_query_field *fields, int field_count, const cui_query_node *nodes, int count, int i, const char *const *values) {
    const cui_query_node *nd = &nodes[i];
    if (!nd->group) {
        const cui_query_field *f = cui__qb_field(fields, field_count, nd->field);
        return f ? cui__qb_rule(f, nd, values ? values[nd->field] : NULL) : true;
    }
    int end = cui__qb_end(nodes, count, i);
    bool any_kid = false;
    for (int j = i + 1; j < end; j = cui__qb_end(nodes, count, j)) {
        bool m = cui__qb_match(fields, field_count, nodes, count, j, values);
        any_kid = true;
        if (nd->any && m) return true;
        if (!nd->any && !m) return false;
    }
    return nd->any ? !any_kid : true;   /* an empty group matches everything */
}
bool cui_query_match(const cui_query_field *fields, int field_count, const cui_query_node *nodes, int count, const char *const *values) {
    if (!nodes || count <= 0) return true;
    return cui__qb_match(fields, field_count, nodes, count, 0, values);
}

/* ---- the editor ---- */

/* an edit is applied after the frame's nodes are built: insert at `at`, or remove [at, end) */
typedef struct { int at, end; bool insert, group; int depth; } cui__qb_edit;

typedef struct {
    cui_ctx *ui;
    const cui_query_opts *o;
    const cui_query_field *fields; int field_count;
    cui_query_node *nodes; int count, capacity;
    bool narrow, changed;
    cui__qb_edit edit;
} cui__qbf;

static void cui__qb_rule_row(cui__qbf *f, int i) {
    cui_ctx *ui = f->ui;
    cui_query_node *nd = &f->nodes[i];
    cui_push_id_int(ui, i);
    /* a phone puts the value on a line of its own, under the field and the operator */
    int outer = -1;
    if (f->narrow) {
        outer = cui__node_new(ui, 0);
        ui->nodes[outer].gap = 6; ui->nodes[outer].w = CUI_FILL;
        cui__open(ui, outer);
    }
    int row = cui__node_new(ui, 0);
    cui_node *rn = &ui->nodes[row];
    rn->dir = CUI_ROW; rn->gap = 8; rn->align = CUI_ALIGN_CENTER; rn->w = CUI_FILL;
    cui__open(ui, row);
    /* field */
    const char **names = cui__frame_alloc(ui, sizeof(char *) * (size_t)CUI_MAX(1, f->field_count));
    for (int k = 0; k < f->field_count; k++) names[k] = f->fields[k].name ? f->fields[k].name : "";
    cui_select_opts so;
    memset(&so, 0, sizeof so);
    so.disabled = f->o->read_only;
    so.width = f->narrow ? CUI_GROW(1) : CUI_PX(150);
    int field = CUI_CLAMP(nd->field, 0, CUI_MAX(0, f->field_count - 1));
    if (f->field_count > 0 && cui_select_ex(ui, "field", names, f->field_count, &field, &so)) {
        if (f->fields[field].type != f->fields[nd->field < f->field_count ? nd->field : 0].type || field != nd->field) { nd->op = 0; nd->value[0] = 0; }
        nd->field = field; f->changed = true;
    }
    const cui_query_field *fd = cui__qb_field(f->fields, f->field_count, nd->field);
    cui_query_type type = fd ? fd->type : CUI_QUERY_TEXT;
    /* operator */
    int nops;
    const char *const *ops = cui__qb_ops(type, &nops);
    int op = CUI_CLAMP(nd->op, 0, nops - 1);
    so.width = f->narrow ? CUI_GROW(1) : CUI_PX(type == CUI_QUERY_NUMBER ? 76 : 150);
    if (cui_select_ex(ui, "op", ops, nops, &op, &so)) { nd->op = op; f->changed = true; }
    bool remove = false;
    cui_button_opts bo;
    memset(&bo, 0, sizeof bo);
    bo.size = CUI_SIZE_SM; bo.variant = CUI_BUTTON_GHOST; bo.tooltip = "Remove condition";
    if (f->narrow && !f->o->read_only) remove = cui_icon_button_ex(ui, CUI_ICON_X, &bo);
    if (f->narrow) {   /* the value's own line */
        cui__close(ui);
        row = cui__node_new(ui, 0);
        ui->nodes[row].dir = CUI_ROW; ui->nodes[row].w = CUI_FILL;
        cui__open(ui, row);
    }
    /* value */
    if (type == CUI_QUERY_CHOICE && fd && fd->choice_count > 0) {
        int ci = 0;
        for (int k = 0; k < fd->choice_count; k++) if (fd->choices[k] && !strcmp(fd->choices[k], nd->value)) ci = k;
        so.width = CUI_FILL;
        if (cui_select_ex(ui, "value", fd->choices, fd->choice_count, &ci, &so) || !nd->value[0]) {
            snprintf(nd->value, sizeof nd->value, "%s", fd->choices[ci] ? fd->choices[ci] : "");
            f->changed = true;
        }
    } else if (type != CUI_QUERY_BOOL) {
        cui_input_opts io;
        memset(&io, 0, sizeof io);
        io.width = CUI_FILL; io.read_only = f->o->read_only;   /* the default size: as tall as the selects */
        io.placeholder = type == CUI_QUERY_NUMBER ? "0" : "Value";
        if (cui_input_ex(ui, "value", nd->value, sizeof nd->value, &io) & CUI_CHANGED) f->changed = true;
        ui->nodes[ui->last_node].min_w = 120;
    } else if (!f->narrow) {
        cui__leaf(ui, 0, CUI_FILL, CUI_PX(1));
    }
    if (!f->narrow && !f->o->read_only) remove = cui_icon_button_ex(ui, CUI_ICON_X, &bo);
    if (remove) f->edit = (cui__qb_edit){ i, i + 1, false, false, 0 };
    cui__close(ui);
    if (outer >= 0) cui__close(ui);
    cui_pop_id(ui);
}

/* a group: all or any, its rules and groups, and the buttons that add to it. Returns the index after it. */
static int cui__qb_group(cui__qbf *f, int i) {
    cui_ctx *ui = f->ui;
    cui_query_node *nd = &f->nodes[i];
    int end = cui__qb_end(f->nodes, f->count, i);
    cui_push_id_int(ui, i);
    int box = cui__node_new(ui, 0);
    cui_node *bn = &ui->nodes[box];
    bn->gap = 8; bn->w = CUI_FILL;
    if (nd->depth > 0) {
        bn->pad[0] = bn->pad[2] = 10; bn->pad[1] = bn->pad[3] = 12;
        bn->radius = CUI_CORNERS(ui->theme.radius_control);
        bn->border = 1; bn->border_color = CUI_C(ui, BORDER);
        bn->bg = cui_mix(CUI_C(ui, SURFACE), CUI_C(ui, SURFACE_MUTED), nd->depth % 2 ? 1.0f : 0.0f);
    }
    cui__open(ui, box);
    /* header: match all or any */
    int head = cui__node_new(ui, 0);
    cui_node *hn = &ui->nodes[head];
    hn->dir = CUI_ROW; hn->gap = 8; hn->align = CUI_ALIGN_CENTER; hn->w = CUI_FILL;
    cui__open(ui, head);
    float fs = cui__fs(ui, ui->theme.text_sm);
    cui__text_node(ui, "Match", -1, CUI_FONT_BODY, fs, CUI_C(ui, TEXT_SECONDARY), false);
    static const char *const join[2] = { "All", "Any" };
    int any = nd->any ? 1 : 0;
    cui_segmented_opts sg;
    memset(&sg, 0, sizeof sg);
    sg.size = CUI_SIZE_SM;
    if (!f->o->read_only && cui_segmented_ex(ui, "join", join, 2, &any, &sg)) { nd->any = any == 1; f->changed = true; }
    if (f->o->read_only) cui__text_node(ui, nd->any ? "any" : "all", -1, CUI_FONT_MEDIUM, fs, CUI_C(ui, FOREGROUND), false);
    int t = cui__text_node(ui, "of these conditions", -1, CUI_FONT_BODY, fs, CUI_C(ui, TEXT_SECONDARY), false);
    ui->nodes[t].w = CUI_FILL;
    if (nd->depth > 0 && !f->o->read_only) {
        cui_button_opts bo;
        memset(&bo, 0, sizeof bo);
        bo.size = CUI_SIZE_SM; bo.variant = CUI_BUTTON_GHOST; bo.tooltip = "Remove group";
        if (cui_icon_button_ex(ui, CUI_ICON_TRASH_2, &bo)) f->edit = (cui__qb_edit){ i, end, false, false, 0 };
    }
    cui__close(ui);
    /* children */
    for (int j = i + 1; j < end;) {
        if (f->nodes[j].group) j = cui__qb_group(f, j);
        else { cui__qb_rule_row(f, j); j++; }
    }
    if (end == i + 1) {
        int e = cui__text_node(ui, "No conditions: everything matches.", -1, CUI_FONT_BODY, fs, CUI_C(ui, TEXT_MUTED), false);
        ui->nodes[e].line_height = 1.6f;
    }
    if (!f->o->read_only) {
        int foot = cui__node_new(ui, 0);
        cui_node *fn = &ui->nodes[foot];
        fn->dir = CUI_ROW; fn->gap = 4; fn->align = CUI_ALIGN_CENTER;
        cui__open(ui, foot);
        cui_button_opts bo;
        memset(&bo, 0, sizeof bo);
        bo.size = CUI_SIZE_SM; bo.variant = CUI_BUTTON_GHOST; bo.icon = CUI_ICON_PLUS;
        bo.disabled = f->count >= f->capacity;
        if (cui_button_ex(ui, "Condition", &bo)) f->edit = (cui__qb_edit){ end, end, true, false, nd->depth + 1 };
        int max_depth = f->o->max_depth > 0 ? f->o->max_depth : 3;
        if (nd->depth + 1 < max_depth) {
            bo.disabled = f->count + 2 > f->capacity;
            if (cui_button_ex(ui, "Group", &bo)) f->edit = (cui__qb_edit){ end, end, true, true, nd->depth + 1 };
        }
        cui__close(ui);
    }
    cui__close(ui);
    cui_pop_id(ui);
    return end;
}

static void cui__qb_apply(cui__qbf *f) {
    cui__qb_edit *e = &f->edit;
    cui_query_node *nodes = f->nodes;
    if (e->insert) {
        int add = e->group ? 2 : 1;   /* a new group starts with one condition */
        if (f->count + add > f->capacity) return;
        memmove(&nodes[e->at + add], &nodes[e->at], sizeof *nodes * (size_t)(f->count - e->at));
        cui_query_node *a = &nodes[e->at];
        memset(a, 0, sizeof *a * (size_t)add);
        a[0].depth = e->depth;
        a[0].group = e->group;
        if (e->group) a[1].depth = e->depth + 1;
        /* a new condition starts with the first field; a choice starts at its first value */
        cui_query_node *r = e->group ? &a[1] : &a[0];
        if (f->field_count > 0 && f->fields[0].type == CUI_QUERY_CHOICE && f->fields[0].choice_count > 0 && f->fields[0].choices[0])
            snprintf(r->value, sizeof r->value, "%s", f->fields[0].choices[0]);
        f->count += add;
    } else if (e->end > e->at && e->at > 0) {
        memmove(&nodes[e->at], &nodes[e->end], sizeof *nodes * (size_t)(f->count - e->end));
        f->count -= e->end - e->at;
    }
    f->changed = true;
}

bool cui_query_builder_ex(cui_ctx *ui, const char *id_str, const cui_query_field *fields, int field_count,
                          cui_query_node *nodes, int *count, int capacity, const cui_query_opts *o) {
    cui_query_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    if (!nodes || !count || capacity <= 0) return false;
    cui__qbf f;
    memset(&f, 0, sizeof f);
    f.ui = ui; f.o = o; f.fields = fields; f.field_count = fields ? field_count : 0;
    f.nodes = nodes; f.count = CUI_CLAMP(*count, 0, capacity); f.capacity = capacity;
    f.edit.at = -1;
    if (f.count == 0) { memset(&nodes[0], 0, sizeof nodes[0]); nodes[0].group = true; f.count = 1; f.changed = true; }
    nodes[0].depth = 0; nodes[0].group = true;
    cui_id id = cui_make_id(ui, id_str);
    cui_rect pr = cui__prev_rect(ui, id);
    f.narrow = pr.w > 0 && pr.w < CUI__QB_NARROW;
    cui_push_id(ui, id_str);
    int col = cui__node_new(ui, id);
    ui->nodes[col].w = o->width.kind ? o->width : CUI_FILL;
    cui__open(ui, col);
    cui__qb_group(&f, 0);
    cui__close(ui);
    cui_pop_id(ui);
    if (f.edit.at >= 0) cui__qb_apply(&f);
    *count = f.count;
    ui->last_id = id;
    return f.changed;
}

/* ======================================================================== */
/* Schedule picker and cron                                                 */
/* ======================================================================== */

typedef struct { uint64_t min; uint32_t hour, dom; uint16_t mon; uint8_t dow; bool dom_any, dow_any; } cui__cron;

static int cui__cron_name(const char *s, int len, const char *const *names, int count, int base) {
    if (len != 3 || !names) return -1;
    for (int i = 0; i < count; i++)
        if (cui__ascii_lower((unsigned char)s[0]) == cui__ascii_lower((unsigned char)names[i][0]) &&
            cui__ascii_lower((unsigned char)s[1]) == cui__ascii_lower((unsigned char)names[i][1]) &&
            cui__ascii_lower((unsigned char)s[2]) == cui__ascii_lower((unsigned char)names[i][2])) return base + i;
    return -1;
}
static bool cui__cron_num(const char *s, int len, const char *const *names, int name_count, int name_base, int *v) {
    if (len <= 0) return false;
    if (s[0] >= '0' && s[0] <= '9') {
        int x = 0;
        for (int i = 0; i < len; i++) { if (s[i] < '0' || s[i] > '9' || x > 1000) return false; x = x * 10 + (s[i] - '0'); }
        *v = x; return true;
    }
    int x = cui__cron_name(s, len, names, name_count, name_base);
    if (x < 0) return false;
    *v = x; return true;
}
/* one field: *, a, a-b, with /step, as a list; sets bits lo..hi of mask */
static bool cui__cron_field(const char *s, int len, int lo, int hi, const char *const *names, int name_count, int name_base, uint64_t *mask, bool *any) {
    *mask = 0; *any = false;
    int i = 0;
    while (i <= len) {
        int j = i;
        while (j < len && s[j] != ',') j++;
        const char *it = s + i;
        int il = j - i, slash = -1, dash = -1;
        for (int k = 0; k < il; k++) { if (it[k] == '/') slash = k; else if (it[k] == '-' && dash < 0 && slash < 0) dash = k; }
        int range_len = slash >= 0 ? slash : il;
        int a, b, step = 1;
        if (slash >= 0 && !cui__cron_num(it + slash + 1, il - slash - 1, NULL, 0, 0, &step)) return false;
        if (step <= 0) return false;
        if (range_len == 1 && it[0] == '*') { a = lo; b = hi; if (slash < 0 && il == 1) *any = true; }
        else if (dash >= 0) {
            if (!cui__cron_num(it, dash, names, name_count, name_base, &a) || !cui__cron_num(it + dash + 1, range_len - dash - 1, names, name_count, name_base, &b)) return false;
        } else {
            if (!cui__cron_num(it, range_len, names, name_count, name_base, &a)) return false;
            b = slash >= 0 ? hi : a;
        }
        if (hi == 6 && b == 7) b = 7;   /* weekday 7 is Sunday too */
        if (a < lo || b > (hi == 6 ? 7 : hi) || a > b) return false;
        for (int v = a; v <= b; v += step) *mask |= 1ull << (hi == 6 && v == 7 ? 0 : v);
        if (j >= len) break;
        i = j + 1;
    }
    return *mask != 0;
}
static bool cui__cron_parse(const char *s, cui__cron *c) {
    static const char *const mons[12] = { "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec" };
    static const char *const days[7] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };
    if (!s) return false;
    const char *f[5]; int fl[5], n = 0;
    while (*s && n < 6) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        const char *b = s;
        while (*s && *s != ' ' && *s != '\t') s++;
        if (n < 5) { f[n] = b; fl[n] = (int)(s - b); }
        n++;
    }
    if (n != 5) return false;
    uint64_t m; bool any;
    memset(c, 0, sizeof *c);
    if (!cui__cron_field(f[0], fl[0], 0, 59, NULL, 0, 0, &m, &any)) return false;
    c->min = m;
    if (!cui__cron_field(f[1], fl[1], 0, 23, NULL, 0, 0, &m, &any)) return false;
    c->hour = (uint32_t)m;
    if (!cui__cron_field(f[2], fl[2], 1, 31, NULL, 0, 0, &m, &any)) return false;
    c->dom = (uint32_t)m; c->dom_any = any;
    if (!cui__cron_field(f[3], fl[3], 1, 12, mons, 12, 1, &m, &any)) return false;
    c->mon = (uint16_t)m;
    if (!cui__cron_field(f[4], fl[4], 0, 6, days, 7, 0, &m, &any)) return false;
    c->dow = (uint8_t)m; c->dow_any = any;
    return true;
}
static bool cui__cron_day(const cui__cron *c, int day, int wd) {
    bool dm = (c->dom >> day) & 1, wm = (c->dow >> wd) & 1;
    if (c->dom_any && c->dow_any) return true;
    if (c->dom_any) return wm;
    if (c->dow_any) return dm;
    return dm || wm;
}

bool cui_cron_next(const char *cron, int64_t after, int utc_offset, int64_t *next) {
    cui__cron c;
    if (!cui__cron_parse(cron, &c)) return false;
    int64_t off = (int64_t)utc_offset * 60;
    int64_t t = ((after + off) / 60 + 1) * 60;   /* local wall time, the next whole minute */
    if (after + off < 0) t = ((after + off - 59) / 60 + 1) * 60;
    int64_t limit = after + off + (int64_t)5 * 366 * 86400;   /* never runs in five years: give up */
    for (int guard = 0; guard < 200000 && t <= limit; guard++) {
        int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
        int64_t sec = t - days * 86400;
        cui_date dt = cui_date_from_days((int)days);
        if (dt.year > 9999) return false;
        if (!((c.mon >> dt.month) & 1)) {   /* to the first of the next month */
            int y = dt.year, mo = dt.month + 1;
            if (mo > 12) { mo = 1; y++; }
            t = (int64_t)cui_date_to_days((cui_date){ y, mo, 1 }) * 86400;
            continue;
        }
        int wd = (int)(((days % 7) + 7 + 4) % 7);   /* 1970-01-01 was a Thursday; 0 = Sunday */
        if (!cui__cron_day(&c, dt.day, wd)) { t = (days + 1) * 86400; continue; }
        int h = (int)(sec / 3600), mi = (int)((sec / 60) % 60);
        if (!((c.hour >> h) & 1)) { t = days * 86400 + (int64_t)(h + 1) * 3600; continue; }
        if (!((c.min >> mi) & 1)) { t += 60; continue; }
        *next = t - off;
        return t - off - after <= (int64_t)5 * 366 * 86400;
    }
    return false;
}

static int cui__cron_count(uint64_t m, int lo, int hi, int *first) {
    int n = 0;
    *first = -1;
    for (int v = lo; v <= hi; v++) if ((m >> v) & 1) { if (*first < 0) *first = v; n++; }
    return n;
}
/* cron weekday (0 = Sunday) to the string table's Monday-first index */
static int cui__cron_wd(int d) { return (d + 6) % 7; }

const char *cui_cron_describe(cui_ctx *ui, const char *cron) {
    cui__cron c;
    if (!cui__cron_parse(cron, &c)) return NULL;
    int m0, h0, d0, mn0, nm = cui__cron_count(c.min, 0, 59, &m0), nh = cui__cron_count(c.hour, 0, 23, &h0);
    int nd = cui__cron_count(c.dom, 1, 31, &d0), nmon = cui__cron_count(c.mon, 1, 12, &mn0);
    int wf, nw = cui__cron_count(c.dow, 0, 6, &wf);
    bool every_hour = nh == 24, every_month = nmon == 12;
    /* every N minutes */
    if (nh == 24 && c.dom_any && c.dow_any && every_month) {
        if (nm == 60) return "Every minute";
        if (nm == 1) return cui__frame_printf(ui, "Every hour at :%02d", m0);
        int step = 0;
        for (int v = m0 + 1; v < 60; v++) if ((c.min >> v) & 1) { step = v - m0; break; }
        bool even = step > 0 && m0 == 0;
        for (int v = 0; even && v < 60; v++) if ((((c.min >> v) & 1) != 0) != (v % step == 0)) even = false;
        if (even) return cui__frame_printf(ui, "Every %d minutes", step);
        return cui__frame_printf(ui, "%d times an hour", nm);
    }
    if (nm != 1 || nh != 1 || !every_month) {
        if (nm == 1 && every_hour && every_month && !c.dom_any && c.dow_any) return cui__frame_printf(ui, "Every hour at :%02d on chosen days", m0);
        return "Custom schedule";
    }
    const char *at = cui__frame_printf(ui, "%02d:%02d", h0, m0);
    if (c.dom_any && c.dow_any) return cui__frame_printf(ui, "Every day at %s", at);
    if (c.dom_any) {
        if (c.dow == 0x3E) return cui__frame_printf(ui, "Every weekday at %s", at);
        if (c.dow == 0x41) return cui__frame_printf(ui, "Every weekend at %s", at);
        char list[160];
        size_t len = 0;
        list[0] = 0;
        int k = 0;
        for (int i = 0; i < 7; i++) {
            int d = (i + 1) % 7;   /* Monday first */
            if (!((c.dow >> d) & 1)) continue;
            const char *nmw = nw > 2 ? cui__weekday_short(ui, cui__cron_wd(d)) : cui__weekday_name(ui, cui__cron_wd(d));
            const char *sep = k == 0 ? "" : k == nw - 1 ? " and " : ", ";
            len += (size_t)snprintf(list + CUI_MIN(len, sizeof list - 1), sizeof list - CUI_MIN(len, sizeof list - 1), "%s%s", sep, nmw);
            k++;
        }
        return cui__frame_printf(ui, "Every %s at %s", list, at);
    }
    if (c.dow_any && nd == 1) return cui__frame_printf(ui, "On day %d of every month at %s", d0, at);
    return "Custom schedule";
}

/* ---- the picker ---- */

enum { CUI__SC_MINUTES = 0, CUI__SC_HOURLY, CUI__SC_DAILY, CUI__SC_WEEKLY, CUI__SC_MONTHLY, CUI__SC_CUSTOM };

typedef struct {
    bool init;
    int mode;
    uint32_t seen;              /* hash of the expression last seen, to notice the app changing it */
    int every, minute, hour, day;
    bool days[7];               /* Monday first */
} cui__sc_state;

/* reads the expression into the picker's controls, picking the mode it is written in */
static void cui__sc_read(cui__sc_state *s, const char *cron) {
    cui__cron c;
    s->every = 15; s->minute = 0; s->hour = 9; s->day = 1;
    for (int i = 0; i < 7; i++) s->days[i] = i < 5;
    s->mode = CUI__SC_CUSTOM;
    if (!cui__cron_parse(cron, &c)) return;
    int m0, h0, d0, mn0, nm = cui__cron_count(c.min, 0, 59, &m0), nh = cui__cron_count(c.hour, 0, 23, &h0);
    int nd = cui__cron_count(c.dom, 1, 31, &d0), nmon = cui__cron_count(c.mon, 1, 12, &mn0);
    if (nmon != 12) return;
    if (nh == 24 && c.dom_any && c.dow_any) {
        if (nm == 1) { s->mode = CUI__SC_HOURLY; s->minute = m0; return; }
        int step = 0;
        for (int v = 1; v < 60; v++) if ((c.min >> v) & 1) { step = v; break; }
        bool even = m0 == 0 && step > 0;
        for (int v = 0; even && v < 60; v++) if ((((c.min >> v) & 1) != 0) != (v % step == 0)) even = false;
        if (even || nm == 60) { s->mode = CUI__SC_MINUTES; s->every = nm == 60 ? 1 : step; }
        return;
    }
    if (nm != 1 || nh != 1) return;
    s->minute = m0; s->hour = h0;
    if (c.dom_any && c.dow_any) { s->mode = CUI__SC_DAILY; return; }
    if (c.dom_any) {
        s->mode = CUI__SC_WEEKLY;
        for (int i = 0; i < 7; i++) s->days[i] = (c.dow >> ((i + 1) % 7)) & 1;
        return;
    }
    if (c.dow_any && nd == 1) { s->mode = CUI__SC_MONTHLY; s->day = d0; }
}

/* writes the controls back as an expression */
static void cui__sc_write(const cui__sc_state *s, char *cron, size_t cap) {
    switch (s->mode) {
    case CUI__SC_MINUTES: if (s->every <= 1) snprintf(cron, cap, "* * * * *"); else snprintf(cron, cap, "*/%d * * * *", s->every); break;
    case CUI__SC_HOURLY: snprintf(cron, cap, "%d * * * *", s->minute); break;
    case CUI__SC_DAILY: snprintf(cron, cap, "%d %d * * *", s->minute, s->hour); break;
    case CUI__SC_WEEKLY: {
        char list[32];
        size_t len = 0;
        list[0] = 0;
        for (int i = 0; i < 7; i++)
            if (s->days[i]) len += (size_t)snprintf(list + len, sizeof list - len, "%s%d", len ? "," : "", (i + 1) % 7);
        if (!len) snprintf(list, sizeof list, "1");
        snprintf(cron, cap, "%d %d * * %s", s->minute, s->hour, list);
        break;
    }
    case CUI__SC_MONTHLY: snprintf(cron, cap, "%d %d %d * *", s->minute, s->hour, s->day); break;
    default: break;
    }
}

/* the time zone offset of `now` in minutes east of UTC */

/* "Mon, Nov 9 · 09:30" in the zone of `offset` */
static const char *cui__sc_when(cui_ctx *ui, int64_t t, int offset) {
    int64_t lt = t + (int64_t)offset * 60;
    int64_t days = lt >= 0 ? lt / 86400 : (lt - 86399) / 86400;
    int64_t sec = lt - days * 86400;
    cui_date dt = cui_date_from_days((int)days);
    int wd = (int)(((days % 7) + 7 + 4) % 7);
    return cui__frame_printf(ui, "%s, %s %d \xc2\xb7 %02d:%02d", cui__weekday_short(ui, cui__cron_wd(wd)), cui__month_short(ui, dt.month - 1), dt.day,
                             (int)(sec / 3600), (int)((sec / 60) % 60));
}

static bool cui__sc_number(cui_ctx *ui, const char *id, int *v, int lo, int hi, const char *suffix) {
    double x = *v;
    cui_number_opts no;
    memset(&no, 0, sizeof no);
    no.step = 1; no.width = CUI_PX(suffix ? 132 : 104); no.suffix = suffix;
    if (cui_number_ex(ui, id, &x, lo, hi, &no)) { *v = CUI_CLAMP((int)(x + 0.5), lo, hi); return true; }
    return false;
}

/* "at" and the time field, kept together when the row wraps */
static bool cui__sc_at_time(cui_ctx *ui, cui__sc_state *s, const cui_time_opts *to, float fs) {
    int g = cui__node_new(ui, 0);
    ui->nodes[g].dir = CUI_ROW; ui->nodes[g].gap = 10; ui->nodes[g].align = CUI_ALIGN_CENTER;
    cui__open(ui, g);
    cui__text_node(ui, "at", -1, CUI_FONT_BODY, fs, CUI_C(ui, TEXT_SECONDARY), false);
    bool changed = cui_time_picker_ex(ui, "time", &s->hour, &s->minute, to);
    cui__close(ui);
    return changed;
}

cui_schedule_result cui_schedule_picker_ex(cui_ctx *ui, const char *id_str, char *cron, size_t cap, const cui_schedule_opts *o) {
    cui_schedule_opts d;
    memset(&d, 0, sizeof d);
    d.next_count = 3;
    if (!o) o = &d;
    cui_schedule_result res = { false, false, 0 };
    if (!cron || cap < 12) return res;
    cui_id id = cui_make_id(ui, id_str);
    cui__sc_state *s = cui_state(ui, id ^ 0x5C4Eu, sizeof *s);
    uint32_t h = cui__hash_str(cron, 0x5C4Eu);
    if (!s->init || h != s->seen) { cui__sc_read(s, cron); s->init = true; }
    cui_push_id(ui, id_str);
    int col = cui__node_new(ui, id);
    cui_node *cn = &ui->nodes[col];
    cn->gap = 12; cn->w = o->width.kind ? o->width : CUI_FILL;
    cui__open(ui, col);
    static const char *const modes[6] = { "Minutes", "Hourly", "Daily", "Weekly", "Monthly", "Custom" };
    cui_segmented_opts sg;
    memset(&sg, 0, sizeof sg);
    sg.size = CUI_SIZE_SM;
    int mode = s->mode;
    bool edited = false;
    if (cui_segmented_ex(ui, "mode", modes, 6, &mode, &sg)) { s->mode = mode; edited = mode != CUI__SC_CUSTOM; }
    /* the controls of the mode */
    int row = cui__node_new(ui, 0);
    cui_node *rn = &ui->nodes[row];
    rn->dir = CUI_ROW; rn->gap = 10; rn->align = CUI_ALIGN_CENTER; rn->w = CUI_FILL; rn->flags |= CUI_NF_ROW_WRAP; rn->min_h = 36;
    cui__open(ui, row);
    float fs = cui__fs(ui, ui->theme.text_sm);
    cui_time_opts to;
    memset(&to, 0, sizeof to);
    to.width = CUI_PX(112);
    switch (s->mode) {
    case CUI__SC_MINUTES:
        cui__text_node(ui, "Every", -1, CUI_FONT_BODY, fs, CUI_C(ui, TEXT_SECONDARY), false);
        if (cui__sc_number(ui, "every", &s->every, 1, 59, "minutes")) edited = true;
        break;
    case CUI__SC_HOURLY:
        cui__text_node(ui, "Every hour at minute", -1, CUI_FONT_BODY, fs, CUI_C(ui, TEXT_SECONDARY), false);
        if (cui__sc_number(ui, "minute", &s->minute, 0, 59, NULL)) edited = true;
        break;
    case CUI__SC_DAILY:
        cui__text_node(ui, "Every day at", -1, CUI_FONT_BODY, fs, CUI_C(ui, TEXT_SECONDARY), false);
        if (cui_time_picker_ex(ui, "time", &s->hour, &s->minute, &to)) edited = true;
        break;
    case CUI__SC_WEEKLY: {
        const char *labels[7];
        for (int i = 0; i < 7; i++) labels[i] = cui__weekday_min(ui, i);
        cui_toggle_group_opts go;
        memset(&go, 0, sizeof go);
        go.size = CUI_SIZE_SM;
        bool before[7];
        memcpy(before, s->days, sizeof before);
        if (cui_toggle_group_ex(ui, "days", labels, 7, s->days, &go)) {
            bool any = false;
            for (int i = 0; i < 7; i++) any |= s->days[i];
            if (!any) memcpy(s->days, before, sizeof before);   /* at least one day */
            edited = true;
        }
        if (cui__sc_at_time(ui, s, &to, fs)) edited = true;
        break;
    }
    case CUI__SC_MONTHLY:
        cui__text_node(ui, "On day", -1, CUI_FONT_BODY, fs, CUI_C(ui, TEXT_SECONDARY), false);
        if (cui__sc_number(ui, "day", &s->day, 1, 31, NULL)) edited = true;
        if (cui__sc_at_time(ui, s, &to, fs)) edited = true;
        break;
    default: {
        cui_input_opts io;
        memset(&io, 0, sizeof io);
        io.width = CUI_FILL; io.placeholder = "minute hour day month weekday";
        cui__cron c;
        bool ok = cui__cron_parse(cron, &c);
        io.error = ok ? NULL : "Five fields: minute hour day month weekday";
        if (cui_input_ex(ui, "cron", cron, cap, &io) & CUI_CHANGED) res.changed = true;
        int fld = ui->last_node;
        ui->nodes[fld].direction = CUI_DIRECTION_LTR;
        break;
    }
    }
    cui__close(ui);
    if (edited) { cui__sc_write(s, cron, cap); res.changed = true; }
    s->seen = cui__hash_str(cron, 0x5C4Eu);
    /* what it means, and when it runs next */
    const char *desc = cui_cron_describe(ui, cron);
    res.valid = desc != NULL;
    int64_t now = o->now ? o->now : (int64_t)time(NULL);
    int offset = o->utc ? 0 : cui__utc_offset(now);
    int64_t next = 0;
    bool has_next = res.valid && cui_cron_next(cron, now, offset, &next);
    if (has_next) res.next = next;
    int sum = cui__node_new(ui, 0);
    cui_node *sn = &ui->nodes[sum];
    sn->dir = CUI_ROW; sn->gap = 8; sn->align = CUI_ALIGN_CENTER; sn->w = CUI_FILL;
    cui__open(ui, sum);
    cui__icon_node(ui, res.valid ? CUI_ICON_CLOCK : CUI_ICON_CIRCLE_ALERT, 15, res.valid ? CUI_C(ui, TEXT_SECONDARY) : CUI_C(ui, DANGER));
    int st = cui__text_node(ui, res.valid ? (has_next ? desc : cui__frame_printf(ui, "%s (never runs)", desc)) : "Not a valid schedule", -1,
                            CUI_FONT_MEDIUM, fs, CUI_C(ui, FOREGROUND), true);
    ui->nodes[st].w = CUI_FILL; ui->nodes[st].line_height = 1.4f;
    if (s->mode != CUI__SC_CUSTOM) {
        int ct = cui__text_node(ui, cron, -1, CUI_FONT_MONO, cui__fs(ui, ui->theme.text_xs), CUI_C(ui, TEXT_MUTED), false);
        ui->nodes[ct].direction = CUI_DIRECTION_LTR;
    }
    cui__close(ui);
    int count = CUI_CLAMP(o->next_count, 0, 10);
    if (has_next && count > 0) {
        int list = cui__node_new(ui, 0);
        cui_node *ln = &ui->nodes[list];
        ln->gap = 2; ln->w = CUI_FILL; ln->pad[3] = 23; ln->pad[1] = 0;
        cui__open(ui, list);
        int64_t t = next;
        for (int k = 0; k < count; k++) {
            int tt = cui__text_node(ui, cui__sc_when(ui, t, offset), -1, CUI_FONT_BODY, cui__fs(ui, ui->theme.text_xs), CUI_C(ui, TEXT_SECONDARY), false);
            ui->nodes[tt].line_height = 1.5f; ui->nodes[tt].flags |= CUI_NF_TABULAR;
            if (!cui_cron_next(cron, t, offset, &t)) break;
        }
        cui__close(ui);
    }
    cui__close(ui);
    cui_pop_id(ui);
    ui->last_id = id;
    return res;
}

/* ======================================================================== */
/* Keymap                                                                   */
/* ======================================================================== */

#define CUI__KM_NAV_W   3.5f      /* the navigation cluster and its gap, in key units */
#define CUI__KM_MAIN_W  15.0f
#define CUI__KM_NO_NAV  600.0f    /* narrower than this: the main block only */
#define CUI__KM_MAX_U   46.0f     /* largest key unit, points */

/* one key: its label, the key it is (CUI_KEY_NONE when CUI has no such key), the modifier it is,
 * where it sits (x and width in key units, row) */
typedef struct { const char *label; cui_key key; uint8_t mod; float x, w; int row; } cui__km_key;

#if defined(__APPLE__)
#define CUI__KM_SUPER "Cmd"
#define CUI__KM_ALT "Opt"
#else
#define CUI__KM_SUPER "Win"
#define CUI__KM_ALT "Alt"
#endif

/* the layout, built once: an ANSI keyboard with a function row and a navigation cluster */
static int cui__km_layout(cui__km_key *out, int cap) {
    typedef struct { const char *l; cui_key k; uint8_t m; float w; } K;
    static const K r0[] = { { "Esc", CUI_KEY_ESCAPE, 0, 1 }, { NULL, 0, 0, 1 }, { "F1", CUI_KEY_F1, 0, 1 }, { "F2", CUI_KEY_F2, 0, 1 }, { "F3", CUI_KEY_F3, 0, 1 },
                            { "F4", CUI_KEY_F4, 0, 1 }, { NULL, 0, 0, 0.5f }, { "F5", CUI_KEY_F5, 0, 1 }, { "F6", CUI_KEY_F6, 0, 1 }, { "F7", CUI_KEY_F7, 0, 1 },
                            { "F8", CUI_KEY_F8, 0, 1 }, { NULL, 0, 0, 0.5f }, { "F9", CUI_KEY_F9, 0, 1 }, { "F10", CUI_KEY_F10, 0, 1 }, { "F11", CUI_KEY_F11, 0, 1 },
                            { "F12", CUI_KEY_F12, 0, 1 } };
    static const K r1[] = { { "`", 0, 0, 1 }, { "1", CUI_KEY_1, 0, 1 }, { "2", CUI_KEY_2, 0, 1 }, { "3", CUI_KEY_3, 0, 1 }, { "4", CUI_KEY_4, 0, 1 },
                            { "5", CUI_KEY_5, 0, 1 }, { "6", CUI_KEY_6, 0, 1 }, { "7", CUI_KEY_7, 0, 1 }, { "8", CUI_KEY_8, 0, 1 }, { "9", CUI_KEY_9, 0, 1 },
                            { "0", CUI_KEY_0, 0, 1 }, { "-", CUI_KEY_MINUS, 0, 1 }, { "=", CUI_KEY_EQUAL, 0, 1 }, { "Bksp", CUI_KEY_BACKSPACE, 0, 2 } };
    static const K r2[] = { { "Tab", CUI_KEY_TAB, 0, 1.5f }, { "Q", CUI_KEY_Q, 0, 1 }, { "W", CUI_KEY_W, 0, 1 }, { "E", CUI_KEY_E, 0, 1 }, { "R", CUI_KEY_R, 0, 1 },
                            { "T", CUI_KEY_T, 0, 1 }, { "Y", CUI_KEY_Y, 0, 1 }, { "U", CUI_KEY_U, 0, 1 }, { "I", CUI_KEY_I, 0, 1 }, { "O", CUI_KEY_O, 0, 1 },
                            { "P", CUI_KEY_P, 0, 1 }, { "[", 0, 0, 1 }, { "]", 0, 0, 1 }, { "\\", 0, 0, 1.5f } };
    static const K r3[] = { { "Caps", 0, 0, 1.75f }, { "A", CUI_KEY_A, 0, 1 }, { "S", CUI_KEY_S, 0, 1 }, { "D", CUI_KEY_D, 0, 1 }, { "F", CUI_KEY_F, 0, 1 },
                            { "G", CUI_KEY_G, 0, 1 }, { "H", CUI_KEY_H, 0, 1 }, { "J", CUI_KEY_J, 0, 1 }, { "K", CUI_KEY_K, 0, 1 }, { "L", CUI_KEY_L, 0, 1 },
                            { ";", 0, 0, 1 }, { "'", 0, 0, 1 }, { "Enter", CUI_KEY_ENTER, 0, 2.25f } };
    static const K r4[] = { { "Shift", 0, CUI_MOD_SHIFT, 2.25f }, { "Z", CUI_KEY_Z, 0, 1 }, { "X", CUI_KEY_X, 0, 1 }, { "C", CUI_KEY_C, 0, 1 }, { "V", CUI_KEY_V, 0, 1 },
                            { "B", CUI_KEY_B, 0, 1 }, { "N", CUI_KEY_N, 0, 1 }, { "M", CUI_KEY_M, 0, 1 }, { ",", CUI_KEY_COMMA, 0, 1 }, { ".", CUI_KEY_PERIOD, 0, 1 },
                            { "/", CUI_KEY_SLASH, 0, 1 }, { "Shift", 0, CUI_MOD_SHIFT, 2.75f } };
    static const K r5[] = { { "Ctrl", 0, CUI_MOD_CTRL, 1.25f }, { CUI__KM_SUPER, 0, CUI_MOD_SUPER, 1.25f }, { CUI__KM_ALT, 0, CUI_MOD_ALT, 1.25f },
                            { "Space", CUI_KEY_SPACE, 0, 6.25f }, { CUI__KM_ALT, 0, CUI_MOD_ALT, 1.25f }, { CUI__KM_SUPER, 0, CUI_MOD_SUPER, 1.25f },
                            { "Menu", 0, 0, 1.25f }, { "Ctrl", 0, CUI_MOD_CTRL, 1.25f } };
    static const struct { const K *k; int n; } rows[6] = { { r0, 16 }, { r1, 14 }, { r2, 14 }, { r3, 13 }, { r4, 12 }, { r5, 8 } };
    int n = 0;
    for (int r = 0; r < 6; r++) {
        float x = 0;
        for (int i = 0; i < rows[r].n; i++) {
            const K *k = &rows[r].k[i];
            if (k->l && n < cap) out[n++] = (cui__km_key){ k->l, k->k, k->m, x, k->w, r };
            x += k->w;
        }
    }
    /* the navigation cluster, right of the main block */
    static const K nav[] = { { "Home", CUI_KEY_HOME, 0, 1 }, { "PgUp", CUI_KEY_PAGE_UP, 0, 1 }, { "Del", CUI_KEY_DELETE, 0, 1 }, { "End", CUI_KEY_END, 0, 1 },
                             { "PgDn", CUI_KEY_PAGE_DOWN, 0, 1 }, { "\xe2\x86\x91", CUI_KEY_UP, 0, 1 }, { "\xe2\x86\x90", CUI_KEY_LEFT, 0, 1 },
                             { "\xe2\x86\x93", CUI_KEY_DOWN, 0, 1 }, { "\xe2\x86\x92", CUI_KEY_RIGHT, 0, 1 } };
    static const float nx[9] = { 1, 2, 0, 1, 2, 1, 0, 1, 2 };
    static const int nr[9] = { 1, 1, 2, 2, 2, 4, 5, 5, 5 };
    for (int i = 0; i < 9 && n < cap; i++) out[n++] = (cui__km_key){ nav[i].l, nav[i].k, 0, CUI__KM_MAIN_W + 0.5f + nx[i], 1, nr[i] + 100 };
    return n;
}

typedef struct { cui_rect r; const char *label, *action; cui_color fill, edge, tint; uint8_t flags; } cui__km_cap;
enum { CUI__KM_BOUND = 1, CUI__KM_CONFLICT = 2, CUI__KM_HOVER = 4, CUI__KM_HELD = 8, CUI__KM_DEAD = 16 };
typedef struct { cui__km_cap *caps; int n; float u; cui_color fg, muted, secondary; } cui__km_paint;

static void cui__paint_keymap(cui_ctx *ui, cui_node *n, void *data) {
    cui__km_paint *p = data;
    float u = p->u;
    float ls = CUI_CLAMP(u * 0.27f, 8.0f, 12.0f), as = CUI_CLAMP(u * 0.22f, 7.0f, 10.5f);
    float lh = cui__font_line_height(ui, CUI_FONT_BODY, ls), ah = cui__font_line_height(ui, CUI_FONT_BODY, as);
    for (int i = 0; i < p->n; i++) {
        const cui__km_cap *c = &p->caps[i];
        cui_rect r = { n->rect.x + c->r.x, n->rect.y + c->r.y, c->r.w, c->r.h };
        float rad = CUI_MIN(6.0f, u * 0.16f);
        cui_draw_rect_border(ui, r, CUI_CORNERS(rad), c->fill, c->edge, (c->flags & (CUI__KM_CONFLICT | CUI__KM_HELD)) ? 1.5f : 1);
        if (c->flags & CUI__KM_BOUND) cui_draw_rect(ui, (cui_rect){ r.x + 3, r.y + r.h - 4, r.w - 6, 2 }, CUI_CORNERS(1), c->tint);
        cui_color lc = (c->flags & CUI__KM_DEAD) ? p->muted : (c->flags & CUI__KM_BOUND) ? p->fg : p->secondary;
        float pad = CUI_CLAMP(u * 0.14f, 3.0f, 6.0f);
        /* a long label ("PgUp", "F10" on a small board) shrinks to fit before it is cut */
        float sz = ls, lw = cui__text_width(ui, CUI_FONT_MEDIUM, ls, c->label, -1, false), room = r.w - 2 * pad;
        if (lw > room && lw > 0) sz = CUI_MAX(ls * 0.7f, ls * room / lw);
        cui__draw_text_ellipsis(ui, CUI_FONT_MEDIUM, sz, r.x + pad, r.y + pad * 0.7f, room, lc, c->label, -1, false);
        if (c->action && r.h >= lh + ah + pad)
            cui__draw_text_ellipsis(ui, CUI_FONT_BODY, as, r.x + pad, r.y + r.h - ah - pad - 1, r.w - 2 * pad, p->secondary, c->action, -1, false);
    }
}

static const cui_tone cui__km_tones[6] = { CUI_TONE_VIOLET, CUI_TONE_INFO, CUI_TONE_SUCCESS, CUI_TONE_WARNING, CUI_TONE_ROSE, CUI_TONE_TEAL };
static cui_color cui__km_tone(cui_ctx *ui, int category) { return cui__tone_color(ui, cui__km_tones[((category % 6) + 6) % 6]); }

typedef struct { bool init; int mods; } cui__km_state;

/* the modifier sets offered, with how many bindings each has */
static const int cui__km_sets[5] = { 0, CUI_MOD_PRIMARY, CUI_MOD_SHIFT, CUI_MOD_ALT, CUI_MOD_PRIMARY | CUI_MOD_SHIFT };

static const char *cui__km_set_name(int m) {
    switch (m) {
    case 0: return "No modifier";
    case CUI_MOD_SHIFT: return "Shift";
    case CUI_MOD_ALT: return CUI__KM_ALT;
    case CUI_MOD_PRIMARY: return CUI_MOD_PRIMARY == CUI_MOD_SUPER ? "Cmd" : "Ctrl";
    default: return CUI_MOD_PRIMARY == CUI_MOD_SUPER ? "Cmd+Shift" : "Ctrl+Shift";
    }
}

cui_keymap_result cui_keymap_ex(cui_ctx *ui, const char *id_str, const cui_keymap_binding *b, int count, const cui_keymap_opts *o) {
    cui_keymap_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui_keymap_result res = { CUI_KEY_NONE, 0 };
    if (!b || count < 0) count = 0;
    cui_id id = cui_make_id(ui, id_str), kid = id ^ 0x4B3Au;
    cui__km_state *st = cui_state(ui, id ^ 0x4B3Bu, sizeof *st);
    int *mods = o->mods ? o->mods : &st->mods;
    /* conflicts: two bindings of one combination */
    for (int i = 0; i < count; i++)
        for (int j = i + 1; j < count; j++)
            if (b[i].combo.key == b[j].combo.key && b[i].combo.mods == b[j].combo.mods && b[i].combo.key != CUI_KEY_NONE) { res.conflicts++; break; }
    cui_push_id(ui, id_str);
    int col = cui__node_new(ui, id);
    cui_node *cn = &ui->nodes[col];
    cn->gap = 12; cn->w = o->width.kind ? o->width : CUI_FILL;
    cui__open(ui, col);
    /* which modifiers, and the legend */
    int bar = cui__node_new(ui, 0);
    ui->nodes[bar].dir = CUI_ROW; ui->nodes[bar].gap = 12; ui->nodes[bar].align = CUI_ALIGN_CENTER;
    ui->nodes[bar].w = CUI_FILL; ui->nodes[bar].flags |= CUI_NF_ROW_WRAP;
    cui__open(ui, bar);
    const char *labels[5];
    int sel = 0;
    for (int k = 0; k < 5; k++) {
        int n = 0;
        for (int i = 0; i < count; i++) if (b[i].combo.mods == cui__km_sets[k]) n++;
        labels[k] = n ? cui__frame_printf(ui, "%s %d", cui__km_set_name(cui__km_sets[k]), n) : cui__km_set_name(cui__km_sets[k]);
        if (*mods == cui__km_sets[k]) sel = k;
    }
    cui_segmented_opts sg;
    memset(&sg, 0, sizeof sg);
    sg.size = CUI_SIZE_SM;
    if (cui_segmented_ex(ui, "mods", labels, 5, &sel, &sg)) *mods = cui__km_sets[sel];
    float fs = cui__fs(ui, ui->theme.text_sm);
    for (int c = 0; c < o->category_count && o->categories; c++) {
        int g = cui__node_new(ui, 0);
        ui->nodes[g].dir = CUI_ROW; ui->nodes[g].gap = 6; ui->nodes[g].align = CUI_ALIGN_CENTER;
        cui__open(ui, g);
        int dot = cui__leaf(ui, 0, CUI_PX(10), CUI_PX(10));
        ui->nodes[dot].bg = cui__km_tone(ui, c); ui->nodes[dot].radius = CUI_CORNERS(3);
        cui__text_node(ui, o->categories[c] ? o->categories[c] : "", -1, CUI_FONT_BODY, fs, CUI_C(ui, TEXT_SECONDARY), false);
        cui__close(ui);
    }
    if (res.conflicts) {
        int g = cui__node_new(ui, 0);
        ui->nodes[g].dir = CUI_ROW; ui->nodes[g].gap = 6; ui->nodes[g].align = CUI_ALIGN_CENTER;
        cui__open(ui, g);
        cui__icon_node(ui, CUI_ICON_TRIANGLE_ALERT, 14, CUI_C(ui, DANGER));
        cui__text_node(ui, cui__frame_printf(ui, "%d conflict%s", res.conflicts, res.conflicts == 1 ? "" : "s"), -1, CUI_FONT_BODY, fs, CUI_C(ui, DANGER), false);
        cui__close(ui);
    }
    cui__close(ui);
    /* the keyboard: one node, drawn; keys found from last frame's size */
    cui_rect pr = cui__prev_rect(ui, kid);
    float avail = pr.w > 0 ? pr.w : 760;
    bool nav = avail >= CUI__KM_NO_NAV, frow = !o->no_function_row;
    float units = CUI__KM_MAIN_W + (nav ? CUI__KM_NAV_W : 0);
    float u = CUI_MIN(CUI__KM_MAX_U, avail / units);
    float top_rows = frow ? 1.25f : 0;
    float h = (top_rows + 5) * u;
    float x0 = CUI_MAX(0.0f, (avail - units * u) * 0.5f);   /* centered when the board is narrower than its space */
    cui__km_key layout[96];
    int nk = cui__km_layout(layout, 96);
    cui_interact ki = cui__interact(ui, kid, 0);
    int hover = -1;
    cui__km_cap *caps = cui__frame_alloc(ui, sizeof(cui__km_cap) * (size_t)nk);
    int nc = 0;
    for (int i = 0; i < nk; i++) {
        const cui__km_key *k = &layout[i];
        int row = k->row >= 100 ? k->row - 100 : k->row;
        if (k->row >= 100 && !nav) continue;
        if (row == 0 && !frow) continue;
        float y = row == 0 ? 0 : (top_rows + row - 1) * u;
        cui_rect r = { x0 + k->x * u + 2, y + 2, k->w * u - 4, u - 4 };
        cui__km_cap *c = &caps[nc];
        memset(c, 0, sizeof *c);
        c->r = r; c->label = k->label;
        if (ki.hovered && pr.w > 0 && ui->mx >= pr.x + r.x && ui->mx < pr.x + r.x + r.w && ui->my >= pr.y + r.y && ui->my < pr.y + r.y + r.h) hover = nc;
        int first = -1, n = 0;
        for (int j = 0; j < count && k->key != CUI_KEY_NONE; j++)
            if (b[j].combo.key == k->key && b[j].combo.mods == *mods) { if (first < 0) first = j; n++; }
        c->fill = CUI_C(ui, SURFACE); c->edge = CUI_C(ui, BORDER);
        if (k->key == CUI_KEY_NONE && !k->mod) c->flags |= CUI__KM_DEAD;
        if (k->mod && (*mods & k->mod)) { c->flags |= CUI__KM_HELD; c->edge = cui__emphasis(ui); c->fill = CUI_C(ui, ACTIVE); }
        if (first >= 0) {
            cui_color tone = cui__km_tone(ui, b[first].category);
            c->flags |= CUI__KM_BOUND;
            c->action = b[first].action;
            c->tint = tone;
            c->fill = cui_mix(CUI_C(ui, SURFACE), tone, ui->mode == CUI_THEME_DARK ? 0.16f : 0.10f);
            if (n > 1) { c->flags |= CUI__KM_CONFLICT; c->edge = CUI_C(ui, DANGER); c->action = cui__frame_printf(ui, "%d actions", n); }
        }
        if (k->key == CUI_KEY_NONE) c->label = k->label;
        nc++;
        (void)i;
    }
    if (hover >= 0) {
        caps[hover].flags |= CUI__KM_HOVER;
        caps[hover].fill = cui_mix(caps[hover].fill, CUI_C(ui, HOVER), 1.0f);
        cui__set_cursor(ui, CUI_CURSOR_HAND);
    }
    int kb = cui__node_new(ui, kid);
    cui_node *kn = &ui->nodes[kb];
    kn->w = CUI_FILL; kn->h = CUI_PX(h);
    kn->direction = CUI_DIRECTION_LTR;   /* a keyboard keeps its layout in a right-to-left interface */
    cui__mark_interactive(ui, kb, 0);
    cui__km_paint *p = cui__frame_alloc(ui, sizeof *p);
    p->caps = caps; p->n = nc; p->u = u;
    p->fg = CUI_C(ui, FOREGROUND); p->muted = cui_alpha(CUI_C(ui, TEXT_MUTED), 0.7f); p->secondary = CUI_C(ui, TEXT_SECONDARY);
    kn->paint = cui__paint_keymap; kn->paint_data = p;
    /* the key under the pointer: every binding on it, as a tooltip; a click reports it */
    if (hover >= 0) {
        const cui__km_cap *c = &caps[hover];
        cui_key key = CUI_KEY_NONE;
        for (int i = 0, vis = 0; i < nk; i++) {
            const cui__km_key *k = &layout[i];
            int row = k->row >= 100 ? k->row - 100 : k->row;
            if ((k->row >= 100 && !nav) || (row == 0 && !frow)) continue;
            if (vis++ == hover) { key = k->key; break; }
        }
        if (ki.clicked && key != CUI_KEY_NONE) res.clicked = key;
        char tip[512];
        size_t len = 0;
        tip[0] = 0;
        for (int j = 0; j < count && key != CUI_KEY_NONE; j++) {
            if (b[j].combo.key != key) continue;
            const char *combo = cui_shortcut_label(ui, b[j].combo);
            len += (size_t)snprintf(tip + CUI_MIN(len, sizeof tip - 1), sizeof tip - CUI_MIN(len, sizeof tip - 1), "%s%s  %s", len ? "\n" : "", combo, b[j].action ? b[j].action : "");
        }
        if (len && ui->active == 0) {
            cui_id tid = kid ^ (0x7117u * (cui_id)(hover + 1));
            cui__open(ui, kb);
            cui__absolute(ui, tid, c->r.x, c->r.y, c->r.w, c->r.h);
            cui__close(ui);
            cui__tooltip_passive(ui, tid, cui__frame_strdup(ui, tip, -1));
        }
    }
    cui__close(ui);
    cui_pop_id(ui);
    ui->last_id = id;
    return res;
}
