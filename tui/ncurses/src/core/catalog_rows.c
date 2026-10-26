#include "catalog_rows.h"
#include "utf8.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ==========================================================================
 * The table
 * ========================================================================== */

void catalog_rows_init(CatalogRows *t, size_t ncols, const char *const *head) {
  memset(t, 0, sizeof *t);
  t->ncols = ncols;
  if (head) {
    t->head = safe_calloc(ncols, sizeof *t->head);
    for (size_t i = 0; i < ncols; i++) t->head[i] = str_dup(head[i] ? head[i] : "");
  }
}

static size_t grow(CatalogRows *t) {
  if (t->nrows == t->cap) {
    t->cap = t->cap ? t->cap * 2 : 16;
    t->cells = safe_realloc(t->cells, t->cap * sizeof *t->cells);
    t->span = safe_realloc(t->span, t->cap * sizeof *t->span);
    t->heading = safe_realloc(t->heading, t->cap * sizeof *t->heading);
    t->tag = safe_realloc(t->tag, t->cap * sizeof *t->tag);
  }
  size_t r = t->nrows++;
  t->cells[r] = safe_calloc(t->ncols ? t->ncols : 1, sizeof **t->cells);
  t->span[r] = t->heading[r] = false;
  t->tag[r] = 0;
  return r;
}

/* One line: control characters become spaces */
static char *clean(const char *s) {
  char *out = str_dup(s ? s : "");
  for (char *c = out; *c; c++)
    if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
  return out;
}

void catalog_rows_add(CatalogRows *t, int tag, ...) {
  size_t r = grow(t);
  t->tag[r] = tag;
  va_list ap;
  va_start(ap, tag);
  for (size_t i = 0; i < t->ncols; i++) t->cells[r][i] = clean(va_arg(ap, const char *));
  va_end(ap);
}

static void add_cells(CatalogRows *t, int tag, const char *const *cells) {
  size_t r = grow(t);
  t->tag[r] = tag;
  for (size_t i = 0; i < t->ncols; i++) t->cells[r][i] = clean(cells[i]);
}

void catalog_rows_span(CatalogRows *t, int tag, const char *text) {
  size_t r = grow(t);
  t->tag[r] = tag;
  t->span[r] = true;
  t->cells[r][0] = clean(text);
  for (size_t i = 1; i < t->ncols; i++) t->cells[r][i] = str_dup("");
}

void catalog_rows_heading(CatalogRows *t, const char *title) {
  catalog_rows_span(t, 0, title);
  t->heading[t->nrows - 1] = true;
}

void catalog_rows_blank(CatalogRows *t) {
  catalog_rows_span(t, 0, "");
  t->heading[t->nrows - 1] = true; /* never selected */
}

void catalog_rows_head_row(CatalogRows *t, const char *const *cells) {
  add_cells(t, 0, cells);
  t->heading[t->nrows - 1] = true;
}

void catalog_rows_free(CatalogRows *t) {
  for (size_t r = 0; r < t->nrows; r++) {
    for (size_t i = 0; i < t->ncols; i++) free(t->cells[r][i]);
    free(t->cells[r]);
  }
  free(t->cells);
  free(t->span);
  free(t->heading);
  free(t->tag);
  for (size_t i = 0; t->head && i < t->ncols; i++) free(t->head[i]);
  free(t->head);
  memset(t, 0, sizeof *t);
}

bool catalog_rows_selectable(const CatalogRows *t, size_t row) { return row < t->nrows && !t->heading[row]; }

/* ==========================================================================
 * Helpers
 * ========================================================================== */

char *catalog_join(char *const *names, size_t n) {
  char *out = str_dup("");
  for (size_t i = 0; i < n; i++) {
    char *next = str_printf("%s%s%s", out, i ? ", " : "", names[i] ? names[i] : "");
    free(out);
    out = next;
  }
  return out;
}

char *catalog_commas(int64_t n) {
  char raw[32];
  snprintf(raw, sizeof raw, "%lld", (long long)(n < 0 ? -n : n));
  size_t len = strlen(raw);
  char out[48];
  size_t o = 0;
  if (n < 0) out[o++] = '-';
  for (size_t i = 0; i < len; i++) {
    if (i && (len - i) % 3 == 0) out[o++] = ',';
    out[o++] = raw[i];
  }
  out[o] = 0;
  return str_dup(out);
}

char *catalog_bytes(int64_t n) {
  if (n < 0) return str_dup("unknown");
  static const char *const unit[] = {"bytes", "KB", "MB", "GB", "TB"};
  double d = (double)n;
  int u = 0;
  while (d >= 1024 && u < 4) {
    d /= 1024;
    u++;
  }
  char *exact = catalog_commas(n);
  char *out = u == 0 ? str_printf("%s bytes", exact) : str_printf("%.1f %s (%s bytes)", d, unit[u], exact);
  free(exact);
  return out;
}

bool catalog_fk_first_column(const LaceSchema *sc, const char *name) {
  for (size_t i = 0; sc && name && i < sc->num_foreign_keys; i++) {
    const LaceForeignKey *fk = &sc->foreign_keys[i];
    if (fk->num_columns && fk->columns[0] && !strcmp(fk->columns[0], name)) return true;
  }
  return false;
}

const char *catalog_object_word(LaceObjectKind kind) {
  switch (kind) {
  case LACE_OBJ_VIEW: return "View";
  case LACE_OBJ_ROUTINE: return "Routine";
  case LACE_OBJ_TRIGGER: return "Trigger";
  default: return "Table";
  }
}

/* ==========================================================================
 * Structure
 * ========================================================================== */

static const char *const STRUCT_HEAD[] = {"#", "Name", "Type", "Null", "Key", "Default", "Extra", "Collation", "Comment"};
enum { C_NUM, C_NAME, C_TYPE, C_NULL, C_KEY, C_DEFAULT, C_EXTRA, C_COLLATION, C_COMMENT, C_COUNT };

/* How many of the columns a schema needs: the collation and comment ones
 * only when a column has one */
static size_t structure_ncols(const LaceSchema *sc) {
  bool collation = false, comment = false;
  for (size_t i = 0; i < sc->num_columns; i++) {
    if (sc->columns[i].collation && *sc->columns[i].collation) collation = true;
    if (sc->columns[i].comment && *sc->columns[i].comment) comment = true;
  }
  return comment ? C_COUNT : collation ? C_COMMENT : C_COLLATION;
}

/* One column's cells (allocated; C_COUNT of them) */
static void column_cells(const LaceSchema *sc, size_t i, char *out[C_COUNT]) {
  const LaceColumn *c = &sc->columns[i];
  bool composite = false; /* a one-column key says PK, a composite one the column's position */
  for (size_t k = 0; c->primary_key && k < sc->num_columns; k++)
    if (k != i && sc->columns[k].primary_key) composite = true;
  char *extra = str_dup("");
  const char *bits[3] = {0};
  char *gen = NULL, *fk = NULL;
  if (c->identity == 1) bits[0] = "identity always";
  else if (c->identity == 2) bits[0] = "identity by default";
  else if (c->auto_increment) bits[0] = "auto-increment";
  if (c->generated) {
    gen = c->generated_expr && *c->generated_expr ? str_printf("generated: %s", c->generated_expr) : str_dup("generated");
    bits[1] = gen;
  }
  if (c->foreign_key) {
    fk = str_printf("\xe2\x86\x92 %s", c->foreign_key);
    bits[2] = fk;
  }
  for (int b = 0; b < 3; b++) {
    if (!bits[b]) continue;
    char *next = str_printf("%s%s%s", extra, *extra ? ", " : "", bits[b]);
    free(extra);
    extra = next;
  }
  out[C_NUM] = str_printf("%zu", i + 1);
  out[C_NAME] = str_dup(c->name ? c->name : "");
  out[C_TYPE] = str_dup(c->type_name ? c->type_name : "");
  out[C_NULL] = str_dup(c->nullable ? "NULL" : "NOT NULL");
  out[C_KEY] = !c->primary_key ? str_dup("")
               : composite && c->key_position > 0 ? str_printf("PK %d", c->key_position)
                                                  : str_dup("PK");
  out[C_DEFAULT] = str_dup(c->default_val ? c->default_val : "");
  out[C_EXTRA] = extra;
  out[C_COLLATION] = str_dup(c->collation ? c->collation : "");
  out[C_COMMENT] = str_dup(c->comment ? c->comment : "");
  free(gen);
  free(fk);
}

static void add_columns(CatalogRows *t, const LaceSchema *sc) {
  for (size_t i = 0; i < sc->num_columns; i++) {
    char *cells[C_COUNT];
    column_cells(sc, i, cells);
    add_cells(t, CATALOG_TAG_COLUMN + (int)i, (const char *const *)cells);
    for (size_t k = 0; k < C_COUNT; k++) free(cells[k]);
  }
}

void catalog_rows_structure(CatalogRows *t, const LaceSchema *sc) {
  catalog_rows_free(t);
  catalog_rows_init(t, structure_ncols(sc), STRUCT_HEAD);
  add_columns(t, sc);
}

/* ==========================================================================
 * Indexes
 * ========================================================================== */

static const char *index_kind(const LaceIndex *ix) {
  switch (ix->kind) {
  case LACE_INDEX_PRIMARY: return "PRIMARY";
  case LACE_INDEX_UNIQUE: return "UNIQUE";
  case LACE_INDEX_CHECK: return "CHECK";
  case LACE_INDEX_EXCLUSION: return "EXCLUSION";
  default: return ix->primary ? "PRIMARY" : ix->unique ? "UNIQUE" : "INDEX";
  }
}

bool catalog_rows_indexes(CatalogRows *t, const LaceSchema *sc) {
  catalog_rows_free(t);
  if (!sc->num_indexes) return false;
  bool any_where = false, any_method = false, any_constraint = false;
  for (size_t i = 0; i < sc->num_indexes; i++) {
    const LaceIndex *ix = &sc->indexes[i];
    if ((ix->where && *ix->where) || (ix->check && *ix->check)) any_where = true;
    if (ix->type && *ix->type) any_method = true;
    if (ix->constraint && *ix->constraint) any_constraint = true;
  }
  static const char *const HEAD[] = {"Name", "Kind", "Columns", "Condition", "Method", "Constraint"};
  catalog_rows_init(t, any_constraint ? 6 : any_method ? 5 : any_where ? 4 : 3, HEAD);
  for (size_t i = 0; i < sc->num_indexes; i++) {
    const LaceIndex *ix = &sc->indexes[i];
    char *cols = catalog_join(ix->columns, ix->num_columns);
    char *cond = ix->check && *ix->check ? str_dup(ix->check)
                 : ix->where && *ix->where ? str_printf("WHERE %s", ix->where)
                                           : str_dup("");
    catalog_rows_add(t, CATALOG_TAG_INDEX + (int)i, ix->name ? ix->name : "", index_kind(ix), cols, cond, ix->type ? ix->type : "",
                     ix->constraint ? ix->constraint : "");
    free(cols);
    free(cond);
  }
  return true;
}

/* ==========================================================================
 * Foreign keys
 * ========================================================================== */

void catalog_rows_fks(CatalogRows *t, const LaceSchema *sc, const char *table, const LaceDependent *deps,
                      size_t num_deps, bool deps_loaded, const char *deps_error) {
  catalog_rows_free(t);
  bool deferrable = false;
  for (size_t i = 0; i < sc->num_foreign_keys; i++)
    if (sc->foreign_keys[i].deferrable) deferrable = true;
  static const char *const HEAD[] = {"Name", "Columns", "References", "ON DELETE", "ON UPDATE", "Deferrable"};
  catalog_rows_init(t, deferrable ? 6 : 5, HEAD);
  if (!sc->num_foreign_keys) catalog_rows_span(t, CATALOG_TAG_NONE, "No foreign keys");
  for (size_t i = 0; i < sc->num_foreign_keys; i++) {
    const LaceForeignKey *fk = &sc->foreign_keys[i];
    char *cols = catalog_join(fk->columns, fk->num_columns);
    char *rcols = catalog_join(fk->ref_columns, fk->num_ref_columns);
    char *ref = str_printf("%s (%s)", fk->ref_table ? fk->ref_table : "", rcols);
    catalog_rows_add(t, CATALOG_TAG_FK + (int)i, fk->name ? fk->name : "", cols, ref, fk->on_delete ? fk->on_delete : "",
                     fk->on_update ? fk->on_update : "",
                     fk->deferrable ? (fk->initially_deferred ? "initially deferred" : "yes") : "");
    free(cols);
    free(rcols);
    free(ref);
  }
  catalog_rows_blank(t);
  catalog_rows_heading(t, "Referenced by");
  if (deps_error) {
    catalog_rows_span(t, CATALOG_TAG_NONE, deps_error);
  } else if (!deps_loaded) {
    catalog_rows_span(t, CATALOG_TAG_NONE, "Loading\xe2\x80\xa6");
  } else if (!num_deps) {
    catalog_rows_span(t, CATALOG_TAG_NONE, "No table references this one");
  } else {
    for (size_t i = 0; i < num_deps; i++) {
      const LaceDependent *d = &deps[i];
      char *cols = catalog_join(d->columns, d->num_columns);
      char *rcols = catalog_join(d->ref_columns, d->num_columns);
      char *ref = str_printf("%s (%s)", table ? table : "", rcols);
      char *tcols = str_printf("%s (%s)", d->table ? d->table : "", cols);
      catalog_rows_add(t, CATALOG_TAG_DEP + (int)i, d->name ? d->name : "", tcols, ref, d->on_delete ? d->on_delete : "", "",
                       "");
      free(cols);
      free(rcols);
      free(ref);
      free(tcols);
    }
  }
}

/* ==========================================================================
 * Definition
 * ========================================================================== */

void catalog_rows_definition(CatalogRows *t, const char *sql, int width, const LaceSchema *cols) {
  catalog_rows_free(t);
  bool columns = cols && cols->num_columns;
  catalog_rows_init(t, columns ? structure_ncols(cols) : 1, NULL);
  if (width < 20) width = 20;
  const char *s = sql ? sql : "";
  while (*s) { /* one span row per line; a wide line wraps at a space */
    const char *nl = strchr(s, '\n');
    size_t len = nl ? (size_t)(nl - s) : strlen(s);
    size_t pos = 0;
    do {
      size_t fit = utf8_fit(s + pos, len - pos, width, NULL);
      if (fit < len - pos) {
        size_t cut = fit;
        while (cut > fit / 3 && s[pos + cut] != ' ') cut--;
        if (cut > fit / 3) fit = cut + 1;
      }
      if (!fit) fit = len - pos;
      char *line = str_ndup(s + pos, fit);
      catalog_rows_span(t, CATALOG_TAG_NONE, line);
      free(line);
      pos += fit;
    } while (pos < len);
    s += len + (nl ? 1 : 0);
  }
  if (columns) {
    catalog_rows_blank(t);
    catalog_rows_heading(t, "Columns");
    catalog_rows_head_row(t, STRUCT_HEAD);
    add_columns(t, cols);
  }
}

/* ==========================================================================
 * Information
 * ========================================================================== */

static void add(CatalogRows *t, int tag, const char *name, const char *value) {
  if (!value) return;
  catalog_rows_add(t, tag, name, value);
}

static void add_owned(CatalogRows *t, int tag, const char *name, char *value) {
  add(t, tag, name, value);
  free(value);
}

void catalog_rows_info(CatalogRows *t, LaceObjectKind kind, const char *name, const char *object_table,
                       const char *object_detail, const LaceTableInfo *in, const LaceAutoInc *a,
                       const char *info_error, int64_t exact, bool counting, const LaceObject *objects,
                       size_t num_objects) {
  catalog_rows_free(t);
  catalog_rows_init(t, 2, NULL);
  add(t, CATALOG_TAG_NONE, catalog_object_word(kind), name);
  if (kind == LACE_OBJ_TRIGGER) {
    add(t, CATALOG_TAG_NONE, "On table", object_table);
    return;
  }
  if (kind == LACE_OBJ_ROUTINE) {
    add(t, CATALOG_TAG_NONE, "Kind", object_detail ? object_detail : "function");
    return;
  }
  if (object_detail) add(t, CATALOG_TAG_NONE, "Kind", object_detail);
  if (info_error) {
    add(t, CATALOG_TAG_NONE, "Information", info_error);
  } else if (!in) {
    add(t, CATALOG_TAG_NONE, "Information", "Loading\xe2\x80\xa6");
  } else {
    char *est = in->row_estimate >= 0 ? catalog_commas(in->row_estimate) : NULL;
    char *rows;
    if (counting) rows = str_dup("counting\xe2\x80\xa6");
    else if (exact >= 0) {
      char *e = catalog_commas(exact);
      rows = est ? str_printf("%s (the catalog estimates %s)", e, est) : str_dup(e);
      free(e);
    } else if (est) rows = str_printf("\xe2\x89\x88 %s (the catalog's estimate; Enter counts)", est);
    else rows = str_dup("unknown (Enter counts)");
    free(est);
    add_owned(t, CATALOG_TAG_ROWS, "Rows", rows);
    if (in->stats_updated) add(t, CATALOG_TAG_NONE, "Statistics updated", in->stats_updated);
    if (in->size_is_file) {
      add_owned(t, CATALOG_TAG_NONE, "File size", catalog_bytes(in->total_bytes));
    } else {
      if (in->data_bytes >= 0) add_owned(t, CATALOG_TAG_NONE, "Data size", catalog_bytes(in->data_bytes));
      if (in->index_bytes >= 0) add_owned(t, CATALOG_TAG_NONE, "Index size", catalog_bytes(in->index_bytes));
      if (in->total_bytes >= 0) add_owned(t, CATALOG_TAG_NONE, "Total size", catalog_bytes(in->total_bytes));
    }
    if (a && a->column) {
      char *next = a->next >= 0 ? catalog_commas(a->next) : str_dup("unknown");
      char *max = a->max >= 0 ? catalog_commas(a->max) : str_dup("none (empty)");
      char *text = a->has_counter ? str_printf("%s: next %s, maximum %s%s%s", a->column, next, max,
                                               a->sequence ? ", sequence " : "", a->sequence ? a->sequence : "")
                                  : str_printf("%s: no counter (the next rowid is the maximum + 1), maximum %s",
                                               a->column, max);
      add_owned(t, CATALOG_TAG_NONE, a->sequence ? "Identity" : "Auto-increment", text);
      free(next);
      free(max);
    }
    if (in->engine) add(t, CATALOG_TAG_NONE, "Engine", in->engine);
    if (in->charset) add(t, CATALOG_TAG_NONE, "Charset", in->charset);
    if (in->collation) add(t, CATALOG_TAG_NONE, "Collation", in->collation);
    if (in->comment && *in->comment) add(t, CATALOG_TAG_NONE, "Comment", in->comment);
    if (in->owner) add(t, CATALOG_TAG_NONE, "Owner", in->owner);
    if (in->schema) add(t, CATALOG_TAG_NONE, "Schema", in->schema);
    if (in->tablespace) add(t, CATALOG_TAG_NONE, "Tablespace", in->tablespace);
    if (in->created) add(t, CATALOG_TAG_NONE, "Created", in->created);
    if (in->updated) add(t, CATALOG_TAG_NONE, "Updated", in->updated);
    if (in->partitioned) add(t, CATALOG_TAG_NONE, "Partitioned", "yes");
  }
  bool any = false;
  for (size_t i = 0; name && i < num_objects; i++) {
    const LaceObject *o = &objects[i];
    if (o->kind != LACE_OBJ_TRIGGER || !o->table || strcmp(o->table, name) != 0) continue;
    if (!any) {
      catalog_rows_blank(t);
      catalog_rows_heading(t, "Triggers");
      any = true;
    }
    catalog_rows_add(t, CATALOG_TAG_TRIGGER + (int)i, o->name ? o->name : "", "Enter opens the trigger");
  }
}
