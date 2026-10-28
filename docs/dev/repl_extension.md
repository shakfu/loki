# Extending the Loki REPL

**Status:** current as of 2026-10-28.

Toggle the docked REPL with `Ctrl-L` while editing a file. Built-ins: `help`, `history`, `clear`, `clear-history`, `exit`. Line editing uses linenoise with Tree-sitter highlighting when available.

## Register project commands

```lua
-- .loki/init.lua (project must be :trust'ed)
function insert_header()
    loki.insert_text("/* generated */\n")
end

loki.repl.register(
    "insert_header",
    "Insert a C-style generated header comment",
    "insert_header()"
)
```

`help` lists these under project commands. Keep names unique; duplicates clutter the list.

## Modules

Group helpers under `.loki/modules/` and `require` them from init. Register only thin wrappers so `help` stays readable.

## Async HTTP from the REPL

Requires an HTTP-enabled build. Callbacks receive a **table** (`status`, `body`, `error`), not a bare string:

```lua
function handle_explain(response)
    if not response or response.error then
        loki.status(tostring(response and response.error))
        return
    end
    if response.body then
        loki.insert_text("\n" .. response.body)
    end
end

function explain_line()
    local row = select(1, loki.get_cursor())
    local line = loki.get_line(row) or ""
    local id, err = loki.async_http(
        "https://example.com/api/explain",
        "POST",
        line,
        {"Content-Type: text/plain"},
        "handle_explain"
    )
    if not id then loki.status(tostring(err)) end
end

loki.repl.register("explain_line", "POST current line to explain API")
```

See `lua_async_http.md` and `docs/security.md`.
