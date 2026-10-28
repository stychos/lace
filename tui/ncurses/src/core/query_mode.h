/*
 * Lace
 * Query mode (-q): run SQL once and print the result, shared by both TUIs.
 * The formats: an aligned table (default), vertical (MySQL's \G: one
 * "column: value" line per field), CSV, JSON (an array of objects) and TSV.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_QUERY_MODE_H
#define LACE_QUERY_MODE_H

#include "../../../liblace/include/types.h"
#include <stdbool.h>
#include <stdio.h>

typedef enum {
  QUERY_OUT_TABLE,    /* aligned columns in an ASCII frame, a row count */
  QUERY_OUT_VERTICAL, /* MySQL's \G: a block per row, one line per field */
  QUERY_OUT_CSV,      /* RFC 4180, a header row; NULL empty, '' as "" */
  QUERY_OUT_JSON,     /* an array of objects, one per line; exact numbers */
  QUERY_OUT_TSV,      /* a header row, tab-separated; \t \n \r \\ escaped, NULL as NULL */
} QueryOutFormat;

/* "table", "vertical" (or "G"), "csv", "json", "tsv"; false for another name */
bool query_out_format_from_name(const char *name, QueryOutFormat *out);
/* The names, for usage text: "table, vertical, csv, json, tsv" */
const char *query_out_format_names(void);

/* A trailing \G (MySQL's terminator for vertical output, after optional
 * spaces and before an optional ';') is cut from sql; true when it was there */
bool query_out_strip_vertical(char *sql);

/* The result in the format. A result without columns (INSERT, DDL) writes
 * its affected count: "N rows affected" for table and vertical,
 * {"rows_affected": N} for JSON, nothing for CSV and TSV (whose callers
 * say it on stderr). Returns false when f could not be written. */
bool query_out_write(FILE *f, const LaceResult *r, QueryOutFormat fmt);

/* -q: connect to connstr through a daemon of spawn_mode (config.json's
 * general.daemon_spawn_mode, a LaceSpawnMode value), run sql,
 * print the result to stdout in fmt (messages and errors to stderr) and
 * disconnect. The exit status: 0, or 1 on any failure. */
int query_mode_run(int spawn_mode, int idle_timeout, const char *connstr, const char *sql, QueryOutFormat fmt);

#endif /* LACE_QUERY_MODE_H */
