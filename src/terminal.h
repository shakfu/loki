/* loki_terminal.h - Terminal I/O abstraction layer
 *
 * This module provides low-level terminal operations including:
 * - Raw mode management (disabling canonical mode, echo, etc.)
 * - Key reading with escape sequence parsing
 * - Window size detection and monitoring
 * - Screen buffer management for efficient rendering
 *
 * These functions are platform-specific (POSIX) and isolate terminal
 * dependencies from the core editor logic.
 */

#ifndef LOKI_TERMINAL_H
#define LOKI_TERMINAL_H

#include "internal.h"  /* For editor_ctx_t, abuf */

/* ======================= Input Reading ==================================== */

/* Read a single key from the terminal, handling escape sequences.
 * Blocks until a key is available or timeout occurs.
 * Returns:
 *   - ASCII value for normal keys (0-127)
 *   - KEY_* constants for special keys (arrows, function keys, etc.)
 *   - Exits on EOF after timeout */
int terminal_read_key(int fd);

/* terminal_read_key, but calls the idle hook on each 100 ms timeout while
 * waiting for the first byte. Only the top-level editor loop uses it: nested
 * prompts (search) hold state that idle work could invalidate. */
int terminal_read_key_idle(int fd);

/* Set the function terminal_read_key_idle calls while idle (NULL: none). */
void terminal_set_idle_hook(void (*fn)(void));

/* ======================= Window Size Detection ============================ */

/* Get current terminal window size in rows and columns.
 * First tries ioctl(TIOCGWINSZ), falls back to VT100 cursor queries.
 * Returns 0 on success, -1 on failure. */
int terminal_get_window_size(int ifd, int ofd, int *rows, int *cols);

/* Query cursor position using VT100 escape sequences.
 * Used as fallback when ioctl fails.
 * Returns 0 on success, -1 on failure. */
int terminal_get_cursor_position(int ifd, int ofd, int *rows, int *cols);

/* Terminal rows and cols, or 24x80 if they cannot be read. */
void terminal_query_window_size(int *rows, int *cols);

/* Set ctx's screen size from terminal rows and cols, before REPL layout. */
void terminal_set_window_size(editor_ctx_t *ctx, int rows, int cols);

/* Query the terminal and set ctx's screen size. */
void terminal_update_window_size(editor_ctx_t *ctx);

/* ======================= Screen Buffer ==================================== */

/* Append string to screen buffer for efficient rendering.
 * Buffers all VT100 escape sequences and content, then flushes
 * in a single write() call to minimize flicker.
 * Exits on allocation failure after attempting cleanup. */
void terminal_buffer_append(struct abuf *ab, const char *s, int len);

/* Free screen buffer memory. */
void terminal_buffer_free(struct abuf *ab);

/* ======================= Signal Handling ================================== */

/* Signal handler for SIGWINCH (window size change).
 * Async-signal-safe: only sets a flag, the main loop in editor.c applies the resize.
 * Should be registered with signal(SIGWINCH, terminal_sig_winch_handler). */
void terminal_sig_winch_handler(int sig);

/* ======================= Terminal Host Abstraction ========================= */

#include <termios.h>
#include <signal.h>

/**
 * TerminalHost - Encapsulates terminal state for signal handling.
 *
 * This replaces the previous global signal_context pattern, providing
 * cleaner separation between terminal state and editor context.
 *
 * POSIX signal handlers cannot access per-context data, so we maintain
 * a global pointer. Only one terminal host can be active at a time.
 * For multi-terminal scenarios (e.g., web editor), use polling instead
 * of signals - the web frontend sends resize events via RPC.
 */
typedef struct TerminalHost {
    struct termios orig_termios;           /* Saved terminal state */
    volatile sig_atomic_t winsize_changed; /* Set by SIGWINCH, cleared by app */
    int rawmode;                           /* Is raw mode currently active? */
    int fd;                                /* Terminal file descriptor */
} TerminalHost;

/* Global terminal host pointer (for signal handler access) */
extern TerminalHost *g_terminal_host;

/* Initialize terminal host and register signal handlers.
 * Should be called once at application startup.
 * Returns 0 on success, -1 on error. */
int terminal_host_init(TerminalHost *host, int fd);

/* Cleanup terminal host (restore terminal state).
 * Safe to call multiple times. */
void terminal_host_cleanup(TerminalHost *host);

/* Enable raw mode on the terminal host.
 * Returns 0 on success, -1 on error. */
int terminal_host_enable_raw_mode(TerminalHost *host);

/* Disable raw mode, restoring original terminal state. */
void terminal_host_disable_raw_mode(TerminalHost *host);

/* Check if window resize is pending.
 * Returns 1 if resize pending, 0 otherwise. */
int terminal_host_resize_pending(TerminalHost *host);

/* Clear resize pending flag (call after handling resize). */
void terminal_host_clear_resize(TerminalHost *host);

#endif /* LOKI_TERMINAL_H */
