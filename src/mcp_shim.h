/* mcp_shim.h - `loki --mcp-shim [SOCK]`: relay stdio to a running loki */

#ifndef LOKI_MCP_SHIM_H
#define LOKI_MCP_SHIM_H

/* Relay stdin/stdout to the socket at sock_path; NULL means $LOKI_MCP_SOCK,
 * else the newest live socket in the socket directory. Returns the exit
 * status. */
int loki_mcp_shim(const char *sock_path);

#endif /* LOKI_MCP_SHIM_H */
