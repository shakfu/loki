/* cui_frontend.h - loki drawn in a CUI window (cui_frontend.c). */
#ifndef LOKI_CUI_FRONTEND_H
#define LOKI_CUI_FRONTEND_H

#include <stddef.h>
#include <cui/cui.h>

/* One frame body, between cui_begin_frame and cui_end_frame: resize the cell
 * grid to size (points), run background work, apply this frame's keys,
 * refresh the current buffer and paint it. Returns -1 when there is no buffer. */
int loki_cui_frame(cui_ctx *ui, cui_vec2 size);
void loki_cui_shutdown(void);
int loki_cui_any_dirty(void);

/* Read back the last refresh: grid size, one row's text (trailing blanks
 * trimmed), cursor cell (0-based). */
int loki_cui_grid_size(int *rows, int *cols);
int loki_cui_grid_row(int row, char *buf, size_t cap);
void loki_cui_cursor(int *row, int *col);

#endif
