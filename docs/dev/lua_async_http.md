# Lua Async HTTP API

Complete documentation for Loki's asynchronous HTTP client API.

## Overview

`loki.async_http()` provides non-blocking HTTP requests for Loki editor and REPL. The editor remains responsive while waiting for responses, making it ideal for AI completions, API integrations, and external service calls.

**Key Features:**

- Non-blocking: Editor/REPL stays responsive during requests
- Callback-based: Results delivered to your Lua function
- Multiple concurrent requests: Up to 10 simultaneous requests
- Full response access: Status codes, headers, errors
- Automatic timeout: 30 seconds per request
- Works in both editor and REPL modes

## API Reference

### Function Signature

```lua
loki.async_http(url, method, body, headers, callback) → request_id | nil, error
```

### Parameters

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `url` | string | Yes | Full URL to request (must be valid HTTP/HTTPS) |
| `method` | string | Yes | HTTP method: "GET", "POST", "PUT", "DELETE", etc. |
| `body` | string or nil | No | Request body (for POST/PUT requests) |
| `headers` | table | Yes | Array of header strings (e.g., `{"Content-Type: application/json"}`) |
| `callback` | string | Yes | Name of global Lua function to call with response |

### Return Value

- **Success**: Returns integer request ID
- **Failure**: Returns `nil` and an error message, for example `nil, "URL points at an internal address"`

### Response Format

The callback function receives a single argument: a **response table** with these fields:

```lua
{
    status = 200,              -- HTTP status code (number)
    body = "response content", -- Response body (string or nil)
    error = nil                -- Error message (string or nil)
}
```

**Response Fields:**

| Field | Type | Description |
|-------|------|-------------|
| `status` | number | HTTP status code (200, 404, 500, etc.) |
| `body` | string or nil | Response body content. `nil` if no body or request failed. |
| `error` | string or nil | Error message. Set if network error or CURL failure. `nil` on success. |

**Important Notes:**

- `response.error` is set for **network/transport errors** (connection failed, timeout, DNS failure)
- `response.error` is also set for **HTTP errors >= 400** with message like "HTTP error 404"
- Even with HTTP errors (4xx/5xx), `response.body` may contain error details from the server
- Always check both `response.error` and `response.status`

## Error Handling

### Recommended Pattern

```lua
function my_callback(response)
    -- 1. Check if response exists
    if not response then
        print("Error: No response received")
        return
    end

    -- 2. Check for errors (network or HTTP)
    if response.error then
        print("Error:", response.error)
        -- Note: response.body might still have server error details
        if response.body then
            print("Server response:", response.body)
        end
        return
    end

    -- 3. Check HTTP status
    if response.status ~= 200 then
        print(string.format("HTTP error %d", response.status))
        return
    end

    -- 4. Process successful response
    if response.body then
        print("Success:", response.body)
    else
        print("Empty response body")
    end
end
```

## Usage Examples

### Example 1: Simple GET Request

```lua
-- Define callback
function handle_response(response)
    if response and response.status == 200 and response.body then
        print("Response:", response.body)
    else
        print("Error:", response.error or "Unknown error")
    end
end

-- Make request
loki.async_http(
    "https://api.github.com/zen",
    "GET",
    nil,
    {"User-Agent: Loki-Editor"},
    "handle_response"
)
```

### Example 2: POST with JSON

```lua
function api_callback(response)
    if not response or response.error or response.status ~= 200 then
        loki.status("API call failed")
        return
    end

    loki.status("API call succeeded!")
end

local json_body = '{"name": "test", "value": 42}'
loki.async_http(
    "https://api.example.com/data",
    "POST",
    json_body,
    {
        "Content-Type: application/json",
        "Authorization: Bearer YOUR_TOKEN"
    },
    "api_callback"
)
```

### Example 3: OpenAI Integration

```lua
function openai_callback(response)
    if not response then
        loki.status("No response from OpenAI")
        return
    end

    if response.error then
        loki.status("OpenAI error: " .. response.error)
        return
    end

    if response.status ~= 200 then
        loki.status(string.format("OpenAI HTTP %d", response.status))
        -- Parse error from body if available
        if response.body then
            local err_msg = response.body:match('"message"%s*:%s*"(.-)"')
            if err_msg then
                print("API Error:", err_msg)
            end
        end
        return
    end

    -- Parse and extract content
    local content = response.body:match('"content"%s*:%s*"(.-[^\\])"')
    if content then
        -- Unescape JSON
        content = content:gsub('\\n', '\n')
        content = content:gsub('\\t', '\t')
        content = content:gsub('\\"', '"')
        loki.insert_text("\n" .. content .. "\n")
        loki.status("AI response inserted!")
    end
end

function ask_openai(prompt)
    local api_key = os.getenv("OPENAI_API_KEY")
    if not api_key then
        loki.status("OPENAI_API_KEY not set")
        return
    end

    local json_body = string.format([[{
        "model": "gpt-4o-mini",
        "messages": [{"role": "user", "content": %s}]
    }]], string.format("%q", prompt))

    loki.async_http(
        "https://api.openai.com/v1/chat/completions",
        "POST",
        json_body,
        {
            "Content-Type: application/json",
            "Authorization: Bearer " .. api_key
        },
        "openai_callback"
    )
    loki.status("Asking OpenAI...")
end
```

### Example 4: Anthropic Claude Integration

```lua
function claude_callback(response)
    if not response or response.error or response.status ~= 200 then
        local err = response and response.error or "No response"
        loki.status("Claude error: " .. err)
        return
    end

    -- Anthropic response format
    local content = response.body:match('"text"%s*:%s*"(.-[^\\])"')
    if content then
        content = content:gsub('\\n', '\n')
        loki.insert_text("\n" .. content .. "\n")
        loki.status("Claude response inserted!")
    end
end

function ask_claude(prompt)
    local api_key = os.getenv("ANTHROPIC_API_KEY")
    if not api_key then
        loki.status("ANTHROPIC_API_KEY not set")
        return
    end

    local json_body = string.format([[{
        "model": "claude-3-5-sonnet-20241022",
        "max_tokens": 1024,
        "messages": [{"role": "user", "content": %s}]
    }]], string.format("%q", prompt))

    loki.async_http(
        "https://api.anthropic.com/v1/messages",
        "POST",
        json_body,
        {
            "Content-Type: application/json",
            "x-api-key: " .. api_key,
            "anthropic-version: 2023-06-01"
        },
        "claude_callback"
    )
    loki.status("Asking Claude...")
end
```

### Example 5: Local LLM (Ollama)

```lua
function ollama_callback(response)
    if not response or response.error or response.status ~= 200 then
        loki.status("Ollama error")
        return
    end

    local content = response.body:match('"response"%s*:%s*"(.-[^\\])"')
    if content then
        content = content:gsub('\\n', '\n')
        loki.insert_text("\n" .. content .. "\n")
        loki.status("Response inserted!")
    end
end

function ask_ollama(prompt)
    local json_body = string.format([[{
        "model": "codellama",
        "prompt": %s,
        "stream": false
    }]], string.format("%q", prompt))

    loki.async_http(
        "http://localhost:11434/api/generate",
        "POST",
        json_body,
        {"Content-Type: application/json"},
        "ollama_callback"
    )
    loki.status("Asking local LLM...")
end
```

### Example 6: Multiple Provider Support

```lua
-- Generic AI interface that works with multiple providers
AI = {}

function AI.openai_handler(response)
    if not response or response.status ~= 200 then return nil end
    return response.body:match('"content"%s*:%s*"(.-[^\\])"')
end

function AI.anthropic_handler(response)
    if not response or response.status ~= 200 then return nil end
    return response.body:match('"text"%s*:%s*"(.-[^\\])"')
end

function AI.ollama_handler(response)
    if not response or response.status ~= 200 then return nil end
    return response.body:match('"response"%s*:%s*"(.-[^\\])"')
end

function AI.response_callback(response)
    local handler = AI.current_handler
    if not handler then return end

    local content = handler(response)
    if content then
        content = content:gsub('\\n', '\n'):gsub('\\t', '\t'):gsub('\\"', '"')
        loki.insert_text("\n" .. content .. "\n")
        loki.status("AI response inserted!")
    else
        loki.status("Failed to parse AI response")
    end
end

function AI.ask(provider, prompt)
    local configs = {
        openai = {
            url = "https://api.openai.com/v1/chat/completions",
            body = string.format([[{"model":"gpt-4o-mini","messages":[{"role":"user","content":%s}]}]],
                   string.format("%q", prompt)),
            headers = {
                "Content-Type: application/json",
                "Authorization: Bearer " .. os.getenv("OPENAI_API_KEY")
            },
            handler = AI.openai_handler
        },
        anthropic = {
            url = "https://api.anthropic.com/v1/messages",
            body = string.format([[{"model":"claude-3-5-sonnet-20241022","max_tokens":1024,"messages":[{"role":"user","content":%s}]}]],
                   string.format("%q", prompt)),
            headers = {
                "Content-Type: application/json",
                "x-api-key: " .. os.getenv("ANTHROPIC_API_KEY"),
                "anthropic-version: 2023-06-01"
            },
            handler = AI.anthropic_handler
        },
        ollama = {
            url = "http://localhost:11434/api/generate",
            body = string.format([[{"model":"codellama","prompt":%s,"stream":false}]],
                   string.format("%q", prompt)),
            headers = {"Content-Type: application/json"},
            handler = AI.ollama_handler
        }
    }

    local config = configs[provider]
    if not config then
        loki.status("Unknown provider: " .. provider)
        return
    end

    AI.current_handler = config.handler
    loki.async_http(config.url, "POST", config.body, config.headers, "AI.response_callback")
    loki.status(string.format("Asking %s...", provider))
end

-- Usage:
-- AI.ask("openai", "Explain this code")
-- AI.ask("anthropic", "Refactor this function")
-- AI.ask("ollama", "Add comments to this code")
```

## Best Practices

### 1. Always Check Response Validity

```lua
function safe_callback(response)
    if not response then
        -- Network failure or no response
        return
    end

    if response.error then
        -- Transport error or HTTP error
        return
    end

    -- Proceed with response.status and response.body
end
```

### 2. Handle Rate Limiting

```lua
function api_callback(response)
    if response and response.status == 429 then
        print("Rate limited - retry after delay")
        -- Could extract Retry-After header from response.body
    end
end
```

### 3. Use Environment Variables for Secrets

```lua
-- GOOD
local api_key = os.getenv("OPENAI_API_KEY")

-- BAD (hardcoded secrets)
local api_key = "sk-proj-..."
```

### 4. Implement Timeouts

The HTTP client has a 30-second timeout per request. For longer operations:

```lua
-- Split into smaller chunks
-- Use streaming APIs where available
-- Implement client-side timeout tracking
```

### 5. Parse JSON Safely

```lua
function parse_json_field(body, field)
    local pattern = string.format('"%s"%s*:%s*"(.-[^\\])"', field)
    local value = body:match(pattern)
    if value then
        -- Unescape
        value = value:gsub('\\n', '\n')
        value = value:gsub('\\t', '\t')
        value = value:gsub('\\"', '"')
        value = value:gsub('\\\\', '\\')
    end
    return value
end
```

## Technical Details

### Implementation

- Uses libcurl multi interface for async I/O
- Non-blocking: main event loop polls for completion
- In editor: polled in `editor_process_keypress()` loop
- In REPL: polled before each prompt
- Maximum 10 concurrent requests
- 30-second timeout per request
- Maximum 10MB response size

### Response Processing

1. Request initiated with `loki.async_http()`
2. curl_multi performs non-blocking work
3. On completion, response code and body are captured
4. Response table is constructed with status, body, error
5. Lua callback is invoked with response table
6. Request resources are freed

### Error Scenarios

| Scenario | `response.error` | `response.status` | `response.body` |
|----------|------------------|-------------------|-----------------|
| Success | `nil` | 200-299 | Response content |
| HTTP 404 | "HTTP error 404" | 404 | Server error page |
| Network failure | CURL error message | 0 | `nil` |
| Timeout | "Timeout" | 0 | `nil` |
| DNS failure | "Couldn't resolve host" | 0 | `nil` |

## Debugging

### Enable HTTP Tracing

```bash
# For REPL mode
KILO_DEBUG=1 ./loki-repl script.lua

# Or use flag
./loki-repl --trace-http script.lua
```

### Check Callback Registration

```lua
-- Verify callback exists
if type(_G.my_callback) == "function" then
    print("Callback registered")
end
```

### Log Response Details

```lua
function debug_callback(response)
    print("=== Response Debug ===")
    print("Type:", type(response))
    if response then
        print("Status:", response.status)
        print("Error:", response.error or "none")
        print("Body length:", response.body and #response.body or 0)
        if response.body then
            print("Body preview:", response.body:sub(1, 100))
        end
    end
end
```

## Limitations

1. **No streaming**: Response is buffered until complete
2. **No progress callbacks**: No way to track upload/download progress
3. **No custom timeouts**: Fixed 30-second timeout
4. **No header access**: Response headers are not exposed (only status code)
5. **10 concurrent limit**: Maximum 10 simultaneous requests
6. **10MB response limit**: Responses larger than 10MB are truncated

## Future Enhancements

Potential improvements for future versions:

- Response header access
- Configurable timeouts per request
- Streaming support for large responses
- Progress callbacks
- Request cancellation
- Custom SSL/TLS options
- Proxy support
- Cookie handling
- Redirect control

## See Also

- [AI Integration Module](../.loki/modules/ai.lua) - Reference implementation
- [CLAUDE.md](../CLAUDE.md) - Full Loki documentation
- [libcurl documentation](https://curl.se/libcurl/) - Underlying HTTP library
