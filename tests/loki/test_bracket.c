/* test_bracket.c - Tests for matching bracket lookup and display */

#include "test_framework.h"
#include "loki/core.h"
#include "internal.h"
#include "bracket.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Load lines into a fresh context, with C highlighting so strings and
 * comments are marked in row->hl. */
static void load(editor_ctx_t *ctx, const char **lines, int n) {
    extern struct t_editor_syntax HLDB[];
    editor_ctx_init(ctx);
    ctx->view.syntax = &HLDB[0];  /* C is first in HLDB */
    for (int i = 0; i < n; i++)
        editor_insert_row(ctx, i, (char *)lines[i], strlen(lines[i]));
}

static int match(editor_ctx_t *ctx, int row, int col, int *mr, int *mc) {
    *mr = *mc = -1;
    return bracket_find_match(ctx, row, col, mr, mc);
}

TEST(bracket_same_line_both_directions) {
    const char *lines[] = {"f(a[1], {b})"};
    editor_ctx_t ctx;
    load(&ctx, lines, 1);
    int r, c;

    ASSERT_TRUE(match(&ctx, 0, 1, &r, &c));   /* ( -> ) */
    ASSERT_EQ(r, 0); ASSERT_EQ(c, 11);
    ASSERT_TRUE(match(&ctx, 0, 11, &r, &c));  /* ) -> ( */
    ASSERT_EQ(c, 1);
    ASSERT_TRUE(match(&ctx, 0, 3, &r, &c));   /* [ -> ] */
    ASSERT_EQ(c, 5);
    ASSERT_TRUE(match(&ctx, 0, 10, &r, &c));  /* } -> { */
    ASSERT_EQ(c, 8);

    editor_ctx_free(&ctx);
}

TEST(bracket_nested_multiline) {
    const char *lines[] = {
        "int f(void) {",
        "    if (x) {",
        "        y();",
        "    }",
        "}",
    };
    editor_ctx_t ctx;
    load(&ctx, lines, 5);
    int r, c;

    ASSERT_TRUE(match(&ctx, 0, 12, &r, &c));
    ASSERT_EQ(r, 4); ASSERT_EQ(c, 0);
    ASSERT_TRUE(match(&ctx, 4, 0, &r, &c));
    ASSERT_EQ(r, 0); ASSERT_EQ(c, 12);
    ASSERT_TRUE(match(&ctx, 3, 4, &r, &c));
    ASSERT_EQ(r, 1); ASSERT_EQ(c, 11);

    editor_ctx_free(&ctx);
}

TEST(bracket_no_match_or_not_a_bracket) {
    const char *lines[] = {"(a", "b", ""};
    editor_ctx_t ctx;
    load(&ctx, lines, 3);
    int r, c;

    ASSERT_FALSE(match(&ctx, 0, 0, &r, &c));  /* unmatched */
    ASSERT_FALSE(match(&ctx, 0, 1, &r, &c));  /* not a bracket */
    ASSERT_FALSE(match(&ctx, 0, 2, &r, &c));  /* past end of row */
    ASSERT_FALSE(match(&ctx, 2, 0, &r, &c));  /* empty row */
    ASSERT_FALSE(match(&ctx, 9, 0, &r, &c));  /* past last row */

    editor_ctx_free(&ctx);
}

TEST(bracket_skips_strings_and_comments) {
    const char *lines[] = {
        "f(\")\", ')', /* ) */",
        "  x)",
    };
    editor_ctx_t ctx;
    load(&ctx, lines, 2);
    int r, c;

    ASSERT_TRUE(match(&ctx, 0, 1, &r, &c));
    ASSERT_EQ(r, 1); ASSERT_EQ(c, 3);

    editor_ctx_free(&ctx);
}

TEST(bracket_inside_string_matches_inside_string) {
    const char *lines[] = {"s = \"(x)\"; g()"};
    editor_ctx_t ctx;
    load(&ctx, lines, 1);
    int r, c;

    ASSERT_TRUE(match(&ctx, 0, 5, &r, &c));
    ASSERT_EQ(c, 7);

    editor_ctx_free(&ctx);
}

/* hl is indexed by render column; a tab before the bracket must not shift
 * the string/comment lookup onto the wrong cell. */
TEST(bracket_tabs_map_to_render_columns) {
    const char *lines[] = {"\t\"(\" (x)"};
    editor_ctx_t ctx;
    load(&ctx, lines, 1);
    int r, c;

    ASSERT_EQ(bracket_render_col(&ctx.model.row[0], 1), 7);
    ASSERT_TRUE(match(&ctx, 0, 5, &r, &c));
    ASSERT_EQ(c, 7);

    editor_ctx_free(&ctx);
}

TEST(bracket_scan_limit) {
    editor_ctx_t ctx;
    const char *open[] = {"("};
    load(&ctx, open, 1);
    int n = BRACKET_MAX_SCAN_ROWS + 1;
    for (int i = 1; i < n; i++) editor_insert_row(&ctx, i, "x", 1);
    editor_insert_row(&ctx, n, ")", 1);
    int r, c;

    ASSERT_FALSE(match(&ctx, 0, 0, &r, &c));        /* 1001 rows away */
    ASSERT_TRUE(match(&ctx, n, 0, &r, &c) == 0);    /* same, backward */

    editor_del_row(&ctx, 1);                         /* now 1000 rows away */
    ASSERT_TRUE(match(&ctx, 0, 0, &r, &c));
    ASSERT_EQ(r, BRACKET_MAX_SCAN_ROWS);

    editor_ctx_free(&ctx);
}

/* Render one frame into a temp file and return its contents. */
static char frame[65536];
static const char *render(editor_ctx_t *ctx) {
    FILE *tmp = tmpfile();
    int saved = dup(STDOUT_FILENO);
    fflush(stdout);
    dup2(fileno(tmp), STDOUT_FILENO);
    editor_refresh_screen(ctx);
    dup2(saved, STDOUT_FILENO);
    close(saved);
    rewind(tmp);
    size_t n = fread(frame, 1, sizeof(frame) - 1, tmp);
    frame[n] = '\0';
    fclose(tmp);
    return frame;
}

static int count(const char *hay, const char *needle) {
    int k = 0;
    for (const char *p = hay; (p = strstr(p, needle)); p++) k++;
    return k;
}

TEST(bracket_highlight_and_offscreen_status) {
    const char *lines[] = {"a(b)c", "{", "x", "x", "x", "}"};
    editor_ctx_t ctx;
    load(&ctx, lines, 6);
    ctx.view.syntax = NULL;           /* plain text: no colour codes */
    ctx.view.screenrows = 3;
    ctx.view.screencols = 80;

    ctx.view.cx = 1;                  /* on "(" */
    const char *out = render(&ctx);
    ASSERT_TRUE(strstr(out, "a\x1b[1;4m(\x1b[22;24mb\x1b[1;4m)\x1b[22;24mc") != NULL);
    ASSERT_TRUE(strstr(out, "match ") == NULL);

    ctx.view.cx = 0;                  /* on "a": nothing highlighted */
    out = render(&ctx);
    ASSERT_EQ(count(out, "\x1b[1;4m"), 0);

    ctx.view.cy = 1;                  /* on "{", match on row 6, off screen */
    out = render(&ctx);
    ASSERT_EQ(count(out, "\x1b[1;4m"), 1);
    ASSERT_TRUE(strstr(out, "match 6  2/6") != NULL);

    editor_ctx_free(&ctx);
}

BEGIN_TEST_SUITE("Bracket Matching")
    RUN_TEST(bracket_same_line_both_directions);
    RUN_TEST(bracket_nested_multiline);
    RUN_TEST(bracket_no_match_or_not_a_bracket);
    RUN_TEST(bracket_skips_strings_and_comments);
    RUN_TEST(bracket_inside_string_matches_inside_string);
    RUN_TEST(bracket_tabs_map_to_render_columns);
    RUN_TEST(bracket_scan_limit);
    RUN_TEST(bracket_highlight_and_offscreen_status);
END_TEST_SUITE()
