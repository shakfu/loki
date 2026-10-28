# Loki Lua Configuration Example

Bundled Lua configuration for loki (init, modules, languages, themes). Copy this tree into a project as `.loki/`, or install pieces under `~/.loki/`.

## Setup

```bash
# Project-local (then :trust inside loki and restart)
cp -r /path/to/loki/.loki .loki

# Or global
mkdir -p ~/.loki/modules
cp init.lua ~/.loki/
cp -r modules/* ~/.loki/modules/
```

## Configuration Priority

1. `.loki/init.lua` — only if the directory is trusted (`:trust` → `~/.loki/trusted`)
2. otherwise `~/.loki/init.lua`

Trusted local config replaces the global one (they are not merged).

## Available Helpers

From the docked REPL (`Ctrl-L`), after config loads:

- `count_lines()`, `insert_timestamp()`, `editor.*` — see `modules/editor.lua`
- `require("acp").ask(text)` / `require("mcp").start()` — the functions behind `:agent` and `:mcp`
- `test.http` / example fetch helpers — see `modules/test.lua`, `modules/example.lua`

## Agents

`:agent <prompt>` runs a coding agent over ACP (`modules/acp.lua`). `:mcp` lets an agent read and edit buffers over MCP (`modules/mcp.lua`). See "Agents over ACP" and "Agents over MCP" in the top-level README.

## Lua API

Access editor functionality via the `loki` global table:

### Synchronous Functions

- `loki.status(msg)` - Set status bar message
- `loki.get_lines()` - Get total line count
- `loki.get_line(row)` - Get line content (0-indexed)
- `loki.get_cursor()` - Get cursor position (row, col)
- `loki.insert_text(text)` - Insert text at cursor
- `loki.get_filename()` - Get current filename

### Async HTTP Function

```lua
local id, err = loki.async_http(url, method, body, headers, callback)
```

Returns a request id, or `nil` and an error message if the request is rejected.

**Parameters:**
- `url` (string): The URL to request
- `method` (string): HTTP method ("GET", "POST", etc.)
- `body` (string or nil): Request body for POST requests
- `headers` (table): Array of header strings (e.g., `{"Content-Type: application/json"}`)
- `callback` (string): Name of a global Lua function. It receives a table with `status`, `body` and `error`.

**Example:**
```lua
function my_callback(response)
    if response.error then
        loki.status("Error: " .. response.error)
    else
        loki.status("Got: " .. response.body)
    end
end

local id, err = loki.async_http(
    "https://api.example.com/data",
    "GET",
    nil,
    {"User-Agent: loki"},
    "my_callback"
)
if not id then loki.status("Error: " .. err) end
```

## Lua Highlight Hook

Define `loki.highlight_row(row_index, line_text, render_text, syntax_type, default_applied)`
to extend or replace built-in syntax highlighting from Lua.

- Return `nil`/`false` to leave the C highlighter output unchanged.
- Return an array of span tables (or `{spans = {...}}`) to colour specific
  regions. Each span supports `start` (1-based column), optional `stop` or
  `length`, and `style`/`type` set to a `loki.hl` constant name (e.g. `"match"`).
- Include `replace = true` alongside `spans` if you want to clear the existing
  highlight before applying your spans.
- `syntax_type` mirrors the internal `HL_TYPE_*` identifiers (`0` for C,
  `1` for Markdown, etc.), while `default_applied` tells you whether the stock
  highlighter already ran so you can layer additional spans.

The bundled `.loki/init.lua` shows how to colour Markdown headings, inline
code, checkboxes, and TODO/FIXME tags:

```lua
local HL_TYPE_MARKDOWN = 1
local TODO_TAGS = { "TODO", "FIXME" }

local function add_span(spans, start_col, length, style)
    if start_col and length and length > 0 then
        table.insert(spans, { start = start_col, length = length, style = style })
    end
end

function loki.highlight_row(idx, text, render, syntax_type, default_applied)
    local spans = {}

    if syntax_type == HL_TYPE_MARKDOWN then
        local hashes, whitespace = render:match("^(#+)(%s*)")
        if hashes then
            add_span(spans, 1, #hashes, "keyword1")
        end
        for start_pos, stop_pos in render:gmatch("()%`[^`]+`()") do
            add_span(spans, start_pos, stop_pos - start_pos, "string")
        end
    end

    for _, tag in ipairs(TODO_TAGS) do
        local search = 1
        while true do
            local s = string.find(render, tag, search, true)
            if not s then break end
            add_span(spans, s, #tag, "match")
            search = s + #tag
        end
    end

    if #spans > 0 then
        return { spans = spans }
    end
end
```

## Implementation Details

- **Non-blocking**: Uses libcurl multi interface for async I/O
- **Event loop**: Checks for completed requests every iteration
- **Callback-based**: Lua functions called when responses arrive
- **Max requests**: 10 concurrent requests (configurable in loki.c)
- **Timeout**: 30 seconds per request

## Customization

Edit `init.lua` to:
- Add custom keybindings
- Create project-specific functions
- Configure AI models or endpoints
- Add additional HTTP integrations

## Requirements

- Lua or LuaJIT
- libcurl
- libuv
- OpenAI API key (for AI functions)
