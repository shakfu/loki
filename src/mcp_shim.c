/* mcp_shim.c - `loki --mcp-shim [SOCK]`: relay stdio to a running loki
 *
 * MCP clients launch stdio servers as subprocesses, but loki's own stdio is
 * the terminal. The shim is that subprocess: it copies stdin to the editor's
 * socket and the socket to stdout, so the editor serves MCP over the socket.
 */

#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include "listen.h"
#include "mcp_shim.h"

static int connect_socket(const char *path) {
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(addr.sun_path)) return -1;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Connect to the most recently created live socket in the socket dir. */
static int connect_newest(char *chosen, size_t size) {
    char dir[256];
    if (loki_socket_dir(dir, sizeof(dir)) != 0) return -1;
    DIR *d = opendir(dir);
    if (!d) return -1;
    int best_fd = -1;
    time_t best_time = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 6 || strcmp(e->d_name + n - 5, ".sock") != 0) continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(path, &st) != 0 || !S_ISSOCK(st.st_mode)) continue;
        if (best_fd >= 0 && st.st_mtime <= best_time) continue;
        int fd = connect_socket(path);
        if (fd < 0) continue;
        if (best_fd >= 0) close(best_fd);
        best_fd = fd;
        best_time = st.st_mtime;
        snprintf(chosen, size, "%s", path);
    }
    closedir(d);
    return best_fd;
}

static int write_all(int fd, const char *buf, ssize_t n) {
    while (n > 0) {
        ssize_t w = write(fd, buf, (size_t)n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf += w;
        n -= w;
    }
    return 0;
}

int loki_mcp_shim(const char *sock_path) {
    char chosen[512] = "";
    if (!sock_path) sock_path = getenv("LOKI_MCP_SOCK");
    int fd = sock_path ? connect_socket(sock_path) : connect_newest(chosen, sizeof(chosen));
    if (fd < 0) {
        fprintf(stderr, "loki --mcp-shim: cannot connect to %s (start the server with :mcp in loki)\n",
                sock_path ? sock_path : "a running loki");
        return 1;
    }

    struct pollfd fds[2] = {
        { .fd = STDIN_FILENO, .events = POLLIN },
        { .fd = fd, .events = POLLIN },
    };
    char buf[65536];
    for (;;) {
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n > 0) {
                if (write_all(fd, buf, n) != 0) break;
            } else if (n == 0 || errno != EINTR) {
                shutdown(fd, SHUT_WR);  /* client is done; drain replies */
                fds[0].fd = -1;
            }
        }
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n > 0) {
                if (write_all(STDOUT_FILENO, buf, n) != 0) break;
            } else if (n == 0 || errno != EINTR) {
                break;  /* editor closed the connection */
            }
        }
    }
    close(fd);
    return 0;
}
