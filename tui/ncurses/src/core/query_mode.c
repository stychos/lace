/*
 * Lace
 * Query mode (-q): see query_mode.h
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "query_mode.h"
#include "utf8.h"
#include "../../../liblace/include/lace.h"
#include "../../../liblace/include/util/mem.h"
#include "../../../liblace/include/util/str.h"
#include <ctype.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>

static const struct {
  const char *name;
  QueryOutFormat fmt;
} g_formats[] = {{"table", QUERY_OUT_TABLE}, {"vertical", QUERY_OUT_VERTICAL}, {"G", QUERY_OUT_VERTICAL},
                 {"csv", QUERY_OUT_CSV},     {"json", QUERY_OUT_JSON},         {"tsv", QUERY_OUT_TSV}};

bool query_out_format_from_name(const char *name, QueryOutFormat *out) {
  for (size_t i = 0; name && i < sizeof g_formats / sizeof *g_formats; i++) {
    if (strcmp(name, g_formats[i].name) == 0) {
      if (out)
        *out = g_formats[i].fmt;
      return true;
    }
  }
  return false;
}

const char *query_out_format_names(void) { return "table, vertical, csv, json, tsv"; }

bool query_out_strip_vertical(char *sql) {
  if (!sql)
    return false;
  size_t n = strlen(sql);
  while (n && isspace((unsigned char)sql[n - 1]))
    n--;
  if (n && sql[n - 1] == ';') /* "SELECT ...\G;" */
    n--;
  while (n && isspace((unsigned char)sql[n - 1]))
    n--;
  if (n < 2 || sql[n - 2] != '\\' || (sql[n - 1] != 'G' && sql[n - 1] != 'g'))
    return false;
  sql[n - 2] = '\0';
  return true;
}

/* ==========================================================================
 * Values
 * ========================================================================== */

static const LaceValue *cell(const LaceResult *r, size_t row, size_t col) {
  if (!r->rows || row >= r->num_rows)
    return NULL;
  const LaceRow *rw = &r->rows[row];
  return rw->cells && col < rw->num_cells ? &rw->cells[col] : NULL;
}

static bool is_null(const LaceValue *v) { return !v || v->is_null || v->type == LACE_TYPE_NULL; }

/* Every byte of a BLOB as x'..' (the display cuts at 32 bytes) */
static char *blob_text(const LaceValue *v) {
  size_t n = v->blob.data ? v->blob.len : 0;
  char *s = safe_malloc(n * 2 + 4);
  size_t p = 0;
  s[p++] = 'x';
  s[p++] = '\'';
  for (size_t i = 0; i < n; i++) {
    static const char hex[] = "0123456789abcdef";
    s[p++] = hex[v->blob.data[i] >> 4];
    s[p++] = hex[v->blob.data[i] & 15];
  }
  s[p++] = '\'';
  s[p] = '\0';
  return s;
}

/* The exact text of a value ("NULL" for NULL); caller frees */
static char *value_text(const LaceValue *v) {
  if (is_null(v))
    return str_dup("NULL");
  if (v->omitted) /* too large to load: its "[TEXT: N bytes]" label */
    return str_dup(v->text.data ? v->text.data : "");
  if (v->type == LACE_TYPE_BLOB)
    return blob_text(v);
  return lace_value_to_string(v);
}

static bool is_numeric(const LaceValue *v) {
  return !is_null(v) && !v->omitted &&
         (v->type == LACE_TYPE_INT || v->type == LACE_TYPE_FLOAT || v->type == LACE_TYPE_DECIMAL);
}

/* A value on one line: control characters as \n, \t, \r, \\ (tsv also
 * escapes the backslash itself, so the field reads back exactly) */
static char *one_line(const char *s, bool escape_backslash) {
  size_t n = strlen(s);
  char *out = safe_malloc(n * 2 + 1), *o = out;
  for (const char *p = s; *p; p++) {
    switch (*p) {
    case '\n': *o++ = '\\'; *o++ = 'n'; break;
    case '\t': *o++ = '\\'; *o++ = 't'; break;
    case '\r': *o++ = '\\'; *o++ = 'r'; break;
    case '\\':
      *o++ = '\\';
      if (escape_backslash)
        *o++ = '\\';
      break;
    default: *o++ = *p;
    }
  }
  *o = '\0';
  return out;
}

static const char *column_name(const LaceResult *r, size_t c) {
  return r->columns && r->columns[c].name ? r->columns[c].name : "";
}

/* ==========================================================================
 * Table and vertical
 * ========================================================================== */

static void repeat(FILE *f, char ch, int n) {
  for (int i = 0; i < n; i++)
    fputc(ch, f);
}

static void pad(FILE *f, const char *s, int width, bool right) {
  int w = utf8_width(s, strlen(s));
  if (right)
    repeat(f, ' ', width - w);
  fputs(s, f);
  if (!right)
    repeat(f, ' ', width - w);
}

static void rule(FILE *f, const int *widths, size_t n) {
  fputc('+', f);
  for (size_t c = 0; c < n; c++) {
    repeat(f, '-', widths[c] + 2);
    fputc('+', f);
  }
  fputc('\n', f);
}

static void footer(FILE *f, const LaceResult *r) {
  fprintf(f, "%zu %s%s\n", r->num_rows, r->num_rows == 1 ? "row" : "rows",
          r->truncated ? " (truncated: the result exceeded the daemon's limit)" : "");
}

static void write_table(FILE *f, const LaceResult *r) {
  size_t nc = r->num_columns, nr = r->num_rows;
  int *widths = safe_calloc(nc, sizeof *widths);
  bool *numeric = safe_calloc(nc, sizeof *numeric);
  char **text = safe_calloc(nr * nc + 1, sizeof *text);
  for (size_t c = 0; c < nc; c++) {
    const char *name = column_name(r, c);
    widths[c] = utf8_width(name, strlen(name));
    numeric[c] = nr > 0;
  }
  for (size_t row = 0; row < nr; row++) {
    for (size_t c = 0; c < nc; c++) {
      const LaceValue *v = cell(r, row, c);
      char *raw = value_text(v);
      text[row * nc + c] = one_line(raw, false);
      str_secure_free(raw);
      int w = utf8_width(text[row * nc + c], strlen(text[row * nc + c]));
      if (w > widths[c])
        widths[c] = w;
      if (!is_null(v) && !is_numeric(v))
        numeric[c] = false; /* a column of numbers (and NULLs) is right-aligned */
    }
  }
  rule(f, widths, nc);
  fputc('|', f);
  for (size_t c = 0; c < nc; c++) {
    fputc(' ', f);
    pad(f, column_name(r, c), widths[c], false);
    fputs(" |", f);
  }
  fputc('\n', f);
  rule(f, widths, nc);
  for (size_t row = 0; row < nr; row++) {
    fputc('|', f);
    for (size_t c = 0; c < nc; c++) {
      fputc(' ', f);
      pad(f, text[row * nc + c], widths[c], numeric[c]);
      fputs(" |", f);
    }
    fputc('\n', f);
  }
  if (nr > 0)
    rule(f, widths, nc);
  footer(f, r);
  for (size_t i = 0; i < nr * nc; i++)
    str_secure_free(text[i]);
  free(text);
  free(widths);
  free(numeric);
}

static void write_vertical(FILE *f, const LaceResult *r) {
  int name_width = 0;
  for (size_t c = 0; c < r->num_columns; c++) {
    int w = utf8_width(column_name(r, c), strlen(column_name(r, c)));
    if (w > name_width)
      name_width = w;
  }
  for (size_t row = 0; row < r->num_rows; row++) {
    fprintf(f, "*************************** %zu. row ***************************\n", row + 1);
    for (size_t c = 0; c < r->num_columns; c++) {
      pad(f, column_name(r, c), name_width, true);
      fputs(": ", f);
      char *s = value_text(cell(r, row, c));
      fputs(s, f); /* as it is, newlines included (MySQL's \G does the same) */
      str_secure_free(s);
      fputc('\n', f);
    }
  }
  footer(f, r);
}

/* ==========================================================================
 * CSV, TSV, JSON
 * ========================================================================== */

static void csv_field(FILE *f, const char *s, bool quote_always) {
  bool quote = quote_always || !*s || s[0] == ' ' || s[strlen(s) - 1] == ' ' || strpbrk(s, ",\"\r\n");
  if (!quote) {
    fputs(s, f);
    return;
  }
  fputc('"', f);
  for (const char *p = s; *p; p++) {
    if (*p == '"')
      fputc('"', f);
    fputc(*p, f);
  }
  fputc('"', f);
}

static void write_csv(FILE *f, const LaceResult *r) {
  for (size_t c = 0; c < r->num_columns; c++) {
    if (c)
      fputc(',', f);
    csv_field(f, column_name(r, c), false);
  }
  fputc('\n', f);
  for (size_t row = 0; row < r->num_rows; row++) {
    for (size_t c = 0; c < r->num_columns; c++) {
      if (c)
        fputc(',', f);
      const LaceValue *v = cell(r, row, c);
      if (is_null(v))
        continue; /* NULL: an empty field; '' is written "" */
      char *s = value_text(v);
      csv_field(f, s, false);
      str_secure_free(s);
    }
    fputc('\n', f);
  }
}

static void write_tsv(FILE *f, const LaceResult *r) {
  for (size_t c = 0; c < r->num_columns; c++) {
    if (c)
      fputc('\t', f);
    char *s = one_line(column_name(r, c), true);
    fputs(s, f);
    free(s);
  }
  fputc('\n', f);
  for (size_t row = 0; row < r->num_rows; row++) {
    for (size_t c = 0; c < r->num_columns; c++) {
      if (c)
        fputc('\t', f);
      char *raw = value_text(cell(r, row, c));
      char *s = one_line(raw, true);
      fputs(s, f);
      str_secure_free(raw);
      str_secure_free(s);
    }
    fputc('\n', f);
  }
}

static void json_string(FILE *f, const char *s) {
  fputc('"', f);
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    switch (*p) {
    case '"': fputs("\\\"", f); break;
    case '\\': fputs("\\\\", f); break;
    case '\n': fputs("\\n", f); break;
    case '\r': fputs("\\r", f); break;
    case '\t': fputs("\\t", f); break;
    default:
      if (*p < 0x20)
        fprintf(f, "\\u%04x", *p);
      else
        fputc(*p, f);
    }
  }
  fputc('"', f);
}

/* JSON's own number grammar: what may be written as a bare number */
static bool json_number(const char *s) {
  const char *p = s;
  if (*p == '-')
    p++;
  if (!isdigit((unsigned char)*p))
    return false;
  if (*p == '0' && isdigit((unsigned char)p[1]))
    return false;
  while (isdigit((unsigned char)*p))
    p++;
  if (*p == '.') {
    p++;
    if (!isdigit((unsigned char)*p))
      return false;
    while (isdigit((unsigned char)*p))
      p++;
  }
  if (*p == 'e' || *p == 'E') {
    p++;
    if (*p == '+' || *p == '-')
      p++;
    if (!isdigit((unsigned char)*p))
      return false;
    while (isdigit((unsigned char)*p))
      p++;
  }
  return *p == '\0';
}

static void json_value(FILE *f, const LaceValue *v) {
  if (is_null(v)) {
    fputs("null", f);
    return;
  }
  if (v->type == LACE_TYPE_BOOL && !v->omitted) {
    fputs(v->bool_val ? "true" : "false", f);
    return;
  }
  char *s = value_text(v);
  /* Numbers exactly as the database gave them (no double in between);
   * NaN and Infinity, which JSON has no number for, as strings */
  if (is_numeric(v) && json_number(s))
    fputs(s, f);
  else
    json_string(f, s);
  str_secure_free(s);
}

static void write_json(FILE *f, const LaceResult *r) {
  fputc('[', f);
  for (size_t row = 0; row < r->num_rows; row++) {
    fputs(row ? ",\n {" : "\n {", f);
    for (size_t c = 0; c < r->num_columns; c++) {
      if (c)
        fputs(", ", f);
      json_string(f, column_name(r, c));
      fputs(": ", f);
      json_value(f, cell(r, row, c));
    }
    fputc('}', f);
  }
  fputs(r->num_rows ? "\n]\n" : "]\n", f);
}

bool query_out_write(FILE *f, const LaceResult *r, QueryOutFormat fmt) {
  if (!f || !r)
    return false;
  if (r->num_columns == 0) {
    long long n = (long long)r->rows_affected;
    if (fmt == QUERY_OUT_JSON)
      fprintf(f, "{\"rows_affected\": %lld}\n", n);
    else if (fmt == QUERY_OUT_TABLE || fmt == QUERY_OUT_VERTICAL)
      fprintf(f, "%lld %s affected\n", n, n == 1 ? "row" : "rows");
    return !ferror(f);
  }
  switch (fmt) {
  case QUERY_OUT_TABLE: write_table(f, r); break;
  case QUERY_OUT_VERTICAL: write_vertical(f, r); break;
  case QUERY_OUT_CSV: write_csv(f, r); break;
  case QUERY_OUT_JSON: write_json(f, r); break;
  case QUERY_OUT_TSV: write_tsv(f, r); break;
  }
  return !ferror(f);
}

/* ==========================================================================
 * Running it
 * ========================================================================== */

int query_mode_run(int spawn_mode, int idle_timeout, const char *connstr, const char *sql, QueryOutFormat fmt) {
  if (!connstr || !*connstr) {
    fprintf(stderr, "-q needs a connection string or a SQLite file\n");
    return 1;
  }
  setlocale(LC_CTYPE, ""); /* display widths of UTF-8 text in the table */
  LaceClientConfig cc = {0};
  cc.mode = LACE_CONN_AUTO;
  cc.spawn_mode = (LaceSpawnMode)spawn_mode;
  cc.idle_timeout = idle_timeout;
  lace_client_t *client = lace_client_create_with_config(&cc);
  if (!client) {
    fprintf(stderr, "Failed to allocate client\n");
    return 1;
  }
  if (!lace_client_connected(client)) {
    fprintf(stderr, "Failed to connect to lace daemon: %s\n", lace_client_error(client));
    lace_client_destroy(client);
    return 1;
  }
  int conn_id = -1;
  if (lace_connect(client, connstr, NULL, &conn_id) != LACE_OK) {
    const char *why = lace_client_error(client);
    fprintf(stderr, "Connection failed: %s\n", why && *why ? why : "unknown error");
    lace_client_destroy(client);
    return 1;
  }
  LaceResult *r = NULL;
  int rc = 0;
  if (lace_exec(client, conn_id, sql, &r) != LACE_OK) {
    const char *why = lace_client_error(client);
    fprintf(stderr, "Query failed: %s\n", why && *why ? why : "unknown error");
    rc = 1;
  } else if (!r) {
    LaceResult none = {.rows_affected = 0};
    query_out_write(stdout, &none, fmt);
  } else if (r->num_columns > 0 && (!r->columns || (r->num_rows > 0 && !r->rows))) {
    fprintf(stderr, "Invalid result set structure\n");
    rc = 1;
  } else {
    if (!query_out_write(stdout, r, fmt)) {
      fprintf(stderr, "Could not write the result\n");
      rc = 1;
    }
    bool machine = fmt == QUERY_OUT_CSV || fmt == QUERY_OUT_JSON || fmt == QUERY_OUT_TSV;
    if (machine && r->num_columns == 0 && fmt != QUERY_OUT_JSON)
      fprintf(stderr, "%lld rows affected\n", (long long)r->rows_affected);
    if (machine && r->truncated)
      fprintf(stderr, "truncated: the result exceeded the daemon's limit (%zu rows written)\n", r->num_rows);
  }
  if (fflush(stdout) != 0)
    rc = 1;
  lace_result_free(r);
  lace_disconnect(client, conn_id);
  lace_client_destroy(client);
  return rc;
}
