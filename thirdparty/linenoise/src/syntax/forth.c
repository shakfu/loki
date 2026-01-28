/* highlight_forth.c -- Tree-sitter based Forth syntax highlighting
 *
 * Uses tree-sitter to parse Forth code and highlight it using query captures
 * from simplified highlights.scm patterns.
 */

#include <stdlib.h>
#include <string.h>
#include "syntax/forth.h"
#include "syntax/theme.h"
#include <tree_sitter/api.h>

/* External function from tree-sitter-forth */
extern const TSLanguage *tree_sitter_forth(void);

/* Global state for the highlighter */
static TSParser *parser = NULL;
static TSQuery *query = NULL;
static TSQueryCursor *cursor = NULL;

/* Simplified Forth highlighting query */
static const char *FORTH_HIGHLIGHT_QUERY =
    /* Definition keywords */
    "[(start_definition) (end_definition)] @keyword\n"

    /* Control flow */
    "(control_flow) @keyword.control\n"

    /* I/O operations */
    "(io) @function.builtin\n"

    /* Operators */
    "(operator) @operator\n"

    /* Core builtins */
    "(core) @type\n"

    /* Numbers */
    "(character_literal) @constant.character\n"
    "(hex_number) @constant.numeric\n"
    "(binary_number) @constant.numeric\n"
    "(octal_number) @constant.numeric\n"
    "(float_number) @constant.numeric\n"
    "(double_cell_number) @constant.numeric\n"
    "(decimal_number) @constant.numeric\n"

    /* Strings */
    "(string) @string\n"

    /* Comments */
    "(line_comment) @comment.line\n"
    "(block_comment) @comment.block\n"
    "(stack_effect) @comment.documentation\n"

    /* User-defined words */
    "(word) @function\n"
;

/* Map capture names to theme token types */
static unsigned char get_color_for_capture(const char *name, uint32_t len) {
    /* Keywords */
    if (len >= 7 && strncmp(name, "keyword", 7) == 0) {
        if (len > 8 && strncmp(name + 8, "control", 7) == 0) {
            return theme_color(TOK_KEYWORD_CONTROL);
        }
        return theme_color(TOK_KEYWORD);
    }

    /* Comments */
    if (len >= 7 && strncmp(name, "comment", 7) == 0) {
        if (len > 8 && strncmp(name + 8, "documentation", 13) == 0) {
            return theme_color(TOK_COMMENT_DOC);
        }
        return theme_color(TOK_COMMENT);
    }

    /* Strings */
    if (len >= 6 && strncmp(name, "string", 6) == 0) {
        return theme_color(TOK_STRING);
    }

    /* Constants (numbers in Forth) */
    if (len >= 8 && strncmp(name, "constant", 8) == 0) {
        if (len > 9 && strncmp(name + 9, "numeric", 7) == 0) {
            return theme_color(TOK_NUMBER);
        }
        if (len > 9 && strncmp(name + 9, "character", 9) == 0) {
            return theme_color(TOK_STRING);
        }
        return theme_color(TOK_CONSTANT);
    }

    /* Functions */
    if (len >= 8 && strncmp(name, "function", 8) == 0) {
        if (len > 9 && strncmp(name + 9, "builtin", 7) == 0) {
            return theme_color(TOK_FUNCTION_BUILTIN);
        }
        return theme_color(TOK_FUNCTION);
    }

    /* Types */
    if (len >= 4 && strncmp(name, "type", 4) == 0) {
        return theme_color(TOK_TYPE);
    }

    /* Operators */
    if (len >= 8 && strncmp(name, "operator", 8) == 0) {
        return theme_color(TOK_OPERATOR);
    }

    return theme_color(TOK_DEFAULT);
}

int forth_highlight_init(void) {
    uint32_t error_offset;
    TSQueryError error_type;

    if (parser != NULL) {
        return 0; /* Already initialized */
    }

    parser = ts_parser_new();
    if (parser == NULL) {
        return -1;
    }

    if (!ts_parser_set_language(parser, tree_sitter_forth())) {
        ts_parser_delete(parser);
        parser = NULL;
        return -1;
    }

    query = ts_query_new(
        tree_sitter_forth(),
        FORTH_HIGHLIGHT_QUERY,
        (uint32_t)strlen(FORTH_HIGHLIGHT_QUERY),
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

void forth_highlight_free(void) {
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

void forth_highlight_callback(const char *buf, char *colors, size_t len) {
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
