/* highlight_haskell.h -- Tree-sitter based Haskell syntax highlighting for linenoise
 *
 * This module provides a syntax highlighting callback that uses tree-sitter
 * to parse Haskell code and colorize it according to the highlights.scm queries.
 */

#ifndef HIGHLIGHT_HASKELL_H
#define HIGHLIGHT_HASKELL_H

#include <stddef.h>

/* Initialize the Haskell highlighter.
 * Must be called before using haskell_highlight_callback.
 * Returns 0 on success, -1 on failure. */
int haskell_highlight_init(void);

/* Free resources used by the Haskell highlighter.
 * Should be called when highlighting is no longer needed. */
void haskell_highlight_free(void);

/* Syntax highlighting callback for linenoise.
 * Can be passed to linenoise_set_highlight_callback().
 *
 * Color values used:
 *   0 = default
 *   1 = red (errors)
 *   2 = green (strings)
 *   3 = yellow (numbers)
 *   4 = blue (functions)
 *   5 = magenta (keywords)
 *   6 = cyan (variables/identifiers)
 *   7 = white
 *   Add 8 for bold variants
 */
void haskell_highlight_callback(const char *buf, char *colors, size_t len);

#endif /* HIGHLIGHT_HASKELL_H */
