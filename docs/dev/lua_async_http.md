# Lua async HTTP API

**Status:** current as of 2026-10-28.

`loki.async_http()` is available only when Loki is built with `-DLOKI_ENABLE_HTTP=ON` (libcurl). Default builds omit it. Limits and SSRF hardening are documented in `docs/security.md`; constants live in `src/http.h`.

## Signature

```lua
loki.async_http(url, method, body, headers, callback) → request_id | nil, error
```

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `url` | string | yes | `http://` or `https://` only |
| `method` | string | yes | e.g. `"GET"`, `"POST"` |
| `body` | string or nil | no | Request body |
| `headers` | table | yes | Array of `"Name: value"` strings (may be `{}`) |
| `callback` | string | yes | Global Lua function name invoked with the response table |

Success returns a request id. Rejection returns `nil, message` (also reflected in the status bar for some limits).

### Response table

```lua
{
    status = 200,    -- number; 0 on transport failure
    body = "...",    -- string or nil
    error = nil,     -- set on transport failure or HTTP status >= 400
}
```

Always check `response` and `response.error` before using `body`.

## Limits (compile-time)

| Limit | Value |
|-------|--------|
| Pending requests | 10 |
| Request timeout | 60 s |
| Connect timeout | 10 s |
| Response size | 10 MB (transfer aborts above) |
| Request body | 5 MB |
| Rate limit | 60 requests / 60 s (process-wide) |
| Redirects | 5, http/https only |

Internal hosts are blocked in the URL and again at connect time. Loopback HTTP proxies usually need `LOKI_HTTP_ALLOW_INTERNAL=1`. See `docs/security.md`.

## Example

```lua
function handle_zen(response)
    if not response or response.error then
        loki.status("HTTP failed: " .. tostring(response and response.error))
        return
    end
    if response.status == 200 and response.body then
        loki.insert_text(response.body)
    end
end

local id, err = loki.async_http(
    "https://api.github.com/zen",
    "GET",
    nil,
    {"User-Agent: loki"},
    "handle_zen"
)
if not id then
    loki.status("rejected: " .. tostring(err))
end
```

Bundled helpers: `.loki/modules/ai.lua`, `.loki/modules/example.lua` (require a trusted project config and an HTTP-enabled binary). Prefer `os.getenv` for API keys.

## Debugging

```bash
LOKI_DEBUG=1 ./build/loki file.lua
# In-editor: Ctrl-L, then exercise loki.async_http
# Header dumps also need LOKI_HTTP_TRACE_INSECURE (never in shared logs)
```

## Tests

```bash
make test-http   # includes test_http_security
```
