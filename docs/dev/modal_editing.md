# Modal editing

**Status:** current as of 2026-10-28.

Loki starts in **NORMAL** mode. Press `i` (or `a` / `o` / `O` / …) for INSERT; `ESC` returns to NORMAL. The mode appears in the status bar.

## Modes

| Mode | Role |
|------|------|
| NORMAL | Navigation and commands (`h/j/k/l`, `x`, `{`/`}`, …) |
| INSERT | Insert text; `ESC` → NORMAL |
| VISUAL | Extend selection; `y` yank, `d` yank+delete, `x` delete |
| COMMAND | Ex-style line (`:w`, `:q`, `:trust`, …) |

**Global (most modes):** `Ctrl-S` save, `Ctrl-Q` quit, `Ctrl-F` find, `Ctrl-L` Lua REPL. In INSERT, `Ctrl-W` toggles word wrap; `Ctrl-C` copies the selection (OSC 52).

### Disable vim-like start

```lua
-- .loki/init.lua (trusted project) or ~/.loki/init.lua
loki.set_mode("insert")
-- or: modal.disable()
```

## Lua API

```lua
loki.get_mode()                 -- "normal" | "insert" | "visual" | "command"
loki.set_mode("insert")

loki.register_command("g", function()
    loki.status("custom g")
end)
```

The bundled `.loki/modules/modal.lua` wraps the same surface (`modal.enable` / `disable` / `register_command` / word motions `w` `b` `e`, etc.) and registers REPL help entries.

Unknown NORMAL keys are dispatched to Lua (`loki_process_normal_key` or the `_loki_commands` table). Prefer `loki.register_command` for new bindings.

## Implementation

| Piece | Location |
|-------|----------|
| Key dispatch | `src/modal.c` |
| Ex commands | `src/command.c`, `src/command/*.c` |
| Selection / clipboard | `src/selection.c` |
| Lua bindings | `src/lua.c`, `.loki/modules/modal.lua` |
