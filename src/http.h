/* http.h - Async HTTP support for Loki
 *
 * Provides asynchronous HTTP requests using libcurl.
 * Features:
 * - Non-blocking HTTP GET/POST requests
 * - Lua callback for response handling
 * - Security validation (URL scheme, length, body size)
 * - Rate limiting (per-minute request limit)
 */

#ifndef LOKI_HTTP_H
#define LOKI_HTTP_H

#include <stddef.h>
#include <lua.h>
#include "loki/core.h"  /* For editor_ctx_t */

/* Configuration constants */
#define LOKI_HTTP_MAX_ASYNC_REQUESTS  10
#define LOKI_HTTP_MAX_RESPONSE_SIZE   (10 * 1024 * 1024)  /* 10MB */
#define LOKI_HTTP_MAX_URL_LENGTH      2048
#define LOKI_HTTP_MAX_BODY_SIZE       (5 * 1024 * 1024)   /* 5MB */
#define LOKI_HTTP_RATE_LIMIT          60      /* requests per minute */
#define LOKI_HTTP_TIMEOUT             60      /* seconds */
#define LOKI_HTTP_CONNECT_TIMEOUT     10      /* seconds */

/**
 * Initialize HTTP subsystem.
 * Must be called before any HTTP operations.
 * Safe to call multiple times.
 */
void loki_http_init(void);

/**
 * Cleanup HTTP subsystem.
 * Cancels pending requests and frees resources.
 */
void loki_http_cleanup(void);

/**
 * Start an async HTTP request.
 *
 * @param ctx Editor context (for status messages)
 * @param url Request URL (must be http:// or https://)
 * @param method HTTP method ("GET" or "POST")
 * @param body Request body (for POST, NULL for GET)
 * @param headers Array of header strings ("Header: Value")
 * @param num_headers Number of headers
 * @param lua_callback Name of Lua function to call with response
 * @return Request ID (>= 0) on success, -1 on failure
 */
int loki_http_request(editor_ctx_t *ctx, const char *url, const char *method,
                      const char *body, const char **headers, int num_headers,
                      const char *lua_callback);

/**
 * Poll for completed HTTP requests.
 * Should be called periodically from main loop.
 * Invokes Lua callbacks for completed requests.
 *
 * @param L Lua state for callbacks
 */
void loki_http_poll(lua_State *L);

/**
 * Get number of pending HTTP requests.
 *
 * @return Number of requests in progress
 */
int loki_http_pending_count(void);

/**
 * Validate URL for HTTP request.
 * Checks scheme, length, and characters.
 *
 * @param url URL to validate
 * @param error_buf Buffer for error message (optional)
 * @param error_buf_size Size of error buffer
 * @return 1 if valid, 0 if invalid
 */
int loki_http_validate_url(const char *url, char *error_buf, size_t error_buf_size);

/**
 * Lua API: loki.async_http(url, method, body, headers, callback)
 * Registers an async HTTP request.
 */
int lua_loki_async_http(lua_State *L);

#endif /* LOKI_HTTP_H */
