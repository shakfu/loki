/* cui_main.c - loki-cui entry point: shared startup, then the CUI loop. */
#include <stdio.h>

#include <cui/cui_app.h>

#include "internal.h"
#include "buffers.h"
#include "cui_frontend.h"

int main(int argc, char **argv) {
    loki_editor_setup(argc, argv);

    /* Dark only: the bundled syntax themes assume a dark background. */
    cui_app *app = cui_app_create(&(cui_app_desc){
        .title = "loki", .width = 960, .height = 640, .theme = CUI_THEME_DARK,
    });
    if (!app) {
        fprintf(stderr, "loki-cui: could not open a window\n");
        return 1;
    }

    int close_warned = 0;
    for (;;) {
        if (!cui_app_frame(app)) {
            /* Window closed with unsaved work: refuse once, honour a repeat. */
            if (loki_cui_any_dirty() && !close_warned) {
                close_warned = 1;
                cui_app_cancel_quit(app);
                editor_set_status_msg(buffer_get_current(),
                    "Unsaved changes: save with Ctrl-S, or close the window again to discard");
                continue;
            }
            break;
        }
        if (loki_cui_frame(cui_app_ui(app), cui_app_size(app)) != 0) break;
        if (!loki_cui_any_dirty()) close_warned = 0;
    }

    cui_app_destroy(app);
    loki_cui_shutdown();
    return 0;
}
