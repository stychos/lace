/*
 * Lace
 * Common database driver utilities - Implementation
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "db_common.h"
#include "../util/mem.h"
#include "db.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

char *db_common_escape_identifier(const char *name, DbQuoteStyle style) {
  if (!name)
    return NULL;

  if (style == DB_QUOTE_BACKTICK) {
    return str_escape_identifier_backtick(name);
  }
  return str_escape_identifier_dquote(name);
}

char *db_common_escape_table(const char *table, DbQuoteStyle style,
                             bool support_schema) {
  if (!table)
    return NULL;

  /* Check for schema.table format */
  const char *dot = strchr(table, '.');
  if (dot && support_schema) {
    /* Schema-qualified name */
    size_t schema_len = (size_t)(dot - table);
    char *schema = str_ndup(table, schema_len);
    char *tbl = str_dup(dot + 1);

    if (!schema || !tbl) {
      free(schema);
      free(tbl);
      return NULL;
    }

    char *escaped_schema = db_common_escape_identifier(schema, style);
    char *escaped_table = db_common_escape_identifier(tbl, style);
    free(schema);
    free(tbl);

    if (!escaped_schema || !escaped_table) {
      free(escaped_schema);
      free(escaped_table);
      return NULL;
    }

    char *result = str_printf("%s.%s", escaped_schema, escaped_table);
    free(escaped_schema);
    free(escaped_table);
    return result;
  }

  /* Simple table name */
  return db_common_escape_identifier(table, style);
}

char *db_common_build_query_page_sql(const char *escaped_table, size_t offset,
                                     size_t limit, const char *order_by,
                                     bool desc, DbQuoteStyle style,
                                     char **err) {
  if (!escaped_table) {
    err_set(err, "Invalid table name");
    return NULL;
  }

  char *sql;
  if (order_by) {
    if (db_order_is_prebuilt(order_by)) {
      /* Pre-built ORDER BY clause - use directly */
      sql = str_printf("SELECT * FROM %s ORDER BY %s LIMIT %zu OFFSET %zu",
                       escaped_table, order_by, limit, offset);
    } else {
      /* Single column name - escape and add direction */
      char *escaped_order = db_common_escape_identifier(order_by, style);
      if (!escaped_order) {
        err_set(err, "Memory allocation failed");
        return NULL;
      }
      sql = str_printf("SELECT * FROM %s ORDER BY %s %s LIMIT %zu OFFSET %zu",
                       escaped_table, escaped_order, desc ? "DESC" : "ASC",
                       limit, offset);
      free(escaped_order);
    }
  } else {
    sql = str_printf("SELECT * FROM %s LIMIT %zu OFFSET %zu", escaped_table,
                     limit, offset);
  }

  if (!sql) {
    err_set(err, "Memory allocation failed");
    return NULL;
  }

  return sql;
}

DbInsertLists db_common_build_insert_lists(const ColumnDef *cols,
                                           const DbValue *vals, size_t num_cols,
                                           DbQuoteStyle style, bool use_dollar,
                                           char **err) {
  DbInsertLists result = {NULL, NULL, 0, NULL};

  if (!cols || !vals || num_cols == 0) {
    return result;
  }

  /* First pass: count columns to insert (skip auto_increment with NULL) */
  size_t insert_count = 0;
  for (size_t i = 0; i < num_cols; i++) {
    if (cols[i].auto_increment && vals[i].is_null) {
      continue;
    }
    insert_count++;
  }

  if (insert_count == 0) {
    /* All columns are auto_increment - return empty lists */
    result.col_list = str_dup("");
    result.val_list = str_dup("");
    result.num_params = 0;
    return result;
  }

  /* Allocate column map */
  result.col_map = safe_calloc(insert_count, sizeof(size_t));

  /* Build column list and value placeholders */
  StringBuilder *col_sb = sb_new(256);
  StringBuilder *val_sb = sb_new(128);

  if (!col_sb || !val_sb) {
    sb_free(col_sb);
    sb_free(val_sb);
    free(result.col_map);
    result.col_map = NULL;
    err_set(err, "Memory allocation failed");
    return result;
  }

  size_t param_idx = 0;
  for (size_t i = 0; i < num_cols; i++) {
    if (cols[i].auto_increment && vals[i].is_null) {
      continue;
    }

    char *escaped_col = db_common_escape_identifier(cols[i].name, style);
    if (!escaped_col) {
      sb_free(col_sb);
      sb_free(val_sb);
      free(result.col_map);
      result.col_map = NULL;
      err_set(err, "Memory allocation failed");
      return result;
    }

    /* Add separator if not first */
    if (param_idx > 0) {
      sb_append(col_sb, ", ");
      sb_append(val_sb, ", ");
    }

    sb_append(col_sb, escaped_col);
    free(escaped_col);

    /* Add placeholder */
    if (use_dollar) {
      sb_printf(val_sb, "$%zu", param_idx + 1);
    } else {
      sb_append(val_sb, "?");
    }

    result.col_map[param_idx] = i;
    param_idx++;
  }

  result.col_list = sb_finish(col_sb);
  result.val_list = sb_finish(val_sb);
  result.num_params = param_idx;

  if (!result.col_list || !result.val_list) {
    free(result.col_list);
    free(result.val_list);
    free(result.col_map);
    result.col_list = NULL;
    result.val_list = NULL;
    result.col_map = NULL;
    result.num_params = 0;
    err_set(err, "Memory allocation failed");
  }

  return result;
}

void db_common_free_insert_lists(DbInsertLists *lists) {
  if (!lists)
    return;
  free(lists->col_list);
  free(lists->val_list);
  free(lists->col_map);
  lists->col_list = NULL;
  lists->val_list = NULL;
  lists->col_map = NULL;
  lists->num_params = 0;
}

void db_common_free_string_list(char **list, size_t count) {
  if (!list)
    return;
  for (size_t i = 0; i < count; i++) {
    free(list[i]);
  }
  free(list);
}

char *db_common_build_update_set(const char *col, DbQuoteStyle style,
                                 bool use_dollar, size_t param_idx,
                                 char **err) {
  char *escaped_col = db_common_escape_identifier(col, style);
  if (!escaped_col) {
    err_set(err, "Memory allocation failed");
    return NULL;
  }

  char *set_clause;
  if (use_dollar) {
    set_clause = str_printf("%s = $%zu", escaped_col, param_idx);
  } else {
    set_clause = str_printf("%s = ?", escaped_col);
  }
  free(escaped_col);

  if (!set_clause) {
    err_set(err, "Memory allocation failed");
  }
  return set_clause;
}

char *db_common_build_update_sql(const char *escaped_table, const char *col,
                                 const char **pk_cols, size_t num_pk_cols,
                                 DbQuoteStyle style, bool use_dollar,
                                 char **err) {
  if (!escaped_table || !col || !pk_cols || num_pk_cols == 0) {
    err_set(err, "Invalid parameters");
    return NULL;
  }

  /* Escape column name */
  char *escaped_col = db_common_escape_identifier(col, style);
  if (!escaped_col) {
    err_set(err, "Memory allocation failed");
    return NULL;
  }

  /* Build WHERE clause - param 1 is the new value, so PK starts at 2 */
  size_t pk_start_idx = use_dollar ? 2 : 1;
  char *where_clause = str_build_pk_where(
      pk_cols, num_pk_cols, use_dollar, pk_start_idx,
      style == DB_QUOTE_BACKTICK);
  if (!where_clause) {
    free(escaped_col);
    err_set(err, "Memory allocation failed");
    return NULL;
  }

  /* Build UPDATE statement */
  char *sql;
  if (use_dollar) {
    sql = str_printf("UPDATE %s SET %s = $1 WHERE %s", escaped_table,
                     escaped_col, where_clause);
  } else {
    sql = str_printf("UPDATE %s SET %s = ? WHERE %s", escaped_table,
                     escaped_col, where_clause);
  }

  free(escaped_col);
  free(where_clause);

  if (!sql) {
    err_set(err, "Memory allocation failed");
  }
  return sql;
}

char *db_common_build_delete_sql(const char *escaped_table,
                                 const char **pk_cols, size_t num_pk_cols,
                                 DbQuoteStyle style, bool use_dollar,
                                 char **err) {
  if (!escaped_table || !pk_cols || num_pk_cols == 0) {
    err_set(err, "Invalid parameters");
    return NULL;
  }

  /* Build WHERE clause */
  char *where_clause =
      str_build_pk_where(pk_cols, num_pk_cols, use_dollar, 1,
                         style == DB_QUOTE_BACKTICK);
  if (!where_clause) {
    err_set(err, "Memory allocation failed");
    return NULL;
  }

  /* Build DELETE statement */
  char *sql =
      str_printf("DELETE FROM %s WHERE %s", escaped_table, where_clause);
  free(where_clause);

  if (!sql) {
    err_set(err, "Memory allocation failed");
  }
  return sql;
}

char *db_common_build_insert_sql(const char *escaped_table,
                                 const ColumnDef *cols, const DbValue *vals,
                                 size_t num_cols, DbQuoteStyle style,
                                 bool use_dollar, DbInsertLists *lists_out,
                                 char **err) {
  if (!escaped_table || !cols || !vals || num_cols == 0 || !lists_out) {
    err_set(err, "Invalid parameters");
    return NULL;
  }

  /* Build column and value lists */
  *lists_out =
      db_common_build_insert_lists(cols, vals, num_cols, style, use_dollar, err);

  if (!lists_out->col_list || !lists_out->val_list) {
    /* Error already set by db_common_build_insert_lists */
    return NULL;
  }

  /* Handle case where all columns are auto_increment */
  char *sql;
  if (lists_out->num_params == 0) {
    sql = str_printf("INSERT INTO %s DEFAULT VALUES", escaped_table);
  } else {
    sql = str_printf("INSERT INTO %s (%s) VALUES (%s)", escaped_table,
                     lists_out->col_list, lists_out->val_list);
  }

  if (!sql) {
    db_common_free_insert_lists(lists_out);
    err_set(err, "Memory allocation failed");
    return NULL;
  }

  return sql;
}

void db_common_free_connection(DbConnection *conn) {
  if (!conn)
    return;
  str_secure_free(conn->connstr);
  free(conn->database);
  free(conn->host);
  free(conn->user);
  free(conn->last_error);
  free(conn);
}

bool db_common_parse_int64(const char *str, int64_t *value_out) {
  if (!str || !value_out || !*str)
    return false;

  char *endptr;
  errno = 0;
  long long parsed = strtoll(str, &endptr, 10);

  if (errno != 0 || endptr == str || *endptr != '\0')
    return false;

  *value_out = parsed;
  return true;
}

bool db_common_parse_double(const char *str, double *value_out) {
  if (!str || !value_out || !*str)
    return false;

  char *endptr;
  errno = 0;
  double parsed = strtod(str, &endptr);

  if (errno != 0 || endptr == str || *endptr != '\0')
    return false;

  *value_out = parsed;
  return true;
}

char **db_common_parse_pg_array(const char *array_str, size_t *count_out) {
  if (!count_out)
    return NULL;
  *count_out = 0;

  if (!array_str || array_str[0] != '{')
    return NULL;

  /* Count elements */
  size_t num_elems = 0;
  bool in_element = false;
  for (const char *p = array_str + 1; *p && *p != '}'; p++) {
    if (*p == ',') {
      if (in_element)
        num_elems++;
      in_element = false;
    } else {
      if (!in_element) {
        in_element = true;
        num_elems++;
      }
    }
  }

  if (num_elems == 0)
    return NULL;

  /* Check for overflow before allocation */
  if (num_elems > SIZE_MAX / sizeof(char *))
    return NULL;

  /* Allocate array */
  char **result = safe_calloc(num_elems, sizeof(char *));

  /* Parse elements */
  const char *p = array_str + 1; /* Skip opening brace */
  size_t idx = 0;
  while (*p && *p != '}' && idx < num_elems) {
    const char *start = p;
    while (*p && *p != ',' && *p != '}')
      p++;
    size_t len = (size_t)(p - start);

    result[idx] = str_ndup(start, len);
    if (!result[idx]) {
      /* Cleanup on failure */
      for (size_t i = 0; i < idx; i++)
        free(result[i]);
      free(result);
      return NULL;
    }
    idx++;

    if (*p == ',')
      p++;
  }

  *count_out = idx;
  return result;
}

void db_common_free_pg_array(char **array, size_t count) {
  if (!array)
    return;
  for (size_t i = 0; i < count; i++)
    free(array[i]);
  free(array);
}
