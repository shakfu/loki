/* http.c - Async HTTP support for Loki
 *
 * Implementation of asynchronous HTTP requests using libcurl.
 */

/* inet_aton is BSD, hidden by the strict _POSIX_C_SOURCE set in CMake. */
#define _DEFAULT_SOURCE
#include "http.h"
#include "internal.h"
#include "loki/lua.h"
#include <lua.h>
#include <lauxlib.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>
#include <strings.h>  /* strcasecmp */
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* Response data structure */
typedef struct {
    char *data;
    size_t size;
} http_response_t;

/* Async request tracking */
typedef struct {
    CURLM *multi_handle;
    CURL *easy_handle;
    http_response_t response;
    char *lua_callback;
    struct curl_slist *header_list;
    int completed;
    int failed;
    char error_buffer[CURL_ERROR_SIZE];
} async_http_request_t;

/* Module state */
static async_http_request_t *pending_requests[LOKI_HTTP_MAX_ASYNC_REQUESTS] = {0};
static int num_pending = 0;
static int curl_initialized = 0;

/* Rate limiting state */
static time_t rate_limit_window_start = 0;
static int rate_limit_count = 0;

/* ======================= Internal Functions ======================= */

/* Curl write callback */
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    http_response_t *resp = (http_response_t *)userp;

    /* Check size limit */
    if (resp->size + realsize > LOKI_HTTP_MAX_RESPONSE_SIZE) {
        return 0;  /* Abort - response too large */
    }

    char *ptr = realloc(resp->data, resp->size + realsize + 1);
    if (!ptr) {
        return 0;  /* Out of memory */
    }

    resp->data = ptr;
    memcpy(&(resp->data[resp->size]), contents, realsize);
    resp->size += realsize;
    resp->data[resp->size] = '\0';

    return realsize;
}

/* Detect CA bundle path */
static const char *detect_ca_bundle_path(void) {
    static const char *ca_paths[] = {
        "/etc/ssl/cert.pem",                      /* macOS */
        "/etc/ssl/certs/ca-certificates.crt",     /* Debian/Ubuntu */
        "/etc/pki/tls/certs/ca-bundle.crt",       /* Fedora/RHEL */
        "/etc/ssl/ca-bundle.pem",                 /* OpenSUSE */
        "/etc/ssl/certs/ca-bundle.crt",           /* Old Red Hat */
        "/usr/local/share/certs/ca-root-nss.crt", /* FreeBSD */
        NULL
    };

    for (int i = 0; ca_paths[i] != NULL; i++) {
        if (access(ca_paths[i], R_OK) == 0) {
            return ca_paths[i];
        }
    }

    return NULL;
}

/* Check rate limiting */
static int check_rate_limit(void) {
    time_t now = time(NULL);

    /* Reset window if expired */
    if (now - rate_limit_window_start >= 60) {
        rate_limit_window_start = now;
        rate_limit_count = 0;
    }

    /* Check limit */
    if (rate_limit_count >= LOKI_HTTP_RATE_LIMIT) {
        return 0;  /* Rate limited */
    }

    rate_limit_count++;
    return 1;
}

/* ======================= Public API ======================= */

void loki_http_init(void) {
    if (!curl_initialized) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl_initialized = 1;
    }
}

void loki_http_cleanup(void) {
    /* Cancel all pending requests */
    for (int i = 0; i < LOKI_HTTP_MAX_ASYNC_REQUESTS; i++) {
        async_http_request_t *req = pending_requests[i];
        if (req) {
            curl_multi_remove_handle(req->multi_handle, req->easy_handle);
            curl_easy_cleanup(req->easy_handle);
            curl_multi_cleanup(req->multi_handle);
            if (req->header_list) {
                curl_slist_free_all(req->header_list);
            }
            free(req->response.data);
            free(req->lua_callback);
            free(req);
            pending_requests[i] = NULL;
        }
    }
    num_pending = 0;

    if (curl_initialized) {
        curl_global_cleanup();
        curl_initialized = 0;
    }
}

/* Reject a header that could inject extra headers or confuse the parser. */
int loki_http_validate_header(const char *header, char *error_buf, size_t error_buf_size) {
    if (!header) {
        if (error_buf) snprintf(error_buf, error_buf_size, "Header is not a string");
        return 0;
    }

    size_t len = strlen(header);
    if (len == 0) {
        if (error_buf) snprintf(error_buf, error_buf_size, "Header is empty");
        return 0;
    }
    if (len > LOKI_HTTP_MAX_HEADER_SIZE) {
        if (error_buf) snprintf(error_buf, error_buf_size,
                                "Header too long (max %d)", LOKI_HTTP_MAX_HEADER_SIZE);
        return 0;
    }

    /* CR/LF would terminate the header and start another one; other control
     * characters are not valid in a field value either. */
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)header[i];
        if (c == '\r' || c == '\n' || c < 32 || c == 127) {
            if (error_buf) snprintf(error_buf, error_buf_size,
                                    "Header contains control characters");
            return 0;
        }
    }

    /* Must look like "Name: value" with a non-empty, token-only name. */
    const char *colon = strchr(header, ':');
    if (!colon || colon == header) {
        if (error_buf) snprintf(error_buf, error_buf_size,
                                "Header must be in 'Name: value' form");
        return 0;
    }
    for (const char *q = header; q < colon; q++) {
        unsigned char c = (unsigned char)*q;
        if (!isalnum(c) && c != '-' && c != '_') {
            if (error_buf) snprintf(error_buf, error_buf_size,
                                    "Header name contains invalid characters");
            return 0;
        }
    }

    return 1;
}

static int allow_internal(void) {
    return getenv("LOKI_HTTP_ALLOW_INTERNAL") != NULL;
}

/* Loopback, private, link-local (incl. cloud metadata), CGNAT, multicast and
 * reserved ranges. `a` is in host byte order. */
static int ipv4_is_internal(uint32_t a) {
    unsigned int o1 = a >> 24, o2 = (a >> 16) & 0xff;
    return o1 == 0 || o1 == 10 || o1 == 127 || o1 >= 224 ||
           (o1 == 100 && (o2 & 0xc0) == 64) ||
           (o1 == 169 && o2 == 254) ||
           (o1 == 172 && (o2 & 0xf0) == 16) ||
           (o1 == 192 && o2 == 168);
}

static int ipv6_is_internal(const unsigned char *b) {
    static const unsigned char zero[12] = {0};
    static const unsigned char mapped[12] = {0,0,0,0,0,0,0,0,0,0,0xff,0xff};
    static const unsigned char nat64[12] = {0,0x64,0xff,0x9b};
    /* ::/96 (::, ::1, IPv4-compatible), ::ffff:0:0/96 and 64:ff9b::/96 all
     * embed an IPv4 address in the low 32 bits. */
    if (!memcmp(b, zero, 12) || !memcmp(b, mapped, 12) || !memcmp(b, nat64, 12)) {
        uint32_t v4 = ((uint32_t)b[12] << 24) | ((uint32_t)b[13] << 16) |
                      ((uint32_t)b[14] << 8) | b[15];
        if (!memcmp(b, zero, 12) && v4 == 1) return 1;   /* ::1 */
        return ipv4_is_internal(v4);
    }
    return (b[0] & 0xfe) == 0xfc ||                   /* unique-local fc00::/7 */
           (b[0] == 0xfe && b[1] >= 0x80) ||          /* link/site-local */
           b[0] == 0xff;                              /* multicast */
}

int loki_http_sockaddr_is_internal(const struct sockaddr *sa) {
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
        return ipv4_is_internal(ntohl(in->sin_addr.s_addr));
    }
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;
        return ipv6_is_internal(in6->sin6_addr.s6_addr);
    }
    return 1;  /* unknown family: refuse */
}

/* Is the URL host an internal name or address literal? inet_aton accepts the
 * same shorthand the resolver does (127.1, 0x7f000001, 0177.0.0.1). Names that
 * merely resolve to internal addresses are caught at connect time. */
static int host_is_internal(const char *host, size_t hostlen) {
    char buf[256];
    if (hostlen == 0 || hostlen >= sizeof(buf)) return 0;
    memcpy(buf, host, hostlen);
    buf[hostlen] = '\0';

    /* Strip an IPv6 bracket pair. */
    char *h = buf;
    if (h[0] == '[') {
        h++;
        char *close = strchr(h, ']');
        if (close) *close = '\0';
    }

    size_t n = strlen(h);
    while (n > 0 && h[n - 1] == '.') h[--n] = '\0';  /* "localhost." */
    if (strcasecmp(h, "localhost") == 0) return 1;
    if (n > 10 && strcasecmp(h + n - 10, ".localhost") == 0) return 1;

    struct in_addr v4;
    struct in6_addr v6;
    if (inet_pton(AF_INET6, h, &v6) == 1) return ipv6_is_internal(v6.s6_addr);
    if (inet_aton(h, &v4)) return ipv4_is_internal(ntohl(v4.s_addr));
    return 0;
}

/* Refuse connections to internal addresses. curl calls this for every
 * connection, so it covers redirects and names resolving to internal hosts. */
static curl_socket_t open_socket_callback(void *clientp, curlsocktype purpose,
                                          struct curl_sockaddr *address) {
    (void)clientp;
    (void)purpose;
    if (!allow_internal() && loki_http_sockaddr_is_internal(&address->addr))
        return CURL_SOCKET_BAD;
    return socket(address->family, address->socktype, address->protocol);
}

int loki_http_validate_url(const char *url, char *error_buf, size_t error_buf_size) {
    if (!url || url[0] == '\0') {
        if (error_buf) snprintf(error_buf, error_buf_size, "URL is empty");
        return 0;
    }

    /* Check length */
    size_t len = strlen(url);
    if (len > LOKI_HTTP_MAX_URL_LENGTH) {
        if (error_buf) snprintf(error_buf, error_buf_size, "URL too long (max %d)", LOKI_HTTP_MAX_URL_LENGTH);
        return 0;
    }

    /* Check scheme - must be http:// or https:// */
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        if (error_buf) snprintf(error_buf, error_buf_size, "URL must use http:// or https://");
        return 0;
    }

    /* Check for control characters */
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)url[i];
        if (c < 32 || c == 127) {
            if (error_buf) snprintf(error_buf, error_buf_size, "URL contains invalid characters");
            return 0;
        }
    }

    /* Reject internal hosts unless explicitly allowed. */
    if (!allow_internal()) {
        const char *host = strstr(url, "://");
        host = host ? host + 3 : url;
        const char *at = strchr(host, '@');       /* skip userinfo */
        size_t authlen = strcspn(host, "/?#");
        if (at && (size_t)(at - host) < authlen) {
            host = at + 1;
            authlen = strcspn(host, "/?#");
        }
        size_t hostlen = authlen;
        if (host[0] == '[') {                     /* IPv6 literal */
            const char *close = memchr(host, ']', authlen);
            if (close) hostlen = (size_t)(close - host) + 1;
        } else {
            const char *colon = memchr(host, ':', authlen);
            if (colon) hostlen = (size_t)(colon - host);
        }
        if (host_is_internal(host, hostlen)) {
            if (error_buf) snprintf(error_buf, error_buf_size,
                                    "URL points at an internal address");
            return 0;
        }
    }

    return 1;
}

/* Report a request failure in the status bar and in error_buf. */
static int request_failed(editor_ctx_t *ctx, char *error_buf, size_t error_buf_size,
                          const char *msg) {
    if (ctx) editor_set_status_msg(ctx, "%s", msg);
    if (error_buf && error_buf_size) snprintf(error_buf, error_buf_size, "%s", msg);
    return -1;
}

int loki_http_request(editor_ctx_t *ctx, const char *url, const char *method,
                      const char *body, const char **headers, int num_headers,
                      const char *lua_callback, char *error_buf, size_t error_buf_size) {
    char msg[256];

    /* Validate URL */
    if (!loki_http_validate_url(url, msg, sizeof(msg)))
        return request_failed(ctx, error_buf, error_buf_size, msg);

    /* Check body size */
    if (body && strlen(body) > LOKI_HTTP_MAX_BODY_SIZE)
        return request_failed(ctx, error_buf, error_buf_size, "Request body too large");

    /* Check rate limit */
    if (!check_rate_limit())
        return request_failed(ctx, error_buf, error_buf_size, "Rate limit exceeded");

    /* Check max pending requests */
    if (num_pending >= LOKI_HTTP_MAX_ASYNC_REQUESTS)
        return request_failed(ctx, error_buf, error_buf_size, "Too many pending requests");

    loki_http_init();

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < LOKI_HTTP_MAX_ASYNC_REQUESTS; i++) {
        if (!pending_requests[i]) {
            slot = i;
            break;
        }
    }
    if (slot == -1)
        return request_failed(ctx, error_buf, error_buf_size, "Too many pending requests");

    /* Allocate request */
    async_http_request_t *req = malloc(sizeof(async_http_request_t));
    if (!req) return request_failed(ctx, error_buf, error_buf_size, "Out of memory");

    memset(req, 0, sizeof(async_http_request_t));
    req->response.data = malloc(1);
    if (!req->response.data) {
        free(req);
        return request_failed(ctx, error_buf, error_buf_size, "Out of memory");
    }
    req->response.data[0] = '\0';
    req->response.size = 0;

    req->lua_callback = strdup(lua_callback);
    if (!req->lua_callback) {
        free(req->response.data);
        free(req);
        return request_failed(ctx, error_buf, error_buf_size, "Out of memory");
    }

    /* Initialize curl handles */
    req->easy_handle = curl_easy_init();
    if (!req->easy_handle) {
        free(req->response.data);
        free(req->lua_callback);
        free(req);
        return request_failed(ctx, error_buf, error_buf_size, "Out of memory");
    }

    req->multi_handle = curl_multi_init();
    if (!req->multi_handle) {
        curl_easy_cleanup(req->easy_handle);
        free(req->response.data);
        free(req->lua_callback);
        free(req);
        return request_failed(ctx, error_buf, error_buf_size, "Out of memory");
    }

    /* Set curl options */
    curl_easy_setopt(req->easy_handle, CURLOPT_URL, url);
    curl_easy_setopt(req->easy_handle, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(req->easy_handle, CURLOPT_WRITEDATA, &req->response);
    curl_easy_setopt(req->easy_handle, CURLOPT_ERRORBUFFER, req->error_buffer);
    curl_easy_setopt(req->easy_handle, CURLOPT_TIMEOUT, (long)LOKI_HTTP_TIMEOUT);
    curl_easy_setopt(req->easy_handle, CURLOPT_CONNECTTIMEOUT, (long)LOKI_HTTP_CONNECT_TIMEOUT);
    curl_easy_setopt(req->easy_handle, CURLOPT_OPENSOCKETFUNCTION, open_socket_callback);
    curl_easy_setopt(req->easy_handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(req->easy_handle, CURLOPT_MAXREDIRS,
                     (long)LOKI_HTTP_MAX_REDIRECTS);
    /* Without this a redirect can hop to file://, gopher://, etc. */
#if LIBCURL_VERSION_NUM >= 0x075500  /* 7.85.0 */
    curl_easy_setopt(req->easy_handle, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(req->easy_handle, CURLOPT_REDIR_PROTOCOLS,
                     (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif

    /* SSL/TLS settings */
    curl_easy_setopt(req->easy_handle, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(req->easy_handle, CURLOPT_SSL_VERIFYHOST, 2L);

    const char *ca_bundle = detect_ca_bundle_path();
    if (ca_bundle) {
        curl_easy_setopt(req->easy_handle, CURLOPT_CAINFO, ca_bundle);
    }

    /* Debug mode. LOKI_DEBUG alone must not dump request headers: curl's
     * verbose output includes Authorization. Require an explicit opt-in. */
    if (getenv("LOKI_DEBUG") && getenv("LOKI_HTTP_TRACE_INSECURE")) {
        curl_easy_setopt(req->easy_handle, CURLOPT_VERBOSE, 1L);
    }

    /* Set method */
    if (method && strcmp(method, "POST") == 0) {
        curl_easy_setopt(req->easy_handle, CURLOPT_POST, 1L);
        if (body) {
            /* The API takes a NUL-terminated body, so embedded NULs are not
             * representable here; set the size explicitly all the same. */
            size_t body_len = strlen(body);
            curl_easy_setopt(req->easy_handle, CURLOPT_POSTFIELDSIZE,
                             (long)body_len);
            /* COPYPOSTFIELDS (not POSTFIELDS): curl takes its own copy, so the
             * buffer stays valid for the whole async transfer even after the
             * caller's string is gone. */
            curl_easy_setopt(req->easy_handle, CURLOPT_COPYPOSTFIELDS, body);
        }
    } else if (method && strcmp(method, "GET") != 0) {
        curl_easy_setopt(req->easy_handle, CURLOPT_CUSTOMREQUEST, method);
    }

    /* Set headers */
    req->header_list = NULL;
    if (headers && num_headers > 0) {
        for (int i = 0; i < num_headers; i++) {
            req->header_list = curl_slist_append(req->header_list, headers[i]);
            if (!req->header_list) break;
        }
        curl_easy_setopt(req->easy_handle, CURLOPT_HTTPHEADER, req->header_list);
    }

    /* Add to multi handle */
    if (curl_multi_add_handle(req->multi_handle, req->easy_handle) != CURLM_OK) {
        /* Otherwise the request would sit in pending_requests[] forever. */
        curl_slist_free_all(req->header_list);
        curl_multi_cleanup(req->multi_handle);
        curl_easy_cleanup(req->easy_handle);
        free(req->response.data);
        free(req->lua_callback);
        free(req);
        return request_failed(ctx, error_buf, error_buf_size,
                              "HTTP request failed to start");
    }

    /* Store request */
    pending_requests[slot] = req;
    num_pending++;

    if (ctx) editor_set_status_msg(ctx, "HTTP request sent...");

    return slot;
}

int loki_http_poll(lua_State *L) {
    int completed = 0;
    for (int i = 0; i < LOKI_HTTP_MAX_ASYNC_REQUESTS; i++) {
        async_http_request_t *req = pending_requests[i];
        if (!req) continue;

        /* Perform non-blocking work */
        int still_running = 0;
        curl_multi_perform(req->multi_handle, &still_running);

        if (still_running == 0) {
            /* Request completed */
            int msgs_left = 0;
            CURLMsg *msg = NULL;

            while ((msg = curl_multi_info_read(req->multi_handle, &msgs_left))) {
                if (msg->msg == CURLMSG_DONE) {
                    if (msg->data.result != CURLE_OK) {
                        req->failed = 1;
                        if (req->error_buffer[0] == '\0') {
                            snprintf(req->error_buffer, CURL_ERROR_SIZE, "%s",
                                     curl_easy_strerror(msg->data.result));
                        }
                    }
                }
            }

            /* Get response code */
            long response_code = 0;
            curl_easy_getinfo(req->easy_handle, CURLINFO_RESPONSE_CODE, &response_code);

            /* Call Lua callback */
            if (L && req->lua_callback) {
                lua_getglobal(L, req->lua_callback);
                if (lua_isfunction(L, -1)) {
                    /* Create response table */
                    lua_newtable(L);

                    lua_pushinteger(L, (lua_Integer)response_code);
                    lua_setfield(L, -2, "status");

                    if (req->response.data && req->response.size > 0) {
                        lua_pushstring(L, req->response.data);
                    } else {
                        lua_pushnil(L);
                    }
                    lua_setfield(L, -2, "body");

                    if (req->failed && req->error_buffer[0] != '\0') {
                        lua_pushstring(L, req->error_buffer);
                    } else if (response_code >= 400) {
                        char errbuf[128];
                        snprintf(errbuf, sizeof(errbuf), "HTTP error %ld", response_code);
                        lua_pushstring(L, errbuf);
                    } else {
                        lua_pushnil(L);
                    }
                    lua_setfield(L, -2, "error");

                    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                        const char *err = lua_tostring(L, -1);
                        fprintf(stderr, "HTTP callback error: %s\n", err);
                        lua_pop(L, 1);
                    }
                } else {
                    lua_pop(L, 1);
                }
            }

            /* Cleanup request */
            curl_multi_remove_handle(req->multi_handle, req->easy_handle);
            curl_easy_cleanup(req->easy_handle);
            curl_multi_cleanup(req->multi_handle);
            if (req->header_list) {
                curl_slist_free_all(req->header_list);
            }
            free(req->response.data);
            free(req->lua_callback);
            free(req);

            pending_requests[i] = NULL;
            num_pending--;
            completed++;
        }
    }
    return completed;
}

int loki_http_pending_count(void) {
    return num_pending;
}

/* ======================= Lua Binding ======================= */

/* Push the (nil, message) failure result. */
static int push_http_error(lua_State *L, const char *msg) {
    lua_pushnil(L);
    lua_pushstring(L, msg);
    return 2;
}

int lua_loki_async_http(lua_State *L) {
    /* C sees each string up to its first NUL, so a NUL would silently
     * truncate what is validated and sent. */
    size_t url_len, body_len = 0;
    const char *url = luaL_checklstring(L, 1, &url_len);
    const char *method = luaL_optstring(L, 2, "GET");
    const char *body = lua_isnil(L, 3) ? NULL : luaL_optlstring(L, 3, NULL, &body_len);
    if (strlen(url) != url_len) return push_http_error(L, "URL contains a NUL byte");
    if (body && strlen(body) != body_len)
        return push_http_error(L, "Request body contains a NUL byte");

    char error_buf[256];
    if (!loki_http_validate_url(url, error_buf, sizeof(error_buf)))
        return push_http_error(L, error_buf);

    if (body && body_len > LOKI_HTTP_MAX_BODY_SIZE)
        return push_http_error(L, "Request body too large");

    /* Check the callback before allocating anything: luaL_checkstring
     * longjmps on a bad argument, which would leak the headers array. */
    const char *callback = luaL_checkstring(L, 5);

    /* Parse headers table */
    const char **headers = NULL;
    int num_headers = 0;

    if (lua_istable(L, 4)) {
        /* Count headers */
        lua_pushnil(L);
        while (lua_next(L, 4) != 0) {
            num_headers++;
            lua_pop(L, 1);
        }

        if (num_headers > LOKI_HTTP_MAX_HEADERS) {
            snprintf(error_buf, sizeof(error_buf), "Too many headers (max %d)",
                     LOKI_HTTP_MAX_HEADERS);
            return push_http_error(L, error_buf);
        }

        if (num_headers > 0) {
            headers = malloc(sizeof(char*) * num_headers);
            if (!headers) return push_http_error(L, "Out of memory");
            int i = 0;
            size_t total = 0;
            int bad = 0;
            lua_pushnil(L);
            while (lua_next(L, 4) != 0) {
                /* lua_tostring returns NULL for a non-string value; strdup(NULL)
                 * is undefined and used to crash here. */
                size_t value_len = 0;
                const char *value = lua_isstring(L, -1)
                                    ? lua_tolstring(L, -1, &value_len) : NULL;
                char *header_str = NULL;
                if (value && strlen(value) != value_len) {
                    snprintf(error_buf, sizeof(error_buf), "Header contains a NUL byte");
                    bad = 1;
                } else if (!loki_http_validate_header(value, error_buf, sizeof(error_buf))) {
                    bad = 1;
                } else if ((total += value_len) > LOKI_HTTP_MAX_HEADERS_TOTAL) {
                    snprintf(error_buf, sizeof(error_buf),
                             "Headers too large (max %d bytes)", LOKI_HTTP_MAX_HEADERS_TOTAL);
                    bad = 1;
                } else if (!(header_str = strdup(value))) {
                    snprintf(error_buf, sizeof(error_buf), "Out of memory");
                    bad = 1;
                }
                if (bad) {
                    lua_pop(L, 2);  /* value + key: stop iterating */
                    break;
                }
                headers[i++] = header_str;
                lua_pop(L, 1);
            }
            num_headers = i;
            if (bad) {
                for (int j = 0; j < i; j++) {
                    free((void*)headers[j]);
                }
                free(headers);
                return push_http_error(L, error_buf);
            }
        }
    }

    /* Editor context. The registry key "loki_ctx" was never set by anything,
     * so ctx was always NULL and every status message here was dropped. */
    editor_ctx_t *ctx = loki_lua_get_editor_context(L);

    /* Start request */
    int req_id = loki_http_request(ctx, url, method, body, headers, num_headers, callback,
                                   error_buf, sizeof(error_buf));

    /* Free headers */
    if (headers) {
        for (int i = 0; i < num_headers; i++) {
            free((void*)headers[i]);
        }
        free(headers);
    }

    if (req_id < 0) return push_http_error(L, error_buf);
    lua_pushinteger(L, req_id);
    return 1;
}
