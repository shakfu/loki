# Local patches to vendored CUI

Upstream: https://github.com/yappologistic/CUI at 79777283a10b940a024b90b0ad91062c434b0b12.
Vendored: `src/`, `include/`, `cmake/`, `third_party/`, `assets/fonts/`, `CMakeLists.txt`, `LICENSE`.
Patched lines carry a `[loki patch]` comment. Both are candidates for upstreaming.

1. `cui_key_events()` (`cui.h`, `cui_core.c`, `cui_internal.h`): key presses
   and typed text in arrival order. Upstream keeps typed text in private
   `ui->text_in`, apart from key state, so an app that handles the keyboard
   itself cannot read `:` or `$`, nor tell `x<Esc>` from `<Esc>x` in one frame.
2. `cui_app_cancel_quit()` (`cui_app.h`, `platform/cui_app.c`): lets the app
   veto a window close. Upstream has no veto, so closing the window with
   unsaved buffers would discard them.
3. `cui_clipboard_set()` / `cui_clipboard_get()` (`cui.h`, `cui_core.c`):
   public access to the backend's clipboard. Upstream reaches it only from
   inside its own components.
