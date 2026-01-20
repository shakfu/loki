/* lang_scheme.h - Scheme R7RS language definition */

#ifndef LOKI_LANG_SCHEME_H
#define LOKI_LANG_SCHEME_H

/* Scheme R7RS keywords (core language only, no domain-specific extensions) */
static char *Scheme_HL_keywords[] = {
    /* Special forms */
    "define","define-syntax","define-library","define-record-type",
    "lambda","let","let*","letrec","letrec*","let-values","let*-values",
    "if","cond","case","else","when","unless",
    "and","or","not","begin","do","set!",
    "quote","quasiquote","unquote","unquote-splicing",
    "import","export","include","syntax-rules",
    /* Common functions */
    "apply","map","for-each","filter","fold","append","reverse",
    "list","cons","car","cdr","cadr","caddr","length",
    "display","newline","write","read","load","eval",
    /* Type predicates (highlighted as types) */
    "null?|","pair?|","list?|","symbol?|","number?|","string?|",
    "boolean?|","procedure?|","vector?|","zero?|","positive?|","negative?|",
    NULL
};

static char *Scheme_HL_extensions[] = {".scm",".ss",".scheme",".sld",NULL};

#endif /* LOKI_LANG_SCHEME_H */
