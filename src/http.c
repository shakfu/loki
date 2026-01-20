/* http.c - Async HTTP support for Loki
 *
 * Implementation of asynchronous HTTP requests using libcurl.
 */

#include "http.h"
#include "internal.h"
#include <lua.h>
#include <lauxlib.h>
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>

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

    return 1;
}

int loki_http_request(editor_ctx_t *ctx, const char *url, const char *method,
                      const char *body, const char **headers, int num_headers,
                      const char *lua_callback) {
    char error_buf[256];

    /* Validate URL */
    if (!loki_http_validate_url(url, error_buf, sizeof(error_buf))) {
        if (ctx) editor_set_status_msg(ctx, "%s", error_buf);
        return -1;
    }

    /* Check body size */
    if (body && strlen(body) > LOKI_HTTP_MAX_BODY_SIZE) {
        if (ctx) editor_set_status_msg(ctx, "Request body too large");
        return -1;
    }

    /* Check rate limit */
    if (!check_rate_limit()) {
        if (ctx) editor_set_status_msg(ctx, "Rate limit exceeded");
        return -1;
    }

    /* Check max pending requests */
    if (num_pending >= LOKI_HTTP_MAX_ASYNC_REQUESTS) {
        if (ctx) editor_set_status_msg(ctx, "Too many pending requests");
        return -1;
    }

    loki_http_init();

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < LOKI_HTTP_MAX_ASYNC_REQUESTS; i++) {
        if (!pending_requests[i]) {
            slot = i;
            break;
        }
    }
    if (slot == -1) return -1;

    /* Allocate request */
    async_http_request_t *req = malloc(sizeof(async_http_request_t));
    if (!req) return -1;

    memset(req, 0, sizeof(async_http_request_t));
    req->response.data = malloc(1);
    if (!req->response.data) {
        free(req);
        return -1;
    }
    req->response.data[0] = '\0';
    req->response.size = 0;

    req->lua_callback = strdup(lua_callback);
    if (!req->lua_callback) {
        free(req->response.data);
        free(req);
        return -1;
    }

    /* Initialize curl handles */
    req->easy_handle = curl_easy_init();
    if (!req->easy_handle) {
        free(req->response.data);
        free(req->lua_callback);
        free(req);
        return -1;
    }

    req->multi_handle = curl_multi_init();
    if (!req->multi_handle) {
        curl_easy_cleanup(req->easy_handle);
        free(req->response.data);
        free(req->lua_callback);
        free(req);
        return -1;
    }

    /* Set curl options */
    curl_easy_setopt(req->easy_handle, CURLOPT_URL, url);
    curl_easy_setopt(req->easy_handle, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(req->easy_handle, CURLOPT_WRITEDATA, &req->response);
    curl_easy_setopt(req->easy_handle, CURLOPT_ERRORBUFFER, req->error_buffer);
    curl_easy_setopt(req->easy_handle, CURLOPT_TIMEOUT, (long)LOKI_HTTP_TIMEOUT);
    curl_easy_setopt(req->easy_handle, CURLOPT_CONNECTTIMEOUT, (long)LOKI_HTTP_CONNECT_TIMEOUT);
    curl_easy_setopt(req->easy_handle, CURLOPT_FOLLOWLOCATION, 1L);

    /* SSL/TLS settings */
    curl_easy_setopt(req->easy_handle, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(req->easy_handle, CURLOPT_SSL_VERIFYHOST, 2L);

    const char *ca_bundle = detect_ca_bundle_path();
    if (ca_bundle) {
        curl_easy_setopt(req->easy_handle, CURLOPT_CAINFO, ca_bundle);
    }

    /* Debug mode */
    if (getenv("LOKI_DEBUG")) {
        curl_easy_setopt(req->easy_handle, CURLOPT_VERBOSE, 1L);
    }

    /* Set method */
    if (method && strcmp(method, "POST") == 0) {
        curl_easy_setopt(req->easy_handle, CURLOPT_POST, 1L);
        if (body) {
            curl_easy_setopt(req->easy_handle, CURLOPT_POSTFIELDS, body);
        }
    }

    /* Set headers */
    req->header_list = NULL;
    if (headers && num_headers > 0) {
        for (int i = 0; i < num_headers; i++) {
            req->header_list = curl_slist_append(req->header_list, headers[i]);
        }
        curl_easy_setopt(req->easy_handle, CURLOPT_HTTPHEADER, req->header_list);
    }

    /* Add to multi handle */
    curl_multi_add_handle(req->multi_handle, req->easy_handle);

    /* Store request */
    pending_requests[slot] = req;
    num_pending++;

    if (ctx) editor_set_status_msg(ctx, "HTTP request sent...");

    return slot;
}

void loki_http_poll(lua_State *L) {
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
        }
    }
}

int loki_http_pending_count(void) {
    return num_pending;
}

/* ======================= Lua Binding ======================= */

int lua_loki_async_http(lua_State *L) {
    const char *url = luaL_checkstring(L, 1);
    const char *method = luaL_optstring(L, 2, "GET");
    const char *body = lua_isnil(L, 3) ? NULL : luaL_optstring(L, 3, NULL);

    /* Validate URL first */
    char error_buf[256];
    if (!loki_http_validate_url(url, error_buf, sizeof(error_buf))) {
        /* Return nil for invalid URLs */
        lua_pushnil(L);
        return 1;
    }

    /* Validate body size */
    if (body && strlen(body) > LOKI_HTTP_MAX_BODY_SIZE) {
        lua_pushnil(L);
        return 1;
    }

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

        if (num_headers > 0) {
            headers = malloc(sizeof(char*) * num_headers);
            if (!headers) {
                return luaL_error(L, "Out of memory");
            }
            int i = 0;
            lua_pushnil(L);
            while (lua_next(L, 4) != 0) {
                const char *header_str = strdup(lua_tostring(L, -1));
                if (!header_str) {
                    for (int j = 0; j < i; j++) {
                        free((void*)headers[j]);
                    }
                    free(headers);
                    return luaL_error(L, "Out of memory");
                }
                headers[i++] = header_str;
                lua_pop(L, 1);
            }
        }
    }

    const char *callback = luaL_checkstring(L, 5);

    /* Get editor context from Lua registry */
    lua_getfield(L, LUA_REGISTRYINDEX, "loki_ctx");
    editor_ctx_t *ctx = lua_touserdata(L, -1);
    lua_pop(L, 1);

    /* Start request */
    int req_id = loki_http_request(ctx, url, method, body, headers, num_headers, callback);

    /* Free headers */
    if (headers) {
        for (int i = 0; i < num_headers; i++) {
            free((void*)headers[i]);
        }
        free(headers);
    }

    if (req_id >= 0) {
        lua_pushinteger(L, req_id);
    } else {
        lua_pushnil(L);
    }

    return 1;
}
