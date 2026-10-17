/* json.c - a small, strict JSON reader for repository indexes.
 *
 * Builds a tree (objects keep member order); strings are decoded, including
 * \uXXXX escapes and surrogate pairs, to UTF-8.  Nesting is bounded so a
 * hostile index cannot exhaust the stack.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xpkg.h"

#define JSON_MAX_DEPTH 64

typedef struct {
    const char *p;
    char *err;
    size_t errsz;
    int depth;
} jp_t;

static void jerr(jp_t *s, const char *msg) {
    if (s->err && !s->err[0]) snprintf(s->err, s->errsz, "%s", msg);
}

static void skip_ws(jp_t *s) {
    while (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r') s->p++;
}

static json_t *jnew(json_type_t t) {
    json_t *j = calloc(1, sizeof(*j));
    if (j) j->type = t;
    return j;
}

static int hex4(const char *p, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

static size_t put_utf8(char *o, unsigned cp) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F)); return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static char *parse_string(jp_t *s) {
    if (*s->p != '"') { jerr(s, "expected string"); return NULL; }
    s->p++;
    const char *start = s->p;
    size_t cap = 0;
    while (start[cap] && start[cap] != '"') { if (start[cap] == '\\' && start[cap + 1]) cap++; cap++; }
    char *out = malloc(cap * 2 + 1);   /* escapes never grow past 2x */
    if (!out) { jerr(s, "out of memory"); return NULL; }
    size_t o = 0;
    for (;;) {
        unsigned char c = (unsigned char)*s->p;
        if (c == '\0') { free(out); jerr(s, "unterminated string"); return NULL; }
        if (c == '"') { s->p++; break; }
        if (c < 0x20) { free(out); jerr(s, "control character in string"); return NULL; }
        if (c != '\\') { out[o++] = (char)c; s->p++; continue; }
        s->p++;
        char e = *s->p++;
        switch (e) {
        case '"': out[o++] = '"'; break;
        case '\\': out[o++] = '\\'; break;
        case '/': out[o++] = '/'; break;
        case 'b': out[o++] = '\b'; break;
        case 'f': out[o++] = '\f'; break;
        case 'n': out[o++] = '\n'; break;
        case 'r': out[o++] = '\r'; break;
        case 't': out[o++] = '\t'; break;
        case 'u': {
            unsigned cp;
            if (hex4(s->p, &cp) != 0) { free(out); jerr(s, "bad \\u escape"); return NULL; }
            s->p += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && s->p[0] == '\\' && s->p[1] == 'u') {
                unsigned lo;
                if (hex4(s->p + 2, &lo) == 0 && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    s->p += 6;
                }
            }
            if (cp == 0) cp = '?';   /* no embedded NULs */
            o += put_utf8(out + o, cp);
            break;
        }
        default: free(out); jerr(s, "bad escape"); return NULL;
        }
    }
    out[o] = '\0';
    return out;
}

static json_t *parse_value(jp_t *s);

static json_t *parse_container(jp_t *s, int obj) {
    if (++s->depth > JSON_MAX_DEPTH) { jerr(s, "nesting too deep"); return NULL; }
    json_t *j = jnew(obj ? JSON_OBJ : JSON_ARR);
    if (!j) return NULL;
    s->p++;
    skip_ws(s);
    char close = obj ? '}' : ']';
    json_t **tail = &j->child;
    if (*s->p == close) { s->p++; s->depth--; return j; }
    for (;;) {
        char *key = NULL;
        skip_ws(s);
        if (obj) {
            key = parse_string(s);
            if (!key) { json_free(j); return NULL; }
            skip_ws(s);
            if (*s->p != ':') { free(key); json_free(j); jerr(s, "expected ':'"); return NULL; }
            s->p++;
        }
        json_t *v = parse_value(s);
        if (!v) { free(key); json_free(j); return NULL; }
        v->key = key;
        *tail = v;
        tail = &v->next;
        skip_ws(s);
        if (*s->p == ',') { s->p++; continue; }
        if (*s->p == close) { s->p++; break; }
        json_free(j);
        jerr(s, obj ? "expected ',' or '}'" : "expected ',' or ']'");
        return NULL;
    }
    s->depth--;
    return j;
}

static json_t *parse_value(jp_t *s) {
    skip_ws(s);
    char c = *s->p;
    if (c == '{' || c == '[') return parse_container(s, c == '{');
    if (c == '"') {
        char *str = parse_string(s);
        if (!str) return NULL;
        json_t *j = jnew(JSON_STR);
        if (!j) { free(str); return NULL; }
        j->str = str;
        return j;
    }
    if (!strncmp(s->p, "true", 4)) { s->p += 4; json_t *j = jnew(JSON_BOOL); if (j) j->boolean = 1; return j; }
    if (!strncmp(s->p, "false", 5)) { s->p += 5; return jnew(JSON_BOOL); }
    if (!strncmp(s->p, "null", 4)) { s->p += 4; return jnew(JSON_NULL); }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char *end;
        double d = strtod(s->p, &end);
        if (end == s->p) { jerr(s, "bad number"); return NULL; }
        s->p = end;
        json_t *j = jnew(JSON_NUM);
        if (j) j->num = d;
        return j;
    }
    jerr(s, "unexpected character");
    return NULL;
}

json_t *json_parse(const char *text, char *err, size_t errsz) {
    jp_t s = { text, err, errsz, 0 };
    if (err && errsz) err[0] = '\0';
    json_t *j = parse_value(&s);
    if (!j) return NULL;
    skip_ws(&s);
    if (*s.p) { json_free(j); jerr(&s, "trailing data"); return NULL; }
    return j;
}

void json_free(json_t *j) {
    while (j) {
        json_t *next = j->next;
        json_free(j->child);
        free(j->str);
        free(j->key);
        free(j);
        j = next;
    }
}

json_t *json_get(const json_t *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJ) return NULL;
    for (json_t *c = obj->child; c; c = c->next)
        if (c->key && !strcmp(c->key, key)) return c;
    return NULL;
}

const char *json_str(const json_t *obj, const char *key, const char *def) {
    json_t *v = json_get(obj, key);
    return (v && v->type == JSON_STR) ? v->str : def;
}

double json_num(const json_t *obj, const char *key, double def) {
    json_t *v = json_get(obj, key);
    return (v && v->type == JSON_NUM) ? v->num : def;
}

void json_write_str(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') fprintf(f, "\\%c", *p);
        else if (*p == '\n') fputs("\\n", f);
        else if (*p == '\t') fputs("\\t", f);
        else if (*p < 0x20) fprintf(f, "\\u%04x", *p);
        else fputc(*p, f);
    }
    fputc('"', f);
}
