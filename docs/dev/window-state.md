# Per-window view state

Status: proposal, 2026-10-25. Decides where cursor, scroll, wrap and line-number state live before split windows and relative line numbers.

## Current layout

- `buffers.c` holds up to 16 `buffer_entry_t`, each embedding a whole `editor_ctx_t` (624 bytes): `EditorModel` (64 bytes) and `EditorView` (544 bytes) by value.
- Switching buffers changes `current_buffer_id`. Nothing is copied.
- `editor_refresh_screen()` (`core.c:782`) redraws the full screen on every refresh. The gutter is rebuilt each frame.
- `row->hl` (highlight bytes) is stored in the model rows. The syntax definition that fills it, `view.syntax`, is in the view.
- `src/` has 420 `model.` and 746 `view.` field accesses, across 21 files for the model.

## Field classification

| State | Belongs to | Reason |
|-------|------------|--------|
| rows, filename, dirty, undo, indent, language state | buffer | Already in `EditorModel` |
| `syntax` | buffer | It fills `row->hl`, which is shared. Two windows with different `syntax` would overwrite each other's highlighting. Currently in `EditorView` (18 sites) |
| `cx`, `cy`, `rowoff`, `coloff`, selection | window | Independent per view, as in Vim |
| `line_numbers`, number mode, `word_wrap` | window | Rendering only; Vim makes `number`, `relativenumber` and `wrap` window-local |
| window height and width | window | Split layout assigns them |
| terminal rows and cols, colors | editor | One terminal, one theme. Today each buffer has its own copy |
| mode, command line, history, `pending_prefix`, `quit_times`, status message | editor | One focused window receives input |

The last two rows have a current cost. Every buffer must be updated on resize (`editor_resize_buffers()`); before 2026-10-25 only the current one was, and other buffers kept the old size.

## Options

**A. Swap.** Keep one `editor_ctx_t` per buffer. Add `Window { buffer_id; EditorView view; }`. Copy the focused window's view into `ctx->view` on focus change, and copy each window's view in to render it.

**B. Pointer.** `Buffer { EditorModel model; }` and `Window { Buffer *buf; EditorView view; }`. `editor_ctx_t` becomes the window, holding `EditorModel *model`. A buffer is freed when its last window closes.

**C. Defer.** No windows. Put the number mode in `EditorView` now, which is per buffer.

## Performance

Measured with `editor_refresh_screen()` writing to `/dev/null`, on `src/core.c` (1166 rows, C highlighting), 2000 frames per run, two runs. The runs vary by up to 40%.

| Case | us/frame |
|------|----------|
| 80x24, no numbers | 10.0-10.3 |
| 80x24, numbers | 11.2-14.3 |
| 240x70, no numbers | 29.0-41.0 |
| 240x70, numbers | 32.8-35.0 |
| `EditorView` swap (2 x 544 B `memcpy`) | 0.017-0.019 |

- Frame cost scales with visible cells. Splitting the screen divides the same cells among windows, so total render cost stays roughly constant under A and B.
- A adds 2 swaps per visible window per frame: 4 windows cost about 0.08 us, under 1% of an 80x24 frame.
- B adds one pointer load per model access. The render loop reads `model.row` and `model.numrows` about once per screen row, so about 70 extra loads per frame at 240x70. Estimated well under 1%; not measured.
- Relative numbers cost nothing beyond the current gutter: it is already rebuilt every frame. The measured gutter cost is 1-4 us, within noise.
- The `/dev/null` numbers exclude the terminal. A real terminal parsing and drawing the output is slower, so the in-process differences between A and B are smaller still in practice.

Performance does not separate A from B. Correctness and churn do:

| | A. Swap | B. Pointer | C. Defer |
|-|---------|------------|----------|
| Two windows on one buffer | Works only if every edit path runs with the right view swapped in. An edit through another window leaves this window's cursor stale; it must be clamped on swap-in | Each window owns its view. Edits still need cursor clamping in other windows | Not possible |
| Code churn | New `Window` and layout code; existing `ctx->view.*` code unchanged | `ctx->model.` to `ctx->model->` at 420 sites (mechanical); buffer lifetime moves to refcount or last-window close | None |
| Hidden-state risk | High: `ctx->view` means "whichever window was swapped in last" | Low | None |

## Recommendation

1. Now, for relative numbers: put the number mode in `EditorView` next to `line_numbers`. It is window state under A and B, so this does not commit to either.
2. Move `syntax` to `EditorModel`, and terminal size and colors to editor-global state. Both are correct under every option, and the second fixes the stale-size-after-resize case.
3. For split windows: B. The model is already separated; B finishes that split. A keeps one struct that means different windows at different times, and its failure mode is a silent wrong cursor.
