# Language definitions

**Status:** current as of 2026-10-28.

## Two layers (intentional)

| Layer | Where | Role |
|-------|--------|------|
| Builtin `HLDB` | `src/languages.c` + `src/syntax/lang_*.h` | Minimal keywords for tests, markdown fenced blocks, and files opened before Lua config loads |
| Lua languages | `.loki/languages/*.lua` via `loki.register_language` | Full keyword sets; lazy-loaded through `.loki/modules/languages.lua` |

Builtin entries cover C, Python, Lua, Markdown, and Scheme. Everything else (Rust, Go, JS, …) is Lua-only. Overlap for Python/Lua/C is **by design**: C stays small and dependency-free; Lua carries the rich definitions used when a trusted project config runs.

Tree-sitter highlighting (via linenoise / `treesitter.c`) is separate from this keyword `HLDB` path.

## Registering a language

```lua
loki.register_language({
    name = "Zig",
    extensions = {".zig"},
    keywords = {"fn", "const", "var", "pub", "try", "return"},
    types = {"i32", "u32", "bool"},
    line_comment = "//",
    block_comment_start = "/*",
    block_comment_end = "*/",
    separators = ",.()+-/*=~%[]{}:;",
    highlight_strings = true,
    highlight_numbers = true,
})
```

| Field | Required | Notes |
|-------|----------|--------|
| `name` | yes | Display name |
| `extensions` | yes | e.g. `{".py", ".pyw"}` |
| `keywords` | no | Ordinary keywords |
| `types` | no | Highlighted as type/keyword2 (or `|` suffix in some lists) |
| `line_comment` | no | Single-line comment starter |
| `block_comment_start` / `block_comment_end` | no | Multi-line comments |
| `separators` | no | Separator character set |
| `highlight_strings` / `highlight_numbers` | no | Default true |

Returns `true` on success, or `nil, error` on failure.

Drop a file under `.loki/languages/` and ensure `languages.init()` runs from init (bundled `.loki/init.lua` does this when trusted). See `.loki/languages/` for examples.
