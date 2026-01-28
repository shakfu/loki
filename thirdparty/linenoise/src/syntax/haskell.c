/* highlight_haskell.c -- Tree-sitter based Haskell syntax highlighting
 *
 * Uses tree-sitter to parse Haskell code and highlight it using query captures
 * from simplified highlights.scm patterns.
 */

#include <stdlib.h>
#include <string.h>
#include "syntax/haskell.h"
#include "syntax/theme.h"
#include <tree_sitter/api.h>

/* External function from tree-sitter-haskell */
extern const TSLanguage *tree_sitter_haskell(void);

/* Global state for the highlighter */
static TSParser *parser = NULL;
static TSQuery *query = NULL;
static TSQueryCursor *cursor = NULL;

/* Simplified Haskell highlighting query */
static const char *HASKELL_HIGHLIGHT_QUERY =
    /* Variables and parameters */
    "(variable) @variable\n"

    /* Literals */
    "(integer) @number\n"
    "(negation) @number\n"
    "(char) @character\n"
    "(string) @string\n"
    "(comment) @comment\n"

    /* Punctuation */
    "[\"(\" \")\" \"{\" \"}\" \"[\" \"]\"] @punctuation.bracket\n"
    "[\",\" \";\"] @punctuation.delimiter\n"

    /* Keywords */
    "[\"if\" \"then\" \"else\" \"case\" \"of\"] @keyword.conditional\n"
    "[\"import\" \"qualified\" \"module\"] @keyword.import\n"
    "[\"where\" \"let\" \"in\" \"class\" \"instance\" \"data\" \"newtype\"\n"
    " \"type\" \"as\" \"hiding\" \"deriving\" \"do\" \"mdo\" \"rec\"\n"
    " \"infix\" \"infixl\" \"infixr\" \"family\" \"pattern\" \"via\"\n"
    " \"stock\" \"anyclass\" \"forall\"] @keyword\n"

    /* Operators */
    "[(operator) (constructor_operator) (all_names) (wildcard)\n"
    " \".\" \"..\" \"=\" \"|\" \"::\" \"=>\" \"->\" \"<-\" \"\\\\\" \"`\" \"@\"] @operator\n"

    /* Types and constructors */
    "(name) @type\n"
    "(constructor) @constructor\n"

    /* Module */
    "(module (module_id) @module)\n"

    /* Booleans */
    "((constructor) @boolean (#any-of? @boolean \"True\" \"False\"))\n"

    /* Pragmas */
    "(pragma) @keyword.directive\n"
;

/* Map capture names to theme token types */
static unsigned char get_color_for_capture(const char *name, uint32_t len) {
    /* Keywords */
    if (len >= 7 && strncmp(name, "keyword", 7) == 0) {
        if (len > 8) {
            const char *suffix = name + 8;
            if (strncmp(suffix, "conditional", 11) == 0) {
                return theme_color(TOK_KEYWORD_CONTROL);
            }
            if (strncmp(suffix, "import", 6) == 0) {
                return theme_color(TOK_KEYWORD_IMPORT);
            }
            if (strncmp(suffix, "directive", 9) == 0) {
                return theme_color(TOK_PREPROCESSOR);
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

    /* Numbers */
    if (len >= 6 && strncmp(name, "number", 6) == 0) {
        return theme_color(TOK_NUMBER);
    }

    /* Characters */
    if (len >= 9 && strncmp(name, "character", 9) == 0) {
        return theme_color(TOK_STRING);
    }

    /* Functions */
    if (len >= 8 && strncmp(name, "function", 8) == 0) {
        return theme_color(TOK_FUNCTION);
    }

    /* Types */
    if (len >= 4 && strncmp(name, "type", 4) == 0) {
        return theme_color(TOK_TYPE);
    }

    /* Constructors */
    if (len >= 11 && strncmp(name, "constructor", 11) == 0) {
        return theme_color(TOK_CONSTRUCTOR);
    }

    /* Operators */
    if (len >= 8 && strncmp(name, "operator", 8) == 0) {
        return theme_color(TOK_OPERATOR);
    }

    /* Booleans */
    if (len >= 7 && strncmp(name, "boolean", 7) == 0) {
        return theme_color(TOK_BOOLEAN);
    }

    /* Module */
    if (len >= 6 && strncmp(name, "module", 6) == 0) {
        return theme_color(TOK_MODULE);
    }

    /* Variables */
    if (len >= 8 && strncmp(name, "variable", 8) == 0) {
        return theme_color(TOK_VARIABLE);
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

int haskell_highlight_init(void) {
    uint32_t error_offset;
    TSQueryError error_type;

    if (parser != NULL) {
        return 0; /* Already initialized */
    }

    parser = ts_parser_new();
    if (parser == NULL) {
        return -1;
    }

    if (!ts_parser_set_language(parser, tree_sitter_haskell())) {
        ts_parser_delete(parser);
        parser = NULL;
        return -1;
    }

    query = ts_query_new(
        tree_sitter_haskell(),
        HASKELL_HIGHLIGHT_QUERY,
        (uint32_t)strlen(HASKELL_HIGHLIGHT_QUERY),
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

void haskell_highlight_free(void) {
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

void haskell_highlight_callback(const char *buf, char *colors, size_t len) {
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
