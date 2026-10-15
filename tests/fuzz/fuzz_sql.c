/*
 * Lace
 * Fuzz: the shared SQL lexer, quoting, and the frontend SQL builders
 * (liblace: lace_sql_*, sql_builder.h)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "fuzz.h"
#include "util/str.h"

/* The builders' output for untrusted text: every clause they make from it
 * must keep it inside quotes, and every scanner must stay inside the text */
static void check_builders(const char *s, size_t len, LaceDriver driver) {
  /* Statement shape and boundaries */
  LaceSqlShape shape;
  lace_sql_shape(s, driver, &shape);
  if (shape.end > len)
    FUZZ_FAIL("shape end %zu past %zu", shape.end, len);
  size_t st = 0, en = 0;
  if (lace_sql_statement_at(s, len / 2, driver, &st, &en) &&
      (st > en || en > len))
    FUZZ_FAIL("statement_at gave [%zu, %zu) of %zu", st, en, len);
  size_t pos = 0, next = 0;
  while (lace_sql_next_statement(s, pos, driver, &st, &en, &next)) {
    if (st > en || en > len || next <= pos || next > len)
      FUZZ_FAIL("next_statement gave [%zu, %zu) next %zu from %zu of %zu", st,
                en, next, pos, len);
    pos = next;
  }

  /* The table of a simple SELECT is a table reference liblace can quote */
  char *tbl = lace_sql_simple_select_table(s, driver);
  if (tbl) {
    char *q = lace_quote_table(driver, tbl);
    if (q)
      fuzz_check_contained(q, driver, ".", "simple select table", s);
    free(q);
  }
  free(tbl);

  /* Filters over a schema of one text and one integer column; the raw
   * condition is the user's own SQL and is left out */
  LaceColumn cols[2] = {{.name = (char *)"c", .type = LACE_TYPE_TEXT},
                        {.name = (char *)"n", .type = LACE_TYPE_INT}};
  LaceSchema schema = {0};
  schema.columns = cols;
  schema.num_columns = 2;
  for (int op = 0; op < LACE_FILTER_OP_COUNT; op++) {
    if (op == LACE_FILTER_RAW)
      continue;
    LaceFilter f = {(char *)(op % 2 ? "c" : "n"), (LaceFilterOp)op, (char *)s,
                    (char *)s};
    char *where = lace_filter_build_where(&f, 1, NULL, &schema, driver);
    if (where)
      fuzz_check_contained(where, driver, "=<>~(),+-.", "filter", s);
    free(where);
  }
  char *search = lace_filter_build_where(NULL, 0, s, &schema, driver);
  if (search)
    fuzz_check_contained(search, driver, "()", "search", s);
  free(search);

  LaceSortSpec sort = {(char *)s, true};
  char *order = lace_sort_build_order_by(&sort, 1, driver);
  if (order)
    fuzz_check_contained(order, driver, ",", "order by", s);
  free(order);

  char *page = lace_sql_table_page(driver, s, NULL, NULL, 10, 0);
  if (page)
    fuzz_check_contained(page, driver, "*.", "table page", s);
  free(page);

  /* Display text of any bytes */
  LaceValue v = lace_value_text(s);
  free(lace_value_display_text(&v, 16));
  lace_value_free(&v);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < 1)
    return 0;
  int driver = data[0] % 5 == 4 ? -1 : fuzz_drivers[data[0] % 5];
  char *s = fuzz_cstr(data + 1, size - 1);
  size_t len = strlen(s);

  /* The lexer always makes progress and stays inside the string */
  for (size_t i = 0; i < len;) {
    bool comment;
    size_t k = lace_sql_skip(s + i, driver, &comment);
    if (k > len - i)
      FUZZ_FAIL("lace_sql_skip went past the end at %zu", i);
    i += k ? k : 1;
  }
  size_t sl = lace_sql_statement_length(s, driver);
  if (sl > len || (s[sl] != ';' && s[sl] != '\0'))
    FUZZ_FAIL("statement length %zu of %zu does not end at ';'", sl, len);
  (void)lace_sql_single_statement(s, driver);
  (void)lace_sql_is_rollback(s);

  /* Whatever the text, its quoted forms stay quoted */
  if (driver >= 0) {
    char *lit = lace_sql_string_literal((LaceDriver)driver, s);
    if (lit)
      fuzz_check_contained(lit, driver, "", "string literal", s);
    free(lit);
    char *id = lace_quote_identifier((LaceDriver)driver, s);
    if (id)
      fuzz_check_contained(id, driver, "", "identifier", s);
    free(id);
    char *tbl = lace_quote_table((LaceDriver)driver, s);
    if (tbl)
      fuzz_check_contained(tbl, driver, ".", "table reference", s);
    free(tbl);
    check_builders(s, len, (LaceDriver)driver);
  }
  free(s);
  return 0;
}
