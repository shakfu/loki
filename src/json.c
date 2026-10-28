/* json.c - Minimal JSON implementation for editor RPC
 *
 * Simple JSON serializer and parser for the JSON-RPC harness.
 */

#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <limits.h>

/* ======================= JSON Builder ====================================== */

#define INITIAL_CAP 1024

void json_builder_init(JsonBuilder *jb) {
    jb->buf = malloc(INITIAL_CAP);
    jb->len = 0;
    jb->cap = INITIAL_CAP;
    jb->error = (jb->buf == NULL);
    jb->depth = 0;
    jb->need_comma = 0;
    if (jb->buf) jb->buf[0] = '\0';
}

void json_builder_free(JsonBuilder *jb) {
    free(jb->buf);
    jb->buf = NULL;
    jb->len = 0;
    jb->cap = 0;
}

const char *json_builder_get(JsonBuilder *jb) {
    if (jb->error) return "{}";
    return jb->buf;
}

void json_builder_reset(JsonBuilder *jb) {
    jb->len = 0;
    jb->error = 0;
    jb->depth = 0;
    jb->need_comma = 0;
    if (jb->buf) jb->buf[0] = '\0';
}

static void json_append(JsonBuilder *jb, const char *s, size_t len) {
    if (jb->error) return;

    while (jb->len + len + 1 > jb->cap) {
        size_t new_cap = jb->cap * 2;
        char *new_buf = realloc(jb->buf, new_cap);
        if (!new_buf) {
            jb->error = 1;
            return;
        }
        jb->buf = new_buf;
        jb->cap = new_cap;
    }

    memcpy(jb->buf + jb->len, s, len);
    jb->len += len;
    jb->buf[jb->len] = '\0';
}

static void json_append_str(JsonBuilder *jb, const char *s) {
    json_append(jb, s, strlen(s));
}

static void json_maybe_comma(JsonBuilder *jb) {
    if (jb->need_comma) {
        json_append_str(jb, ",");
    }
    jb->need_comma = 0;
}

void json_object_start(JsonBuilder *jb) {
    json_maybe_comma(jb);
    json_append_str(jb, "{");
    jb->depth++;
    jb->need_comma = 0;
}

void json_object_end(JsonBuilder *jb) {
    jb->depth--;
    json_append_str(jb, "}");
    jb->need_comma = 1;
}

void json_array_start(JsonBuilder *jb) {
    json_maybe_comma(jb);
    json_append_str(jb, "[");
    jb->depth++;
    jb->need_comma = 0;
}

void json_array_end(JsonBuilder *jb) {
    jb->depth--;
    json_append_str(jb, "]");
    jb->need_comma = 1;
}

static void json_escape_string(JsonBuilder *jb, const char *s, size_t len);

void json_key(JsonBuilder *jb, const char *key) {
    json_key_len(jb, key, strlen(key));
}

void json_key_len(JsonBuilder *jb, const char *key, size_t len) {
    json_maybe_comma(jb);
    json_escape_string(jb, key, len);
    json_append_str(jb, ":");
}

/* Escape special characters in string */
static void json_escape_string(JsonBuilder *jb, const char *s, size_t len) {
    json_append_str(jb, "\"");

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  json_append_str(jb, "\\\""); break;
            case '\\': json_append_str(jb, "\\\\"); break;
            case '\b': json_append_str(jb, "\\b"); break;
            case '\f': json_append_str(jb, "\\f"); break;
            case '\n': json_append_str(jb, "\\n"); break;
            case '\r': json_append_str(jb, "\\r"); break;
            case '\t': json_append_str(jb, "\\t"); break;
            default:
                if (c < 32) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    json_append_str(jb, buf);
                } else {
                    json_append(jb, (const char *)&c, 1);
                }
                break;
        }
    }

    json_append_str(jb, "\"");
}

void json_string(JsonBuilder *jb, const char *value) {
    json_maybe_comma(jb);
    if (value) {
        json_escape_string(jb, value, strlen(value));
    } else {
        json_append_str(jb, "null");
    }
    jb->need_comma = 1;
}

void json_string_len(JsonBuilder *jb, const char *value, size_t len) {
    json_maybe_comma(jb);
    if (value) {
        json_escape_string(jb, value, len);
    } else {
        json_append_str(jb, "null");
    }
    jb->need_comma = 1;
}

void json_int(JsonBuilder *jb, int value) {
    json_maybe_comma(jb);
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    json_append_str(jb, buf);
    jb->need_comma = 1;
}

void json_int64(JsonBuilder *jb, long long value) {
    json_maybe_comma(jb);
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", value);
    json_append_str(jb, buf);
    jb->need_comma = 1;
}

int json_double(JsonBuilder *jb, double value) {
    if (value != value || value - value != 0) return -1;  /* NaN, inf */
    json_maybe_comma(jb);
    char buf[32];
    snprintf(buf, sizeof(buf), "%.17g", value);
    /* Keep it a double on the way back: "1e+20" and "1.5" already are. */
    if (!strpbrk(buf, ".eE")) strcat(buf, ".0");
    json_append_str(jb, buf);
    jb->need_comma = 1;
    return 0;
}

void json_bool(JsonBuilder *jb, int value) {
    json_maybe_comma(jb);
    json_append_str(jb, value ? "true" : "false");
    jb->need_comma = 1;
}

void json_null(JsonBuilder *jb) {
    json_maybe_comma(jb);
    json_append_str(jb, "null");
    jb->need_comma = 1;
}

void json_kv_string(JsonBuilder *jb, const char *key, const char *value) {
    json_key(jb, key);
    jb->need_comma = 0;
    json_string(jb, value);
}

void json_kv_int(JsonBuilder *jb, const char *key, int value) {
    json_key(jb, key);
    jb->need_comma = 0;
    json_int(jb, value);
}

void json_kv_bool(JsonBuilder *jb, const char *key, int value) {
    json_key(jb, key);
    jb->need_comma = 0;
    json_bool(jb, value);
}

/* ======================= JSON Parser ======================================= */

typedef struct {
    const char *json;
    size_t pos;
    size_t len;
    int depth;      /* Current container nesting; capped at JSON_MAX_DEPTH */
} JsonParser;

static void skip_whitespace(JsonParser *p) {
    while (p->pos < p->len && isspace((unsigned char)p->json[p->pos])) {
        p->pos++;
    }
}

static int peek(JsonParser *p) {
    skip_whitespace(p);
    if (p->pos >= p->len) return -1;
    return p->json[p->pos];
}

static int consume(JsonParser *p) {
    skip_whitespace(p);
    if (p->pos >= p->len) return -1;
    return p->json[p->pos++];
}

static int expect(JsonParser *p, char c) {
    if (consume(p) != c) return 0;
    return 1;
}

static JsonValue parse_value(JsonParser *p);

/* Parse 4 hex digits at p->pos into *out. Returns 0 if they are not there. */
static int parse_hex4(JsonParser *p, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        if (p->pos >= p->len) return 0;
        int c = (unsigned char)p->json[p->pos++];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

/* Write code point cp as UTF-8; returns bytes written (1-4). */
static size_t utf8_encode(unsigned cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Decode \uXXXX (p->pos just past the 'u'), joining surrogate pairs. A lone
 * surrogate becomes U+FFFD, since it has no UTF-8 encoding. Returns the code
 * point, or -1 on malformed hex. */
static long parse_unicode_escape(JsonParser *p) {
    unsigned hi;
    if (!parse_hex4(p, &hi)) return -1;
    if (hi < 0xD800 || hi > 0xDFFF) return (long)hi;
    if (hi >= 0xDC00) return 0xFFFD;  /* low surrogate first */
    if (p->pos + 1 < p->len && p->json[p->pos] == '\\' && p->json[p->pos + 1] == 'u') {
        size_t save = p->pos;
        unsigned lo;
        p->pos += 2;
        if (!parse_hex4(p, &lo)) return -1;
        if (lo >= 0xDC00 && lo <= 0xDFFF)
            return 0x10000 + (((long)hi - 0xD800) << 10) + ((long)lo - 0xDC00);
        p->pos = save;  /* not a pair: decode the next escape on its own */
    }
    return 0xFFFD;
}

static JsonValue parse_string(JsonParser *p) {
    JsonValue result = { .type = JSON_ERROR };

    if (!expect(p, '"')) return result;

    size_t start = p->pos;

    /* First pass: find the closing quote. Decoding never lengthens the
     * input (\uXXXX is 6 bytes for at most 3; a pair is 12 for 4), so the
     * span length bounds the output. */
    while (p->pos < p->len && p->json[p->pos] != '"') {
        p->pos += (p->json[p->pos] == '\\' && p->pos + 1 < p->len) ? 2 : 1;
    }

    if (p->pos >= p->len) return result;

    char *str = malloc(p->pos - start + 1);
    if (!str) return result;

    p->pos = start;
    size_t out = 0;

    while (p->pos < p->len && p->json[p->pos] != '"') {
        if (p->json[p->pos] == '\\' && p->pos + 1 < p->len) {
            p->pos++;
            switch (p->json[p->pos]) {
                case '"':  str[out++] = '"'; break;
                case '\\': str[out++] = '\\'; break;
                case 'b':  str[out++] = '\b'; break;
                case 'f':  str[out++] = '\f'; break;
                case 'n':  str[out++] = '\n'; break;
                case 'r':  str[out++] = '\r'; break;
                case 't':  str[out++] = '\t'; break;
                case 'u': {
                    p->pos++;
                    long cp = parse_unicode_escape(p);
                    if (cp < 0) {
                        free(str);
                        return result;
                    }
                    out += utf8_encode((unsigned)cp, str + out);
                    continue;  /* p->pos is already past the escape */
                }
                default:
                    str[out++] = p->json[p->pos];
                    break;
            }
            p->pos++;
        } else {
            str[out++] = p->json[p->pos++];
        }
    }
    str[out] = '\0';

    if (!expect(p, '"')) {
        free(str);
        return result;
    }

    result.type = JSON_STRING;
    result.data.string_val.str = str;
    result.data.string_val.len = out;
    return result;
}

static JsonValue parse_number(JsonParser *p) {
    JsonValue result = { .type = JSON_ERROR };
    size_t start = p->pos;

    if (p->json[p->pos] == '-') p->pos++;

    /* Accumulate with saturation: signed overflow would be undefined. */
    int val = 0;
    int overflow = 0;
    int digits = 0;
    while (p->pos < p->len && isdigit((unsigned char)p->json[p->pos])) {
        int d = p->json[p->pos] - '0';
        if (val > (INT_MAX - d) / 10) {
            overflow = 1;
        } else {
            val = val * 10 + d;
        }
        digits++;
        p->pos++;
    }
    if (digits == 0) return result;   /* "-" with no digits is not a number */

    int is_double = overflow;
    if (p->pos < p->len && p->json[p->pos] == '.') {
        is_double = 1;
        p->pos++;
        while (p->pos < p->len && isdigit((unsigned char)p->json[p->pos])) {
            p->pos++;
        }
    }
    if (p->pos < p->len && (p->json[p->pos] == 'e' || p->json[p->pos] == 'E')) {
        is_double = 1;
        p->pos++;
        if (p->pos < p->len && (p->json[p->pos] == '+' || p->json[p->pos] == '-')) {
            p->pos++;
        }
        while (p->pos < p->len && isdigit((unsigned char)p->json[p->pos])) {
            p->pos++;
        }
    }

    if (is_double) {
        /* strtod stops at the same place: the span is a valid JSON number. */
        result.type = JSON_DOUBLE;
        result.data.double_val = strtod(p->json + start, NULL);
        return result;
    }

    result.type = JSON_INT;
    result.data.int_val = (p->json[start] == '-') ? -val : val;
    return result;
}

static JsonValue parse_array(JsonParser *p) {
    JsonValue result = { .type = JSON_ERROR };

    if (!expect(p, '[')) return result;

    size_t cap = 8;
    size_t count = 0;
    JsonValue *items = malloc(cap * sizeof(JsonValue));
    if (!items) return result;

    skip_whitespace(p);
    if (peek(p) != ']') {
        while (1) {
            if (count >= cap) {
                cap *= 2;
                JsonValue *new_items = realloc(items, cap * sizeof(JsonValue));
                if (!new_items) {
                    for (size_t i = 0; i < count; i++) {
                        json_value_free(&items[i]);
                    }
                    free(items);
                    return result;
                }
                items = new_items;
            }

            items[count] = parse_value(p);
            if (items[count].type == JSON_ERROR) {
                for (size_t i = 0; i < count; i++) {
                    json_value_free(&items[i]);
                }
                free(items);
                return result;
            }
            count++;

            skip_whitespace(p);
            if (peek(p) == ',') {
                consume(p);
            } else {
                break;
            }
        }
    }

    if (!expect(p, ']')) {
        for (size_t i = 0; i < count; i++) {
            json_value_free(&items[i]);
        }
        free(items);
        return result;
    }

    result.type = JSON_ARRAY;
    result.data.array_val.items = items;
    result.data.array_val.count = count;
    return result;
}

static JsonValue parse_object(JsonParser *p) {
    JsonValue result = { .type = JSON_ERROR };

    if (!expect(p, '{')) return result;

    size_t cap = 8;
    size_t count = 0;
    char **keys = malloc(cap * sizeof(char *));
    JsonValue *values = malloc(cap * sizeof(JsonValue));
    if (!keys || !values) {
        free(keys);
        free(values);
        return result;
    }

    skip_whitespace(p);
    if (peek(p) != '}') {
        while (1) {
            if (count >= cap) {
                cap *= 2;
                char **new_keys = realloc(keys, cap * sizeof(char *));
                if (!new_keys) {
                    for (size_t i = 0; i < count; i++) {
                        free(keys[i]);
                        json_value_free(&values[i]);
                    }
                    free(keys);
                    free(values);
                    return result;
                }
                keys = new_keys;
                JsonValue *new_values = realloc(values, cap * sizeof(JsonValue));
                if (!new_values) {
                    for (size_t i = 0; i < count; i++) {
                        free(keys[i]);
                        json_value_free(&values[i]);
                    }
                    free(keys);
                    free(values);
                    return result;
                }
                values = new_values;
            }

            JsonValue key_val = parse_string(p);
            if (key_val.type != JSON_STRING) {
                for (size_t i = 0; i < count; i++) {
                    free(keys[i]);
                    json_value_free(&values[i]);
                }
                free(keys);
                free(values);
                return result;
            }
            keys[count] = key_val.data.string_val.str;

            skip_whitespace(p);
            if (!expect(p, ':')) {
                free(keys[count]);
                for (size_t i = 0; i < count; i++) {
                    free(keys[i]);
                    json_value_free(&values[i]);
                }
                free(keys);
                free(values);
                return result;
            }

            values[count] = parse_value(p);
            if (values[count].type == JSON_ERROR) {
                free(keys[count]);
                for (size_t i = 0; i < count; i++) {
                    free(keys[i]);
                    json_value_free(&values[i]);
                }
                free(keys);
                free(values);
                return result;
            }
            count++;

            skip_whitespace(p);
            if (peek(p) == ',') {
                consume(p);
            } else {
                break;
            }
        }
    }

    if (!expect(p, '}')) {
        for (size_t i = 0; i < count; i++) {
            free(keys[i]);
            json_value_free(&values[i]);
        }
        free(keys);
        free(values);
        return result;
    }

    result.type = JSON_OBJECT;
    result.data.object_val.keys = keys;
    result.data.object_val.values = values;
    result.data.object_val.count = count;
    return result;
}

/* Is the byte at 'at' one that could continue a bare literal? Used so that
 * "trueX" is rejected rather than parsed as true followed by garbage. */
static int is_literal_char(JsonParser *p, size_t at) {
    if (at >= p->len) return 0;
    unsigned char c = (unsigned char)p->json[at];
    return isalnum(c) || c == '_';
}

static JsonValue parse_value(JsonParser *p) {
    JsonValue result = { .type = JSON_ERROR };

    skip_whitespace(p);
    if (p->pos >= p->len) return result;

    int c = p->json[p->pos];

    if (c == '"') {
        return parse_string(p);
    } else if (c == '{' || c == '[') {
        if (p->depth >= JSON_MAX_DEPTH) return result;  /* too deeply nested */
        p->depth++;
        result = (c == '{') ? parse_object(p) : parse_array(p);
        p->depth--;
        return result;
    } else if (c == '-' || isdigit(c)) {
        return parse_number(p);
    } else if (strncmp(p->json + p->pos, "true", 4) == 0 &&
               !is_literal_char(p, p->pos + 4)) {
        p->pos += 4;
        result.type = JSON_BOOL;
        result.data.bool_val = 1;
        return result;
    } else if (strncmp(p->json + p->pos, "false", 5) == 0 &&
               !is_literal_char(p, p->pos + 5)) {
        p->pos += 5;
        result.type = JSON_BOOL;
        result.data.bool_val = 0;
        return result;
    } else if (strncmp(p->json + p->pos, "null", 4) == 0 &&
               !is_literal_char(p, p->pos + 4)) {
        p->pos += 4;
        result.type = JSON_NULL;
        return result;
    }

    return result;
}

JsonValue json_parse(const char *json) {
    if (!json) {
        JsonValue result = { .type = JSON_ERROR };
        return result;
    }

    JsonParser p = {
        .json = json,
        .pos = 0,
        .len = strlen(json),
        .depth = 0
    };
    JsonValue v = parse_value(&p);
    if (v.type == JSON_ERROR) return v;

    /* Reject trailing content: '{"cmd":"load"}xyz' used to parse as a valid
     * object with the garbage silently ignored. */
    skip_whitespace(&p);
    if (p.pos != p.len) {
        json_value_free(&v);
        JsonValue err = { .type = JSON_ERROR };
        return err;
    }
    return v;
}

void json_value_free(JsonValue *val) {
    if (!val) return;

    switch (val->type) {
        case JSON_STRING:
            free(val->data.string_val.str);
            break;
        case JSON_ARRAY:
            for (size_t i = 0; i < val->data.array_val.count; i++) {
                json_value_free(&val->data.array_val.items[i]);
            }
            free(val->data.array_val.items);
            break;
        case JSON_OBJECT:
            for (size_t i = 0; i < val->data.object_val.count; i++) {
                free(val->data.object_val.keys[i]);
                json_value_free(&val->data.object_val.values[i]);
            }
            free(val->data.object_val.keys);
            free(val->data.object_val.values);
            break;
        default:
            break;
    }

    val->type = JSON_NULL;
}

const char *json_object_get_string(const JsonValue *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT) return NULL;

    for (size_t i = 0; i < obj->data.object_val.count; i++) {
        if (strcmp(obj->data.object_val.keys[i], key) == 0) {
            JsonValue *v = &obj->data.object_val.values[i];
            if (v->type == JSON_STRING) {
                return v->data.string_val.str;
            }
            return NULL;
        }
    }
    return NULL;
}

int json_object_get_int(const JsonValue *obj, const char *key, int def) {
    if (!obj || obj->type != JSON_OBJECT) return def;

    for (size_t i = 0; i < obj->data.object_val.count; i++) {
        if (strcmp(obj->data.object_val.keys[i], key) == 0) {
            JsonValue *v = &obj->data.object_val.values[i];
            if (v->type == JSON_INT) {
                return v->data.int_val;
            }
            if (v->type == JSON_DOUBLE) {  /* truncate, saturating */
                double d = v->data.double_val;
                if (d != d) return def;
                if (d >= (double)INT_MAX) return INT_MAX;
                if (d <= (double)INT_MIN) return INT_MIN;
                return (int)d;
            }
            return def;
        }
    }
    return def;
}

int json_object_get_bool(const JsonValue *obj, const char *key, int def) {
    if (!obj || obj->type != JSON_OBJECT) return def;

    for (size_t i = 0; i < obj->data.object_val.count; i++) {
        if (strcmp(obj->data.object_val.keys[i], key) == 0) {
            JsonValue *v = &obj->data.object_val.values[i];
            if (v->type == JSON_BOOL) {
                return v->data.bool_val;
            }
            return def;
        }
    }
    return def;
}
