/* highlight_faust.c -- Tree-sitter based Faust syntax highlighting
 *
 * Uses tree-sitter to parse Faust code and highlight it using query captures
 * from simplified highlights.scm patterns.
 */

#include <stdlib.h>
#include <string.h>
#include "syntax/faust.h"
#include "syntax/theme.h"
#include <tree_sitter/api.h>

/* External function from tree-sitter-faust */
extern const TSLanguage *tree_sitter_faust(void);

/* Global state for the highlighter */
static TSParser *parser = NULL;
static TSQuery *query = NULL;
static TSQueryCursor *cursor = NULL;

/* Simplified Faust highlighting query */
static const char *FAUST_HIGHLIGHT_QUERY =
    /* Identifiers */
    "(identifier) @variable\n"
    "[\"process\" \"effect\"] @variable.builtin\n"

    /* Literals */
    "(documentation) @string.documentation\n"
    "[(string) (fstring)] @string\n"
    "(int) @number\n"
    "(real) @number.float\n"

    /* Functions */
    "(function_definition name: (identifier) @function)\n"
    "(function_call (identifier) @function.call)\n"

    /* Builtin functions */
    "[\"exp\" \"log\" \"log10\" \"sqrt\" \"abs\" \"floor\" \"ceil\" \"rint\" \"round\"\n"
    " \"acos\" \"asin\" \"atan\" \"cos\" \"sin\" \"tan\" \"atan2\" \"int\" \"float\"\n"
    " \"pow\" \"min\" \"max\" \"fmod\" \"remainder\" \"prefix\" \"attach\" \"enable\"\n"
    " \"control\" \"rdtable\" \"rwtable\" \"select2\" \"select3\" \"lowest\" \"highest\"\n"
    " \"assertbounds\" \"button\" \"checkbox\" \"soundfile\" \"inputs\" \"outputs\"\n"
    " \"route\"] @function.builtin\n"

    /* Operators */
    "[\"=\" \"=>\" \"->\"] @operator\n"
    "(recursive \"~\" @operator)\n"
    "(sequential \":\" @operator)\n"
    "(split \"<:\" @operator)\n"
    "(merge \":>\" @operator)\n"
    "(parallel \",\" @operator)\n"

    /* Keywords */
    "(file_import \"import\" @keyword.import)\n"
    "[\"declare\" \"with\" \"environment\" \"case\" \"ffunction\" \"fconstant\" \"fvariable\"] @keyword\n"

    /* Punctuation */
    "[\";\" \".\"] @punctuation.delimiter\n"
    "[\"(\" \")\" \"[\" \"]\" \"{\" \"}\"] @punctuation.bracket\n"

    /* Comments */
    "(comment) @comment\n"
;

/* Map capture names to theme token types */
static unsigned char get_color_for_capture(const char *name, uint32_t len) {
    /* Keywords */
    if (len >= 7 && strncmp(name, "keyword", 7) == 0) {
        if (len > 8 && strncmp(name + 8, "import", 6) == 0) {
            return theme_color(TOK_KEYWORD_IMPORT);
        }
        return theme_color(TOK_KEYWORD);
    }

    /* Comments */
    if (len >= 7 && strncmp(name, "comment", 7) == 0) {
        return theme_color(TOK_COMMENT);
    }

    /* Strings */
    if (len >= 6 && strncmp(name, "string", 6) == 0) {
        if (len > 7 && strncmp(name + 7, "documentation", 13) == 0) {
            return theme_color(TOK_COMMENT_DOC);
        }
        return theme_color(TOK_STRING);
    }

    /* Numbers */
    if (len >= 6 && strncmp(name, "number", 6) == 0) {
        if (len > 7 && strncmp(name + 7, "float", 5) == 0) {
            return theme_color(TOK_NUMBER_FLOAT);
        }
        return theme_color(TOK_NUMBER);
    }

    /* Functions */
    if (len >= 8 && strncmp(name, "function", 8) == 0) {
        if (len > 9 && strncmp(name + 9, "builtin", 7) == 0) {
            return theme_color(TOK_FUNCTION_BUILTIN);
        }
        if (len > 9 && strncmp(name + 9, "call", 4) == 0) {
            return theme_color(TOK_FUNCTION_CALL);
        }
        return theme_color(TOK_FUNCTION);
    }

    /* Variables */
    if (len >= 8 && strncmp(name, "variable", 8) == 0) {
        if (len > 9 && strncmp(name + 9, "builtin", 7) == 0) {
            return theme_color(TOK_VARIABLE_BUILTIN);
        }
        return theme_color(TOK_VARIABLE);
    }

    /* Operators */
    if (len >= 8 && strncmp(name, "operator", 8) == 0) {
        return theme_color(TOK_OPERATOR);
    }

    /* Punctuation */
    if (len >= 11 && strncmp(name, "punctuation", 11) == 0) {
        if (len > 12 && strncmp(name + 12, "bracket", 7) == 0) {
            return theme_color(TOK_PUNCTUATION_BRACKET);
        }
        if (len > 12 && strncmp(name + 12, "delimiter", 9) == 0) {
            return theme_color(TOK_PUNCTUATION_DELIMITER);
        }
        return theme_color(TOK_PUNCTUATION);
    }

    return theme_color(TOK_DEFAULT);
}

int faust_highlight_init(void) {
    uint32_t error_offset;
    TSQueryError error_type;

    if (parser != NULL) {
        return 0; /* Already initialized */
    }

    parser = ts_parser_new();
    if (parser == NULL) {
        return -1;
    }

    if (!ts_parser_set_language(parser, tree_sitter_faust())) {
        ts_parser_delete(parser);
        parser = NULL;
        return -1;
    }

    query = ts_query_new(
        tree_sitter_faust(),
        FAUST_HIGHLIGHT_QUERY,
        (uint32_t)strlen(FAUST_HIGHLIGHT_QUERY),
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

void faust_highlight_free(void) {
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

void faust_highlight_callback(const char *buf, char *colors, size_t len) {
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
