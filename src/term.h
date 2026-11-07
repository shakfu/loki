/* term.h - built-in terminal pane (libghostty-vt + pty)
 *
 * One editor-wide pane runs a child on a pty. libghostty-vt parses the
 * child's output; loki draws the resulting grid into a region of its own
 * screen, docked at the bottom or on the right. While the pane has focus,
 * stdin bytes go to the pty unchanged, except Ctrl-\ which returns focus
 * and Shift-PgUp/PgDn which page through scrollback. Ctrl-\ [ enters a
 * scroll mode that works when the host terminal keeps Shift-PgUp. Idle
 * history is compressed in steps from loki_term_pump.
 *
 * Without LOKI_ENABLE_TERMINAL every function is a no-op stub.
 */

#ifndef LOKI_TERM_H
#define LOKI_TERM_H

#include <stddef.h>
#include "internal.h"

#define TERM_FOCUS_KEY 28       /* Ctrl-\ */

typedef enum { TERM_POS_BOTTOM = 0, TERM_POS_RIGHT = 1 } TermPosition;

/* Pane region in 0-based screen cells, including its header row. */
typedef struct {
    int top, left, height, width;
    int grid_rows, grid_cols;   /* the child's terminal size */
} TermRect;

/* Pane memory, both screens summed. */
typedef struct {
    unsigned long long resident_bytes;  /* physical; budget against this */
    unsigned long long virtual_bytes;
    unsigned long long pages, compressed_pages;
    int compression_supported;
} TermMemory;

#ifdef LOKI_ENABLE_TERMINAL

/* Start argv (NULL: $SHELL, else /bin/sh) in the pane, sized for ctx's
 * screen. Returns 0, or -1 if a pane is already open or the start failed. */
int loki_term_open(editor_ctx_t *ctx, const char *const *argv);

/* Hang up the child and free the pane. */
void loki_term_close(void);

int loki_term_is_open(void);
int loki_term_focused(void);
void loki_term_set_focus(int focused);

/* Pane region for ctx's screen. Returns 0 when closed or too small. */
int loki_term_rect(const editor_ctx_t *ctx, TermRect *r);

/* Rows and cols the pane takes from the text area, for layout. */
void loki_term_reserved(const editor_ctx_t *ctx, int *rows, int *cols);

/* Set placement; size is rows (bottom) or cols (right), 0 for default.
 * Returns 0, or -1 for an unknown position or negative size. */
int loki_term_set_layout(const char *position, int size);
TermPosition loki_term_position(void);
int loki_term_size(void);

/* Read pending child output and reap an exited child. Returns 1 if the
 * pane changed (output, or the child exited and the pane closed). */
int loki_term_pump(void);

/* Master fd for the input wait loop, or -1. */
int loki_term_fd(void);

/* Append the pane to ab. When the pane has focus and a visible cursor,
 * set *crow / *ccol (1-based) to it and return 1. */
int loki_term_render(editor_ctx_t *ctx, struct abuf *ab, int *crow, int *ccol);

/* Move the viewport by rows (negative: into history). */
void loki_term_scroll(int rows);

/* Rows between the viewport and the bottom; 0 when following output. */
int loki_term_scroll_offset(void);

/* Write bytes to the child, translating cursor keys for DECCKM. */
int loki_term_send(const char *buf, size_t len);

/* Focused-pane input step: wait for stdin, forward it. Returns -1 when
 * input is closed, else 0. Sets *key to a byte that left the pane after
 * Ctrl-\ and belongs to the editor, else -1. */
int loki_term_handle_input(int fd, int *key);

/* Scroll mode (Ctrl-\ [): keys move the viewport instead of the child. */
int loki_term_scrolling(void);

/* Fill m for the open pane. Returns 0, or -1 when closed. Scans every
 * page, so do not call it per frame. */
int loki_term_memory(TermMemory *m);

/* Plain text of the visible grid (malloc'd), or NULL. For tests. */
char *loki_term_screen_text(void);

/* Exit status of the last child, or -1 if none has exited. */
int loki_term_last_status(void);

/* Called on each 100 ms input timeout while the pane has focus. */
void loki_term_set_idle_hook(void (*fn)(void));

/* Open the pane if closed (argv as for loki_term_open), then focus it.
 * Reports failure in the status bar and returns -1. */
int loki_term_focus_or_open(editor_ctx_t *ctx, const char *const *argv);

/* Ex commands and :set options (termpos, termsize). */
int cmd_term(editor_ctx_t *ctx, const char *args);
int cmd_term_close(editor_ctx_t *ctx, const char *args);
int loki_term_set_option(editor_ctx_t *ctx, const char *option, const char *value);

#else

static inline int loki_term_open(editor_ctx_t *c, const char *const *a) { (void)c; (void)a; return -1; }
static inline void loki_term_close(void) {}
static inline int loki_term_is_open(void) { return 0; }
static inline int loki_term_focused(void) { return 0; }
static inline void loki_term_set_focus(int f) { (void)f; }
static inline int loki_term_rect(const editor_ctx_t *c, TermRect *r) { (void)c; (void)r; return 0; }
static inline void loki_term_reserved(const editor_ctx_t *c, int *r, int *k) { (void)c; *r = 0; *k = 0; }
static inline int loki_term_set_layout(const char *p, int s) { (void)p; (void)s; return -1; }
static inline int loki_term_pump(void) { return 0; }
static inline int loki_term_fd(void) { return -1; }
static inline int loki_term_render(editor_ctx_t *c, struct abuf *ab, int *r, int *k) { (void)c; (void)ab; (void)r; (void)k; return 0; }
static inline int loki_term_handle_input(int fd, int *k) { (void)fd; *k = -1; return 0; }
static inline void loki_term_set_idle_hook(void (*fn)(void)) { (void)fn; }
static inline int loki_term_focus_or_open(editor_ctx_t *c, const char *const *a) { (void)c; (void)a; return -1; }

#endif

#endif /* LOKI_TERM_H */
