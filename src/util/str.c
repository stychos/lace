/*
 * lace - Database Viewer and Manager
 * String utilities implementation
 */

#include "str.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>

#define SB_INITIAL_CAP 64
#define SB_GROWTH_FACTOR 2

char *str_dup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *dup = malloc(len + 1);
    if (dup) {
        memcpy(dup, s, len + 1);
    }
    return dup;
}

char *str_ndup(const char *s, size_t n) {
    if (!s) return NULL;
    /* Find actual length within first n bytes */
    size_t len = 0;
    while (len < n && s[len] != '\0') {
        len++;
    }
    char *dup = malloc(len + 1);
    if (dup) {
        memcpy(dup, s, len);
        dup[len] = '\0';
    }
    return dup;
}

char *str_printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char *result = str_vprintf(fmt, args);
    va_end(args);
    return result;
}

char *str_vprintf(const char *fmt, va_list args) {
    va_list args_copy;
    va_copy(args_copy, args);

    int len = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);

    if (len < 0) return NULL;

    char *buf = malloc(len + 1);
    if (!buf) return NULL;

    vsnprintf(buf, len + 1, fmt, args);
    return buf;
}

bool str_eq(const char *a, const char *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return strcmp(a, b) == 0;
}

bool str_eq_nocase(const char *a, const char *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return strcasecmp(a, b) == 0;
}

bool str_starts_with(const char *s, const char *prefix) {
    if (!s || !prefix) return false;
    size_t slen = strlen(s);
    size_t plen = strlen(prefix);
    if (plen > slen) return false;
    return strncmp(s, prefix, plen) == 0;
}

bool str_ends_with(const char *s, const char *suffix) {
    if (!s || !suffix) return false;
    size_t slen = strlen(s);
    size_t suflen = strlen(suffix);
    if (suflen > slen) return false;
    return strcmp(s + slen - suflen, suffix) == 0;
}

bool str_contains(const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;
    return strstr(haystack, needle) != NULL;
}

char *str_trim(char *s) {
    if (!s) return NULL;

    /* Trim leading whitespace */
    while (isspace((unsigned char)*s)) s++;

    if (*s == '\0') return s;

    /* Trim trailing whitespace */
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) end--;
    end[1] = '\0';

    return s;
}

char *str_trim_dup(const char *s) {
    if (!s) return NULL;

    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0') return str_dup("");

    const char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) end--;

    return str_ndup(s, end - s + 1);
}

char *str_lower(char *s) {
    if (!s) return NULL;
    for (char *p = s; *p; p++) {
        *p = tolower((unsigned char)*p);
    }
    return s;
}

char *str_upper(char *s) {
    if (!s) return NULL;
    for (char *p = s; *p; p++) {
        *p = toupper((unsigned char)*p);
    }
    return s;
}

char *str_replace(const char *s, const char *old, const char *new_str) {
    if (!s || !old || !new_str) return NULL;

    size_t old_len = strlen(old);
    size_t new_len = strlen(new_str);
    if (old_len == 0) return str_dup(s);

    /* Count occurrences */
    size_t count = 0;
    const char *p = s;
    while ((p = strstr(p, old)) != NULL) {
        count++;
        p += old_len;
    }

    if (count == 0) return str_dup(s);

    /* Allocate result */
    size_t result_len = strlen(s) + count * (new_len - old_len);
    char *result = malloc(result_len + 1);
    if (!result) return NULL;

    /* Build result */
    char *dst = result;
    p = s;
    const char *prev = s;
    while ((p = strstr(p, old)) != NULL) {
        size_t prefix_len = p - prev;
        memcpy(dst, prev, prefix_len);
        dst += prefix_len;
        memcpy(dst, new_str, new_len);
        dst += new_len;
        p += old_len;
        prev = p;
    }
    strcpy(dst, prev);

    return result;
}

char **str_split(const char *s, char delim, size_t *count) {
    if (!s || !count) return NULL;

    /* Count parts */
    size_t n = 1;
    for (const char *p = s; *p; p++) {
        if (*p == delim) n++;
    }

    char **parts = malloc((n + 1) * sizeof(char *));
    if (!parts) return NULL;

    /* Split */
    size_t i = 0;
    const char *start = s;
    for (const char *p = s; ; p++) {
        if (*p == delim || *p == '\0') {
            parts[i] = str_ndup(start, p - start);
            if (!parts[i]) {
                str_split_free(parts, i);
                return NULL;
            }
            i++;
            if (*p == '\0') break;
            start = p + 1;
        }
    }

    parts[i] = NULL;
    *count = n;
    return parts;
}

void str_split_free(char **parts, size_t count) {
    if (!parts) return;
    for (size_t i = 0; i < count; i++) {
        free(parts[i]);
    }
    free(parts);
}

char *str_join(const char **parts, size_t count, const char *sep) {
    if (!parts || count == 0) return str_dup("");

    size_t sep_len = sep ? strlen(sep) : 0;
    size_t total_len = 0;

    for (size_t i = 0; i < count; i++) {
        if (parts[i]) total_len += strlen(parts[i]);
        if (i < count - 1) total_len += sep_len;
    }

    char *result = malloc(total_len + 1);
    if (!result) return NULL;

    char *p = result;
    for (size_t i = 0; i < count; i++) {
        if (parts[i]) {
            size_t len = strlen(parts[i]);
            memcpy(p, parts[i], len);
            p += len;
        }
        if (i < count - 1 && sep) {
            memcpy(p, sep, sep_len);
            p += sep_len;
        }
    }
    *p = '\0';

    return result;
}

char *str_url_encode(const char *s) {
    if (!s) return NULL;

    StringBuilder *sb = sb_new(strlen(s) * 3);
    if (!sb) return NULL;

    for (const char *p = s; *p; p++) {
        unsigned char c = *p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            sb_append_char(sb, c);
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", c);
            sb_append(sb, hex);
        }
    }

    return sb_to_string(sb);
}

char *str_url_decode(const char *s) {
    if (!s) return NULL;

    size_t len = strlen(s);
    char *result = malloc(len + 1);
    if (!result) return NULL;

    char *dst = result;
    for (const char *p = s; *p; p++) {
        if (*p == '%' && p[1] && p[2]) {
            char hex[3] = { p[1], p[2], '\0' };
            char *endptr;
            long val = strtol(hex, &endptr, 16);
            if (*endptr == '\0') {
                *dst++ = (char)val;
                p += 2;
                continue;
            }
        } else if (*p == '+') {
            *dst++ = ' ';
            continue;
        }
        *dst++ = *p;
    }
    *dst = '\0';

    return result;
}

char *str_escape(const char *s) {
    if (!s) return NULL;

    StringBuilder *sb = sb_new(strlen(s) * 2);
    if (!sb) return NULL;

    for (const char *p = s; *p; p++) {
        switch (*p) {
            case '\n': sb_append(sb, "\\n"); break;
            case '\r': sb_append(sb, "\\r"); break;
            case '\t': sb_append(sb, "\\t"); break;
            case '\\': sb_append(sb, "\\\\"); break;
            case '"':  sb_append(sb, "\\\""); break;
            default:
                if ((unsigned char)*p < 32) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\x%02x", (unsigned char)*p);
                    sb_append(sb, buf);
                } else {
                    sb_append_char(sb, *p);
                }
        }
    }

    return sb_to_string(sb);
}

char *str_unescape(const char *s) {
    if (!s) return NULL;

    size_t len = strlen(s);
    char *result = malloc(len + 1);
    if (!result) return NULL;

    char *dst = result;
    for (const char *p = s; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': *dst++ = '\n'; break;
                case 'r': *dst++ = '\r'; break;
                case 't': *dst++ = '\t'; break;
                case '\\': *dst++ = '\\'; break;
                case '"': *dst++ = '"'; break;
                case 'x':
                    if (p[1] && p[2]) {
                        char hex[3] = { p[1], p[2], '\0' };
                        char *endptr;
                        long val = strtol(hex, &endptr, 16);
                        if (*endptr == '\0') {
                            *dst++ = (char)val;
                            p += 2;
                            continue;
                        }
                    }
                    *dst++ = *p;
                    break;
                default:
                    *dst++ = *p;
            }
        } else {
            *dst++ = *p;
        }
    }
    *dst = '\0';

    return result;
}

char *str_escape_sql(const char *s) {
    if (!s) return NULL;

    StringBuilder *sb = sb_new(strlen(s) * 2);
    if (!sb) return NULL;

    for (const char *p = s; *p; p++) {
        if (*p == '\'') {
            sb_append(sb, "''");
        } else {
            sb_append_char(sb, *p);
        }
    }

    return sb_to_string(sb);
}

bool str_to_int(const char *s, int *out) {
    if (!s || !out) return false;
    char *endptr;
    errno = 0;
    long val = strtol(s, &endptr, 10);
    if (errno != 0 || *endptr != '\0' || endptr == s) return false;
    if (val < INT32_MIN || val > INT32_MAX) return false;
    *out = (int)val;
    return true;
}

bool str_to_long(const char *s, long *out) {
    if (!s || !out) return false;
    char *endptr;
    errno = 0;
    long val = strtol(s, &endptr, 10);
    if (errno != 0 || *endptr != '\0' || endptr == s) return false;
    *out = val;
    return true;
}

bool str_to_int64(const char *s, int64_t *out) {
    if (!s || !out) return false;
    char *endptr;
    errno = 0;
    long long val = strtoll(s, &endptr, 10);
    if (errno != 0 || *endptr != '\0' || endptr == s) return false;
    *out = (int64_t)val;
    return true;
}

bool str_to_double(const char *s, double *out) {
    if (!s || !out) return false;
    char *endptr;
    errno = 0;
    double val = strtod(s, &endptr);
    if (errno != 0 || *endptr != '\0' || endptr == s) return false;
    *out = val;
    return true;
}

bool str_to_bool(const char *s, bool *out) {
    if (!s || !out) return false;
    if (str_eq_nocase(s, "true") || str_eq_nocase(s, "yes") ||
        str_eq_nocase(s, "1") || str_eq_nocase(s, "on")) {
        *out = true;
        return true;
    }
    if (str_eq_nocase(s, "false") || str_eq_nocase(s, "no") ||
        str_eq_nocase(s, "0") || str_eq_nocase(s, "off")) {
        *out = false;
        return true;
    }
    return false;
}

/* StringBuilder implementation */

StringBuilder *sb_new(size_t initial_cap) {
    StringBuilder *sb = malloc(sizeof(StringBuilder));
    if (!sb) return NULL;

    if (initial_cap == 0) initial_cap = SB_INITIAL_CAP;

    sb->data = malloc(initial_cap);
    if (!sb->data) {
        free(sb);
        return NULL;
    }

    sb->data[0] = '\0';
    sb->len = 0;
    sb->cap = initial_cap;
    return sb;
}

void sb_free(StringBuilder *sb) {
    if (sb) {
        free(sb->data);
        free(sb);
    }
}

static bool sb_grow(StringBuilder *sb, size_t min_cap) {
    size_t new_cap = sb->cap;
    while (new_cap < min_cap) {
        new_cap *= SB_GROWTH_FACTOR;
    }

    char *new_data = realloc(sb->data, new_cap);
    if (!new_data) return false;

    sb->data = new_data;
    sb->cap = new_cap;
    return true;
}

bool sb_append(StringBuilder *sb, const char *s) {
    if (!sb || !s) return false;
    return sb_append_len(sb, s, strlen(s));
}

bool sb_append_len(StringBuilder *sb, const char *s, size_t len) {
    if (!sb || !s) return false;

    size_t needed = sb->len + len + 1;
    if (needed > sb->cap && !sb_grow(sb, needed)) {
        return false;
    }

    memcpy(sb->data + sb->len, s, len);
    sb->len += len;
    sb->data[sb->len] = '\0';
    return true;
}

bool sb_append_char(StringBuilder *sb, char c) {
    if (!sb) return false;

    size_t needed = sb->len + 2;
    if (needed > sb->cap && !sb_grow(sb, needed)) {
        return false;
    }

    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
    return true;
}

bool sb_printf(StringBuilder *sb, const char *fmt, ...) {
    if (!sb || !fmt) return false;

    va_list args;
    va_start(args, fmt);

    va_list args_copy;
    va_copy(args_copy, args);
    int len = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);

    if (len < 0) {
        va_end(args);
        return false;
    }

    size_t needed = sb->len + len + 1;
    if (needed > sb->cap && !sb_grow(sb, needed)) {
        va_end(args);
        return false;
    }

    vsnprintf(sb->data + sb->len, len + 1, fmt, args);
    sb->len += len;
    va_end(args);

    return true;
}

char *sb_to_string(StringBuilder *sb) {
    if (!sb) return NULL;
    char *result = sb->data;
    free(sb);
    return result;
}

const char *sb_data(StringBuilder *sb) {
    return sb ? sb->data : NULL;
}

size_t sb_len(StringBuilder *sb) {
    return sb ? sb->len : 0;
}

void sb_clear(StringBuilder *sb) {
    if (sb) {
        sb->len = 0;
        sb->data[0] = '\0';
    }
}
