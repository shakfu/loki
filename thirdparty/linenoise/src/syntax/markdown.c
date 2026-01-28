/* markdown.c -- Tree-sitter based Markdown syntax highlighting
 *
 * Uses tree-sitter to parse Markdown and highlight it using query captures.
 */

#include <stdlib.h>
#include <string.h>
#include "syntax/markdown.h"
#include "syntax/theme.h"
#include <tree_sitter/api.h>

/* External function from tree-sitter-markdown */
extern const TSLanguage *tree_sitter_markdown(void);

/* Global state for the highlighter */
static TSParser *parser = NULL;
static TSQuery *query = NULL;
static TSQueryCursor *cursor = NULL;

/* Markdown highlighting query - block-level elements only
 * This grammar parses block structure, not inline formatting */
static const char *MARKDOWN_HIGHLIGHT_QUERY =
    /* Headings */
    "(atx_heading) @markup.heading\n"
    "(setext_heading) @markup.heading\n"
    "[(atx_h1_marker) (atx_h2_marker) (atx_h3_marker)\n"
    " (atx_h4_marker) (atx_h5_marker) (atx_h6_marker)] @punctuation.special\n"
    "[(setext_h1_underline) (setext_h2_underline)] @punctuation.special\n"

    /* Code blocks */
    "(indented_code_block) @markup.raw\n"
    "(fenced_code_block) @markup.raw\n"
    "(fenced_code_block_delimiter) @punctuation.delimiter\n"
    "(code_fence_content) @markup.raw\n"
    "(info_string) @label\n"
    "(language) @label\n"

    /* Links and references */
    "(link_destination) @markup.link\n"
    "(link_title) @string\n"
    "(link_label) @markup.link\n"

    /* Lists */
    "[(list_marker_plus) (list_marker_minus) (list_marker_star)\n"
    " (list_marker_dot) (list_marker_parenthesis)] @punctuation.special\n"

    /* Block quotes */
    "(block_quote_marker) @punctuation.special\n"

    /* Thematic breaks */
    "(thematic_break) @punctuation.special\n"

    /* Escape sequences */
    "(backslash_escape) @string.escape\n"

    /* Tables */
    "(pipe_table) @markup.raw\n"
;

/* Map capture names to theme token types */
static unsigned char get_color_for_capture(const char *name, uint32_t len) {
    /* Markup headings */
    if (len >= 6 && strncmp(name, "markup", 6) == 0) {
        if (len > 7) {
            const char *suffix = name + 7;
            if (strncmp(suffix, "heading", 7) == 0) {
                return theme_color(TOK_KEYWORD);  /* Bold color for headings */
            }
            if (strncmp(suffix, "raw", 3) == 0) {
                return theme_color(TOK_STRING);  /* Code blocks */
            }
            if (strncmp(suffix, "link", 4) == 0) {
                return theme_color(TOK_FUNCTION);  /* Links */
            }
            if (strncmp(suffix, "italic", 6) == 0) {
                return theme_color(TOK_COMMENT);  /* Italic text */
            }
            if (strncmp(suffix, "bold", 4) == 0) {
                return theme_color(TOK_KEYWORD);  /* Bold text */
            }
        }
        return theme_color(TOK_DEFAULT);
    }

    /* Strings */
    if (len >= 6 && strncmp(name, "string", 6) == 0) {
        if (len > 7 && strncmp(name + 7, "escape", 6) == 0) {
            return theme_color(TOK_STRING_ESCAPE);
        }
        return theme_color(TOK_STRING);
    }

    /* Labels (info strings for code blocks) */
    if (len >= 5 && strncmp(name, "label", 5) == 0) {
        return theme_color(TOK_TYPE);
    }

    /* Punctuation */
    if (len >= 11 && strncmp(name, "punctuation", 11) == 0) {
        if (len > 12 && strncmp(name + 12, "special", 7) == 0) {
            return theme_color(TOK_OPERATOR);
        }
        if (len > 12 && strncmp(name + 12, "delimiter", 9) == 0) {
            return theme_color(TOK_PUNCTUATION_DELIMITER);
        }
        return theme_color(TOK_PUNCTUATION);
    }

    return theme_color(TOK_DEFAULT);
}

int markdown_highlight_init(void) {
    uint32_t error_offset;
    TSQueryError error_type;

    if (parser != NULL) {
        return 0; /* Already initialized */
    }

    parser = ts_parser_new();
    if (parser == NULL) {
        return -1;
    }

    if (!ts_parser_set_language(parser, tree_sitter_markdown())) {
        ts_parser_delete(parser);
        parser = NULL;
        return -1;
    }

    query = ts_query_new(
        tree_sitter_markdown(),
        MARKDOWN_HIGHLIGHT_QUERY,
        (uint32_t)strlen(MARKDOWN_HIGHLIGHT_QUERY),
        &error_offset,
        &error_type
    );
    if (query == NULL) {
        ts_parser_delete(parser);
        parser = NULL;
        return -1;
    }

    cursor = ts_query_cursor_new();
    if (cursor == NULL) {
        ts_query_delete(query);
        ts_parser_delete(parser);
        query = NULL;
        parser = NULL;
        return -1;
    }

    return 0;
}

void markdown_highlight_free(void) {
    if (cursor != NULL) {
        ts_query_cursor_delete(cursor);
        cursor = NULL;
    }
    if (query != NULL) {
        ts_query_delete(query);
        query = NULL;
    }
    if (parser != NULL) {
        ts_parser_delete(parser);
        parser = NULL;
    }
}

void markdown_highlight_callback(const char *buf, char *colors, size_t len) {
    TSTree *tree;
    TSNode root;
    TSQueryMatch match;
    uint32_t capture_index;

    if (parser == NULL || query == NULL || cursor == NULL) {
        return;
    }

    if (len == 0) {
        return;
    }

    tree = ts_parser_parse_string(parser, NULL, buf, (uint32_t)len);
    if (tree == NULL) {
        return;
    }

    root = ts_tree_root_node(tree);
    ts_query_cursor_exec(cursor, query, root);

    while (ts_query_cursor_next_capture(cursor, &match, &capture_index)) {
        TSQueryCapture capture = match.captures[capture_index];
        uint32_t start = ts_node_start_byte(capture.node);
        uint32_t end = ts_node_end_byte(capture.node);
        uint32_t name_len;
        const char *capture_name;
        unsigned char color;
        uint32_t i;

        capture_name = ts_query_capture_name_for_id(query, capture.index, &name_len);
        color = get_color_for_capture(capture_name, name_len);

        if (start < len && color != 0) {
            if (end > len) {
                end = (uint32_t)len;
            }
            for (i = start; i < end; i++) {
                if (colors[i] == 0) {
                    colors[i] = (char)color;
                }
            }
        }
    }

    ts_tree_delete(tree);
}
