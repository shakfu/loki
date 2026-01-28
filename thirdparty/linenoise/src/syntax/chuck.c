/* highlight_chuck.c -- Tree-sitter based ChucK syntax highlighting
 *
 * Uses tree-sitter to parse ChucK code and highlight it using query captures
 * from simplified highlights.scm patterns.
 */

#include <stdlib.h>
#include <string.h>
#include "syntax/chuck.h"
#include "syntax/theme.h"
#include <tree_sitter/api.h>

/* External function from tree-sitter-chuck */
extern const TSLanguage *tree_sitter_chuck(void);

/* Global state for the highlighter */
static TSParser *parser = NULL;
static TSQuery *query = NULL;
static TSQueryCursor *cursor = NULL;

/* Simplified ChucK highlighting query */
static const char *CHUCK_HIGHLIGHT_QUERY =
    /* Keywords */
    "\"@doc\" @special\n"
    "\"do\" @keyword.control.repeat\n"
    "\"fun\" @keyword.function\n"
    "\"function\" @keyword.function\n"
    "\"if\" @keyword.control.conditional\n"
    "\"repeat\" @keyword.control.repeat\n"
    "\"return\" @keyword.control.return\n"
    "\"spork\" @function.builtin\n"
    "\"until\" @keyword.control.repeat\n"
    "\"while\" @keyword.control.repeat\n"
    "(keyword) @keyword\n"

    /* Comments */
    "(block_comment) @comment.block\n"
    "(line_comment) @comment.line\n"

    /* Literals */
    "(boolean_literal_value) @constant.builtin.boolean\n"
    "(float) @constant.numeric.float\n"
    "(hexidecimal) @constant.numeric\n"
    "(int) @constant.numeric.integer\n"
    "(special_literal_value) @constant.builtin\n"
    "(string) @string\n"

    /* Types */
    "(class_identifier) @type\n"
    "(duration_identifier) @type\n"
    "(primitive_type) @type.builtin\n"

    /* Functions */
    "(function_definition name: (class_identifier) @function)\n"
    "(function_definition name: (variable_identifier) @function)\n"

    /* Operators */
    "(chuck_operator) @operator\n"
    "(operator) @operator\n"

    /* Builtins */
    "(global_unit_generator) @variable.builtin\n"
;

/* Map capture names to theme token types */
static unsigned char get_color_for_capture(const char *name, uint32_t len) {
    /* Keywords */
    if (len >= 7 && strncmp(name, "keyword", 7) == 0) {
        if (len > 8) {
            const char *suffix = name + 8;
            if (strncmp(suffix, "control", 7) == 0) {
                return theme_color(TOK_KEYWORD_CONTROL);
            }
            if (strncmp(suffix, "function", 8) == 0) {
                return theme_color(TOK_KEYWORD_FUNCTION);
            }
        }
        return theme_color(TOK_KEYWORD);
    }

    /* Comments */
    if (len >= 7 && strncmp(name, "comment", 7) == 0) {
        return theme_color(TOK_COMMENT);
    }

    /* Strings */
    if (len >= 6 && strncmp(name, "string", 6) == 0) {
        return theme_color(TOK_STRING);
    }

    /* Constants */
    if (len >= 8 && strncmp(name, "constant", 8) == 0) {
        if (len > 9 && strncmp(name + 9, "builtin", 7) == 0) {
            if (len > 17 && strncmp(name + 17, "boolean", 7) == 0) {
                return theme_color(TOK_BOOLEAN);
            }
            return theme_color(TOK_CONSTANT_BUILTIN);
        }
        if (len > 9 && strncmp(name + 9, "numeric", 7) == 0) {
            if (len > 17 && strncmp(name + 17, "float", 5) == 0) {
                return theme_color(TOK_NUMBER_FLOAT);
            }
            return theme_color(TOK_NUMBER);
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
        if (len > 5 && strncmp(name + 5, "builtin", 7) == 0) {
            return theme_color(TOK_TYPE_BUILTIN);
        }
        return theme_color(TOK_TYPE);
    }

    /* Operators */
    if (len >= 8 && strncmp(name, "operator", 8) == 0) {
        return theme_color(TOK_OPERATOR);
    }

    /* Special */
    if (len >= 7 && strncmp(name, "special", 7) == 0) {
        return theme_color(TOK_PREPROCESSOR);
    }

    /* Variables */
    if (len >= 8 && strncmp(name, "variable", 8) == 0) {
        if (len > 9 && strncmp(name + 9, "builtin", 7) == 0) {
            return theme_color(TOK_VARIABLE_BUILTIN);
        }
        return theme_color(TOK_VARIABLE);
    }

    return theme_color(TOK_DEFAULT);
}

int chuck_highlight_init(void) {
    uint32_t error_offset;
    TSQueryError error_type;

    if (parser != NULL) {
        return 0; /* Already initialized */
    }

    parser = ts_parser_new();
    if (parser == NULL) {
        return -1;
    }

    if (!ts_parser_set_language(parser, tree_sitter_chuck())) {
        ts_parser_delete(parser);
        parser = NULL;
        return -1;
    }

    query = ts_query_new(
        tree_sitter_chuck(),
        CHUCK_HIGHLIGHT_QUERY,
        (uint32_t)strlen(CHUCK_HIGHLIGHT_QUERY),
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

void chuck_highlight_free(void) {
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

void chuck_highlight_callback(const char *buf, char *colors, size_t len) {
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
