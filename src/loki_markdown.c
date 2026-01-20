/* loki_markdown.c - Markdown parsing and rendering using cmark
 *
 * This module integrates the cmark library (CommonMark parser) into loki.
 * It provides functions for:
 * - Parsing markdown text into an AST
 * - Rendering markdown to HTML
 * - Rendering markdown to other formats (XML, LaTeX, etc.)
 * - Extracting document structure and metadata
 */

#include "loki_markdown.h"
#include "internal.h"
#include "loki/core.h"
#include <cmark.h>
#include <stdlib.h>
#include <string.h>

/* ======================= Markdown Parsing ================================= */

loki_markdown_doc *loki_markdown_parse(const char *text, size_t len, int options) {
    if (!text) {
        return NULL;
    }

    loki_markdown_doc *doc = malloc(sizeof(loki_markdown_doc));
    if (!doc) {
        return NULL;
    }

    /* Parse markdown text to AST using cmark */
    doc->root = cmark_parse_document(text, len, options);
    if (!doc->root) {
        free(doc);
        return NULL;
    }

    doc->options = options;
    return doc;
}

loki_markdown_doc *loki_markdown_parse_file(const char *filename, int options) {
    if (!filename) {
        return NULL;
    }

    FILE *fp = fopen(filename, "r");
    if (!fp) {
        return NULL;
    }

    /* Read entire file into buffer */
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size < 0) {
        fclose(fp);
        return NULL;
    }

    char *buffer = malloc(size + 1);
    if (!buffer) {
        fclose(fp);
        return NULL;
    }

    size_t read_size = fread(buffer, 1, size, fp);
    fclose(fp);
    buffer[read_size] = '\0';

    /* Parse buffer */
    loki_markdown_doc *doc = loki_markdown_parse(buffer, read_size, options);
    free(buffer);

    return doc;
}

void loki_markdown_free(loki_markdown_doc *doc) {
    if (!doc) {
        return;
    }

    if (doc->root) {
        cmark_node_free(doc->root);
    }

    free(doc);
}

/* ======================= Markdown Rendering =============================== */

char *loki_markdown_render_html(loki_markdown_doc *doc, int options) {
    if (!doc || !doc->root) {
        return NULL;
    }

    return cmark_render_html(doc->root, options);
}

char *loki_markdown_render_xml(loki_markdown_doc *doc, int options) {
    if (!doc || !doc->root) {
        return NULL;
    }

    return cmark_render_xml(doc->root, options);
}

char *loki_markdown_render_man(loki_markdown_doc *doc, int options, int width) {
    if (!doc || !doc->root) {
        return NULL;
    }

    return cmark_render_man(doc->root, options, width);
}

char *loki_markdown_render_commonmark(loki_markdown_doc *doc, int options, int width) {
    if (!doc || !doc->root) {
        return NULL;
    }

    return cmark_render_commonmark(doc->root, options, width);
}

char *loki_markdown_render_latex(loki_markdown_doc *doc, int options, int width) {
    if (!doc || !doc->root) {
        return NULL;
    }

    return cmark_render_latex(doc->root, options, width);
}

/* ======================= Direct Conversion Functions ====================== */

char *loki_markdown_to_html(const char *text, size_t len, int options) {
    if (!text) {
        return NULL;
    }

    return cmark_markdown_to_html(text, len, options);
}

/* ======================= Document Structure Extraction ==================== */

int loki_markdown_count_headings(loki_markdown_doc *doc) {
    if (!doc || !doc->root) {
        return 0;
    }

    int count = 0;
    cmark_iter *iter = cmark_iter_new(doc->root);
    cmark_event_type ev_type;

    while ((ev_type = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
        cmark_node *cur = cmark_iter_get_node(iter);
        if (ev_type == CMARK_EVENT_ENTER &&
            cmark_node_get_type(cur) == CMARK_NODE_HEADING) {
            count++;
        }
    }

    cmark_iter_free(iter);
    return count;
}

int loki_markdown_count_code_blocks(loki_markdown_doc *doc) {
    if (!doc || !doc->root) {
        return 0;
    }

    int count = 0;
    cmark_iter *iter = cmark_iter_new(doc->root);
    cmark_event_type ev_type;

    while ((ev_type = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
        cmark_node *cur = cmark_iter_get_node(iter);
        if (ev_type == CMARK_EVENT_ENTER &&
            cmark_node_get_type(cur) == CMARK_NODE_CODE_BLOCK) {
            count++;
        }
    }

    cmark_iter_free(iter);
    return count;
}

int loki_markdown_count_links(loki_markdown_doc *doc) {
    if (!doc || !doc->root) {
        return 0;
    }

    int count = 0;
    cmark_iter *iter = cmark_iter_new(doc->root);
    cmark_event_type ev_type;

    while ((ev_type = cmark_iter_next(iter)) != CMARK_EVENT_DONE) {
        cmark_node *cur = cmark_iter_get_node(iter);
        if (ev_type == CMARK_EVENT_ENTER &&
            cmark_node_get_type(cur) == CMARK_NODE_LINK) {
            count++;
        }
    }

    cmark_iter_free(iter);
    return count;
}

/* ======================= Heading Extraction ================================ */

loki_markdown_heading *loki_markdown_extract_headings(loki_markdown_doc *doc, int *count) {
    if (!doc || !doc->root || !count) {
        if (count) {
            *count = 0;
        }
        return NULL;
    }

    /* First pass: count headings */
    int num_headings = loki_markdown_count_headings(doc);
    if (num_headings == 0) {
        *count = 0;
        return NULL;
    }

    /* Allocate array for headings */
    loki_markdown_heading *headings = malloc(sizeof(loki_markdown_heading) * num_headings);
    if (!headings) {
        *count = 0;
        return NULL;
    }

    /* Second pass: extract heading information */
    int idx = 0;
    cmark_iter *iter = cmark_iter_new(doc->root);
    cmark_event_type ev_type;

    while ((ev_type = cmark_iter_next(iter)) != CMARK_EVENT_DONE && idx < num_headings) {
        cmark_node *cur = cmark_iter_get_node(iter);
        if (ev_type == CMARK_EVENT_ENTER &&
            cmark_node_get_type(cur) == CMARK_NODE_HEADING) {

            headings[idx].level = cmark_node_get_heading_level(cur);

            /* Extract heading text from child nodes */
            cmark_node *child = cmark_node_first_child(cur);
            if (child && cmark_node_get_type(child) == CMARK_NODE_TEXT) {
                const char *text = cmark_node_get_literal(child);
                if (text) {
                    headings[idx].text = strdup(text);
                } else {
                    headings[idx].text = NULL;
                }
            } else {
                headings[idx].text = NULL;
            }

            idx++;
        }
    }

    cmark_iter_free(iter);
    *count = idx;
    return headings;
}

void loki_markdown_free_headings(loki_markdown_heading *headings, int count) {
    if (!headings) {
        return;
    }

    for (int i = 0; i < count; i++) {
        free(headings[i].text);
    }

    free(headings);
}

/* ======================= Link Extraction =================================== */

loki_markdown_link *loki_markdown_extract_links(loki_markdown_doc *doc, int *count) {
    if (!doc || !doc->root || !count) {
        if (count) {
            *count = 0;
        }
        return NULL;
    }

    /* First pass: count links */
    int num_links = loki_markdown_count_links(doc);
    if (num_links == 0) {
        *count = 0;
        return NULL;
    }

    /* Allocate array for links */
    loki_markdown_link *links = malloc(sizeof(loki_markdown_link) * num_links);
    if (!links) {
        *count = 0;
        return NULL;
    }

    /* Second pass: extract link information */
    int idx = 0;
    cmark_iter *iter = cmark_iter_new(doc->root);
    cmark_event_type ev_type;

    while ((ev_type = cmark_iter_next(iter)) != CMARK_EVENT_DONE && idx < num_links) {
        cmark_node *cur = cmark_iter_get_node(iter);
        if (ev_type == CMARK_EVENT_ENTER &&
            cmark_node_get_type(cur) == CMARK_NODE_LINK) {

            /* Extract URL */
            const char *url = cmark_node_get_url(cur);
            if (url) {
                links[idx].url = strdup(url);
            } else {
                links[idx].url = NULL;
            }

            /* Extract title (if present) */
            const char *title = cmark_node_get_title(cur);
            if (title && title[0] != '\0') {
                links[idx].title = strdup(title);
            } else {
                links[idx].title = NULL;
            }

            /* Extract link text from child nodes */
            cmark_node *child = cmark_node_first_child(cur);
            if (child && cmark_node_get_type(child) == CMARK_NODE_TEXT) {
                const char *text = cmark_node_get_literal(child);
                if (text) {
                    links[idx].text = strdup(text);
                } else {
                    links[idx].text = NULL;
                }
            } else {
                links[idx].text = NULL;
            }

            idx++;
        }
    }

    cmark_iter_free(iter);
    *count = idx;
    return links;
}

void loki_markdown_free_links(loki_markdown_link *links, int count) {
    if (!links) {
        return;
    }

    for (int i = 0; i < count; i++) {
        free(links[i].url);
        free(links[i].title);
        free(links[i].text);
    }

    free(links);
}

/* ======================= Utility Functions ================================= */

const char *loki_markdown_version(void) {
    return cmark_version_string();
}

int loki_markdown_validate(const char *text, size_t len) {
    if (!text) {
        return 0;
    }

    /* Try to parse - if parsing succeeds, markdown is valid */
    cmark_node *doc = cmark_parse_document(text, len, CMARK_OPT_DEFAULT);
    if (doc) {
        cmark_node_free(doc);
        return 1;
    }

    return 0;
}
