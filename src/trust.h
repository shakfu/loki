/* trust.h - Trust list for project-local Lua configuration
 *
 * A project's .loki/init.lua and .loki modules run only if the project
 * directory is listed in ~/.loki/trusted (one canonical path per line).
 */

#ifndef LOKI_TRUST_H
#define LOKI_TRUST_H

/* Return 1 if 'project_dir' is in the trust list, 0 otherwise. */
int loki_trust_is_trusted(const char *project_dir);

/* Add 'project_dir' to the trust list. Returns 0 on success, -1 on error. */
int loki_trust_add(const char *project_dir);

#endif /* LOKI_TRUST_H */
