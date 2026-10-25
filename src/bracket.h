/* bracket.h - Matching bracket lookup for (), [] and {} */

#ifndef LOKI_BRACKET_H
#define LOKI_BRACKET_H

#include "internal.h"

/* Rows scanned in each direction before giving up on a match. */
#define BRACKET_MAX_SCAN_ROWS 1000

/* Find the bracket matching the one at chars position (row, col).
 * Brackets in strings and comments are skipped unless the starting bracket
 * is itself in one. Returns 1 and sets *match_row, *match_col (chars
 * position) on success, 0 otherwise. */
int bracket_find_match(const editor_ctx_t *ctx, int row, int col,
                       int *match_row, int *match_col);

/* Convert a chars column to its column in row->render (tab expansion). */
int bracket_render_col(const t_erow *row, int col);

#endif /* LOKI_BRACKET_H */
