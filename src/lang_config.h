/**
 * @file lang_config.h
 * @brief Language state configuration for Loki editor.
 *
 * This file defines the language-specific state fields that can be embedded
 * in EditorModel. In the core Loki editor, no domain-specific languages are
 * included - this is a stub that defines an empty macro.
 *
 * Projects embedding Loki can define their own language states here.
 */

#ifndef LANG_CONFIG_H
#define LANG_CONFIG_H

/* No domain-specific language states in core Loki.
 * This macro expands to nothing - EditorModel has no language-specific fields.
 *
 * To add language states, define fields like:
 *   #define LOKI_LANG_STATE_FIELDS \
 *       struct MyLangState *my_lang;
 */
#define LOKI_LANG_STATE_FIELDS /* empty */

/* No languages to initialize in core Loki.
 * In psnd, this calls alda_loki_lang_init(), joy_loki_lang_init(), etc.
 */
#define LOKI_LANG_INIT_ALL() /* empty */

#endif /* LANG_CONFIG_H */
