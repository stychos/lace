/*
 * Lace
 * Fuzz: the TUI's query scanners and filter-to-WHERE builder
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "fuzz.h"
#include "query_internal.h"
#include "core/app_state.h"

static const char *const driver_names[] = {"sqlite", "postgres", "mysql", "mariadb"};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < 3)
    return 0;
  size_t d = data[0] % 4;
  uint8_t op = data[1];
  char *s = fuzz_cstr(data + 2, size - 2);
  size_t len = strlen(s);

  /* Scanners over arbitrary SQL */
  QuerySqlShape sh;
  query_sql_shape(s, driver_names[d], &sh);
  if (sh.end > len)
    FUZZ_FAIL("shape end %zu past length %zu", sh.end, len);
  (void)query_sql_pageable(&sh);
  (void)query_page_needs_wrap(&sh);
  size_t sl = query_statement_length(s, driver_names[d]);
  if (sl > len)
    FUZZ_FAIL("statement length %zu past length %zu", sl, len);
  free(query_extract_table_name(s, (LaceDriver)fuzz_drivers[d]));

  /* A filter value never escapes its literal: split the text into value and
   * value2 at the first '\x01' */
  char *v2 = strchr(s, '\x01');
  if (v2)
    *v2++ = '\0';
  ColumnDef cols[2];
  memset(cols, 0, sizeof(cols));
  cols[0].name = "id";
  cols[0].type = LACE_TYPE_INT;
  cols[1].name = (op & 0x80) ? "we\"ird`name" : "name";
  cols[1].type = LACE_TYPE_TEXT;
  TableSchema schema;
  memset(&schema, 0, sizeof(schema));
  schema.columns = cols;
  schema.num_columns = 2;
  TableFilters f = {0};
  FilterOperator fop = (FilterOperator)(op % FILTER_OP_RAW); /* Not RAW */
  if (filters_add(&f, (op >> 4) & 1, fop, s)) {
    if (v2)
      snprintf(f.filters[0].value2, sizeof(f.filters[0].value2), "%s", v2);
    char *err = NULL;
    char *where = filters_build_where(&f, &schema, driver_names[d], &err);
    if (where)
      fuzz_check_contained(where, fuzz_drivers[d], "=<>!(),.-+*%~", "WHERE clause", s);
    free(where);
    free(err);
  }
  filters_free(&f);
  free(s);
  return 0;
}
