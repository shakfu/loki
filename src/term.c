/* term.c - built-in terminal pane: pty child + libghostty-vt grid
 *
 * The grid is redrawn in full on every editor refresh, like the text area.
 * Input forwarding is raw: loki's key decoder drops sequences it does not
 * know, so decoding and re-encoding would lose keys. The one translation is
 * cursor keys under DECCKM, because the host terminal does not know the
 * child asked for application cursor keys.
 */

#ifndef __APPLE__
#define _DEFAULT_SOURCE         /* forkpty, setenv under _POSIX_C_SOURCE */
#endif

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

#include <ghostty/vt.h>

#include "term.h"
#include "terminal.h"
#include "buffers.h"

#define TERM_DEFAULT_ROWS 12
#define TERM_MIN_TEXT_COLS 20
#define TERM_CELL_W_PX 8        /* nominal; only size reports use pixels */
#define TERM_CELL_H_PX 16
#define TERM_PUMP_MAX (1 << 20) /* bytes per pump, so input is not starved */
#define TERM_SCROLLBACK_LINES 10000
#define TERM_PREFIX_MS 300      /* wait for the key after Ctrl-\ */
#define TERM_COMPRESS_IDLE_MS 2000 /* quiet time before compressing history */

static struct {
    GhosttyTerminal vt;
    GhosttyRenderState rs;
    GhosttyRenderStateRowIterator rows;
    GhosttyRenderStateRowCells cells;
    int fd;
    pid_t pid;
    int focused;
    int scrolling;              /* scroll mode: keys move the viewport */
    int grid_rows, grid_cols;
    uint64_t activity;          /* compression activity token */
    long quiet_since;           /* ms when activity last changed */
    int compressed;             /* pass complete until activity changes */
    char name[32];
} T = { .fd = -1, .pid = -1 };

static TermPosition g_pos = TERM_POS_BOTTOM;
static int g_size = 0;
static int g_last_status = -1;
static void (*g_idle)(void);

/* ======================= Layout ========================================== */

int loki_term_is_open(void) { return T.vt != NULL; }
int loki_term_focused(void) { return T.vt != NULL && T.focused; }
int loki_term_fd(void) { return T.fd; }
TermPosition loki_term_position(void) { return g_pos; }
int loki_term_size(void) { return g_size; }
int loki_term_last_status(void) { return g_last_status; }
void loki_term_set_idle_hook(void (*fn)(void)) { g_idle = fn; }

int loki_term_scrolling(void) { return T.vt != NULL && T.scrolling; }

void loki_term_set_focus(int focused) {
    T.focused = T.vt != NULL && focused;
    if (!T.focused) T.scrolling = 0;
}

int loki_term_rect(const editor_ctx_t *ctx, TermRect *r) {
    if (!T.vt || !ctx) return 0;
    int rows = ctx->view.screenrows_total + STATUS_ROWS;
    int cols = ctx->view.screencols_total > 0 ? ctx->view.screencols_total
                                              : ctx->view.screencols;
    if (g_pos == TERM_POS_BOTTOM) {
        int repl = (ctx->lua_host && ctx->lua_host->repl.active) ? LUA_REPL_TOTAL_ROWS : 0;
        int h = (g_size > 0 ? g_size : TERM_DEFAULT_ROWS) + 1;
        int max = rows - STATUS_ROWS - repl - 1;    /* keep one text row */
        if (h > max) h = max;
        if (h < 2 || cols < 1) return 0;
        *r = (TermRect){ rows - h, 0, h, cols, h - 1, cols };
    } else {
        int w = (g_size > 0 ? g_size : cols / 2) + 1;
        int max = cols - TERM_MIN_TEXT_COLS;
        if (w > max) w = max;
        if (w < 2 || rows < 2) return 0;
        *r = (TermRect){ 0, cols - w, rows, w, rows - 1, w - 1 };
    }
    return 1;
}

void loki_term_reserved(const editor_ctx_t *ctx, int *rows, int *cols) {
    TermRect r;
    *rows = *cols = 0;
    if (!loki_term_rect(ctx, &r)) return;
    if (g_pos == TERM_POS_BOTTOM) *rows = r.height;
    else *cols = r.width;
}

static void relayout(void) {
    editor_ctx_t *ctx = buffer_get_current();
    if (ctx) editor_update_repl_layout_all(ctx);
}

int loki_term_set_layout(const char *position, int size) {
    if (size < 0) return -1;
    if (position) {
        if (strcmp(position, "bottom") == 0) g_pos = TERM_POS_BOTTOM;
        else if (strcmp(position, "right") == 0) g_pos = TERM_POS_RIGHT;
        else return -1;
    }
    g_size = size;
    relayout();
    return 0;
}

/* ======================= Child process =================================== */

static void on_write_pty(GhosttyTerminal vt, void *ud, const uint8_t *data, size_t len) {
    (void)vt; (void)ud;
    /* Replies to queries (DSR, DA) are short; drop what the pty cannot take. */
    while (len > 0 && T.fd >= 0) {
        ssize_t n = write(T.fd, data, len);
        if (n > 0) { data += n; len -= (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
}

static void free_vt(void) {
    if (T.cells) ghostty_render_state_row_cells_free(T.cells);
    if (T.rows) ghostty_render_state_row_iterator_free(T.rows);
    if (T.rs) ghostty_render_state_free(T.rs);
    if (T.vt) ghostty_terminal_free(T.vt);
    T.cells = NULL; T.rows = NULL; T.rs = NULL; T.vt = NULL;
}

static void exec_child(const char *const *argv) {
    static const int sigs[] = { SIGINT, SIGQUIT, SIGTSTP, SIGTTIN, SIGTTOU,
                                SIGCHLD, SIGPIPE, SIGWINCH, SIGHUP, SIGTERM };
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) signal(sigs[i], SIG_DFL);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    /* xterm-ghostty terminfo is often not installed; xterm-256color is. */
    setenv("TERM", "xterm-256color", 1);
    setenv("COLORTERM", "truecolor", 1);
    unsetenv("LINES");
    unsetenv("COLUMNS");
    execvp(argv[0], (char *const *)argv);
    _exit(127);
}

int loki_term_open(editor_ctx_t *ctx, const char *const *argv) {
    if (T.vt || !ctx) return -1;

    const char *shell_argv[2] = { NULL, NULL };
    if (!argv || !argv[0]) {
        const char *sh = getenv("SHELL");
        shell_argv[0] = (sh && sh[0] == '/') ? sh : "/bin/sh";
        argv = shell_argv;
    }

    if (ghostty_terminal_new(NULL, &T.vt, 80, 24) != GHOSTTY_SUCCESS ||
        ghostty_render_state_new(NULL, &T.rs) != GHOSTTY_SUCCESS ||
        ghostty_render_state_row_iterator_new(NULL, &T.rows) != GHOSTTY_SUCCESS ||
        ghostty_render_state_row_cells_new(NULL, &T.cells) != GHOSTTY_SUCCESS) {
        free_vt();
        return -1;
    }
    ghostty_terminal_set(T.vt, GHOSTTY_TERMINAL_OPT_WRITE_PTY, (const void *)on_write_pty);
    /* The default cap is 10,000 bytes (about one page); cap by lines instead. */
    size_t no_byte_cap = SIZE_MAX, lines = TERM_SCROLLBACK_LINES;
    ghostty_terminal_set(T.vt, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_BYTES, &no_byte_cap);
    ghostty_terminal_set(T.vt, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_LINES, &lines);

    TermRect r;
    if (!loki_term_rect(ctx, &r)) { free_vt(); return -1; }
    ghostty_terminal_resize(T.vt, (uint16_t)r.grid_cols, (uint16_t)r.grid_rows,
                            TERM_CELL_W_PX, TERM_CELL_H_PX);
    T.grid_rows = r.grid_rows;
    T.grid_cols = r.grid_cols;

    struct winsize ws = { (unsigned short)r.grid_rows, (unsigned short)r.grid_cols,
                          (unsigned short)(r.grid_cols * TERM_CELL_W_PX),
                          (unsigned short)(r.grid_rows * TERM_CELL_H_PX) };
    int fd;
    pid_t pid = forkpty(&fd, NULL, NULL, &ws);
    if (pid < 0) { free_vt(); return -1; }
    if (pid == 0) exec_child(argv);

    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    T.fd = fd;
    T.pid = pid;
    T.focused = 0;
    const char *base = strrchr(argv[0], '/');
    snprintf(T.name, sizeof(T.name), "%s", base ? base + 1 : argv[0]);
    relayout();
    return 0;
}

static void close_child(int sig) {
    if (T.fd >= 0) close(T.fd);     /* the child sees a hangup */
    T.fd = -1;
    if (T.pid > 0) {
        int st;
        pid_t got = waitpid(T.pid, &st, WNOHANG);
        if (got == 0 && sig) {
            kill(T.pid, SIGHUP);
            usleep(20000);
            got = waitpid(T.pid, &st, WNOHANG);
            if (got == 0) { kill(T.pid, SIGKILL); got = waitpid(T.pid, &st, 0); }
        }
        if (got == T.pid) g_last_status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    }
    T.pid = -1;
    T.focused = 0;
    T.scrolling = 0;
    free_vt();
}

void loki_term_close(void) {
    if (!T.vt) return;
    close_child(1);
    relayout();
}

/* ======================= Output ========================================== */

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The library has no timers: after TERM_COMPRESS_IDLE_MS without activity,
 * run one bounded compression step per pump until the pass completes. */
static void compress_step(void) {
    uint64_t a;
    if (ghostty_terminal_compression_activity(T.vt, &a) != GHOSTTY_SUCCESS) return;
    long now = now_ms();
    if (a != T.activity) {
        T.activity = a;
        T.quiet_since = now;
        T.compressed = 0;
        return;
    }
    if (T.compressed || now - T.quiet_since < TERM_COMPRESS_IDLE_MS) return;
    GhosttyTerminalCompressionResult r;
    if (ghostty_terminal_compress(T.vt, GHOSTTY_TERMINAL_COMPRESSION_MODE_INCREMENTAL, &r)
            != GHOSTTY_SUCCESS || r != GHOSTTY_TERMINAL_COMPRESSION_RESULT_PENDING)
        T.compressed = 1;
    /* Compressing may move the token; do not count that as new activity. */
    if (ghostty_terminal_compression_activity(T.vt, &a) == GHOSTTY_SUCCESS) T.activity = a;
}

int loki_term_memory(TermMemory *m) {
    if (!T.vt) return -1;
    GhosttyTerminalMemoryUsage u = GHOSTTY_INIT_SIZED(GhosttyTerminalMemoryUsage);
    if (ghostty_terminal_get(T.vt, GHOSTTY_TERMINAL_DATA_MEMORY_USAGE, &u) != GHOSTTY_SUCCESS)
        return -1;
    m->resident_bytes = u.primary_resident_bytes + u.alternate_resident_bytes;
    m->virtual_bytes = u.primary_virtual_bytes + u.alternate_virtual_bytes;
    m->pages = u.primary_pages + u.alternate_pages;
    m->compressed_pages = u.primary_compressed_pages + u.alternate_compressed_pages;
    m->compression_supported = u.compression_supported;
    return 0;
}

int loki_term_pump(void) {
    if (!T.vt) return 0;
    int changed = 0, hup = 0;
    size_t total = 0;
    uint8_t buf[16384];
    while (total < TERM_PUMP_MAX) {
        ssize_t n = read(T.fd, buf, sizeof(buf));
        if (n > 0) {
            ghostty_terminal_vt_write(T.vt, buf, (size_t)n);
            total += (size_t)n;
            changed = 1;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        hup = 1;                    /* 0 or EIO: the slave side closed */
        break;
    }

    int st;
    pid_t got = waitpid(T.pid, &st, WNOHANG);
    if (got == T.pid) {
        g_last_status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
        T.pid = -1;
        hup = 1;
    }
    if (hup) {
        close_child(0);
        relayout();
        return 1;
    }
    compress_step();
    return changed;
}

/* ======================= Scrollback ====================================== */

void loki_term_scroll(int rows) {
    if (!T.vt) return;
    GhosttyTerminalScrollViewport s = { .tag = GHOSTTY_TERMINAL_SCROLL_VIEWPORT_DELTA,
                                        .value = { .delta = rows } };
    ghostty_terminal_scroll_viewport(T.vt, s);
}

static void scroll_to(GhosttyTerminalScrollViewportTag tag) {
    GhosttyTerminalScrollViewport s = { .tag = tag };
    ghostty_terminal_scroll_viewport(T.vt, s);
}

/* True while the viewport follows output. Cheap, unlike a scrollbar read
 * after a relative scroll, which can walk every page. */
static int viewport_active(void) {
    bool active = true;
    ghostty_terminal_get(T.vt, GHOSTTY_TERMINAL_DATA_VIEWPORT_ACTIVE, &active);
    return active;
}

static void scroll_to_bottom(void) {
    if (!viewport_active()) scroll_to(GHOSTTY_TERMINAL_SCROLL_VIEWPORT_BOTTOM);
}

/* Move by n shell prompts; needs OSC 133 marks from the shell. */
static void scroll_prompts(int n) {
    GhosttyTerminalScrollViewport s = { .tag = GHOSTTY_TERMINAL_SCROLL_VIEWPORT_DELTA_PROMPT,
                                        .value = { .delta_prompt = n } };
    ghostty_terminal_scroll_viewport(T.vt, s);
}

int loki_term_scroll_offset(void) {
    if (!T.vt || viewport_active()) return 0;
    GhosttyTerminalScrollbar sb;
    if (ghostty_terminal_get(T.vt, GHOSTTY_TERMINAL_DATA_SCROLLBAR, &sb) != GHOSTTY_SUCCESS ||
        sb.offset + sb.len >= sb.total)
        return 0;
    return (int)(sb.total - sb.offset - sb.len);
}

/* ======================= Input =========================================== */

static int cursor_keys_mode(void) {
    GhosttyTerminalModeConfig m = { .mode = GHOSTTY_MODE_CURSOR_KEYS, .value = false };
    return ghostty_terminal_get(T.vt, GHOSTTY_TERMINAL_DATA_MODE, &m) == GHOSTTY_SUCCESS && m.value;
}

static int write_all(const char *p, size_t len) {
    while (len > 0) {
        ssize_t n = write(T.fd, p, len);
        if (n > 0) { p += n; len -= (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* The child may be blocked writing to us: drain, then wait. */
            loki_term_pump();
            if (!T.vt) return -1;
            struct pollfd p1 = { .fd = T.fd, .events = POLLOUT, .revents = 0 };
            poll(&p1, 1, 50);
            continue;
        }
        return -1;
    }
    return 0;
}

int loki_term_send(const char *buf, size_t len) {
    if (!T.vt || T.fd < 0) return -1;
    if (!cursor_keys_mode()) return write_all(buf, len);
    /* DECCKM: CSI A/B/C/D/H/F become SS3 A/B/C/D/H/F. */
    size_t start = 0;
    for (size_t i = 0; i + 2 < len; i++) {
        if (buf[i] == '\x1b' && buf[i + 1] == '[' && strchr("ABCDHF", buf[i + 2]) && buf[i + 2]) {
            if (write_all(buf + start, i - start) < 0) return -1;
            char ss3[3] = { '\x1b', 'O', buf[i + 2] };
            if (write_all(ss3, 3) < 0) return -1;
            i += 2;
            start = i + 1;
        }
    }
    return write_all(buf + start, len - start);
}

static void leave_scroll(void) {
    T.scrolling = 0;
    scroll_to_bottom();
}

/* Scroll mode: vi/less-style keys move the viewport; nothing reaches the
 * child. q, Esc or Enter leave; Ctrl-\ also returns focus to the editor.
 * Returns the bytes consumed; the rest follow scroll mode's exit. */
static size_t scroll_mode_keys(const char *b, size_t k) {
    int page = T.grid_rows > 1 ? T.grid_rows - 1 : 1;
    int half = page / 2 > 0 ? page / 2 : 1;
    size_t i = 0;
    while (i < k && T.scrolling) {
        unsigned char c = (unsigned char)b[i++];
        if (c == '\x1b') {
            if (i == k) { leave_scroll(); break; }       /* lone Esc */
            if (b[i] != '[' && b[i] != 'O') { i++; continue; }
            size_t j = i + 1;
            while (j < k && !((unsigned char)b[j] >= 0x40 && (unsigned char)b[j] <= 0x7e)) j++;
            if (j >= k) break;
            const char *p = b + i + 1;
            size_t n = j - i;               /* params plus final byte */
            if (n == 1 && *p == 'A') loki_term_scroll(-1);
            else if (n == 1 && *p == 'B') loki_term_scroll(1);
            else if (n == 1 && *p == 'H') scroll_to(GHOSTTY_TERMINAL_SCROLL_VIEWPORT_TOP);
            else if (n == 1 && *p == 'F') scroll_to_bottom();
            else if (*p == '5' && b[j] == '~') loki_term_scroll(-page);
            else if (*p == '6' && b[j] == '~') loki_term_scroll(page);
            i = j + 1;
            continue;
        }
        switch (c) {
        case 'k': loki_term_scroll(-1); break;
        case 'j': loki_term_scroll(1); break;
        case 'b': case 0x02: loki_term_scroll(-page); break;
        case 'f': case ' ': case 0x06: loki_term_scroll(page); break;
        case 0x15: loki_term_scroll(-half); break;
        case 0x04: loki_term_scroll(half); break;
        case 'g': scroll_to(GHOSTTY_TERMINAL_SCROLL_VIEWPORT_TOP); break;
        case 'G': scroll_to_bottom(); break;
        case '[': scroll_prompts(-1); break;
        case ']': scroll_prompts(1); break;
        case 'q': case '\r': leave_scroll(); break;
        case TERM_FOCUS_KEY: leave_scroll(); T.focused = 0; break;
        default: break;
        }
    }
    return i;
}

/* One byte from fd within ms, or -1. */
static int read_byte_within(int fd, int ms) {
    struct pollfd p = { .fd = fd, .events = POLLIN, .revents = 0 };
    unsigned char c;
    if (poll(&p, 1, ms) <= 0 || read(fd, &c, 1) != 1) return -1;
    return c;
}

static int term_bytes(int fd, char *buf, size_t k, int *key);

int loki_term_handle_input(int fd, int *key) {
    *key = -1;
    if (!loki_term_focused()) return 0;
    struct pollfd p[2] = {
        { .fd = fd, .events = POLLIN, .revents = 0 },
        { .fd = T.fd, .events = POLLIN, .revents = 0 },
    };
    int n = poll(p, 2, 100);
    if (n < 0) return 0;            /* EINTR (SIGWINCH): the loop resizes */
    if (n == 0) { if (g_idle) g_idle(); return 0; }
    if (!(p[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) return 0;

    char buf[4096];
    ssize_t k = read(fd, buf, sizeof(buf));
    if (k == 0) return -1;          /* readable but empty: hangup */
    if (k < 0) return (errno == EINTR || errno == EAGAIN) ? 0 : -1;
    return term_bytes(fd, buf, (size_t)k, key);
}

/* Route one read of stdin: scroll mode first, then the child. */
static int term_bytes(int fd, char *buf, size_t k, int *key) {
    if (T.scrolling) {
        size_t used = scroll_mode_keys(buf, k);
        if (T.scrolling || !T.focused || used >= k) return 0;
        buf += used;
        k -= used;
    }

    char *esc = memchr(buf, TERM_FOCUS_KEY, k);
    size_t fwd = esc ? (size_t)(esc - buf) : k;

    /* Shift-PgUp/PgDn page through history; anything typed snaps back. */
    int page = T.grid_rows > 1 ? T.grid_rows - 1 : 1;
    size_t start = 0, i = 0;
    while (i < fwd) {
        if (fwd - i >= 6 && (memcmp(buf + i, "\x1b[5;2~", 6) == 0 ||
                             memcmp(buf + i, "\x1b[6;2~", 6) == 0)) {
            if (i > start) { scroll_to_bottom(); loki_term_send(buf + start, i - start); }
            loki_term_scroll(buf[i + 2] == '5' ? -page : page);
            i += 6;
            start = i;
        } else {
            i++;
        }
    }
    if (fwd > start) { scroll_to_bottom(); loki_term_send(buf + start, fwd - start); }
    if (!esc) return 0;

    /* Ctrl-\ prefix. The byte after it decides: [ scrolls, Ctrl-\ sends a
     * literal Ctrl-\, anything else returns focus and goes to the editor.
     * After [ or Ctrl-\ the rest of the read is handled in the new state;
     * after a key for the editor it is dropped. */
    size_t after = fwd + 1;
    int next = after < k ? (unsigned char)buf[after] : read_byte_within(fd, TERM_PREFIX_MS);
    if (next == '[') {
        T.scrolling = 1;
    } else if (next == TERM_FOCUS_KEY) {
        scroll_to_bottom();
        loki_term_send("\x1c", 1);
    } else {
        T.focused = 0;
        *key = next;
        return 0;
    }
    if (after + 1 < k) return term_bytes(fd, buf + after + 1, k - after - 1, key);
    return 0;
}

/* ======================= Rendering ======================================= */

static void append_utf8(struct abuf *ab, uint32_t cp) {
    char b[4];
    int n;
    if (cp < 0x80) { b[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F)); n = 3;
    } else if (cp < 0x110000) {
        b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F)); n = 4;
    } else { b[0] = '?'; n = 1; }
    terminal_buffer_append(ab, b, n);
}

/* base is 30 (fg) or 40 (bg). Palette colors stay indexed so the host
 * terminal's theme applies. */
static int sgr_color(char *out, size_t cap, GhosttyStyleColor c, int base) {
    switch (c.tag) {
    case GHOSTTY_STYLE_COLOR_PALETTE: {
        int n = c.value.palette;
        if (n < 8) return snprintf(out, cap, ";%d", base + n);
        if (n < 16) return snprintf(out, cap, ";%d", base + 60 + n - 8);
        return snprintf(out, cap, ";%d;5;%d", base + 8, n);
    }
    case GHOSTTY_STYLE_COLOR_RGB:
        return snprintf(out, cap, ";%d;2;%d;%d;%d", base + 8,
                        c.value.rgb.r, c.value.rgb.g, c.value.rgb.b);
    default:
        return 0;
    }
}

static void append_sgr(struct abuf *ab, const GhosttyStyle *s) {
    char b[96];
    int n = snprintf(b, sizeof(b), "\x1b[0");
    static const struct { GhosttyStyleFlags f; const char *code; } attrs[] = {
        { GHOSTTY_STYLE_FLAG_BOLD, ";1" }, { GHOSTTY_STYLE_FLAG_FAINT, ";2" },
        { GHOSTTY_STYLE_FLAG_ITALIC, ";3" }, { GHOSTTY_STYLE_FLAG_BLINK, ";5" },
        { GHOSTTY_STYLE_FLAG_INVERSE, ";7" }, { GHOSTTY_STYLE_FLAG_INVISIBLE, ";8" },
        { GHOSTTY_STYLE_FLAG_STRIKETHROUGH, ";9" },
    };
    for (size_t i = 0; i < sizeof(attrs) / sizeof(attrs[0]); i++)
        if (s->flags & attrs[i].f) n += snprintf(b + n, sizeof(b) - n, "%s", attrs[i].code);
    if (GHOSTTY_STYLE_UNDERLINE(*s)) n += snprintf(b + n, sizeof(b) - n, ";4");
    n += sgr_color(b + n, sizeof(b) - n, s->fg_color, 30);
    n += sgr_color(b + n, sizeof(b) - n, s->bg_color, 40);
    n += snprintf(b + n, sizeof(b) - n, "m");
    terminal_buffer_append(ab, b, n);
}

static void move_to(struct abuf *ab, int row0, int col0) {
    char b[32];
    int n = snprintf(b, sizeof(b), "\x1b[%d;%dH", row0 + 1, col0 + 1);
    terminal_buffer_append(ab, b, n);
}

static void render_header(struct abuf *ab, const TermRect *r) {
    char text[128], scroll[32] = "";
    int back = loki_term_scroll_offset();
    if (back > 0) snprintf(scroll, sizeof(scroll), " [scroll -%d]", back);
    const char *hint = T.scrolling ? "SCROLL  q to exit"
                     : T.focused ? "Ctrl-\\ to leave, Ctrl-\\ [ to scroll"
                     : "Ctrl-\\ to focus";
    int n = snprintf(text, sizeof(text), " [term] %s%s  %s ", T.name, scroll, hint);
    if (n > r->width) n = r->width;
    move_to(ab, r->top, r->left);
    terminal_buffer_append(ab, T.focused ? "\x1b[0;1;7m" : "\x1b[0;7m", T.focused ? 8 : 6);
    terminal_buffer_append(ab, text, n);
    for (int i = n; i < r->width; i++) terminal_buffer_append(ab, " ", 1);
    terminal_buffer_append(ab, "\x1b[0m", 4);
}

static void sync_size(const TermRect *r) {
    if (r->grid_rows == T.grid_rows && r->grid_cols == T.grid_cols) return;
    ghostty_terminal_resize(T.vt, (uint16_t)r->grid_cols, (uint16_t)r->grid_rows,
                            TERM_CELL_W_PX, TERM_CELL_H_PX);
    struct winsize ws = { (unsigned short)r->grid_rows, (unsigned short)r->grid_cols,
                          (unsigned short)(r->grid_cols * TERM_CELL_W_PX),
                          (unsigned short)(r->grid_rows * TERM_CELL_H_PX) };
    ioctl(T.fd, TIOCSWINSZ, &ws);   /* the kernel sends the child SIGWINCH */
    T.grid_rows = r->grid_rows;
    T.grid_cols = r->grid_cols;
}

int loki_term_render(editor_ctx_t *ctx, struct abuf *ab, int *crow, int *ccol) {
    TermRect r;
    if (!loki_term_rect(ctx, &r)) return 0;
    sync_size(&r);
    if (ghostty_render_state_update(T.rs, T.vt) != GHOSTTY_SUCCESS) return 0;

    terminal_buffer_append(ab, "\x1b[?7l", 5);     /* no autowrap: never scroll */
    render_header(ab, &r);

    int gx = r.left, gy = r.top + 1;
    if (g_pos == TERM_POS_RIGHT) {
        for (int y = gy; y < r.top + r.height; y++) {
            move_to(ab, y, r.left);
            terminal_buffer_append(ab, "\x1b[0;90m|", 8);
        }
        gx++;
    }

    if (ghostty_render_state_get(T.rs, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &T.rows) == GHOSTTY_SUCCESS) {
        int y = 0;
        while (y < r.grid_rows && ghostty_render_state_row_iterator_next(T.rows)) {
            move_to(ab, gy + y, gx);
            terminal_buffer_append(ab, "\x1b[0m", 4);
            GhosttyStyle last;
            ghostty_style_default(&last);
            if (ghostty_render_state_row_get(T.rows, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &T.cells) == GHOSTTY_SUCCESS) {
                int x = 0;
                while (x < r.grid_cols && ghostty_render_state_row_cells_next(T.cells)) {
                    GhosttyCell raw = 0;
                    GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
                    ghostty_render_state_row_cells_get(T.cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW, &raw);
                    ghostty_cell_get(raw, GHOSTTY_CELL_DATA_WIDE, &wide);
                    x++;
                    if (wide == GHOSTTY_CELL_WIDE_SPACER_TAIL) continue;

                    GhosttyStyle s;
                    ghostty_render_state_row_cells_get(T.cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &s);
                    if (memcmp(&s, &last, sizeof(s)) != 0) { append_sgr(ab, &s); last = s; }

                    uint32_t len = 0;
                    ghostty_render_state_row_cells_get(T.cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN, &len);
                    if (len == 0 || wide == GHOSTTY_CELL_WIDE_SPACER_HEAD) {
                        terminal_buffer_append(ab, " ", 1);
                        continue;
                    }
                    uint32_t cps[16];
                    GhosttyCodepointBuffer g = { .ptr = cps, .cap = 16 };
                    ghostty_render_state_row_cells_get(T.cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF, &g);
                    size_t m = g.len < g.cap ? g.len : g.cap;
                    for (size_t i = 0; i < m; i++) append_utf8(ab, cps[i]);
                }
                /* Pad a row shorter than the pane (during a resize). */
                if (x < r.grid_cols) terminal_buffer_append(ab, "\x1b[0m", 4);
                for (; x < r.grid_cols; x++) terminal_buffer_append(ab, " ", 1);
            }
            y++;
        }
        for (; y < r.grid_rows; y++) {
            move_to(ab, gy + y, gx);
            terminal_buffer_append(ab, "\x1b[0m", 4);
            for (int x = 0; x < r.grid_cols; x++) terminal_buffer_append(ab, " ", 1);
        }
    }
    terminal_buffer_append(ab, "\x1b[0m\x1b[?7h", 9);

    if (!T.focused || T.scrolling) return 0;
    GhosttyRenderStateCursor cur = GHOSTTY_INIT_SIZED(GhosttyRenderStateCursor);
    if (ghostty_render_state_get(T.rs, GHOSTTY_RENDER_STATE_DATA_CURSOR, &cur) != GHOSTTY_SUCCESS ||
        !cur.visible || !cur.viewport_has_value)
        return 0;
    *crow = gy + cur.viewport_y + 1;
    *ccol = gx + cur.viewport_x + 1;
    return 1;
}

char *loki_term_screen_text(void) {
    if (!T.vt) return NULL;
    GhosttyFormatter f = NULL;
    if (ghostty_formatter_terminal_new(NULL, &f, T.vt) != GHOSTTY_SUCCESS) return NULL;
    uint8_t *out = NULL;
    size_t len = 0;
    char *text = NULL;
    if (ghostty_formatter_format_alloc(f, NULL, &out, &len) == GHOSTTY_SUCCESS) {
        text = malloc(len + 1);
        if (text) { memcpy(text, out, len); text[len] = '\0'; }
        ghostty_free(NULL, out, len);
    }
    ghostty_formatter_free(f);
    return text;
}

/* ======================= Commands ======================================== */

int loki_term_focus_or_open(editor_ctx_t *ctx, const char *const *argv) {
    if (!T.vt && loki_term_open(ctx, argv) < 0) {
        editor_set_status_msg(ctx, "Terminal: could not start (screen too small?)");
        return -1;
    }
    TermRect r;
    if (!loki_term_rect(ctx, &r)) {
        editor_set_status_msg(ctx, "Terminal: screen too small to show the pane");
        return -1;
    }
    T.focused = 1;
    return 0;
}

/* :term [cmd] - open (running cmd under /bin/sh -c) or focus the pane */
int cmd_term(editor_ctx_t *ctx, const char *args) {
    if (args && args[0]) {
        if (T.vt) {
            editor_set_status_msg(ctx, "Terminal already open; :term! closes it");
            return 0;
        }
        const char *argv[] = { "/bin/sh", "-c", args, NULL };
        return loki_term_focus_or_open(ctx, argv) == 0;
    }
    return loki_term_focus_or_open(ctx, NULL) == 0;
}

/* :term! - hang up the child and close the pane */
int cmd_term_close(editor_ctx_t *ctx, const char *args) {
    (void)args;
    if (!T.vt) {
        editor_set_status_msg(ctx, "No terminal open");
        return 0;
    }
    loki_term_close();
    editor_set_status_msg(ctx, "Terminal closed");
    return 1;
}

/* :set termpos=bottom|right, :set termsize=N */
int loki_term_set_option(editor_ctx_t *ctx, const char *option, const char *value) {
    if (strcmp(option, "termpos") == 0) {
        if (loki_term_set_layout(value, g_size) < 0) {
            editor_set_status_msg(ctx, "termpos: bottom or right");
            return 0;
        }
    } else {
        char *end;
        long n = strtol(value, &end, 10);
        if (*value == '\0' || *end != '\0' || n < 0 || n > 1000 ||
            loki_term_set_layout(NULL, (int)n) < 0) {
            editor_set_status_msg(ctx, "termsize: rows (bottom) or cols (right), 0 = default");
            return 0;
        }
    }
    editor_set_status_msg(ctx, "Terminal: %s, size %d",
                          g_pos == TERM_POS_BOTTOM ? "bottom" : "right", g_size);
    return 1;
}
