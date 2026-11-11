# loki-cui: native-window frontend

**Status:** experimental, `cui` branch. GUI is a non-goal in `architecture.md`; this stays an opt-in target.

## Build and run

```bash
cmake -S . -B build/cui -DLOKI_ENABLE_CUI=ON
cmake --build build/cui
./build/cui/loki-cui <filename>
ctest --test-dir build/cui --output-on-failure   # includes test_cui_frontend
```

Backends come from CUI: Cocoa (macOS), Win32, X11 (needs `libx11-dev`), or SDL2/GLFW via `-DCUI_PLATFORM=`.

## Shape

| File | Role |
|------|------|
| `src/cui_main.c` | `loki_editor_setup()`, then the `cui_app_frame` loop; vetoes one window close with unsaved buffers |
| `src/cui_frontend.c` | Grid `Renderer`, key mapping, canvas painting, `loki_cui_frame()` |
| `tests/loki/test_cui_frontend.c` | Headless frames through CUI's software renderer with injected input |
| `thirdparty/cui/` | CUI at `7977728`, plus the patches in `PATCHES.md` |

Per frame: resize grid and buffers -> `editor_run_background()` -> keys -> `editor_refresh_screen()` -> paint. The frame wakes every 50 ms when idle, so timers, `loki.spawn` and HTTP callbacks still run.

Syntax colors come from the buffer's `view.colors` (the Lua theme). The CUI theme is fixed to dark, because the bundled themes assume a dark background.

## Keys

- Printable input arrives as CUI text events and goes to loki as bytes.
- Ctrl+letter becomes the control code; Esc, Enter, Tab, Backspace, Delete, arrows, Home/End and PgUp/PgDn map to loki keycodes.
- macOS: Cmd-S saves, Cmd-Q quits, Cmd-V pastes. Elsewhere: Ctrl-Shift-V pastes.
- Paste works only in INSERT, COMMAND or the REPL. In NORMAL mode the clipboard would run as commands.
- Visual-mode yank writes to the system clipboard.
- Left click moves the cursor.

## Known gaps

- `/` and Ctrl-F search: `editor_find()` (`search.c`) blocks on `terminal_read_key(fd)`. Fixing it means turning search into an event-driven mode.
- Non-ASCII text: one cell per code point. Wide (CJK) and combining characters misalign.
- Bold uses a 0.6 pt overdraw, because CUI embeds no bold mono face.
- Bracket-match highlighting and the `match <line>` status hint exist only on the VT100 path, not the `Renderer` path.
