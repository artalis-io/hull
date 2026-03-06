/*
 * sh_json.c - Arena-Allocated JSON Parser
 *
 * Recursive descent parser with full JSON spec compliance.
 * All allocations come from a caller-provided arena.
 */

#include <sh_json.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <inttypes.h>

/* ============================================================================
 * Parser State
 * ============================================================================ */

typedef struct {
    const char *json;       /* Input JSON string */
    size_t len;             /* Input length */
    size_t pos;             /* Current position */
    int depth;              /* Nesting depth */
    SHArena *arena;         /* Allocation arena */
    ShJsonStatus status;    /* Error status */
} ShJsonParser;

/* Forward declarations */
static ShJsonValue *parse_value(ShJsonParser *p);
static ShJsonValue *parse_string(ShJsonParser *p);
static ShJsonValue *parse_number(ShJsonParser *p);
static ShJsonValue *parse_array(ShJsonParser *p);
static ShJsonValue *parse_object(ShJsonParser *p);

/* ============================================================================
 * Parser Helpers
 * ============================================================================ */

static inline bool at_end(const ShJsonParser *p)
{
    return p->pos >= p->len;
}

static inline char peek(const ShJsonParser *p)
{
    return at_end(p) ? '\0' : p->json[p->pos];
}

static inline char advance(ShJsonParser *p)
{
    return at_end(p) ? '\0' : p->json[p->pos++];
}

static inline bool match(ShJsonParser *p, char c)
{
    if (peek(p) == c) {
        p->pos++;
        return true;
    }
    return false;
}

static void skip_whitespace(ShJsonParser *p)
{
    while (!at_end(p)) {
        char c = peek(p);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            p->pos++;
        } else {
            break;
        }
    }
}

static bool match_literal(ShJsonParser *p, const char *literal)
{
    size_t len = strlen(literal);
    if (p->pos + len > p->len) return false;
    if (memcmp(p->json + p->pos, literal, len) != 0) return false;
    p->pos += len;
    return true;
}

static ShJsonValue *alloc_value(ShJsonParser *p)
{
    ShJsonValue *v = sh_arena_calloc(p->arena, 1, sizeof(ShJsonValue));
    if (!v) {
        p->status = SH_JSON_ERR_ARENA_FULL;
    }
    return v;
}

/* ============================================================================
 * String Parsing
 * ============================================================================ */

/* Decode hex digit (returns -1 on error) */
static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

/* Parse 4-digit hex escape (\uXXXX) */
static int parse_hex4(ShJsonParser *p)
{
    if (p->pos + 4 > p->len) return -1;

    int value = 0;
    for (int i = 0; i < 4; i++) {
        int digit = hex_digit(p->json[p->pos++]);
        if (digit < 0) return -1;
        value = (value << 4) | digit;
    }
    return value;
}

/* Encode Unicode code point as UTF-8, returns bytes written */
static size_t encode_utf8(unsigned int cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else if (cp < 0x110000) {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;  /* Invalid code point */
}

static ShJsonValue *parse_string(ShJsonParser *p)
{
    if (!match(p, '"')) {
        p->status = SH_JSON_ERR_SYNTAX;
        return NULL;
    }

    /* First pass: count output length (handles escapes) */
    size_t start = p->pos;
    size_t out_len = 0;
    bool has_escapes = false;

    while (!at_end(p) && peek(p) != '"') {
        char c = advance(p);
        if (c == '\\') {
            has_escapes = true;
            if (at_end(p)) {
                p->status = SH_JSON_ERR_UNTERMINATED_STRING;
                return NULL;
            }
            c = advance(p);
            switch (c) {
                case '"': case '\\': case '/':
                case 'b': case 'f': case 'n': case 'r': case 't':
                    out_len++;
                    break;
                case 'u': {
                    int cp = parse_hex4(p);
                    if (cp < 0) {
                        p->status = SH_JSON_ERR_INVALID_ESCAPE;
                        return NULL;
                    }
                    /* Check for surrogate pair */
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        /* High surrogate - expect low surrogate */
                        if (p->pos + 6 > p->len ||
                            p->json[p->pos] != '\\' ||
                            p->json[p->pos + 1] != 'u') {
                            p->status = SH_JSON_ERR_INVALID_ESCAPE;
                            return NULL;
                        }
                        p->pos += 2;
                        int low = parse_hex4(p);
                        if (low < 0 || low < 0xDC00 || low > 0xDFFF) {
                            p->status = SH_JSON_ERR_INVALID_ESCAPE;
                            return NULL;
                        }
                        /* Combine surrogates into code point */
                        unsigned int full_cp = 0x10000 +
                            ((unsigned int)(cp - 0xD800) << 10) +
                            (unsigned int)(low - 0xDC00);
                        char tmp[4];
                        out_len += encode_utf8(full_cp, tmp);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        /* Unpaired low surrogate */
                        p->status = SH_JSON_ERR_INVALID_ESCAPE;
                        return NULL;
                    } else {
                        char tmp[4];
                        out_len += encode_utf8((unsigned int)cp, tmp);
                    }
                    break;
                }
                default:
                    p->status = SH_JSON_ERR_INVALID_ESCAPE;
                    return NULL;
            }
        } else if ((unsigned char)c < 0x20) {
            /* Control characters not allowed in strings */
            p->status = SH_JSON_ERR_SYNTAX;
            return NULL;
        } else {
            out_len++;
        }
    }

    if (!match(p, '"')) {
        p->status = SH_JSON_ERR_UNTERMINATED_STRING;
        return NULL;
    }

    /* Allocate output buffer */
    char *str = sh_arena_alloc(p->arena, out_len + 1);
    if (!str) {
        p->status = SH_JSON_ERR_ARENA_FULL;
        return NULL;
    }

    /* Second pass: decode escapes */
    if (!has_escapes) {
        /* Fast path: no escapes */
        memcpy(str, p->json + start, out_len);
    } else {
        size_t in_pos = start;
        size_t out_pos = 0;

        while (in_pos < p->pos - 1) {  /* -1 to skip closing quote */
            char c = p->json[in_pos++];
            if (c == '\\') {
                c = p->json[in_pos++];
                switch (c) {
                    case '"':  str[out_pos++] = '"'; break;
                    case '\\': str[out_pos++] = '\\'; break;
                    case '/':  str[out_pos++] = '/'; break;
                    case 'b':  str[out_pos++] = '\b'; break;
                    case 'f':  str[out_pos++] = '\f'; break;
                    case 'n':  str[out_pos++] = '\n'; break;
                    case 'r':  str[out_pos++] = '\r'; break;
                    case 't':  str[out_pos++] = '\t'; break;
                    case 'u': {
                        /* Parse hex value (we already validated in first pass) */
                        int cp = 0;
                        for (int i = 0; i < 4; i++) {
                            cp = (cp << 4) | hex_digit(p->json[in_pos++]);
                        }
                        /* Check for surrogate pair */
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            in_pos += 2;  /* Skip \u */
                            int low = 0;
                            for (int i = 0; i < 4; i++) {
                                low = (low << 4) | hex_digit(p->json[in_pos++]);
                            }
                            unsigned int full_cp = 0x10000 +
                                ((unsigned int)(cp - 0xD800) << 10) +
                                (unsigned int)(low - 0xDC00);
                            out_pos += encode_utf8(full_cp, str + out_pos);
                        } else {
                            out_pos += encode_utf8((unsigned int)cp, str + out_pos);
                        }
                        break;
                    }
                    default:
                        /* Already validated in first pass */
                        break;
                }
            } else {
                str[out_pos++] = c;
            }
        }
    }
    str[out_len] = '\0';

    ShJsonValue *v = alloc_value(p);
    if (!v) return NULL;

    v->type = SH_JSON_STRING;
    v->u.string_val.str = str;
    v->u.string_val.len = out_len;
    return v;
}

/* ============================================================================
 * Number Parsing
 * ============================================================================ */

static ShJsonValue *parse_number(ShJsonParser *p)
{
    size_t start = p->pos;

    /* Optional minus */
    match(p, '-');

    /* Integer part */
    if (peek(p) == '0') {
        advance(p);
        /* Leading zero must not be followed by digit */
        if (isdigit((unsigned char)peek(p))) {
            p->status = SH_JSON_ERR_INVALID_NUMBER;
            return NULL;
        }
    } else if (isdigit((unsigned char)peek(p))) {
        while (isdigit((unsigned char)peek(p))) advance(p);
    } else {
        p->status = SH_JSON_ERR_INVALID_NUMBER;
        return NULL;
    }

    /* Fractional part */
    if (peek(p) == '.') {
        advance(p);
        if (!isdigit((unsigned char)peek(p))) {
            p->status = SH_JSON_ERR_INVALID_NUMBER;
            return NULL;
        }
        while (isdigit((unsigned char)peek(p))) advance(p);
    }

    /* Exponent part */
    if (peek(p) == 'e' || peek(p) == 'E') {
        advance(p);
        if (peek(p) == '+' || peek(p) == '-') advance(p);
        if (!isdigit((unsigned char)peek(p))) {
            p->status = SH_JSON_ERR_INVALID_NUMBER;
            return NULL;
        }
        while (isdigit((unsigned char)peek(p))) advance(p);
    }

    /* Parse the number using strtod */
    size_t num_len = p->pos - start;
    char *buf = sh_arena_alloc(p->arena, num_len + 1);
    if (!buf) {
        p->status = SH_JSON_ERR_ARENA_FULL;
        return NULL;
    }
    memcpy(buf, p->json + start, num_len);
    buf[num_len] = '\0';

    char *end;
    double val = strtod(buf, &end);

    if (end != buf + num_len) {
        p->status = SH_JSON_ERR_INVALID_NUMBER;
        return NULL;
    }

    ShJsonValue *v = alloc_value(p);
    if (!v) return NULL;

    v->type = SH_JSON_NUMBER;
    v->u.num_val = val;
    return v;
}

/* ============================================================================
 * Array Parsing
 * ============================================================================ */

static ShJsonValue *parse_array(ShJsonParser *p)
{
    if (!match(p, '[')) {
        p->status = SH_JSON_ERR_SYNTAX;
        return NULL;
    }

    if (++p->depth > SH_JSON_MAX_DEPTH) {
        p->status = SH_JSON_ERR_DEPTH_EXCEEDED;
        return NULL;
    }

    /* First pass: count elements */
    size_t save_pos = p->pos;
    size_t count = 0;

    skip_whitespace(p);
    if (peek(p) != ']') {
        /* Non-empty array */
        int depth = 0;
        bool in_string = false;
        bool escaped = false;

        while (!at_end(p)) {
            char c = peek(p);

            if (in_string) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    in_string = false;
                }
            } else {
                if (c == '"') {
                    in_string = true;
                } else if (c == '[' || c == '{') {
                    depth++;
                } else if (c == ']' || c == '}') {
                    if (depth == 0 && c == ']') break;
                    depth--;
                } else if (c == ',' && depth == 0) {
                    count++;
                }
            }
            advance(p);
        }
        count++;  /* One more element than commas */
    }

    /* Allocate array */
    ShJsonValue **items = NULL;
    if (count > 0) {
        items = sh_arena_calloc(p->arena, count, sizeof(ShJsonValue *));
        if (!items) {
            p->status = SH_JSON_ERR_ARENA_FULL;
            return NULL;
        }
    }

    /* Second pass: parse elements */
    p->pos = save_pos;
    skip_whitespace(p);

    size_t i = 0;
    if (peek(p) != ']') {
        while (p->status == SH_JSON_OK) {
            skip_whitespace(p);
            items[i] = parse_value(p);
            if (!items[i]) break;
            i++;

            skip_whitespace(p);
            if (match(p, ',')) {
                continue;
            } else {
                break;
            }
        }
    }

    if (!match(p, ']')) {
        if (p->status == SH_JSON_OK) {
            p->status = SH_JSON_ERR_SYNTAX;
        }
        p->depth--;
        return NULL;
    }

    p->depth--;

    ShJsonValue *v = alloc_value(p);
    if (!v) return NULL;

    v->type = SH_JSON_ARRAY;
    v->u.array_val.items = items;
    v->u.array_val.count = i;
    return v;
}

/* ============================================================================
 * Object Parsing
 * ============================================================================ */

static ShJsonValue *parse_object(ShJsonParser *p)
{
    if (!match(p, '{')) {
        p->status = SH_JSON_ERR_SYNTAX;
        return NULL;
    }

    if (++p->depth > SH_JSON_MAX_DEPTH) {
        p->status = SH_JSON_ERR_DEPTH_EXCEEDED;
        return NULL;
    }

    /* First pass: count members */
    size_t save_pos = p->pos;
    size_t count = 0;

    skip_whitespace(p);
    if (peek(p) != '}') {
        int depth = 0;
        bool in_string = false;
        bool escaped = false;

        while (!at_end(p)) {
            char c = peek(p);

            if (in_string) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    in_string = false;
                }
            } else {
                if (c == '"') {
                    in_string = true;
                } else if (c == '[' || c == '{') {
                    depth++;
                } else if (c == ']' || c == '}') {
                    if (depth == 0 && c == '}') break;
                    depth--;
                } else if (c == ',' && depth == 0) {
                    count++;
                }
            }
            advance(p);
        }
        count++;  /* One more member than commas */
    }

    /* Allocate members array */
    ShJsonMember *members = NULL;
    if (count > 0) {
        members = sh_arena_calloc(p->arena, count, sizeof(ShJsonMember));
        if (!members) {
            p->status = SH_JSON_ERR_ARENA_FULL;
            return NULL;
        }
    }

    /* Second pass: parse members */
    p->pos = save_pos;
    skip_whitespace(p);

    size_t i = 0;
    if (peek(p) != '}') {
        while (p->status == SH_JSON_OK) {
            skip_whitespace(p);

            /* Parse key (must be string) */
            if (peek(p) != '"') {
                p->status = SH_JSON_ERR_SYNTAX;
                break;
            }
            ShJsonValue *key_val = parse_string(p);
            if (!key_val) break;

            members[i].key = key_val->u.string_val.str;
            members[i].key_len = key_val->u.string_val.len;

            /* Parse colon */
            skip_whitespace(p);
            if (!match(p, ':')) {
                p->status = SH_JSON_ERR_SYNTAX;
                break;
            }

            /* Parse value */
            skip_whitespace(p);
            members[i].value = parse_value(p);
            if (!members[i].value) break;
            i++;

            skip_whitespace(p);
            if (match(p, ',')) {
                continue;
            } else {
                break;
            }
        }
    }

    if (!match(p, '}')) {
        if (p->status == SH_JSON_OK) {
            p->status = SH_JSON_ERR_SYNTAX;
        }
        p->depth--;
        return NULL;
    }

    p->depth--;

    ShJsonValue *v = alloc_value(p);
    if (!v) return NULL;

    v->type = SH_JSON_OBJECT;
    v->u.object_val.members = members;
    v->u.object_val.count = i;
    return v;
}

/* ============================================================================
 * Value Parsing (Dispatcher)
 * ============================================================================ */

static ShJsonValue *parse_value(ShJsonParser *p)
{
    skip_whitespace(p);

    if (at_end(p)) {
        p->status = SH_JSON_ERR_SYNTAX;
        return NULL;
    }

    char c = peek(p);

    switch (c) {
        case 'n':
            if (match_literal(p, "null")) {
                ShJsonValue *v = alloc_value(p);
                if (v) v->type = SH_JSON_NULL;
                return v;
            }
            p->status = SH_JSON_ERR_SYNTAX;
            return NULL;

        case 't':
            if (match_literal(p, "true")) {
                ShJsonValue *v = alloc_value(p);
                if (v) {
                    v->type = SH_JSON_BOOL;
                    v->u.bool_val = true;
                }
                return v;
            }
            p->status = SH_JSON_ERR_SYNTAX;
            return NULL;

        case 'f':
            if (match_literal(p, "false")) {
                ShJsonValue *v = alloc_value(p);
                if (v) {
                    v->type = SH_JSON_BOOL;
                    v->u.bool_val = false;
                }
                return v;
            }
            p->status = SH_JSON_ERR_SYNTAX;
            return NULL;

        case '"':
            return parse_string(p);

        case '[':
            return parse_array(p);

        case '{':
            return parse_object(p);

        case '-':
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            return parse_number(p);

        default:
            p->status = SH_JSON_ERR_SYNTAX;
            return NULL;
    }
}

/* ============================================================================
 * Public API: Parsing
 * ============================================================================ */

ShJsonStatus sh_json_parse(const char *json, size_t len,
                           SHArena *arena, ShJsonValue **out)
{
    if (!out) return SH_JSON_ERR_NULL;
    *out = NULL;

    if (!json) return SH_JSON_ERR_NULL;
    if (!arena) return SH_JSON_ERR_NULL;
    if (len == 0) return SH_JSON_ERR_EMPTY;

    ShJsonParser p = {
        .json = json,
        .len = len,
        .pos = 0,
        .depth = 0,
        .arena = arena,
        .status = SH_JSON_OK
    };

    skip_whitespace(&p);
    if (at_end(&p)) {
        return SH_JSON_ERR_EMPTY;
    }

    *out = parse_value(&p);

    if (p.status != SH_JSON_OK) {
        return p.status;
    }

    /* Check for trailing content */
    skip_whitespace(&p);
    if (!at_end(&p)) {
        return SH_JSON_ERR_SYNTAX;
    }

    return SH_JSON_OK;
}

const char *sh_json_status_str(ShJsonStatus status)
{
    switch (status) {
        case SH_JSON_OK:                    return "OK";
        case SH_JSON_ERR_NULL:              return "NULL input";
        case SH_JSON_ERR_EMPTY:             return "Empty input";
        case SH_JSON_ERR_SYNTAX:            return "Syntax error";
        case SH_JSON_ERR_UNTERMINATED_STRING: return "Unterminated string";
        case SH_JSON_ERR_INVALID_ESCAPE:    return "Invalid escape sequence";
        case SH_JSON_ERR_INVALID_NUMBER:    return "Invalid number";
        case SH_JSON_ERR_DEPTH_EXCEEDED:    return "Maximum depth exceeded";
        case SH_JSON_ERR_ARENA_FULL:        return "Arena out of memory";
        default:                            return "Unknown error";
    }
}

/* ============================================================================
 * Public API: Type Checking
 * ============================================================================ */

ShJsonType sh_json_type(const ShJsonValue *v)
{
    return v ? v->type : SH_JSON_NULL;
}

bool sh_json_is_null(const ShJsonValue *v)
{
    return v == NULL || v->type == SH_JSON_NULL;
}

/* ============================================================================
 * Public API: Value Access
 * ============================================================================ */

bool sh_json_as_bool(const ShJsonValue *v, bool def)
{
    if (!v || v->type != SH_JSON_BOOL) return def;
    return v->u.bool_val;
}

double sh_json_as_double(const ShJsonValue *v, double def)
{
    if (!v || v->type != SH_JSON_NUMBER) return def;
    return v->u.num_val;
}

int sh_json_as_int(const ShJsonValue *v, int def)
{
    if (!v || v->type != SH_JSON_NUMBER) return def;
    double d = v->u.num_val;
    if (d >= 2147483647.0) return 2147483647;
    if (d <= -2147483648.0) return -2147483648;
    return (int)d;
}

const char *sh_json_as_string(const ShJsonValue *v, const char *def)
{
    if (!v || v->type != SH_JSON_STRING) return def;
    return v->u.string_val.str;
}

size_t sh_json_string_len(const ShJsonValue *v)
{
    if (!v || v->type != SH_JSON_STRING) return 0;
    return v->u.string_val.len;
}

/* ============================================================================
 * Public API: Array Access
 * ============================================================================ */

size_t sh_json_array_len(const ShJsonValue *v)
{
    if (!v || v->type != SH_JSON_ARRAY) return 0;
    return v->u.array_val.count;
}

ShJsonValue *sh_json_array_get(const ShJsonValue *v, size_t index)
{
    if (!v || v->type != SH_JSON_ARRAY) return NULL;
    if (index >= v->u.array_val.count) return NULL;
    return v->u.array_val.items[index];
}

/* ============================================================================
 * Public API: Object Access
 * ============================================================================ */

size_t sh_json_object_len(const ShJsonValue *v)
{
    if (!v || v->type != SH_JSON_OBJECT) return 0;
    return v->u.object_val.count;
}

ShJsonValue *sh_json_get_n(const ShJsonValue *v, const char *key, size_t key_len)
{
    if (!v || v->type != SH_JSON_OBJECT || !key) return NULL;

    for (size_t i = 0; i < v->u.object_val.count; i++) {
        const ShJsonMember *m = &v->u.object_val.members[i];
        if (m->key_len == key_len && memcmp(m->key, key, key_len) == 0) {
            return m->value;
        }
    }
    return NULL;
}

ShJsonValue *sh_json_get(const ShJsonValue *v, const char *key)
{
    if (!key) return NULL;
    return sh_json_get_n(v, key, strlen(key));
}

/* ============================================================================
 * Public API: Path Access
 * ============================================================================ */

ShJsonValue *sh_json_get_path(const ShJsonValue *v, const char *path)
{
    if (!v || !path) return NULL;

    const ShJsonValue *current = v;
    const char *p = path;

    while (*p && current) {
        /* Skip leading dots */
        if (*p == '.') {
            p++;
            continue;
        }

        /* Check for array index */
        if (*p == '[') {
            p++;  /* Skip '[' */

            /* Parse index */
            if (!isdigit((unsigned char)*p)) return NULL;

            size_t index = 0;
            while (isdigit((unsigned char)*p)) {
                index = index * 10 + (size_t)(*p - '0');
                p++;
            }

            if (*p != ']') return NULL;
            p++;  /* Skip ']' */

            current = sh_json_array_get(current, index);
        } else {
            /* Parse key until . or [ or end */
            const char *key_start = p;
            while (*p && *p != '.' && *p != '[') {
                p++;
            }
            size_t key_len = (size_t)(p - key_start);
            if (key_len == 0) return NULL;

            current = sh_json_get_n(current, key_start, key_len);
        }
    }

    return (ShJsonValue *)current;
}

/* ============================================================================
 * JSON Writer Implementation
 * ============================================================================ */

void sh_json_writer_init(ShJsonWriter *w, ShJsonWriteFn write_fn, void *ctx) {
    if (!w) return;
    w->write_fn = write_fn;
    w->ctx = ctx;
    w->depth = 0;
    w->needs_comma = 0;
    w->error = 0;
    w->first_in_container = 0;
}

int sh_json_writer_error(const ShJsonWriter *w) {
    return w ? w->error : 1;
}

/* Internal: write bytes through callback */
static int jw_write(ShJsonWriter *w, const char *data, size_t len) {
    if (!w || w->error) return -1;
    if (!data || len == 0) return 0;
    if (w->write_fn(w->ctx, data, len) != 0) {
        w->error = 1;
        return -1;
    }
    return 0;
}

/* Internal: write null-terminated string */
static int jw_writes(ShJsonWriter *w, const char *str) {
    return jw_write(w, str, strlen(str));
}

/* Internal: write comma if needed before next element */
static int jw_comma(ShJsonWriter *w) {
    if (w->needs_comma) {
        return jw_writes(w, ",");
    }
    return 0;
}

/* Internal: mark that next element needs comma */
static void jw_mark_comma(ShJsonWriter *w) {
    w->needs_comma = 1;
}

int sh_json_write_null(ShJsonWriter *w) {
    if (!w || w->error) return -1;
    if (jw_comma(w) != 0) return -1;
    jw_mark_comma(w);
    return jw_writes(w, "null");
}

int sh_json_write_bool(ShJsonWriter *w, bool val) {
    if (!w || w->error) return -1;
    if (jw_comma(w) != 0) return -1;
    jw_mark_comma(w);
    return jw_writes(w, val ? "true" : "false");
}

int sh_json_write_int(ShJsonWriter *w, int64_t val) {
    if (!w || w->error) return -1;
    if (jw_comma(w) != 0) return -1;
    jw_mark_comma(w);

    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%" PRId64, val);
    if (len < 0 || (size_t)len >= sizeof(buf)) {
        w->error = 1;
        return -1;
    }
    return jw_write(w, buf, (size_t)len);
}

int sh_json_write_double(ShJsonWriter *w, double val) {
    return sh_json_write_double_fmt(w, val, 6);
}

int sh_json_write_double_fmt(ShJsonWriter *w, double val, int precision) {
    if (!w || w->error) return -1;
    if (jw_comma(w) != 0) return -1;
    jw_mark_comma(w);

    char buf[64];
    int len;

    /* Handle special cases */
    if (val != val) { /* NaN */
        return jw_writes(w, "null");
    }
    if (val == (1.0 / 0.0) || val == (-1.0 / 0.0)) { /* Inf */
        return jw_writes(w, "null");
    }

    /* Format with specified precision */
    if (precision < 0) precision = 6;
    if (precision > 17) precision = 17;
    len = snprintf(buf, sizeof(buf), "%.*g", precision, val);
    if (len < 0 || (size_t)len >= sizeof(buf)) {
        w->error = 1;
        return -1;
    }
    return jw_write(w, buf, (size_t)len);
}

/* Internal: write escaped JSON string content (without quotes) */
static int jw_escape_string(ShJsonWriter *w, const char *str, size_t len) {
    const char *p = str;
    const char *end = str + len;
    const char *chunk_start = p;

    while (p < end) {
        unsigned char c = (unsigned char)*p;
        const char *escape = NULL;
        char hex_buf[8];

        if (c == '"') escape = "\\\"";
        else if (c == '\\') escape = "\\\\";
        else if (c == '\b') escape = "\\b";
        else if (c == '\f') escape = "\\f";
        else if (c == '\n') escape = "\\n";
        else if (c == '\r') escape = "\\r";
        else if (c == '\t') escape = "\\t";
        else if (c < 0x20) {
            /* Control character - use \uXXXX */
            snprintf(hex_buf, sizeof(hex_buf), "\\u%04x", c);
            escape = hex_buf;
        }

        if (escape) {
            /* Write pending chunk */
            if (chunk_start < p) {
                if (jw_write(w, chunk_start, (size_t)(p - chunk_start)) != 0) return -1;
            }
            if (jw_writes(w, escape) != 0) return -1;
            p++;
            chunk_start = p;
        } else {
            p++;
        }
    }

    /* Write remaining chunk */
    if (chunk_start < p) {
        if (jw_write(w, chunk_start, (size_t)(p - chunk_start)) != 0) return -1;
    }

    return 0;
}

int sh_json_write_string(ShJsonWriter *w, const char *str) {
    if (!str) {
        return sh_json_write_null(w);
    }
    return sh_json_write_string_n(w, str, strlen(str));
}

int sh_json_write_string_n(ShJsonWriter *w, const char *str, size_t len) {
    if (!w || w->error) return -1;
    if (!str) {
        return sh_json_write_null(w);
    }
    if (jw_comma(w) != 0) return -1;
    jw_mark_comma(w);

    if (jw_writes(w, "\"") != 0) return -1;
    if (jw_escape_string(w, str, len) != 0) return -1;
    return jw_writes(w, "\"");
}

int sh_json_write_raw(ShJsonWriter *w, const char *raw, size_t len) {
    if (!w || w->error) return -1;
    if (!raw || len == 0) return 0;
    if (jw_comma(w) != 0) return -1;
    jw_mark_comma(w);
    return jw_write(w, raw, len);
}

int sh_json_write_object_start(ShJsonWriter *w) {
    if (!w || w->error) return -1;
    if (jw_comma(w) != 0) return -1;
    w->depth++;
    w->needs_comma = 0;  /* First element doesn't need comma */
    return jw_writes(w, "{");
}

int sh_json_write_object_end(ShJsonWriter *w) {
    if (!w || w->error) return -1;
    if (w->depth > 0) w->depth--;
    w->needs_comma = 1;  /* After closing, next sibling needs comma */
    return jw_writes(w, "}");
}

int sh_json_write_key(ShJsonWriter *w, const char *key) {
    if (!w || w->error) return -1;
    if (!key) return -1;
    if (jw_comma(w) != 0) return -1;

    if (jw_writes(w, "\"") != 0) return -1;
    if (jw_escape_string(w, key, strlen(key)) != 0) return -1;
    if (jw_writes(w, "\":") != 0) return -1;

    w->needs_comma = 0;  /* Value follows immediately, no comma */
    return 0;
}

int sh_json_write_kv_null(ShJsonWriter *w, const char *key) {
    if (sh_json_write_key(w, key) != 0) return -1;
    return sh_json_write_null(w);
}

int sh_json_write_kv_bool(ShJsonWriter *w, const char *key, bool val) {
    if (sh_json_write_key(w, key) != 0) return -1;
    return sh_json_write_bool(w, val);
}

int sh_json_write_kv_int(ShJsonWriter *w, const char *key, int64_t val) {
    if (sh_json_write_key(w, key) != 0) return -1;
    return sh_json_write_int(w, val);
}

int sh_json_write_kv_double(ShJsonWriter *w, const char *key, double val) {
    if (sh_json_write_key(w, key) != 0) return -1;
    return sh_json_write_double(w, val);
}

int sh_json_write_kv_double_fmt(ShJsonWriter *w, const char *key, double val, int precision) {
    if (sh_json_write_key(w, key) != 0) return -1;
    return sh_json_write_double_fmt(w, val, precision);
}

int sh_json_write_kv_string(ShJsonWriter *w, const char *key, const char *val) {
    if (sh_json_write_key(w, key) != 0) return -1;
    return sh_json_write_string(w, val);
}

int sh_json_write_array_start(ShJsonWriter *w) {
    if (!w || w->error) return -1;
    if (jw_comma(w) != 0) return -1;
    w->depth++;
    w->needs_comma = 0;  /* First element doesn't need comma */
    return jw_writes(w, "[");
}

int sh_json_write_array_end(ShJsonWriter *w) {
    if (!w || w->error) return -1;
    if (w->depth > 0) w->depth--;
    w->needs_comma = 1;  /* After closing, next sibling needs comma */
    return jw_writes(w, "]");
}

/* ============================================================================
 * JSON Buffer Helper Implementation
 * ============================================================================ */

void sh_json_buf_init(ShJsonBuf *jb) {
    if (!jb) return;
    jb->buf = NULL;
    jb->len = 0;
    jb->cap = 0;
}

void sh_json_buf_free(ShJsonBuf *jb) {
    if (!jb) return;
    free(jb->buf);
    jb->buf = NULL;
    jb->len = 0;
    jb->cap = 0;
}

void sh_json_buf_reset(ShJsonBuf *jb) {
    if (!jb) return;
    jb->len = 0;
    if (jb->buf) jb->buf[0] = '\0';
}

int sh_json_buf_write(void *ctx, const char *data, size_t len) {
    ShJsonBuf *jb = (ShJsonBuf *)ctx;
    if (!jb || !data) return -1;
    if (len == 0) return 0;

    /* Grow buffer if needed */
    while (jb->len + len + 1 > jb->cap) {
        size_t new_cap = jb->cap * 2;
        if (new_cap < 1024) new_cap = 1024;
        if (new_cap < jb->len + len + 1) new_cap = jb->len + len + 1;
        char *new_buf = realloc(jb->buf, new_cap);
        if (!new_buf) return -1;
        jb->buf = new_buf;
        jb->cap = new_cap;
    }

    memcpy(jb->buf + jb->len, data, len);
    jb->len += len;
    jb->buf[jb->len] = '\0';
    return 0;
}

char *sh_json_buf_take(ShJsonBuf *jb) {
    if (!jb || !jb->buf) return NULL;
    char *result = jb->buf;
    jb->buf = NULL;
    jb->len = 0;
    jb->cap = 0;
    return result;
}
