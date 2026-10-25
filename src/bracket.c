/* bracket.c - Matching bracket lookup for (), [] and {}
 *
 * Counts nesting depth from the starting bracket, forward for an opening
 * bracket and backward for a closing one, across rows. Uses row->hl to skip
 * brackets in strings and comments; hl is indexed by render column, so each
 * scanned row gets a chars-to-render column map.
 */

#include "bracket.h"
#include <stdlib.h>

/* Same tab expansion as editor_update_row(). */
int bracket_render_col(const t_erow *row, int col) {
    int idx = 0;
    for (int j = 0; j < col && j < row->size; j++) {
        if (row->chars[j] == TAB) {
            idx++;
            while ((idx + 1) % 8 != 0) idx++;
        } else {
            idx++;
        }
    }
    return idx;
}

/* Fill map[j] with the render column of chars[j]. */
static void fill_render_map(const t_erow *row, int *map) {
    int idx = 0;
    for (int j = 0; j < row->size; j++) {
        map[j] = idx;
        if (row->chars[j] == TAB) {
            idx++;
            while ((idx + 1) % 8 != 0) idx++;
        } else {
            idx++;
        }
    }
}

static int is_quoted(const t_erow *row, int rcol) {
    if (!row->hl || rcol >= row->rsize) return 0;
    int hl = row->hl[rcol];
    return hl == HL_STRING || hl == HL_COMMENT || hl == HL_MLCOMMENT;
}

/* Grow *map to hold n ints. Returns 0 on allocation failure. */
static int reserve(int **map, int *cap, int n) {
    if (n <= *cap) return 1;
    int *p = realloc(*map, (size_t)n * sizeof(int));
    if (!p) return 0;
    *map = p;
    *cap = n;
    return 1;
}

int bracket_find_match(const editor_ctx_t *ctx, int row, int col,
                       int *match_row, int *match_col) {
    if (row < 0 || row >= ctx->model.numrows) return 0;
    const t_erow *start = &ctx->model.row[row];
    if (col < 0 || col >= start->size) return 0;

    char open, close;
    int dir;
    switch (start->chars[col]) {
        case '(': open = '('; close = ')'; dir = 1; break;
        case '[': open = '['; close = ']'; dir = 1; break;
        case '{': open = '{'; close = '}'; dir = 1; break;
        case ')': open = '('; close = ')'; dir = -1; break;
        case ']': open = '['; close = ']'; dir = -1; break;
        case '}': open = '{'; close = '}'; dir = -1; break;
        default: return 0;
    }

    int *map = NULL, cap = 0, found = 0;
    if (!reserve(&map, &cap, start->size)) return 0;
    fill_render_map(start, map);
    /* A bracket inside a string or comment matches within the same context. */
    int skip_quoted = !is_quoted(start, map[col]);

    int depth = 0;
    int last = row + dir * BRACKET_MAX_SCAN_ROWS;
    for (int r = row; dir > 0 ? (r <= last && r < ctx->model.numrows)
                              : (r >= last && r >= 0); r += dir) {
        const t_erow *cur = &ctx->model.row[r];
        if (r != row) {
            if (!reserve(&map, &cap, cur->size)) break;
            fill_render_map(cur, map);
        }
        int j = (r == row) ? col : (dir > 0 ? 0 : cur->size - 1);
        for (; j >= 0 && j < cur->size; j += dir) {
            char c = cur->chars[j];
            if (c != open && c != close) continue;
            if (skip_quoted && is_quoted(cur, map[j])) continue;
            depth += (c == open) ? dir : -dir;
            if (depth == 0) {
                *match_row = r;
                *match_col = j;
                found = 1;
                goto done;
            }
        }
    }
done:
    free(map);
    return found;
}
