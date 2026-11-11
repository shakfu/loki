/* A small backtracking regular expression engine for the regex tester, and for
 * apps that want one (cui_regex_search). The syntax is the common subset of
 * JavaScript, PCRE and Python: literals and escapes, . [] [^] with ranges,
 * \d \w \s and their negations, \b \B, ^ $, groups ( ) and (?: ), named groups
 * (?<name> ), alternation |, and the quantifiers * + ? {n} {n,} {n,m} with a
 * lazy ? after them. Flags: ignore case (ASCII), multi-line ^ $, and dot
 * matches newline. Text is bytes; . steps over a whole UTF-8 character.
 *
 * A pattern compiles into a small tree held on the stack, then matches by
 * recursion with continuations, so every construct backtracks correctly. A
 * repeat of a single character class is matched in a loop, which keeps the
 * recursion shallow for the common .* and \w+. A step budget stops patterns
 * that would backtrack for ever. */
#include "cui_internal.h"

#define CUI__RX_NODES   512
#define CUI__RX_KIDS    1024
#define CUI__RX_SETS    96
#define CUI__RX_STEPS   2000000
#define CUI__RX_DEPTH   2400

enum { RX_CHAR, RX_ANY, RX_SET, RX_BOL, RX_EOL, RX_WORDB, RX_NWORDB, RX_SEQ, RX_ALT, RX_GROUP, RX_REPEAT };

typedef struct {
    uint8_t type, lazy;
    int16_t group;              /* RX_GROUP: capture index, or -1 for (?: ) */
    int16_t set;                /* RX_SET: index of its byte set */
    int ch;                     /* RX_CHAR: the byte */
    int min, max;               /* RX_REPEAT: max -1 for no limit */
    int kid, nkid;              /* children: a run in the kid pool */
} cui__rx_node;

typedef struct {
    cui__rx_node node[CUI__RX_NODES]; int nn;
    int kid[CUI__RX_KIDS]; int nk;
    uint8_t set[CUI__RX_SETS][32]; int ns;
    int groups;                 /* captures, not counting the whole match */
    char names[CUI_REGEX_GROUPS][24];
    int flags;
    const char *p, *end;        /* the pattern while parsing */
    const char *error;
    int root;
} cui__rx;

/* ---- parsing ---- */

static int cui__rx_new(cui__rx *R, int type) {
    if (R->nn >= CUI__RX_NODES) { R->error = "Pattern is too long"; return -1; }
    cui__rx_node *n = &R->node[R->nn];
    memset(n, 0, sizeof *n);
    n->type = (uint8_t)type; n->group = -1; n->set = -1;
    return R->nn++;
}
static int cui__rx_newset(cui__rx *R) {
    if (R->ns >= CUI__RX_SETS) { R->error = "Too many character classes"; return -1; }
    memset(R->set[R->ns], 0, 32);
    return R->ns++;
}
static void cui__rx_add(uint8_t *s, int c) { s[(c & 255) >> 3] |= (uint8_t)(1u << (c & 7)); }
static bool cui__rx_has(const uint8_t *s, int c) { return (s[(c & 255) >> 3] >> (c & 7)) & 1; }
static void cui__rx_range(uint8_t *s, int a, int b) { for (int c = a; c <= b; c++) cui__rx_add(s, c); }
static bool cui__rx_word(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

/* \d \w \s and friends into a set; false when `c` is not one of them */
static bool cui__rx_class_escape(uint8_t *s, int c) {
    uint8_t t[32];
    memset(t, 0, sizeof t);
    switch (c | 32) {
    case 'd': cui__rx_range(t, '0', '9'); break;
    case 'w': cui__rx_range(t, 'a', 'z'); cui__rx_range(t, 'A', 'Z'); cui__rx_range(t, '0', '9'); cui__rx_add(t, '_'); break;
    case 's': cui__rx_add(t, ' '); cui__rx_range(t, '\t', '\r'); break;
    default: return false;
    }
    bool neg = c >= 'A' && c <= 'Z';
    for (int i = 0; i < 32; i++) s[i] |= neg ? (uint8_t)~t[i] : t[i];
    return true;
}

static int cui__rx_hex(int c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; }

/* the byte an escape stands for (\n, \x41, \.), or -1 for a class escape */
static int cui__rx_escape(cui__rx *R) {
    if (R->p >= R->end) { R->error = "Pattern ends with a backslash"; return -2; }
    int c = (unsigned char)*R->p++;
    switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'f': return '\f';
    case 'v': return '\v';
    case '0': return 0;
    case 'x': {
        int h = R->p + 1 < R->end ? cui__rx_hex((unsigned char)R->p[0]) : -1, l = h >= 0 ? cui__rx_hex((unsigned char)R->p[1]) : -1;
        if (h < 0 || l < 0) { R->error = "\\x needs two hex digits"; return -2; }
        R->p += 2;
        return h * 16 + l;
    }
    default:
        if ((c | 32) == 'd' || (c | 32) == 'w' || (c | 32) == 's') { R->p--; return -1; }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '1' && c <= '9')) {
            if (c != 'b' && c != 'B') { R->error = "Unknown escape"; return -2; }
        }
        return c;
    }
}

static int cui__rx_alt(cui__rx *R);

static int cui__rx_class(cui__rx *R) {
    int si = cui__rx_newset(R);
    if (si < 0) return -1;
    uint8_t *s = R->set[si];
    bool neg = R->p < R->end && *R->p == '^';
    if (neg) R->p++;
    bool first = true;
    while (R->p < R->end && (*R->p != ']' || first)) {
        first = false;
        int a = (unsigned char)*R->p++;
        if (a == '\\') {
            a = cui__rx_escape(R);
            if (a == -2) return -1;
            if (a == -1) { cui__rx_class_escape(s, (unsigned char)*R->p++); continue; }
        }
        if (R->p + 1 < R->end && *R->p == '-' && R->p[1] != ']') {
            R->p++;
            int b = (unsigned char)*R->p++;
            if (b == '\\') {
                b = cui__rx_escape(R);
                if (b < 0) { if (b == -1) R->error = "A range cannot end in a class"; return -1; }
            }
            if (b < a) { R->error = "Range out of order"; return -1; }
            cui__rx_range(s, a, b);
        } else cui__rx_add(s, a);
    }
    if (R->p >= R->end) { R->error = "Missing ]"; return -1; }
    R->p++;
    if (R->flags & CUI_REGEX_ICASE)
        for (int c = 'a'; c <= 'z'; c++)
            if (cui__rx_has(s, c) || cui__rx_has(s, c - 32)) { cui__rx_add(s, c); cui__rx_add(s, c - 32); }
    if (neg) for (int i = 0; i < 32; i++) s[i] = (uint8_t)~s[i];
    int n = cui__rx_new(R, RX_SET);
    if (n >= 0) R->node[n].set = (int16_t)si;
    return n;
}

/* a literal byte: a two-letter set when case is ignored */
static int cui__rx_literal(cui__rx *R, int c) {
    if ((R->flags & CUI_REGEX_ICASE) && ((c | 32) >= 'a' && (c | 32) <= 'z')) {
        int si = cui__rx_newset(R);
        if (si < 0) return -1;
        cui__rx_add(R->set[si], c | 32); cui__rx_add(R->set[si], (c | 32) - 32);
        int n = cui__rx_new(R, RX_SET);
        if (n >= 0) R->node[n].set = (int16_t)si;
        return n;
    }
    int n = cui__rx_new(R, RX_CHAR);
    if (n >= 0) R->node[n].ch = c;
    return n;
}

static int cui__rx_atom(cui__rx *R) {
    int c = (unsigned char)*R->p++;
    switch (c) {
    case '(': {
        int g = -1;
        if (R->p < R->end && *R->p == '?') {
            if (R->p + 1 < R->end && R->p[1] == ':') R->p += 2;
            else if (R->p + 1 < R->end && (R->p[1] == '<' || R->p[1] == 'P') && R->p + 2 < R->end && R->p[2] != '=' && R->p[2] != '!') {
                R->p += R->p[1] == 'P' ? 3 : 2;   /* (?<name> or (?P<name> */
                const char *nm = R->p;
                while (R->p < R->end && *R->p != '>') R->p++;
                if (R->p >= R->end || R->p == nm) { R->error = "Group name needs a closing >"; return -1; }
                if (R->groups >= CUI_REGEX_GROUPS - 1) { R->error = "Too many groups"; return -1; }
                g = ++R->groups;
                int len = (int)CUI_MIN((size_t)(R->p - nm), sizeof R->names[0] - 1);
                memcpy(R->names[g], nm, (size_t)len); R->names[g][len] = 0;
                R->p++;
            } else { R->error = "Lookaround is not supported"; return -1; }
        } else {
            if (R->groups >= CUI_REGEX_GROUPS - 1) { R->error = "Too many groups"; return -1; }
            g = ++R->groups;
        }
        int body = cui__rx_alt(R);
        if (body < 0) return -1;
        if (R->p >= R->end || *R->p != ')') { R->error = "Missing )"; return -1; }
        R->p++;
        int n = cui__rx_new(R, RX_GROUP);
        if (n < 0 || R->nk >= CUI__RX_KIDS) { if (!R->error) R->error = "Pattern is too long"; return -1; }
        R->node[n].group = (int16_t)g; R->node[n].kid = R->nk; R->node[n].nkid = 1;
        R->kid[R->nk++] = body;
        return n;
    }
    case '[': return cui__rx_class(R);
    case '.': return cui__rx_new(R, RX_ANY);
    case '^': return cui__rx_new(R, RX_BOL);
    case '$': return cui__rx_new(R, RX_EOL);
    case ')': R->error = "Unmatched )"; return -1;
    case '*': case '+': case '?': R->error = "Nothing to repeat"; return -1;
    case '\\': {
        if (R->p < R->end && (*R->p == 'b' || *R->p == 'B')) return cui__rx_new(R, *R->p++ == 'b' ? RX_WORDB : RX_NWORDB);
        int e = cui__rx_escape(R);
        if (e == -2) return -1;
        if (e == -1) {
            int si = cui__rx_newset(R);
            if (si < 0) return -1;
            cui__rx_class_escape(R->set[si], (unsigned char)*R->p++);
            int n = cui__rx_new(R, RX_SET);
            if (n >= 0) R->node[n].set = (int16_t)si;
            return n;
        }
        return cui__rx_literal(R, e);
    }
    default: return cui__rx_literal(R, c);
    }
}

static bool cui__rx_num(cui__rx *R, int *v) {
    if (R->p >= R->end || *R->p < '0' || *R->p > '9') return false;
    int x = 0;
    while (R->p < R->end && *R->p >= '0' && *R->p <= '9') { x = x * 10 + (*R->p++ - '0'); if (x > 100000) x = 100000; }
    *v = x;
    return true;
}

static int cui__rx_quant(cui__rx *R) {
    int a = cui__rx_atom(R);
    if (a < 0) return -1;
    while (R->p < R->end) {
        int mn, mx;
        const char *save = R->p;
        char q = *R->p;
        if (q == '*') { mn = 0; mx = -1; R->p++; }
        else if (q == '+') { mn = 1; mx = -1; R->p++; }
        else if (q == '?') { mn = 0; mx = 1; R->p++; }
        else if (q == '{') {
            R->p++;
            if (!cui__rx_num(R, &mn)) { R->p = save; break; }   /* a literal { */
            mx = mn;
            if (R->p < R->end && *R->p == ',') { R->p++; if (!cui__rx_num(R, &mx)) mx = -1; }
            if (R->p >= R->end || *R->p != '}') { R->p = save; break; }
            R->p++;
            if (mx >= 0 && mx < mn) { R->error = "Repeat range out of order"; return -1; }
        } else break;
        uint8_t t = R->node[a].type;
        if (t == RX_BOL || t == RX_EOL || t == RX_WORDB || t == RX_NWORDB || t == RX_REPEAT) { R->error = "Nothing to repeat"; return -1; }
        int n = cui__rx_new(R, RX_REPEAT);
        if (n < 0 || R->nk >= CUI__RX_KIDS) { if (!R->error) R->error = "Pattern is too long"; return -1; }
        R->node[n].min = mn; R->node[n].max = mx;
        if (R->p < R->end && *R->p == '?') { R->node[n].lazy = 1; R->p++; }
        else if (R->p < R->end && *R->p == '+') { R->error = "Possessive repeats are not supported"; return -1; }
        R->node[n].kid = R->nk; R->node[n].nkid = 1;
        R->kid[R->nk++] = a;
        a = n;
    }
    return a;
}

/* a sequence: atoms until | or ) or the end; its kids are contiguous in the pool */
static int cui__rx_seq(cui__rx *R) {
    int items[256], ni = 0;
    while (R->p < R->end && *R->p != '|' && *R->p != ')') {
        int q = cui__rx_quant(R);
        if (q < 0) return -1;
        if (ni >= 256) { R->error = "Pattern is too long"; return -1; }
        items[ni++] = q;
    }
    int n = cui__rx_new(R, RX_SEQ);
    if (n < 0 || R->nk + ni > CUI__RX_KIDS) { if (!R->error) R->error = "Pattern is too long"; return -1; }
    R->node[n].kid = R->nk; R->node[n].nkid = ni;
    memcpy(R->kid + R->nk, items, sizeof(int) * (size_t)ni);
    R->nk += ni;
    return n;
}

static int cui__rx_alt(cui__rx *R) {
    int alts[64], na = 0;
    for (;;) {
        int s = cui__rx_seq(R);
        if (s < 0) return -1;
        if (na >= 64) { R->error = "Too many alternatives"; return -1; }
        alts[na++] = s;
        if (R->p < R->end && *R->p == '|') { R->p++; continue; }
        break;
    }
    if (na == 1) return alts[0];
    int n = cui__rx_new(R, RX_ALT);
    if (n < 0 || R->nk + na > CUI__RX_KIDS) { if (!R->error) R->error = "Pattern is too long"; return -1; }
    R->node[n].kid = R->nk; R->node[n].nkid = na;
    memcpy(R->kid + R->nk, alts, sizeof(int) * (size_t)na);
    R->nk += na;
    return n;
}

static bool cui__rx_compile(cui__rx *R, const char *pattern, int flags) {
    memset(R->names, 0, sizeof R->names);
    R->nn = R->nk = R->ns = R->groups = 0;
    R->flags = flags;
    R->error = NULL;
    R->p = pattern; R->end = pattern + strlen(pattern);
    R->root = cui__rx_alt(R);
    if (R->root >= 0 && R->p < R->end) { R->error = "Unmatched )"; R->root = -1; }
    if (R->root < 0 && !R->error) R->error = "Invalid pattern";
    return R->root >= 0;
}

/* ---- matching ---- */

/* what to match after the current item: the rest of a sequence, another turn of a repeat,
 * or the end of a group */
enum { RK_SEQ, RK_REP, RK_GROUP_END };
typedef struct cui__rx_k {
    int type;
    int node;                   /* RK_SEQ: the sequence; RK_REP: the repeat; RK_GROUP_END: the group */
    int index;                  /* RK_SEQ: next item; RK_REP: turns so far */
    int from;                   /* RK_REP: where this turn started, to stop empty loops */
    const struct cui__rx_k *next;
} cui__rx_k;

typedef struct {
    const cui__rx *R;
    const char *s; int len;
    int cap[CUI_REGEX_GROUPS][2];
    long steps; int depth;
    bool abort;
    int end;
} cui__rx_m;

static int cui__rx_match(cui__rx_m *M, int node, int i, const cui__rx_k *k);
static int cui__rx_cont(cui__rx_m *M, int i, const cui__rx_k *k);


/* one character at i for a single-character node: the byte count it takes, or 0 */
static int cui__rx_one(const cui__rx_m *M, const cui__rx_node *n, int i) {
    if (i >= M->len) return 0;
    int c = (unsigned char)M->s[i];
    switch (n->type) {
    case RX_CHAR: return c == n->ch ? 1 : 0;
    case RX_SET: return cui__rx_has(M->R->set[n->set], c) ? 1 : 0;
    case RX_ANY: {
        if (c == '\n' && !(M->R->flags & CUI_REGEX_DOTALL)) return 0;
        int j = i + 1;
        if (c >= 0xC0) while (j < M->len && ((unsigned char)M->s[j] & 0xC0) == 0x80) j++;
        return j - i;
    }
    default: return 0;
    }
}
static bool cui__rx_single(const cui__rx_node *n) { return n->type == RX_CHAR || n->type == RX_SET || n->type == RX_ANY; }

static bool cui__rx_budget(cui__rx_m *M) {
    if (M->abort) return false;
    if (++M->steps > CUI__RX_STEPS || M->depth > CUI__RX_DEPTH) { M->abort = true; return false; }
    return true;
}

static int cui__rx_cont(cui__rx_m *M, int i, const cui__rx_k *k) {
    if (!k) { M->end = i; return 1; }
    if (!cui__rx_budget(M)) return 0;
    const cui__rx *R = M->R;
    M->depth++;
    int r = 0;
    switch (k->type) {
    case RK_SEQ: {
        const cui__rx_node *sq = &R->node[k->node];
        if (k->index >= sq->nkid) { r = cui__rx_cont(M, i, k->next); break; }
        cui__rx_k nk = { RK_SEQ, k->node, k->index + 1, 0, k->next };
        r = cui__rx_match(M, R->kid[sq->kid + k->index], i, &nk);
        break;
    }
    case RK_REP: {
        const cui__rx_node *rp = &R->node[k->node];
        int child = R->kid[rp->kid], turns = k->index;
        if (i == k->from && turns > rp->min) { r = 0; break; }   /* an empty turn would loop for ever */
        cui__rx_k again = { RK_REP, k->node, turns + 1, i, k->next };
        bool more = rp->max < 0 || turns < rp->max, done = turns >= rp->min;
        if (rp->lazy) {
            r = done ? cui__rx_cont(M, i, k->next) : 0;
            if (!r && more && !M->abort) r = cui__rx_match(M, child, i, &again);
        } else {
            r = more ? cui__rx_match(M, child, i, &again) : 0;
            if (!r && done && !M->abort) r = cui__rx_cont(M, i, k->next);
        }
        break;
    }
    case RK_GROUP_END: {
        int g = R->node[k->node].group;
        int old = M->cap[g][1];
        M->cap[g][1] = i;
        r = cui__rx_cont(M, i, k->next);
        if (!r) M->cap[g][1] = old;
        break;
    }
    }
    M->depth--;
    return r;
}

/* a repeated single character: take as many as allowed and give back one at a time (greedy),
 * or take one more at a time (lazy), without recursing per character */
static int cui__rx_rep_single(cui__rx_m *M, const cui__rx_node *n, const cui__rx_node *c, int i, const cui__rx_k *k) {
    int lim = n->max < 0 ? 1 << 30 : n->max, count = 0, j = i, r = 0;
    M->depth += 4;   /* this frame is bigger than the others */
    if (n->lazy) {
        for (;;) {
            if (count >= n->min && (r = cui__rx_cont(M, j, k)) != 0) break;
            if (M->abort || count >= lim) break;
            int w = cui__rx_one(M, c, j);
            if (!w) break;
            j += w; count++;
            if (!cui__rx_budget(M)) break;
        }
        M->depth -= 4;
        return r;
    }
    while (count < lim) {   /* take as many as allowed */
        int w = cui__rx_one(M, c, j);
        if (!w) break;
        j += w; count++;
        if ((count & 1023) == 0 && !cui__rx_budget(M)) break;
    }
    for (; count >= n->min && !M->abort; count--) {   /* then give back one character at a time */
        if ((r = cui__rx_cont(M, j, k)) != 0) break;
        if (!cui__rx_budget(M) || count == 0) break;
        j--;
        while (j > i && ((unsigned char)M->s[j] & 0xC0) == 0x80) j--;   /* . takes whole characters */
    }
    M->depth -= 4;
    return r;
}

static int cui__rx_match(cui__rx_m *M, int node, int i, const cui__rx_k *k) {
    if (!cui__rx_budget(M)) return 0;
    const cui__rx *R = M->R;
    const cui__rx_node *n = &R->node[node];
    bool ml = (R->flags & CUI_REGEX_MULTILINE) != 0;
    M->depth++;
    int r = 0;
    switch (n->type) {
    case RX_CHAR: case RX_SET: case RX_ANY: {
        int w = cui__rx_one(M, n, i);
        r = w ? cui__rx_cont(M, i + w, k) : 0;
        break;
    }
    case RX_BOL: r = (i == 0 || (ml && M->s[i - 1] == '\n')) ? cui__rx_cont(M, i, k) : 0; break;
    case RX_EOL: r = (i == M->len || (ml && M->s[i] == '\n')) ? cui__rx_cont(M, i, k) : 0; break;
    case RX_WORDB: case RX_NWORDB: {
        bool a = i > 0 && cui__rx_word((unsigned char)M->s[i - 1]), b = i < M->len && cui__rx_word((unsigned char)M->s[i]);
        r = ((a != b) == (n->type == RX_WORDB)) ? cui__rx_cont(M, i, k) : 0;
        break;
    }
    case RX_SEQ: {
        cui__rx_k nk = { RK_SEQ, node, 0, 0, k };
        r = cui__rx_cont(M, i, &nk);
        break;
    }
    case RX_ALT:
        for (int a = 0; a < n->nkid && !r && !M->abort; a++) r = cui__rx_match(M, R->kid[n->kid + a], i, k);
        break;
    case RX_GROUP: {
        int g = n->group;
        int o0 = M->cap[g][0], o1 = M->cap[g][1];
        M->cap[g][0] = i;
        cui__rx_k end = { RK_GROUP_END, node, 0, 0, k };
        r = cui__rx_match(M, R->kid[n->kid], i, &end);
        if (!r) { M->cap[g][0] = o0; M->cap[g][1] = o1; }
        break;
    }
    case RX_REPEAT: {
        const cui__rx_node *c = &R->node[R->kid[n->kid]];
        if (cui__rx_single(c)) { r = cui__rx_rep_single(M, n, c, i, k); break; }
        cui__rx_k first = { RK_REP, node, 0, -1, k };
        r = cui__rx_cont(M, i, &first);
        break;
    }
    }
    M->depth--;
    return r;
}

int cui_regex_search(const char *pattern, int flags, const char *text, int len, int from, cui_regex_match *m, const char **error) {
    cui__rx R;   /* about 20 KB */
    if (error) *error = NULL;
    if (m) memset(m, 0, sizeof *m);
    if (!pattern || !text) return 0;
    if (len < 0) len = (int)strlen(text);
    if (!cui__rx_compile(&R, pattern, flags)) { if (error) *error = R.error; return -1; }
    cui__rx_m M;
    memset(&M, 0, sizeof M);
    M.R = &R; M.s = text; M.len = len;
    for (int i = CUI_MAX(0, from); i <= len; i++) {
        if (i > from && i < len && ((unsigned char)text[i] & 0xC0) == 0x80) continue;   /* start at whole characters */
        for (int g = 0; g < CUI_REGEX_GROUPS; g++) M.cap[g][0] = M.cap[g][1] = -1;
        M.depth = 0;
        if (cui__rx_match(&M, R.root, i, NULL)) {
            if (m) {
                m->start = i; m->end = M.end;
                m->groups = R.groups;
                for (int g = 1; g <= R.groups; g++) {
                    m->group_start[g] = M.cap[g][0]; m->group_end[g] = M.cap[g][0] >= 0 ? M.cap[g][1] : -1;
                    memcpy(m->group_name[g], R.names[g], sizeof m->group_name[g]);
                }
                m->group_start[0] = i; m->group_end[0] = M.end;
            }
            return 1;
        }
        if (M.abort) { if (error) *error = "Too complex: the pattern backtracks too much"; return -2; }
    }
    return 0;
}
