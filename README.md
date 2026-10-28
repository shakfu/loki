# loki — a Lua-powered terminal text editor

A modular text editor descended from antirez's [kilo](https://github.com/antirez/kilo). Vim-like modal editing, embedded Lua (config + docked REPL), Tree-sitter-aware highlighting, multi-buffer tabs, and optional async HTTP for AI/API workflows. Zero config required: compile and open a file.

## Features

- **Modal editing** — NORMAL / INSERT / VISUAL / COMMAND (`:w`, `:q`, `:trust`, …)
- **Lua** — project or home config, `Ctrl-L` REPL, `register_language` / `register_command`
- **Syntax** — builtin keyword DB plus Lua language packs; Tree-sitter via linenoise for REPL highlighting
- **Editing basics** — multi-buffer tabs, undo/redo, search, indent, bracket match, OSC 52 clipboard
- **Optional HTTP** — `loki.async_http()` when built with `-DLOKI_ENABLE_HTTP=ON` (libcurl; off by default)
- **Trust model** — project `.loki/init.lua` runs only after `:trust` (see below)

Rough size: ~20K lines of editor C (C11), plus vendored cmark and linenoise/Tree-sitter. Details: [`docs/dev/architecture.md`](docs/dev/architecture.md).

## Building

**Dependencies:** C11 toolchain, Lua (or LuaJIT), libuv, pkg-config. Optional: libcurl for HTTP. cmark and linenoise are vendored under `thirdparty/`.

```bash
# macOS
brew install lua libuv
# brew install curl   # only if you want -DLOKI_ENABLE_HTTP=ON

# Debian/Ubuntu
sudo apt install build-essential cmake pkg-config liblua5.4-dev libuv1-dev
# sudo apt install libcurl4-openssl-dev   # for HTTP
```

```bash
cmake -S . -B build
cmake --build build
# or: make

ctest --test-dir build --output-on-failure
# or: make test

# HTTP-enabled build + tests
cmake -S . -B build -DLOKI_ENABLE_HTTP=ON && cmake --build build
# or: make test-http
```

Artifacts: `build/loki`, `build/libloki.a`.

## Usage

```bash
./build/loki <filename>
./build/loki --help
./build/loki --version
```

A filename is required. There is no separate REPL binary and no `--complete` / `--explain` CLI; use the docked Lua REPL and modules instead.

### Keybindings

**Global:** `Ctrl-S` save · `Ctrl-Q` quit · `Ctrl-F` find · `Ctrl-L` Lua REPL · `Ctrl-T` new buffer · `Ctrl-X` then `n`/`p`/`k` buffer next/prev/kill

Starts in **NORMAL** mode. `i` / `a` / `o` / `O` → INSERT; `ESC` → NORMAL; `v` → VISUAL (`y` yank, `d` yank+delete, `x` delete). `:` opens COMMAND mode.

Traditional always-insert start (in a trusted `.loki/init.lua` or `~/.loki/init.lua`):

```lua
modal.disable()   -- or loki.set_mode("insert")
```

More detail: [`docs/dev/modal_editing.md`](docs/dev/modal_editing.md).

## Lua configuration

Load order:

1. `.loki/init.lua` in the current directory — **only if trusted**
2. otherwise `~/.loki/init.lua`

Trusted local config replaces the global one (global is not layered on top). Run `:trust` once in the project directory (records the resolved path in `~/.loki/trusted`), then restart. Untrusted projects also skip project `.loki/` and relative entries on `package.path` / `package.cpath`.

The repo ships a full example tree at [`.loki/`](.loki/) (init, modules, languages, themes). For a project:

```bash
cp -r /path/to/loki/.loki .loki   # or copy pieces you need
# open a file in that directory, run :trust, restart
```

Or install a global config: `mkdir -p ~/.loki && cp .loki/init.lua ~/.loki/` (and modules as needed).

### Docked REPL (`Ctrl-L`)

Evaluate Lua at `>> `; builtins `help`, `history`, `clear`, `clear-history`, `exit`. History prefers `.loki/repl_history` when present. Line editing uses **linenoise** (with Tree-sitter highlighting), not GNU readline.

```lua
>> count_lines()
>> loki.status("Hello!")
>> ai.complete()          -- needs HTTP build + OPENAI_API_KEY + trusted config
```

Register help entries with `loki.repl.register(...)` — see [`docs/dev/repl_extension.md`](docs/dev/repl_extension.md).

### Core Lua API

| Call | Purpose |
|------|---------|
| `loki.status(msg)` | Status bar |
| `loki.get_lines()` / `loki.get_line(row)` | Buffer read (0-based rows) |
| `loki.get_cursor()` / `loki.insert_text(text)` | Cursor / insert |
| `loki.get_filename()` | Current path |
| `loki.async_http(url, method, body, headers, callback)` | Async HTTP (HTTP builds only) |

`async_http` invokes a **global** callback with a response table `{ status, body, error }`, not a bare string. Limits and SSRF rules: [`docs/security.md`](docs/security.md), [`docs/dev/lua_async_http.md`](docs/dev/lua_async_http.md).

Bundled modules (when config loads): `editor.*`, `ai.complete` / `ai.explain`, languages, themes, markdown helpers.

### Highlight hook

```lua
function loki.highlight_row(idx, text, render, syntax_type, default_applied)
    return {
        replace = true,  -- optional
        spans = {
            { start = 1, length = 3, style = "keyword1" },
            { start = 10, stop = 20, style = "comment" },
        },
    }
end
```

Return `nil` to keep defaults. Styles are under `loki.hl`.

## Documentation

| Doc | Contents |
|-----|----------|
| [`docs/dev/README.md`](docs/dev/README.md) | Index of developer notes |
| [`ROADMAP.md`](ROADMAP.md) | Shipped vs planned |
| [`docs/security.md`](docs/security.md) | Trust, Lua, HTTP threat model |
| [`CHANGELOG.md`](CHANGELOG.md) | Release history |

## Credits

Original [kilo](https://github.com/antirez/kilo) by Salvatore Sanfilippo (antirez). Lua integration and later modules added 2025–2026.

Released under the BSD 2-Clause license (see `LICENSE`).
