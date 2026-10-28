# Markdown rendering

**Status:** current as of 2026-10-28.

cmark is vendored and exposed to Lua through the `markdown` module and helpers in `.loki/modules/editor.lua`.

## Quick start

```bash
./build/loki notes.md
# Ctrl-L, then:
editor.render_markdown_to_html()
```

Writes `notes.html` beside the buffer file (save the buffer first so a filename exists).

Other helpers:

```lua
editor.markdown_stats()           -- status: headings / links / code blocks
editor.insert_markdown_as_html()  -- insert HTML at cursor
```

## Lower-level API

```lua
local doc = markdown.parse(text)
local html = doc:render_html(markdown.OPT_SAFE)
-- also: render_xml, render_man, render_commonmark, render_latex
doc:count_headings()
doc:count_links()
doc:count_code_blocks()

-- one-shot
local html = markdown.to_html(text, markdown.OPT_SAFE)
```

C entry points live in `src/loki_markdown.c` / `loki_markdown.h` if you embed without Lua.
