/* markdown.h -- Tree-sitter based Markdown syntax highlighting for linenoise
 *
 * This module provides a syntax highlighting callback that uses tree-sitter
 * to parse Markdown and colorize it.
 */

#ifndef HIGHLIGHT_MARKDOWN_H
#define HIGHLIGHT_MARKDOWN_H

#include <stddef.h>

/* Initialize the Markdown highlighter.
 * Must be called before using markdown_highlight_callback.
 * Returns 0 on success, -1 on failure. */
int markdown_highlight_init(void);

/* Free resources used by the Markdown highlighter.
 * Should be called when highlighting is no longer needed. */
void markdown_highlight_free(void);

/* Syntax highlighting callback for linenoise.
 * Can be passed to linenoise_set_highlight_callback(). */
void markdown_highlight_callback(const char *buf, char *colors, size_t len);

#endif /* HIGHLIGHT_MARKDOWN_H */
