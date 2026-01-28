/**
 * @file test_linenoise_repl.c
 * @brief Interactive test for linenoise REPL with syntax highlighting.
 *
 * This is a simple test program to verify the linenoise integration works.
 * Run it directly to test syntax highlighting, history, and completion.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef LOKI_USE_LINENOISE
#include <linenoise.h>
#include <syntax/lua.h>
#include <syntax/theme.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("Linenoise REPL Test with Lua Syntax Highlighting\n");
    printf("Type Lua code to see highlighting. Type 'quit' to exit.\n");
    printf("Try: local x = 42, function foo() end, -- comment\n\n");

    /* Initialize Lua highlighter */
    if (lua_highlight_init() != 0) {
        fprintf(stderr, "Failed to initialize Lua highlighter\n");
        return 1;
    }

    /* Create linenoise context */
    linenoise_context_t *ctx = linenoise_context_create();
    if (!ctx) {
        fprintf(stderr, "Failed to create linenoise context\n");
        lua_highlight_free();
        return 1;
    }

    /* Set syntax highlighting callback */
    linenoise_set_highlight_callback(ctx, lua_highlight_callback);

    /* Set history size */
    linenoise_history_set_max_len(ctx, 100);

    /* Main loop */
    char *line;
    while ((line = linenoise_read(ctx, "lua> ")) != NULL) {
        if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
            linenoise_free(line);
            break;
        }

        /* Add to history if non-empty */
        if (line[0] != '\0') {
            linenoise_history_add(ctx, line);
        }

        /* Echo the line */
        printf("You entered: %s\n", line);

        linenoise_free(line);
    }

    printf("\nGoodbye!\n");

    /* Cleanup */
    linenoise_context_destroy(ctx);
    lua_highlight_free();

    return 0;
}

#else

int main(void) {
    printf("LOKI_USE_LINENOISE not defined - linenoise not available\n");
    return 1;
}

#endif
