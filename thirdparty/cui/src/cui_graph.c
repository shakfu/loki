/* Graphs of things that point at each other: the commit graph (version
 * history with its branch lanes) and the dependency graph (nodes and edges
 * laid out in layers). Both work out their geometry only when the data
 * changes, keep it in their state, and draw it with paint callbacks. */
#include "cui_internal.h"

/* ======================================================================== */
/* Commit graph                                                             */
/* ======================================================================== */

#define CUI__CG_ROW     30.0f
#define CUI__CG_LANES   12          /* lanes drawn; a wider history shares the last one */
#define CUI__CG_LANE_W  14.0f
#define CUI__CG_NARROW  560.0f      /* below this width the author and date are left out */

/* each row: the commit's lane, where every lane coming in from above goes (to the commit, or
 * straight on), where every lane going down comes from, and up to two lines from the commit to
 * lanes that already wait for its parents */
typedef struct {
    int8_t lane;
    int8_t in[CUI__CG_LANES];       /* lane at the middle each top lane reaches, -1 none */
    int8_t out[CUI__CG_LANES];      /* lane at the middle each bottom lane starts from, -1 none */
    int8_t join[2];                 /* extra lines from the commit down to these lanes, -1 none */
    bool merge;
} cui__cg_row;

typedef struct {
    bool init;
    int own_sel;
    uint32_t key;                   /* the data the lanes were worked out for */
    int n, lanes;
    cui__cg_row *rows;
    bool scroll_to_sel;
} cui__cg_state;

static void cui__cg_dtor(cui_ctx *ui, void *data) { cui__free(ui, ((cui__cg_state *)data)->rows); }

static uint32_t cui__cg_key(const cui_commit *c, int n) {
    uint32_t h = cui__hash(&n, sizeof n, 0xC0117u);
    for (int i = 0; i < n; i++) h = cui__hash(c[i].parent, sizeof c[i].parent, h);
    return h;
}

/* the lanes: walking from the newest commit, each lane waits for one commit (a parent seen above) */
static void cui__cg_layout(cui_ctx *ui, cui__cg_state *s, const cui_commit *c, int n) {
    cui__free(ui, s->rows);
    s->rows = n > 0 ? cui__alloc(ui, sizeof(cui__cg_row) * (size_t)n) : NULL;
    s->n = s->rows ? n : 0;
    s->lanes = 1;
    int cur[CUI__CG_LANES];
    for (int j = 0; j < CUI__CG_LANES; j++) cur[j] = -1;
    for (int i = 0; i < s->n; i++) {
        cui__cg_row *r = &s->rows[i];
        memset(r, -1, sizeof *r);
        r->merge = false;
        int lane = -1;
        for (int j = 0; j < CUI__CG_LANES; j++) if (cur[j] == i) { lane = j; break; }
        if (lane < 0) for (int j = 0; j < CUI__CG_LANES; j++) if (cur[j] < 0) { lane = j; break; }
        if (lane < 0) lane = CUI__CG_LANES - 1;
        r->lane = (int8_t)lane;
        int next[CUI__CG_LANES];
        bool from_commit[CUI__CG_LANES];
        for (int j = 0; j < CUI__CG_LANES; j++) {
            r->in[j] = (int8_t)(cur[j] < 0 ? -1 : cur[j] == i ? lane : j);
            next[j] = cur[j] == i ? -1 : cur[j];
            from_commit[j] = false;
        }
        int nj = 0;
        for (int k = 0; k < 2; k++) {
            int p = c[i].parent[k];
            if (p <= i || p >= n) continue;
            if (k == 1) r->merge = true;
            int wait = -1;
            for (int j = 0; j < CUI__CG_LANES; j++) if (next[j] == p) { wait = j; break; }
            if (wait >= 0) {   /* a lane already waits for this parent: join it */
                if (nj < 2) r->join[nj++] = (int8_t)wait;
                continue;
            }
            int f = next[lane] < 0 ? lane : -1;
            if (f < 0) for (int j = 0; j < CUI__CG_LANES; j++) if (next[j] < 0) { f = j; break; }
            if (f < 0) f = CUI__CG_LANES - 1;
            next[f] = p;
            from_commit[f] = true;
        }
        for (int j = 0; j < CUI__CG_LANES; j++) {
            r->out[j] = (int8_t)(next[j] < 0 ? -1 : from_commit[j] ? lane : j);
            if (next[j] >= 0 || cur[j] >= 0) s->lanes = CUI_MAX(s->lanes, j + 1);
            cur[j] = next[j];
        }
        s->lanes = CUI_MAX(s->lanes, lane + 1);
    }
}

typedef struct { cui__cg_row row; int lanes; bool selected; cui_color colors[6]; cui_color surface; } cui__cg_paint;

static float cui__cg_x(cui_rect r, int lane) { return r.x + 10 + lane * CUI__CG_LANE_W; }
/* a line between two lanes over part of the row: straight down, or an S curve */
static void cui__cg_link(cui_ctx *ui, cui_rect r, int from, float y0, int to, float y1, cui_color c) {
    float x0 = cui__cg_x(r, from), x1 = cui__cg_x(r, to);
    if (from == to) { cui_draw_line(ui, (cui_vec2){ x0, y0 }, (cui_vec2){ x1, y1 }, 1.5f, c); return; }
    cui_vec2 pts[9];
    for (int k = 0; k <= 8; k++) {
        float t = k / 8.0f, e = t * t * (3 - 2 * t);
        pts[k] = (cui_vec2){ x0 + (x1 - x0) * e, y0 + (y1 - y0) * t };
    }
    cui_draw_polyline(ui, pts, 9, 1.5f, c, false);
}
static void cui__paint_commit_lanes(cui_ctx *ui, cui_node *n, void *data) {
    cui__cg_paint *p = data;
    cui_rect r = n->rect;
    float mid = r.y + r.h * 0.5f, bot = r.y + r.h;
    const cui__cg_row *row = &p->row;
    for (int j = 0; j < CUI__CG_LANES; j++) {
        if (row->in[j] >= 0) cui__cg_link(ui, r, j, r.y, row->in[j], mid, p->colors[j % 6]);
        if (row->out[j] >= 0) cui__cg_link(ui, r, row->out[j], mid, j, bot, p->colors[j % 6]);
    }
    for (int k = 0; k < 2; k++)
        if (row->join[k] >= 0) cui__cg_link(ui, r, row->lane, mid, row->join[k], bot, p->colors[row->join[k] % 6]);
    cui_vec2 c = { cui__cg_x(r, row->lane), mid };
    cui_color lc = p->colors[row->lane % 6];
    if (p->selected) cui_draw_circle(ui, c, 6.5f, cui_alpha(lc, 0.3f));
    cui_draw_circle(ui, c, 4.5f, lc);
    if (row->merge) cui_draw_circle(ui, c, 2.2f, p->surface);   /* a merge is a ring */
}

/* "main, origin/main, tag: v1.2" as badges: tags violet with a tag icon, remote branches plain */
static void cui__cg_refs(cui_ctx *ui, const char *refs) {
    if (!refs || !refs[0]) return;
    const char *s = refs;
    for (int guard = 0; *s && guard < 8; guard++) {
        while (*s == ' ' || *s == ',') s++;
        const char *e = s;
        while (*e && *e != ',') e++;
        int len = (int)(e - s);
        while (len > 0 && s[len - 1] == ' ') len--;
        if (len > 0) {
            bool tag = len > 5 && !strncmp(s, "tag: ", 5);
            bool remote = !tag && memchr(s, '/', (size_t)len) != NULL;
            const char *txt = cui__frame_strdup(ui, tag ? s + 5 : s, tag ? len - 5 : len);
            cui_badge_opts bo;
            memset(&bo, 0, sizeof bo);
            bo.size = CUI_SIZE_SM;
            bo.tone = tag ? CUI_TONE_VIOLET : remote ? CUI_TONE_NEUTRAL : CUI_TONE_SUCCESS;
            bo.icon = tag ? CUI_ICON_TAG : CUI_ICON_GIT_BRANCH;
            cui_badge_ex(ui, txt, &bo);
        }
        s = e;
    }
}

cui_commit_graph_result cui_commit_graph_ex(cui_ctx *ui, const char *id_str, const cui_commit *commits, int count, const cui_commit_graph_opts *o) {
    cui_commit_graph_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui_commit_graph_result res = { -1, -1, -1 };
    if (!commits || count < 0) count = 0;
    cui_id id = cui_make_id(ui, id_str), lid = id ^ 0xC01Du;
    cui__cg_state *s = cui_state(ui, id ^ 0xC0117u, sizeof *s);
    cui__state_set_dtor(ui, id ^ 0xC0117u, cui__cg_dtor);
    if (!s->init) { s->init = true; s->own_sel = -1; s->key = 0; }
    int *sel = o->selected ? o->selected : &s->own_sel;
    if (*sel >= count) *sel = -1;
    uint32_t key = cui__cg_key(commits, count);
    if (key != s->key || s->n != count) { cui__cg_layout(ui, s, commits, count); s->key = key; }
    cui_rect pr = cui__prev_rect(ui, id);
    bool narrow = pr.w > 0 && pr.w < CUI__CG_NARROW;
    /* keys on the list */
    cui_interact lr = cui__interact(ui, lid, CUI_IF_FOCUSABLE);
    if (lr.focused && s->n > 0) {
        int to = *sel;
        if (cui_key_pressed(ui, CUI_KEY_DOWN)) to = *sel < 0 ? 0 : *sel + 1;
        if (cui_key_pressed(ui, CUI_KEY_UP)) to = *sel < 0 ? 0 : *sel - 1;
        if (cui_key_pressed(ui, CUI_KEY_HOME)) to = 0;
        if (cui_key_pressed(ui, CUI_KEY_END)) to = s->n - 1;
        to = CUI_CLAMP(to, -1, s->n - 1);
        if (to != *sel && to >= 0) { *sel = to; s->scroll_to_sel = true; }
        if (cui_key_pressed(ui, CUI_KEY_ENTER) && *sel >= 0) res.activated = *sel;
    }
    cui_push_id(ui, id_str);
    int frame = cui__node_new(ui, id);
    cui_node *fn = &ui->nodes[frame];
    fn->w = o->width.kind ? o->width : CUI_FILL;
    fn->h = o->height < 0 ? CUI_FILL : CUI_PX(o->height > 0 ? o->height : 320);
    fn->radius = CUI_CORNERS(ui->theme.radius_control);
    fn->border = 1; fn->border_color = CUI_C(ui, BORDER);
    fn->bg = CUI_C(ui, SURFACE);
    fn->flags |= CUI_NF_CLIP;
    cui__open(ui, frame);
    int list = cui__node_new(ui, lid);
    ui->nodes[list].w = CUI_FILL; ui->nodes[list].h = CUI_FILL;
    cui__mark_interactive(ui, list, CUI_IF_FOCUSABLE);
    cui__focus_ring(ui, frame, lr.focus_visible ? 1.0f : 0.0f);
    cui__open(ui, list);
    if (s->scroll_to_sel && *sel >= 0) {
        cui_scroll_state *ss = cui_state(ui, cui_make_id(ui, "rows"), sizeof *ss);
        float y = *sel * CUI__CG_ROW, vh = ss->view_h > 0 ? ss->view_h : 240;
        if (y < ss->ty) ss->ty = y;
        else if (y + CUI__CG_ROW > ss->ty + vh) ss->ty = y + CUI__CG_ROW - vh;
        s->scroll_to_sel = false;
    }
    if (s->n == 0) {
        int e = cui__node_new(ui, 0);
        ui->nodes[e].w = CUI_FILL; ui->nodes[e].h = CUI_FILL; ui->nodes[e].align = CUI_ALIGN_CENTER; ui->nodes[e].justify = CUI_JUSTIFY_CENTER;
        cui__open(ui, e);
        cui__text_node(ui, "No commits", -1, CUI_FONT_BODY, cui__fs(ui, ui->theme.text_sm), CUI_C(ui, TEXT_MUTED), false);
        cui__close(ui);
    } else {
        int first, last;
        cui_virtual_list_begin(ui, "rows", s->n, CUI__CG_ROW, CUI_FILL, &first, &last);
        float fs = cui__fs(ui, ui->theme.text_sm), fx = cui__fs(ui, ui->theme.text_xs);
        float graph_w = 20 + (s->lanes - 1) * CUI__CG_LANE_W;
        for (int i = first; i < last; i++) {
            const cui_commit *c = &commits[i];
            cui_id rid = cui__hash(&i, sizeof i, id ^ 0xC0DEu);
            cui_interact rr = cui__interact(ui, rid, CUI_IF_KEEP_FOCUS);
            if (rr.clicked) {
                *sel = i; res.clicked = i; ui->focus = lid;
                if (ui->click_count >= 2) res.activated = i;
            }
            if (rr.hovered) res.hovered = i;
            bool selected = *sel == i;
            int row = cui__node_new(ui, rid);
            cui_node *rn = &ui->nodes[row];
            rn->dir = CUI_ROW; rn->gap = 10; rn->align = CUI_ALIGN_CENTER; rn->w = CUI_FILL; rn->h = CUI_PX(CUI__CG_ROW);
            rn->pad[1] = 12;
            rn->bg = selected ? (lr.focused ? cui_mix(CUI_C(ui, SURFACE), cui__emphasis(ui), ui->accent == CUI_ACCENT_NEUTRAL ? 0.08f : 0.16f) : CUI_C(ui, ACTIVE))
                              : rr.hovered ? CUI_C(ui, HOVER) : cui__rgba(0, 0, 0, 0);
            cui__mark_interactive(ui, row, 0);
            cui__open(ui, row);
            /* the lanes */
            int g = cui__leaf(ui, 0, CUI_PX(graph_w), CUI_FILL);
            cui__cg_paint *p = cui__frame_alloc(ui, sizeof *p);
            p->row = s->rows[i]; p->lanes = s->lanes; p->selected = selected; p->surface = CUI_C(ui, SURFACE);
            for (int k = 0; k < 6; k++) p->colors[k] = ui->theme.colors[CUI_TOKEN_SERIES_1 + k];
            ui->nodes[g].paint = cui__paint_commit_lanes; ui->nodes[g].paint_data = p;
            ui->nodes[g].direction = CUI_DIRECTION_LTR;
            /* refs and the first line of the message */
            int msg = cui__node_new(ui, 0);
            ui->nodes[msg].dir = CUI_ROW; ui->nodes[msg].gap = 6; ui->nodes[msg].align = CUI_ALIGN_CENTER;
            ui->nodes[msg].w = CUI_FILL; ui->nodes[msg].flags |= CUI_NF_CLIP;
            cui__open(ui, msg);
            if (!narrow || selected) cui__cg_refs(ui, c->refs);
            const char *m = c->message ? c->message : "";
            const char *nl = strchr(m, '\n');
            int t = cui__text_node(ui, m, nl ? (int)(nl - m) : -1, CUI_FONT_BODY, fs, CUI_C(ui, FOREGROUND), false);
            ui->nodes[t].line_height = 1.3f; ui->nodes[t].w = CUI_FILL;
            cui__close(ui);
            if (!narrow) {
                int at = cui__text_node(ui, c->author ? c->author : "", -1, CUI_FONT_BODY, fx, CUI_C(ui, TEXT_SECONDARY), false);
                ui->nodes[at].w = CUI_PX(120); ui->nodes[at].line_height = 1.3f; ui->nodes[at].flags |= CUI_NF_CLIP;
                int dt = cui__text_node(ui, c->date ? c->date : "", -1, CUI_FONT_BODY, fx, CUI_C(ui, TEXT_MUTED), false);
                ui->nodes[dt].w = CUI_PX(96); ui->nodes[dt].line_height = 1.3f; ui->nodes[dt].flags |= CUI_NF_CLIP;
            }
            int ht = cui__text_node(ui, c->hash ? c->hash : "", c->hash ? CUI_MIN((int)strlen(c->hash), 7) : 0, CUI_FONT_MONO, fx, CUI_C(ui, TEXT_MUTED), false);
            ui->nodes[ht].w = CUI_PX(60); ui->nodes[ht].line_height = 1.3f; ui->nodes[ht].direction = CUI_DIRECTION_LTR;
            cui__close(ui);
            if (rr.hovered && ui->active == 0 && c->message)
                cui__tooltip_passive(ui, rid, cui__frame_printf(ui, "%s\n%s \xc2\xb7 %s \xc2\xb7 %.12s", m, c->author ? c->author : "", c->date ? c->date : "", c->hash ? c->hash : ""));
        }
        cui_virtual_list_end(ui);
    }
    cui__close(ui);   /* list */
    cui__close(ui);   /* frame */
    cui_pop_id(ui);
    ui->last_id = id;
    return res;
}

/* ======================================================================== */
/* Dependency graph                                                         */
/* ======================================================================== */

#define CUI__GV_GAP_X  64.0f       /* between layers */
#define CUI__GV_GAP_Y  18.0f       /* between nodes of a layer */
#define CUI__GV_PAD    24.0f       /* around the graph when fitted */
#define CUI__GV_NODE_H 34.0f
#define CUI__GV_DETAIL 14.0f       /* extra height for a detail line */
#define CUI__GV_DRAG   3.0f

typedef struct { float x, y, w, h; int layer; } cui__gv_box;

typedef struct {
    bool init, fit;              /* fit: fit the view to the graph next frame */
    int own_sel;
    uint32_t key;
    int n;                       /* nodes; box[n..nv) are waypoints of edges that skip layers */
    int nv;
    cui__gv_box *box;
    int ne;                      /* edges as laid out: each runs from[k] -> its waypoints -> to[k] */
    int *from, *to, *rstart, *rlen, *route;
    float gw, gh;                /* the graph's size at zoom 1 */
    bool top_down;
    float px, py, zoom;          /* the view: graph point (0, 0) sits at (px, py) from the panel's corner */
    int mode;                    /* 0 idle, 1 pressed, 2 panning */
    float press_x, press_y, last_x, last_y;
    int press_node;
    float focus;
} cui__gv_state;

static void cui__gv_free(cui_ctx *ui, cui__gv_state *s) {
    cui__free(ui, s->box); cui__free(ui, s->from);
    s->box = NULL; s->from = s->to = s->rstart = s->rlen = s->route = NULL;
}
static void cui__gv_dtor(cui_ctx *ui, void *data) { cui__gv_free(ui, (cui__gv_state *)data); }

static uint32_t cui__gv_key(const cui_graph_node *nodes, int n, const cui_graph_edge *edges, int e, bool top_down) {
    uint32_t h = cui__hash(&n, sizeof n, 0x6A9Fu);
    h = cui__hash(&e, sizeof e, h);
    h = cui__hash(&top_down, sizeof top_down, h);
    for (int i = 0; i < n; i++) {
        h = cui__hash_str(nodes[i].label ? nodes[i].label : "", h);
        h = cui__hash_str(nodes[i].detail ? nodes[i].detail : "", h);
    }
    if (edges && e > 0) h = cui__hash(edges, sizeof *edges * (size_t)e, h);
    return h;
}

/* Layers: an edge points from a layer to a later one; edges that close a cycle (found by a depth
 * first walk) are left out of this. An edge that skips layers gets a waypoint in each layer it
 * crosses, so it is routed between the nodes instead of through them. Then each layer is ordered
 * by where its neighbours sit, a few sweeps, to cross fewer edges. Runs once per change. */
static void cui__gv_layout(cui_ctx *ui, cui__gv_state *s, const cui_graph_node *nodes, int n, const cui_graph_edge *edges, int ne, bool top_down) {
    cui__gv_free(ui, s);
    s->n = s->nv = s->ne = 0;
    s->gw = s->gh = 0;
    s->top_down = top_down;
    if (n <= 0) return;
    /* the edges that are valid, without loops */
    size_t ee = (size_t)CUI_MAX(1, ne);
    int *ef = cui__frame_alloc(ui, sizeof(int) * ee), *et = cui__frame_alloc(ui, sizeof(int) * ee), m = 0;
    for (int k = 0; k < ne; k++)
        if (edges[k].from >= 0 && edges[k].from < n && edges[k].to >= 0 && edges[k].to < n && edges[k].from != edges[k].to) { ef[m] = edges[k].from; et[m] = edges[k].to; m++; }
    ne = m;
    size_t nn = (size_t)n;
    int *start = cui__frame_alloc(ui, sizeof(int) * (nn + 1)), *adj = cui__frame_alloc(ui, sizeof(int) * ee), *eid = cui__frame_alloc(ui, sizeof(int) * ee);
    memset(start, 0, sizeof(int) * (nn + 1));
    for (int k = 0; k < ne; k++) start[ef[k] + 1]++;
    for (int i = 0; i < n; i++) start[i + 1] += start[i];
    int *fill = cui__frame_alloc(ui, sizeof(int) * nn);
    for (int i = 0; i < n; i++) fill[i] = start[i];
    for (int k = 0; k < ne; k++) { adj[fill[ef[k]]] = et[k]; eid[fill[ef[k]]++] = k; }
    /* depth first: an edge to a node still on the walk closes a cycle */
    uint8_t *mark = cui__frame_alloc(ui, nn);
    memset(mark, 0, nn);
    bool *back = cui__frame_alloc(ui, sizeof(bool) * ee);
    memset(back, 0, sizeof(bool) * ee);
    int *stack = cui__frame_alloc(ui, sizeof(int) * nn), *it = cui__frame_alloc(ui, sizeof(int) * nn);
    for (int root = 0; root < n; root++) {
        if (mark[root]) continue;
        int sp = 0;
        stack[sp] = root; it[sp] = start[root]; sp++; mark[root] = 1;
        while (sp > 0) {
            int v = stack[sp - 1];
            if (it[sp - 1] < start[v + 1]) {
                int k = it[sp - 1]++, w = adj[k];
                if (mark[w] == 1) back[eid[k]] = true;
                else if (mark[w] == 0) { mark[w] = 1; stack[sp] = w; it[sp] = start[w]; sp++; }
            } else { mark[v] = 2; sp--; }
        }
    }
    /* longest path layering, in topological order */
    int *layer = cui__frame_alloc(ui, sizeof(int) * nn), *indeg = cui__frame_alloc(ui, sizeof(int) * nn), *queue = cui__frame_alloc(ui, sizeof(int) * nn);
    memset(indeg, 0, sizeof(int) * nn);
    for (int k = 0; k < ne; k++) if (!back[k]) indeg[et[k]]++;
    int qh = 0, qt = 0;
    for (int v = 0; v < n; v++) { layer[v] = 0; if (!indeg[v]) queue[qt++] = v; }
    while (qh < qt) {
        int v = queue[qh++];
        for (int a = start[v]; a < start[v + 1]; a++) {
            int k = eid[a], w = adj[a];
            if (back[k]) continue;
            layer[w] = CUI_MAX(layer[w], layer[v] + 1);
            if (--indeg[w] == 0) queue[qt++] = w;
        }
    }
    int layers = 0, waypoints = 0;
    for (int v = 0; v < n; v++) layers = CUI_MAX(layers, layer[v] + 1);
    for (int k = 0; k < ne; k++) if (!back[k]) waypoints += CUI_MAX(0, layer[et[k]] - layer[ef[k]] - 1);
    /* storage: boxes for nodes and waypoints, and the routes */
    int nv = n + waypoints;
    s->box = cui__alloc(ui, sizeof(cui__gv_box) * (size_t)nv);
    s->from = cui__alloc(ui, sizeof(int) * (ee * 4 + (size_t)CUI_MAX(1, waypoints)));
    if (!s->box || !s->from) { cui__gv_free(ui, s); return; }
    s->to = s->from + ee; s->rstart = s->to + ee; s->rlen = s->rstart + ee; s->route = s->rlen + ee;
    s->n = n; s->nv = nv; s->ne = ne;
    float fs = cui__fs(ui, ui->theme.text_sm), fx = cui__fs(ui, ui->theme.text_xs);
    for (int v = 0; v < n; v++) {
        float lw = cui__text_width(ui, CUI_FONT_MEDIUM, fs, nodes[v].label ? nodes[v].label : "", -1, false);
        float dw = nodes[v].detail ? cui__text_width(ui, CUI_FONT_BODY, fx, nodes[v].detail, -1, false) : 0;
        s->box[v] = (cui__gv_box){ 0, 0, CUI_CLAMP(CUI_MAX(lw, dw) + 34, 72.0f, 220.0f), CUI__GV_NODE_H + (nodes[v].detail ? CUI__GV_DETAIL : 0), layer[v] };
    }
    /* segments of the expanded graph: node -> waypoint -> ... -> node */
    size_t sn = (size_t)CUI_MAX(1, ne + waypoints);
    int *sa = cui__frame_alloc(ui, sizeof(int) * sn), *sb = cui__frame_alloc(ui, sizeof(int) * sn), ns = 0, wv = n, rp = 0;
    for (int k = 0; k < ne; k++) {
        s->from[k] = ef[k]; s->to[k] = et[k]; s->rstart[k] = rp; s->rlen[k] = 0;
        if (back[k]) { sa[ns] = ef[k]; sb[ns] = et[k]; ns++; continue; }   /* a cycle's edge is drawn, not used for order */
        int prev = ef[k];
        for (int L = layer[ef[k]] + 1; L < layer[et[k]]; L++) {
            s->box[wv] = (cui__gv_box){ 0, 0, top_down ? 10.0f : 0.0f, top_down ? 0.0f : 10.0f, L };
            s->route[rp++] = wv; s->rlen[k]++;
            sa[ns] = prev; sb[ns] = wv; ns++;
            prev = wv++;
        }
        sa[ns] = prev; sb[ns] = et[k]; ns++;
    }
    /* order within layers: by the mean position of neighbours, a few sweeps */
    size_t vv = (size_t)nv;
    float *pos = cui__frame_alloc(ui, sizeof(float) * vv), *sum = cui__frame_alloc(ui, sizeof(float) * vv);
    int *deg = cui__frame_alloc(ui, sizeof(int) * vv), *order = cui__frame_alloc(ui, sizeof(int) * vv);
    int *cnt = cui__frame_alloc(ui, sizeof(int) * (size_t)layers);
    memset(cnt, 0, sizeof(int) * (size_t)layers);
    for (int v = 0; v < nv; v++) pos[v] = (float)cnt[s->box[v].layer]++;
    for (int sweep = 0; sweep < 8; sweep++) {
        memset(sum, 0, sizeof(float) * vv); memset(deg, 0, sizeof(int) * vv);
        for (int k = 0; k < ns; k++) {
            if (s->box[sa[k]].layer == s->box[sb[k]].layer) continue;
            sum[sb[k]] += pos[sa[k]]; deg[sb[k]]++;
            sum[sa[k]] += pos[sb[k]]; deg[sa[k]]++;
        }
        for (int v = 0; v < nv; v++) if (deg[v]) pos[v] = pos[v] * 0.25f + 0.75f * sum[v] / deg[v];
        for (int L = 0; L < layers; L++) {
            int c = 0;
            for (int v = 0; v < nv; v++) if (s->box[v].layer == L) order[c++] = v;
            for (int a = 1; a < c; a++) { int x = order[a], b2 = a - 1; while (b2 >= 0 && pos[order[b2]] > pos[x]) { order[b2 + 1] = order[b2]; b2--; } order[b2 + 1] = x; }
            for (int r = 0; r < c; r++) pos[order[r]] = (float)r;
        }
    }
    /* coordinates: layers side by side (or stacked), each layer centered on the longest; a
     * waypoint spans its layer's thickness, so its edge runs straight across the layer */
    float *lsize = cui__frame_alloc(ui, sizeof(float) * (size_t)layers), *lthick = cui__frame_alloc(ui, sizeof(float) * (size_t)layers);
    float *lpos = cui__frame_alloc(ui, sizeof(float) * (size_t)layers);
    for (int L = 0; L < layers; L++) { lsize[L] = 0; lthick[L] = 0; }
    for (int v = 0; v < nv; v++) {
        int L = s->box[v].layer;
        float along = top_down ? s->box[v].w : s->box[v].h, across = top_down ? s->box[v].h : s->box[v].w;
        lsize[L] += along + CUI__GV_GAP_Y;
        lthick[L] = CUI_MAX(lthick[L], across);
    }
    float longest = 0, at = 0;
    for (int L = 0; L < layers; L++) { lsize[L] -= CUI__GV_GAP_Y; longest = CUI_MAX(longest, lsize[L]); }
    for (int L = 0; L < layers; L++) { lpos[L] = at; at += lthick[L] + CUI__GV_GAP_X * (top_down ? 0.6f : 1.0f); }
    for (int L = 0; L < layers; L++) {
        int c = 0;
        for (int v = 0; v < nv; v++) if (s->box[v].layer == L) order[c++] = v;
        for (int a = 1; a < c; a++) { int x = order[a], b2 = a - 1; while (b2 >= 0 && pos[order[b2]] > pos[x]) { order[b2 + 1] = order[b2]; b2--; } order[b2 + 1] = x; }
        float cur = (longest - lsize[L]) * 0.5f;
        for (int r = 0; r < c; r++) {
            cui__gv_box *bx = &s->box[order[r]];
            bool way = order[r] >= n;
            if (top_down) { if (way) bx->h = lthick[L]; bx->x = cur; bx->y = lpos[L] + (lthick[L] - bx->h) * 0.5f; cur += bx->w + CUI__GV_GAP_Y; }
            else { if (way) bx->w = lthick[L]; bx->y = cur; bx->x = lpos[L] + (lthick[L] - bx->w) * 0.5f; cur += bx->h + CUI__GV_GAP_Y; }
        }
    }
    for (int v = 0; v < nv; v++) { s->gw = CUI_MAX(s->gw, s->box[v].x + s->box[v].w); s->gh = CUI_MAX(s->gh, s->box[v].y + s->box[v].h); }
}

typedef struct {
    const cui__gv_state *s;
    const char **label, **detail;
    cui_tone *tone;
    uint8_t *lit;                     /* per node: 1 hovered or selected, 2 a neighbour */
    float px, py, zoom;
    int hover, sel;
    bool empty;
    float focus;
} cui__gv_paint;

/* where an edge leaves (out) or enters a box, on screen */
static cui_vec2 cui__gv_port(const cui__gv_paint *p, cui_rect r, int v, bool out) {
    const cui__gv_box *b = &p->s->box[v];
    bool td = p->s->top_down;
    float x = r.x + p->px + (b->x + (td ? b->w * 0.5f : out ? b->w : 0)) * p->zoom;
    float y = r.y + p->py + (b->y + (td ? (out ? b->h : 0) : b->h * 0.5f)) * p->zoom;
    return (cui_vec2){ x, y };
}

/* an S curve between two ports, along the layers' axis */
static int cui__gv_curve(cui_vec2 s, cui_vec2 e, bool td, float z, cui_vec2 *pts) {
    float bend = (td ? fabsf(e.y - s.y) : fabsf(e.x - s.x)) * 0.5f + 4 * z;
    cui_vec2 c1 = td ? (cui_vec2){ s.x, s.y + bend } : (cui_vec2){ s.x + bend, s.y };
    cui_vec2 c2 = td ? (cui_vec2){ e.x, e.y - bend } : (cui_vec2){ e.x - bend, e.y };
    for (int i = 0; i <= 12; i++) {
        float t = i / 12.0f, u = 1 - t;
        pts[i].x = u * u * u * s.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * e.x;
        pts[i].y = u * u * u * s.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * e.y;
    }
    return 13;
}

static void cui__paint_graph_view(cui_ctx *ui, cui_node *n, void *data) {
    cui__gv_paint *p = data;
    const cui__gv_state *s = p->s;
    cui_rect r = n->rect;
    float z = p->zoom;
    bool td = s->top_down;
    cui_push_clip(ui, cui__rect_inset(r, 1));
    bool any_lit = p->hover >= 0 || p->sel >= 0;
    cui_color edge = CUI_C(ui, BORDER_STRONG), lit_edge = cui__emphasis(ui);
    /* edges: S curves from box to box through the waypoints, straight across each waypoint, then an arrowhead */
    for (int pass = 0; pass < 2; pass++)       /* lit edges last, so they sit on top */
    for (int k = 0; k < s->ne; k++) {
        int a = s->from[k], b = s->to[k];
        bool lit = any_lit && ((p->lit[a] == 1 && p->lit[b]) || (p->lit[b] == 1 && p->lit[a]));
        if (lit != (pass == 1)) continue;
        cui_color c = lit ? lit_edge : any_lit ? cui_alpha(edge, 0.45f) : edge;
        float th = lit ? 2.0f : 1.25f;
        int prev = a;
        cui_vec2 pts[13];
        for (int w = 0; w <= s->rlen[k]; w++) {
            int next = w < s->rlen[k] ? s->route[s->rstart[k] + w] : b;
            cui_vec2 s0 = cui__gv_port(p, r, prev, true), e0 = cui__gv_port(p, r, next, false);
            int np = cui__gv_curve(s0, e0, td, z, pts);
            cui_draw_polyline(ui, pts, np, th, c, false);
            if (next != b) cui_draw_line(ui, e0, cui__gv_port(p, r, next, true), th, c);
            prev = next;
        }
        cui_vec2 e = cui__gv_port(p, r, b, false);
        float ah = 6 * CUI_MIN(1.0f, z + 0.2f);
        cui_vec2 tri[3];
        if (td) { tri[0] = e; tri[1] = (cui_vec2){ e.x - ah * 0.6f, e.y - ah }; tri[2] = (cui_vec2){ e.x + ah * 0.6f, e.y - ah }; }
        else { tri[0] = e; tri[1] = (cui_vec2){ e.x - ah, e.y - ah * 0.6f }; tri[2] = (cui_vec2){ e.x - ah, e.y + ah * 0.6f }; }
        cui_draw_polygon(ui, tri, 3, c);
    }
    /* nodes */
    float fs = cui__fs(ui, ui->theme.text_sm) * z, fx = cui__fs(ui, ui->theme.text_xs) * z;
    float lh = cui__font_line_height(ui, CUI_FONT_MEDIUM, fs);
    for (int v = 0; v < s->n; v++) {
        const cui__gv_box *b = &s->box[v];
        cui_rect br = { r.x + p->px + b->x * z, r.y + p->py + b->y * z, b->w * z, b->h * z };
        if (br.x > r.x + r.w || br.y > r.y + r.h || br.x + br.w < r.x || br.y + br.h < r.y) continue;
        bool dim = any_lit && !p->lit[v];
        cui_color tone = p->tone[v] ? cui__tone_color(ui, p->tone[v]) : CUI_C(ui, TEXT_MUTED);
        cui_color fill = v == p->hover ? cui_mix(CUI_C(ui, SURFACE), CUI_C(ui, HOVER), 1.0f) : CUI_C(ui, SURFACE);
        cui_color border = v == p->sel ? lit_edge : p->lit[v] == 2 ? cui_alpha(lit_edge, 0.6f) : CUI_C(ui, BORDER);
        if (dim) fill = cui_mix(fill, CUI_C(ui, BACKGROUND), 0.5f);
        cui_draw_rect_border(ui, br, CUI_CORNERS(6 * z), fill, border, v == p->sel ? 2.0f : 1.0f);
        cui_draw_circle(ui, (cui_vec2){ br.x + 12 * z, br.y + (p->detail[v] ? 17 * z : br.h * 0.5f) }, 3.5f * z, dim ? cui_alpha(tone, 0.4f) : tone);
        if (fs >= 4) {
            cui_color tc = dim ? CUI_C(ui, TEXT_MUTED) : CUI_C(ui, FOREGROUND);
            float ty = p->detail[v] ? br.y + 17 * z - lh * 0.5f : br.y + (br.h - lh) * 0.5f;
            cui__draw_text_ellipsis(ui, CUI_FONT_MEDIUM, fs, br.x + 22 * z, ty, br.w - 25 * z, tc, p->label[v], -1, false);
            if (p->detail[v] && fx >= 4)
                cui__draw_text_ellipsis(ui, CUI_FONT_BODY, fx, br.x + 22 * z, ty + lh, br.w - 25 * z, CUI_C(ui, TEXT_MUTED), p->detail[v], -1, false);
        }
    }
    if (p->empty) {
        const char *msg = "No nodes";
        float tfs = cui__fs(ui, ui->theme.text_sm);
        float lw = cui__text_width(ui, CUI_FONT_BODY, tfs, msg, -1, false);
        cui__draw_text_line(ui, CUI_FONT_BODY, tfs, r.x + (r.w - lw) * 0.5f, r.y + r.h * 0.5f - 8, CUI_C(ui, TEXT_MUTED), msg, -1, false);
    }
    cui_pop_clip(ui);
    if (p->focus > 0.01f) cui__draw_focus_ring(ui, r, CUI_CORNERS(ui->theme.radius_panel), p->focus);
}

/* the wheel goes to the graph while the pointer is over its visible part */
static void cui__paint_gv_wheel(cui_ctx *ui, cui_node *n, void *data) { cui__paint_wheel_hit(ui, *(cui_id *)data, n->rect); }

static int cui__gv_hit(const cui__gv_state *s, cui_rect R, float mx, float my) {
    for (int v = s->n - 1; v >= 0; v--) {
        const cui__gv_box *b = &s->box[v];
        float x = R.x + s->px + b->x * s->zoom, y = R.y + s->py + b->y * s->zoom;
        if (mx >= x && mx <= x + b->w * s->zoom && my >= y && my <= y + b->h * s->zoom) return v;
    }
    return -1;
}

static void cui__gv_fit(cui__gv_state *s, cui_rect R) {
    if (s->gw <= 0 || s->gh <= 0 || R.w <= 0) { s->zoom = 1; s->px = s->py = CUI__GV_PAD; return; }
    float z = CUI_MIN((R.w - 2 * CUI__GV_PAD) / s->gw, (R.h - 2 * CUI__GV_PAD) / s->gh);
    s->zoom = CUI_CLAMP(z, 0.2f, 1.0f);
    s->px = (R.w - s->gw * s->zoom) * 0.5f;
    s->py = (R.h - s->gh * s->zoom) * 0.5f;
}

static void cui__gv_zoom_at(cui__gv_state *s, float nz, float x, float y) {   /* x, y from the panel's corner */
    nz = CUI_CLAMP(nz, 0.2f, 2.5f);
    float gx = (x - s->px) / s->zoom, gy = (y - s->py) / s->zoom;
    s->zoom = nz;
    s->px = x - gx * nz; s->py = y - gy * nz;
}

/* arrows move the selection: along edges (Right and Left, or Down and Up when top down), or across a layer */
static int cui__gv_step(const cui__gv_state *s, const cui_graph_edge *edges, int ne, int sel, int dx, int dy) {
    if (sel < 0 || sel >= s->n) return s->n > 0 ? 0 : -1;
    bool along = s->top_down ? dy != 0 : dx != 0;
    int dir = s->top_down ? dy : dx;
    int best = -1;
    float bd = 1e30f;
    const cui__gv_box *a = &s->box[sel];
    if (along) {
        for (int k = 0; k < ne; k++) {
            int o = dir > 0 ? (edges[k].from == sel ? edges[k].to : -1) : (edges[k].to == sel ? edges[k].from : -1);
            if (o < 0 || o >= s->n) continue;
            float d = s->top_down ? fabsf(s->box[o].x - a->x) : fabsf(s->box[o].y - a->y);
            if (d < bd) { bd = d; best = o; }
        }
        return best >= 0 ? best : sel;
    }
    int across = s->top_down ? dx : dy;
    for (int v = 0; v < s->n; v++) {
        if (v == sel || s->box[v].layer != a->layer) continue;
        float d = s->top_down ? s->box[v].x - a->x : s->box[v].y - a->y;
        if ((across > 0 && d > 0 && d < bd) || (across < 0 && d < 0 && -d < bd)) { bd = fabsf(d); best = v; }
    }
    return best >= 0 ? best : sel;
}

cui_graph_result cui_graph_view_ex(cui_ctx *ui, const char *id_str, const cui_graph_node *nodes, int node_count,
                                   const cui_graph_edge *edges, int edge_count, const cui_graph_opts *o) {
    cui_graph_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui_graph_result res = { -1, -1, -1 };
    if (!nodes || node_count < 0) node_count = 0;
    if (!edges || edge_count < 0) edge_count = 0;
    cui_id id = cui_make_id(ui, id_str), wid = id ^ 0x6A90u;
    cui__gv_state *s = cui_state(ui, id ^ 0x6A9Fu, sizeof *s);
    cui__state_set_dtor(ui, id ^ 0x6A9Fu, cui__gv_dtor);
    if (!s->init) { s->init = true; s->own_sel = -1; s->zoom = 1; s->fit = true; }
    int *sel = o->selected ? o->selected : &s->own_sel;
    if (*sel >= node_count) *sel = -1;
    uint32_t key = cui__gv_key(nodes, node_count, edges, edge_count, o->top_down);
    if (key != s->key || s->n != node_count) { cui__gv_layout(ui, s, nodes, node_count, edges, edge_count, o->top_down); s->key = key; s->fit = true; }
    bool has_rect = cui__has_prev_rect(ui, id);
    cui_rect R = cui__prev_rect(ui, id);
    if (s->fit && has_rect && R.w > 0) { cui__gv_fit(s, R); s->fit = false; }
    if (!has_rect) ui->animating = true;   /* fit once the size is known */
    cui_interact bi = cui__interact(ui, id, CUI_IF_FOCUSABLE | CUI_IF_DRAG);
    float mx = ui->mx - R.x, my = ui->my - R.y;
    if (has_rect) {
        if (ui->scroll_target == wid && (ui->scroll_dy != 0 || ui->scroll_dx != 0)) {
            if (ui->scroll_dx != 0 || (ui->mods & CUI_MOD_SHIFT)) { s->px += (ui->scroll_dx != 0 ? ui->scroll_dx : ui->scroll_dy) * 40; }
            else cui__gv_zoom_at(s, s->zoom * powf(1.12f, ui->scroll_dy), mx, my);
            ui->scroll_dx = ui->scroll_dy = 0;
        }
        if (bi.hovered && ui->mpressed[0] && s->mode == 0) {
            s->mode = 1; s->press_x = s->last_x = ui->mx; s->press_y = s->last_y = ui->my;
            s->press_node = cui__gv_hit(s, R, ui->mx, ui->my);
        }
        if (s->mode == 1 && ui->mdown[0]) {
            float dx = ui->mx - s->press_x, dy = ui->my - s->press_y;
            if (dx * dx + dy * dy > CUI__GV_DRAG * CUI__GV_DRAG) s->mode = 2;
        }
        if (s->mode == 2) {
            s->px += ui->mx - s->last_x; s->py += ui->my - s->last_y;
            s->last_x = ui->mx; s->last_y = ui->my;
            cui__set_cursor(ui, CUI_CURSOR_MOVE);
        }
        if (s->mode && !ui->mdown[0]) {
            if (s->mode == 1) {
                *sel = s->press_node;
                if (s->press_node >= 0) { res.clicked = s->press_node; if (bi.double_clicked || ui->click_count >= 2) res.activated = s->press_node; }
            }
            s->mode = 0;
        }
        if (bi.focused) {
            int to = *sel;
            if (cui_key_pressed(ui, CUI_KEY_RIGHT)) to = cui__gv_step(s, edges, edge_count, to, 1, 0);
            if (cui_key_pressed(ui, CUI_KEY_LEFT)) to = cui__gv_step(s, edges, edge_count, to, -1, 0);
            if (cui_key_pressed(ui, CUI_KEY_DOWN)) to = cui__gv_step(s, edges, edge_count, to, 0, 1);
            if (cui_key_pressed(ui, CUI_KEY_UP)) to = cui__gv_step(s, edges, edge_count, to, 0, -1);
            if (to != *sel && to >= 0) {
                *sel = to;
                /* keep it in view */
                const cui__gv_box *b = &s->box[to];
                float x = s->px + b->x * s->zoom, y = s->py + b->y * s->zoom, w = b->w * s->zoom, h = b->h * s->zoom;
                if (x < 8) s->px += 8 - x;
                if (x + w > R.w - 8) s->px -= x + w - (R.w - 8);
                if (y < 8) s->py += 8 - y;
                if (y + h > R.h - 8) s->py -= y + h - (R.h - 8);
            }
            if (cui_key_pressed(ui, CUI_KEY_ENTER) && *sel >= 0) res.activated = *sel;
            if (cui_key_pressed(ui, CUI_KEY_EQUAL)) cui__gv_zoom_at(s, s->zoom * 1.25f, R.w * 0.5f, R.h * 0.5f);
            if (cui_key_pressed(ui, CUI_KEY_MINUS)) cui__gv_zoom_at(s, s->zoom / 1.25f, R.w * 0.5f, R.h * 0.5f);
            if (cui_key_pressed(ui, CUI_KEY_F) && !(ui->mods & CUI_MOD_PRIMARY)) cui__gv_fit(s, R);
            if (cui_key_pressed(ui, CUI_KEY_ESCAPE)) *sel = -1;
            ui->kpressed[CUI_KEY_LEFT] = ui->kpressed[CUI_KEY_RIGHT] = ui->kpressed[CUI_KEY_UP] = ui->kpressed[CUI_KEY_DOWN] = 0;
        }
    }
    int hover = has_rect && bi.hovered && s->mode != 2 ? cui__gv_hit(s, R, ui->mx, ui->my) : -1;
    res.hovered = hover;
    if (hover >= 0) cui__set_cursor(ui, CUI_CURSOR_HAND);
    /* the panel and what it draws, copied into frame memory */
    int en = cui__node_new(ui, id);
    cui_node *nd = &ui->nodes[en];
    nd->w = o->width.kind ? o->width : CUI_FILL;
    nd->h = CUI_PX(o->height > 0 ? o->height : 360);
    nd->bg = ui->mode == CUI_THEME_DARK ? cui_mix(CUI_C(ui, BACKGROUND), CUI_C(ui, SURFACE), 0.45f) : CUI_C(ui, SURFACE_MUTED);
    nd->border = 1; nd->border_color = CUI_C(ui, BORDER);
    nd->radius = CUI_CORNERS(ui->theme.radius_panel);
    nd->flags |= CUI_NF_CLIP;
    nd->direction = CUI_DIRECTION_LTR;
    cui__mark_interactive(ui, en, CUI_IF_FOCUSABLE | CUI_IF_DRAG);
    cui__gv_paint *p = cui__frame_alloc(ui, sizeof *p);
    memset(p, 0, sizeof *p);
    size_t nn = (size_t)CUI_MAX(1, s->n);
    p->s = s;
    p->label = cui__frame_alloc(ui, sizeof(char *) * nn);
    p->detail = cui__frame_alloc(ui, sizeof(char *) * nn);
    p->tone = cui__frame_alloc(ui, sizeof(cui_tone) * nn);
    p->lit = cui__frame_alloc(ui, nn);
    memset(p->lit, 0, nn);
    for (int v = 0; v < s->n; v++) {
        p->label[v] = cui__frame_strdup(ui, nodes[v].label ? nodes[v].label : "", -1);
        p->detail[v] = nodes[v].detail ? cui__frame_strdup(ui, nodes[v].detail, -1) : NULL;
        p->tone[v] = nodes[v].tone;
    }
    int focus_node = hover >= 0 ? hover : *sel;
    if (focus_node >= 0 && focus_node < s->n) {
        p->lit[focus_node] = 1;
        for (int k = 0; k < s->ne; k++) {
            if (s->from[k] == focus_node && !p->lit[s->to[k]]) p->lit[s->to[k]] = 2;
            if (s->to[k] == focus_node && !p->lit[s->from[k]]) p->lit[s->from[k]] = 2;
        }
    }
    p->px = s->px; p->py = s->py; p->zoom = s->zoom;
    p->hover = hover; p->sel = *sel; p->empty = s->n == 0;
    cui__tween(ui, &s->focus, bi.focus_visible ? 1.0f : 0.0f, CUI__T_FOCUS);
    p->focus = s->focus;
    nd->paint = cui__paint_graph_view; nd->paint_data = p;
    /* the wheel goes to the graph while the pointer is over it */
    cui__open(ui, en);
    int wheel = cui__node_new(ui, 0);
    ui->nodes[wheel].w = CUI_FILL; ui->nodes[wheel].h = CUI_FILL;
    ui->nodes[wheel].paint = cui__paint_gv_wheel;
    cui_id *wp = cui__frame_alloc(ui, sizeof *wp);
    *wp = wid;
    ui->nodes[wheel].paint_data = wp;
    if (hover >= 0 && ui->active == 0 && nodes[hover].detail) {
        const cui__gv_box *b = &s->box[hover];
        cui_id tid = id ^ (0x7117u * (cui_id)(hover + 1));
        cui__absolute(ui, tid, s->px + b->x * s->zoom, s->py + b->y * s->zoom, b->w * s->zoom, b->h * s->zoom);
        cui__tooltip_passive(ui, tid, cui__frame_printf(ui, "%s\n%s", nodes[hover].label ? nodes[hover].label : "", nodes[hover].detail));
    }
    cui__close(ui);
    ui->last_id = id;
    return res;
}
