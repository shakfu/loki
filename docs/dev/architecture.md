# Loki Architecture

**Status:** current as of 2026-10-28.  
**Companion:** [`coverage.md`](coverage.md) (tests), [`window-state.md`](window-state.md) (open design), [`ROADMAP.md`](../../ROADMAP.md).

## Product shape

Loki is a **POSIX terminal editor**: one C11 static library and one CLI.

| CMake target | Artifact | Role |
|--------------|----------|------|
| `libloki` (`OUTPUT_NAME loki`) | `libloki.a` / `libloki.so` | Editor core, Lua bridge, optional HTTP, session/host, REPL helpers |
| `loki` | `build/loki` | `src/main.c` → `loki_editor_main()` |

CLI today: `loki <filename>`, plus `--help` / `--version`. The Lua REPL is the **docked panel** (`Ctrl-L`), not a separate binary or `--repl` flag. Agents run over ACP (`:agent`) and MCP (`:mcp`); see [`agent-integration.md`](agent-integration.md).

There are no `loki-editor` / `loki-repl` executables. Older design notes that described that split are obsolete.

```
include/loki/     Thin public headers (entry, version, Lua bootstrap, core API)
src/              Implementation (also on the library include path today)
.loki/            Bundled Lua: init, modules, languages, themes
thirdparty/       cmark; linenoise + Tree-sitter grammars
tests/            Unit tests linked against libloki
```

Rough size: ~20K lines under `src/` + `include/` (editor C), not counting vendored trees.

## Layering

```
┌─────────────────────────────────────────────────────────┐
│  CLI / hosts                                             │
│  editor.c (terminal loop) · host.c · jsonrpc.c · cli.c   │
├─────────────────────────────────────────────────────────┤
│  Session API                                             │
│  session.c — opaque EditorSession, events, view-model    │
├─────────────────────────────────────────────────────────┤
│  Editor features                                         │
│  modal · command · search · selection · undo · indent    │
│  bracket · buffers · syntax · treesitter · markdown      │
│  trust · http (optional) · lua · repl_*                  │
├─────────────────────────────────────────────────────────┤
│  Core model / I/O                                        │
│  core.c · terminal.c · renderer.c · event.c · async_queue│
├─────────────────────────────────────────────────────────┤
│  Dependencies                                            │
│  Lua · libuv · (libcurl) · cmark · linenoise/Tree-sitter │
└─────────────────────────────────────────────────────────┘
```

Two ways to drive the editor coexist:

1. **Direct terminal path** (what `loki` uses): `loki_editor_main` owns an `editor_ctx_t`, enables raw mode via `terminal_host_*`, and loops `editor_refresh_screen` / `editor_process_keypress`.
2. **Host + session path** (embedding / psnd / JSON-RPC): `EditorHost` + `EditorSession` — read events, handle them, snapshot an `EditorViewModel` for rendering. `host.c` implements a terminal host on this path; `jsonrpc.c` speaks stdio JSON-RPC for headless clients.

Prefer new embedding work on the session API; keep the direct path until it is fully migrated (see open items in `window-state.md`).

## Model and view

`editor_ctx_t` (`internal.h`) splits state:

| Part | Holds |
|------|--------|
| `EditorModel` | Rows, filename, dirty, undo, indent, language/ts state |
| `EditorView` | Cursor, scroll, screen size, selection, syntax pointer, colors, mode, command line, status |
| `LuaHost *` | Lua state + docked REPL |
| `Renderer *` | Optional render abstraction (may be NULL → direct VT100) |

**Buffers:** `buffers.c` keeps up to 16 `buffer_entry_t`, each embedding a full `editor_ctx_t`. Switching buffers changes `current_buffer_id`; nothing is copied. That is enough for tabs today; split windows need a real window object (proposal in `window-state.md`).

Compatibility macros (`ctx_cx(ctx)`, …) still alias into model/view for older call sites.

## Module map

| Area | Files | Notes |
|------|--------|------|
| Startup / loop | `main.c`, `editor.c` | Filename required; Lua bootstrap; buffer init; resize; async dispatch |
| Core buffer I/O | `core.c` | Open/save, rows, refresh, binary-file reject |
| Terminal / draw | `terminal.c`, `renderer.c`, `event.c` | Raw mode, VT100, input events |
| Modal / ex | `modal.c`, `command.c`, `command/*` | NORMAL/INSERT/VISUAL/COMMAND |
| Multi-buffer | `buffers.c` | Tabs; Ctrl-T / Ctrl-X n|p|k |
| Edit support | `undo.c`, `indent.c`, `search.c`, `selection.c`, `bracket.c` | |
| Syntax | `syntax.c`, `languages.c`, `syntax/lang_*.h`, `treesitter.c` | Minimal static `HLDB` + Lua `register_language`; see `languages.md` |
| Lua | `lua.c`, `lang_bridge.c`, `trust.c` | Bindings, config load, `:trust` list |
| REPL UI | docked REPL in `lua.c`; helpers in `repl.c`, `repl_linenoise.c`, `repl_helpers.c` | Linenoise, not readline |
| Markdown | `loki_markdown.c` | cmark wrapper |
| HTTP | `http.c` | Only if `LOKI_ENABLE_HTTP`; limits in `http.h` |
| Embed | `session.c`, `host.c`, `cli.c`, `jsonrpc.c`, `serialize.c`, `json.c`, `async_queue.c` | Shared / psnd-oriented |

psnd stubs (audio/MIDI, live-loop, Joy/Alda/Csound headers) remain so shared sources compile; they are no-ops in core Loki.

## Startup sequence (`loki_editor_main`)

1. `loki_lang_init`, `async_queue_init`, `atexit(editor_atexit)`
2. Parse argv (filename / help / version only)
3. `init_editor`, syntax (and Tree-sitter) for filename, `editor_open`
4. `loki_lua_bootstrap` with editor (+ HTTP if built); `lua_host_init_repl`
5. Re-select syntax after Lua languages register; `buffers_init`
6. Optional language-bridge init for the file type
7. Raw mode; main loop: resize → live_loop/async dispatch → refresh → keypress (exit on hangup)

Project `.loki/init.lua` loads only when `trust.c` says the cwd is trusted; otherwise `~/.loki/init.lua`.

## Extension points

**Lua (preferred for features):**

- Config: trusted `.loki/init.lua` or `~/.loki/init.lua`
- Modules / languages / themes under `.loki/`
- APIs: `loki.*`, `loki.repl.register`, `loki.register_command`, `loki.register_language`, optional `loki.async_http`

**C:**

- Add sources to `LOKI_SOURCES` in `CMakeLists.txt`
- Gate heavy deps with options (pattern: `LOKI_ENABLE_HTTP`)
- Keep `lua.c` / `modal.c` from growing without a split

**psnd sync:** `make check-sync` / `scripts/check-psnd-sync.sh` compares the shared “generic core” file list.

## Build options

| Option | Default | Effect |
|--------|---------|--------|
| `LOKI_ENABLE_HTTP` | OFF | Compile `http.c`, define `LOKI_ENABLE_HTTP`, link libcurl; editor sets `bind_http = 1` |
| `LOKI_BUILD_SHARED` | OFF | Shared `libloki` |
| `BUILD_TESTS` | ON | Unit test executables + CTest |

Requires C11 (`_Atomic` in `async_queue.c`), Lua, libuv, pkg-config. Warnings: `-Wall -Wextra -pedantic`.

```bash
cmake -S . -B build
cmake -S . -B build -DLOKI_ENABLE_HTTP=ON
cmake --build build
ctest --test-dir build --output-on-failure
make test / make test-http   # Makefile wrappers
```

## Non-goals

GUI, built-in terminal emulator, debugger, full LSP client, and a package manager are out of scope (`ROADMAP.md`). Tree-sitter is already in-tree for highlighting; unrelated parser features should be weighed against dependency cost.

## Related docs

| Doc | Topic |
|-----|--------|
| [`coverage.md`](coverage.md) | Test inventory and how to run |
| [`modal_editing.md`](modal_editing.md) | Modes and Lua hooks |
| [`languages.md`](languages.md) | HLDB vs Lua languages |
| [`lua_async_http.md`](lua_async_http.md) | HTTP API |
| [`../security.md`](../security.md) | Trust + HTTP threat model |
