/* trust.c - Trust list for project-local Lua configuration
 *
 * Opening a file must not run code shipped in an untrusted checkout, so a
 * project's Lua config loads only after the user has trusted its directory.
 * Entries are canonical paths (realpath), so symlinks and "./" spellings of
 * the same directory match.
 */

/* glibc declares realpath only with _DEFAULT_SOURCE or XSI; under the strict
 * _POSIX_C_SOURCE set in CMake it was visible only via -O2 fortify headers. */
#define _DEFAULT_SOURCE
#include "trust.h"
#include "loki.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Path of the trust list, or -1 if HOME is unset or the path is too long. */
static int trust_file_path(char *buf, size_t n) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) return -1;
    int w = snprintf(buf, n, "%s/" LOKI_CONFIG_DIR "/trusted", home);
    return (w > 0 && (size_t)w < n) ? 0 : -1;
}

int loki_trust_is_trusted(const char *project_dir) {
    char canon[PATH_MAX];
    char path[PATH_MAX];
    if (!project_dir || !realpath(project_dir, canon)) return 0;
    if (trust_file_path(path, sizeof(path)) != 0) return 0;

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[PATH_MAX + 2];
    int found = 0;
    while (!found && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        found = strcmp(line, canon) == 0;
    }
    fclose(f);
    return found;
}

int loki_trust_add(const char *project_dir) {
    char canon[PATH_MAX];
    char path[PATH_MAX];
    if (!project_dir || !realpath(project_dir, canon)) return -1;
    if (loki_trust_is_trusted(canon)) return 0;
    if (trust_file_path(path, sizeof(path)) != 0) return -1;

    /* Create ~/.loki if needed; the list file itself is created by fopen. */
    char *slash = strrchr(path, '/');
    *slash = '\0';
    if (mkdir(path, 0700) != 0 && errno != EEXIST) return -1;
    *slash = '/';

    FILE *f = fopen(path, "a");
    if (!f) return -1;
    int ok = fprintf(f, "%s\n", canon) > 0;
    return (fclose(f) == 0 && ok) ? 0 : -1;
}
