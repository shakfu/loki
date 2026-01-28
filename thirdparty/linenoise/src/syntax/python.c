/* python.c -- Tree-sitter based Python syntax highlighting
 *
 * Uses tree-sitter to parse Python code and highlight it using query captures.
 */

#include <stdlib.h>
#include <string.h>
#include "syntax/python.h"
#include "syntax/theme.h"
#include <tree_sitter/api.h>

/* External function from tree-sitter-python */
extern const TSLanguage *tree_sitter_python(void);

/* Global state for the highlighter */
static TSParser *parser = NULL;
static TSQuery *query = NULL;
static TSQueryCursor *cursor = NULL;

/* Python highlighting query */
static const char *PYTHON_HIGHLIGHT_QUERY =
    /* Keywords */
    "[\"as\" \"assert\" \"async\" \"await\" \"break\" \"class\" \"continue\"\n"
    " \"def\" \"del\" \"elif\" \"else\" \"except\" \"finally\" \"for\" \"from\"\n"
    " \"global\" \"if\" \"import\" \"lambda\" \"nonlocal\" \"pass\" \"raise\"\n"
    " \"return\" \"try\" \"while\" \"with\" \"yield\" \"match\" \"case\"] @keyword\n"

    /* Operators */
    "[\"and\" \"in\" \"is\" \"not\" \"or\" \"is not\" \"not in\"] @keyword.operator\n"
    "[\"-\" \"!=\" \"*\" \"**\" \"/\" \"//\" \"%\" \"^\" \"+\" \"<\" \"<=\"\n"
    " \"=\" \"==\" \">\" \">=\" \"|\" \"~\" \"@\" \"&\" \"<<\" \">>\"\n"
    " \"->\" \":=\"] @operator\n"

    /* Constants */
    "[(none) (true) (false)] @constant.builtin\n"

    /* Literals */
    "[(integer) (float)] @number\n"
    "(string) @string\n"
    "(escape_sequence) @string.escape\n"
    "(comment) @comment\n"

    /* Identifiers */
    "(identifier) @variable\n"
    "((identifier) @constant (#match? @constant \"^[A-Z][A-Z_]*$\"))\n"

    /* Functions */
    "(function_definition name: (identifier) @function)\n"
    "(call function: (identifier) @function.call)\n"
    "(call function: (attribute attribute: (identifier) @function.method))\n"
    "(decorator (identifier) @function)\n"

    /* Types */
    "(type (identifier) @type)\n"
    "(attribute attribute: (identifier) @property)\n"

    /* Punctuation */
    "[\"(\" \")\" \"[\" \"]\" \"{\" \"}\"] @punctuation.bracket\n"
    "[\",\" \".\" \":\" \";\"] @punctuation.delimiter\n"
;

/* Map capture names to theme colors */
static unsigned char get_color_for_capture(const char *name, uint32_t len) {
    /* Keywords */
    if (len >= 7 && strncmp(name, "keyword", 7) == 0) {
        if (len > 8 && strncmp(name + 8, "operator", 8) == 0) {
            return theme_color(TOK_KEYWORD_OPERATOR);
        }
        return theme_color(TOK_KEYWORD);
    }

    /* Comments */
    if (len >= 7 && strncmp(name, "comment", 7) == 0) {
        return theme_color(TOK_COMMENT);
    }

    /* Strings */
    if (len >= 6 && strncmp(name, "string", 6) == 0) {
        if (len > 7 && strncmp(name + 7, "escape", 6) == 0) {
            return theme_color(TOK_STRING_ESCAPE);
        }
        return theme_color(TOK_STRING);
    }

    /* Numbers */
    if (len >= 6 && strncmp(name, "number", 6) == 0) {
        return theme_color(TOK_NUMBER);
    }

    /* Functions */
    if (len >= 8 && strncmp(name, "function", 8) == 0) {
        if (len > 9 && strncmp(name + 9, "call", 4) == 0) {
            return theme_color(TOK_FUNCTION_CALL);
        }
        if (len > 9 && strncmp(name + 9, "method", 6) == 0) {
            return theme_color(TOK_FUNCTION_METHOD);
        }
        return theme_color(TOK_FUNCTION);
    }

    /* Constants */
    if (len >= 8 && strncmp(name, "constant", 8) == 0) {
        if (len > 9 && strncmp(name + 9, "builtin", 7) == 0) {
            return theme_color(TOK_CONSTANT_BUILTIN);
        }
        return theme_color(TOK_CONSTANT);
    }

    /* Operators */
    if (len >= 8 && strncmp(name, "operator", 8) == 0) {
        return theme_color(TOK_OPERATOR);
    }

    /* Types */
    if (len >= 4 && strncmp(name, "type", 4) == 0) {
        return theme_color(TOK_TYPE);
    }

    /* Property */
    if (len >= 8 && strncmp(name, "property", 8) == 0) {
        return theme_color(TOK_VARIABLE_PROPERTY);
    }

    /* Punctuation */
    if (len >= 11 && strncmp(name, "punctuation", 11) == 0) {
        if (len > 12 && strncmp(name + 12, "bracket", 7) == 0) {
            return theme_color(TOK_PUNCTUATION_BRACKET);
        }
        return theme_color(TOK_PUNCTUATION_DELIMITER);
    }

    /* Variables */
    if (len >= 8 && strncmp(name, "variable", 8) == 0) {
        return theme_color(TOK_VARIABLE);
    }

    return theme_color(TOK_DEFAULT);
}

int python_highlight_init(void) {
    uint32_t error_offset;
    TSQueryError error_type;

    if (parser != NULL) {
        return 0;
    }

    parser = ts_parser_new();
    if (parser == NULL) {
        return -1;
    }

    if (!ts_parser_set_language(parser, tree_sitter_python())) {
        ts_parser_delete(parser);
        parser = NULL;
        return -1;
    }

    query = ts_query_new(
        tree_sitter_python(),
        PYTHON_HIGHLIGHT_QUERY,
        (uint32_t)strlen(PYTHON_HIGHLIGHT_QUERY),
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

void python_highlight_free(void) {
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

void python_highlight_callback(const char *buf, char *colors, size_t len) {
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
