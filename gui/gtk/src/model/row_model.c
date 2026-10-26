/*
 * Lace GTK frontend
 * LaceRowModel and LaceRowItem
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "row_model.h"
#include <util/mem.h>
#include <util/str.h>
#include <stdlib.h>

/* Pages kept before the least recently used one is dropped */
#define ROW_MODEL_MAX_PAGES 8

/* GtkListView builds widgets for up to 200 rows at once, about 170 us per
 * cell, which blocks the main loop for half a second on a wide table. The
 * first rows are revealed in chunks over a few frames instead, so the
 * first ones paint at once; past REVEAL_UNTIL rows the view builds no more
 * widgets and the rest is revealed in one go. */
#define REVEAL_CHUNK 50
#define REVEAL_UNTIL 200
#define REVEAL_INTERVAL_MS 20

typedef struct {
  gsize page;
  LaceResult *result;
  guint64 last_used;
} Page;

struct _LaceRowModel {
  GObject parent_instance;
  gsize page_size;
  guint64 generation;
  guint64 use_counter;
  LaceRowModelFetch fetch;
  gpointer fetch_data;

  GHashTable *pages;   /* page index -> Page* */
  GHashTable *pending; /* page index -> TRUE */
  GHashTable *items;   /* position -> LaceRowItem* (weak) */

  LaceColumn *columns;
  gsize num_columns;

  gsize total;
  gboolean total_known;
  gboolean total_approximate;
  gsize seen;   /* Rows actually loaded: the floor of any count */
  gsize extent; /* Rows the view may scroll to while the total is unknown:
                   what was seen plus one page after a full page */
  gsize n_items; /* What the view sees */
  gsize reveal_target;
  guint reveal_source;
};

struct _LaceRowItem {
  GObject parent_instance;
  LaceRowModel *model; /* Weak: cleared when the model goes */
  gsize position;
};

enum { ITEM_CHANGED, N_ITEM_SIGNALS };
static guint item_signals[N_ITEM_SIGNALS];

enum { MODEL_COLUMNS_CHANGED, MODEL_TOTAL_CHANGED, N_MODEL_SIGNALS };
static guint model_signals[N_MODEL_SIGNALS];

/* ==========================================================================
 * Item
 * ========================================================================== */

G_DEFINE_TYPE(LaceRowItem, lace_row_item, G_TYPE_OBJECT)

static void lace_row_item_finalize(GObject *obj) {
  LaceRowItem *item = LACE_ROW_ITEM(obj);
  if (item->model && item->model->items)
    g_hash_table_remove(item->model->items, GSIZE_TO_POINTER(item->position));
  G_OBJECT_CLASS(lace_row_item_parent_class)->finalize(obj);
}

static void lace_row_item_class_init(LaceRowItemClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = lace_row_item_finalize;
  item_signals[ITEM_CHANGED] = g_signal_new("changed", LACE_TYPE_ROW_ITEM,
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void lace_row_item_init(LaceRowItem *item) { (void)item; }

gsize lace_row_item_get_position(LaceRowItem *item) { return item->position; }
LaceRowModel *lace_row_item_get_model(LaceRowItem *item) { return item->model; }

const LaceValue *lace_row_item_get_cell(LaceRowItem *item, gsize col) {
  if (!item->model)
    return NULL;
  return lace_row_model_get_cell(item->model, item->position, col);
}

gboolean lace_row_item_is_loaded(LaceRowItem *item) {
  return item->model && lace_row_model_row_loaded(item->model, item->position);
}

static void item_changed(LaceRowModel *m, gsize position) {
  LaceRowItem *item = g_hash_table_lookup(m->items, GSIZE_TO_POINTER(position));
  if (item)
    g_signal_emit(item, item_signals[ITEM_CHANGED], 0);
}

/* ==========================================================================
 * Model
 * ========================================================================== */

static void page_free(gpointer p) {
  Page *page = p;
  lace_result_free(page->result);
  g_free(page);
}

static GType model_get_item_type(GListModel *list) {
  (void)list;
  return LACE_TYPE_ROW_ITEM;
}

static guint model_get_n_items(GListModel *list) {
  return (guint)LACE_ROW_MODEL(list)->n_items;
}

static gpointer model_get_item(GListModel *list, guint position) {
  LaceRowModel *m = LACE_ROW_MODEL(list);
  if (position >= m->n_items)
    return NULL;
  LaceRowItem *item = g_hash_table_lookup(m->items, GSIZE_TO_POINTER((gsize)position));
  if (item)
    return g_object_ref(item);
  item = g_object_new(LACE_TYPE_ROW_ITEM, NULL);
  item->model = m;
  item->position = position;
  g_hash_table_insert(m->items, GSIZE_TO_POINTER((gsize)position), item);
  lace_row_model_request(m, position);
  return item;
}

static void model_iface_init(GListModelInterface *iface) {
  iface->get_item_type = model_get_item_type;
  iface->get_n_items = model_get_n_items;
  iface->get_item = model_get_item;
}

G_DEFINE_TYPE_WITH_CODE(LaceRowModel, lace_row_model, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, model_iface_init))

static void clear_columns(LaceRowModel *m) {
  for (gsize i = 0; i < m->num_columns; i++) {
    free(m->columns[i].name);
    free(m->columns[i].type_name);
    free(m->columns[i].default_val);
    free(m->columns[i].foreign_key);
  }
  free(m->columns);
  m->columns = NULL;
  m->num_columns = 0;
}

static void detach_items(LaceRowModel *m) {
  GHashTableIter it;
  gpointer key, value;
  g_hash_table_iter_init(&it, m->items);
  while (g_hash_table_iter_next(&it, &key, &value))
    ((LaceRowItem *)value)->model = NULL;
  g_hash_table_remove_all(m->items);
}

static void lace_row_model_dispose(GObject *obj) {
  LaceRowModel *m = LACE_ROW_MODEL(obj);
  if (m->reveal_source) {
    g_source_remove(m->reveal_source);
    m->reveal_source = 0;
  }
  if (m->items) {
    detach_items(m);
    g_hash_table_unref(m->items);
    m->items = NULL;
  }
  g_clear_pointer(&m->pages, g_hash_table_unref);
  g_clear_pointer(&m->pending, g_hash_table_unref);
  clear_columns(m);
  G_OBJECT_CLASS(lace_row_model_parent_class)->dispose(obj);
}

static void lace_row_model_class_init(LaceRowModelClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = lace_row_model_dispose;
  model_signals[MODEL_COLUMNS_CHANGED] = g_signal_new("columns-changed",
      LACE_TYPE_ROW_MODEL, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  model_signals[MODEL_TOTAL_CHANGED] = g_signal_new("total-changed",
      LACE_TYPE_ROW_MODEL, G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void lace_row_model_init(LaceRowModel *m) {
  m->page_size = 1000;
  m->pages = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, page_free);
  m->pending = g_hash_table_new(g_direct_hash, g_direct_equal);
  m->items = g_hash_table_new(g_direct_hash, g_direct_equal);
}

LaceRowModel *lace_row_model_new(gsize page_size) {
  LaceRowModel *m = g_object_new(LACE_TYPE_ROW_MODEL, NULL);
  if (page_size > 0)
    m->page_size = page_size;
  return m;
}

gsize lace_row_model_get_page_size(LaceRowModel *m) { return m->page_size; }

void lace_row_model_set_page_size(LaceRowModel *m, gsize page_size) {
  if (page_size > 0 && g_hash_table_size(m->pages) == 0)
    m->page_size = page_size;
}

void lace_row_model_set_fetch(LaceRowModel *m, LaceRowModelFetch fetch,
                              gpointer user_data) {
  m->fetch = fetch;
  m->fetch_data = user_data;
}

static gboolean reveal_step(gpointer data);
static void set_n_items(LaceRowModel *m, gsize n);

/* Set the visible count and tell the view about the difference */
static void apply_n_items(LaceRowModel *m, gsize n) {
  gsize old = m->n_items;
  if (n == old)
    return;
  m->n_items = n;
  if (n > old) {
    g_list_model_items_changed(G_LIST_MODEL(m), (guint)old, 0, (guint)(n - old));
  } else {
    /* Items past the end are gone */
    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, m->items);
    while (g_hash_table_iter_next(&it, &key, &value)) {
      if (GPOINTER_TO_SIZE(key) >= n) {
        ((LaceRowItem *)value)->model = NULL;
        g_hash_table_iter_remove(&it);
      }
    }
    g_list_model_items_changed(G_LIST_MODEL(m), (guint)n, (guint)(old - n), 0);
  }
}

void lace_row_model_invalidate(LaceRowModel *m);

static gboolean reveal_step(gpointer data) {
  LaceRowModel *m = data;
  m->reveal_source = 0;
  set_n_items(m, m->reveal_target);
  return G_SOURCE_REMOVE;
}

static void set_n_items(LaceRowModel *m, gsize n) {
  m->reveal_target = n;
  gsize old = m->n_items;
  if (n > old && old < REVEAL_UNTIL && n - old > REVEAL_CHUNK) {
    gsize step = old + REVEAL_CHUNK;
    if (step > n)
      step = n;
    apply_n_items(m, step);
    if (!m->reveal_source)
      m->reveal_source = g_timeout_add_full(G_PRIORITY_DEFAULT_IDLE, REVEAL_INTERVAL_MS, reveal_step, m, NULL);
    return;
  }
  if (m->reveal_source) {
    g_source_remove(m->reveal_source);
    m->reveal_source = 0;
  }
  apply_n_items(m, n);
}

static void recompute_n_items(LaceRowModel *m) {
  set_n_items(m, m->total_known ? m->total : m->extent);
  g_signal_emit(m, model_signals[MODEL_TOTAL_CHANGED], 0);
}

void lace_row_model_reset(LaceRowModel *m) {
  m->generation++;
  if (m->reveal_source) {
    g_source_remove(m->reveal_source);
    m->reveal_source = 0;
  }
  g_hash_table_remove_all(m->pages);
  g_hash_table_remove_all(m->pending);
  clear_columns(m);
  m->total = 0;
  m->total_known = FALSE;
  m->total_approximate = FALSE;
  m->seen = 0;
  m->extent = 0;
  /* Every item is stale: drop them all and start over */
  gsize old = m->n_items;
  detach_items(m);
  m->n_items = 0;
  if (old > 0)
    g_list_model_items_changed(G_LIST_MODEL(m), 0, (guint)old, 0);
  g_signal_emit(m, model_signals[MODEL_COLUMNS_CHANGED], 0);
  g_signal_emit(m, model_signals[MODEL_TOTAL_CHANGED], 0);
}

guint64 lace_row_model_get_generation(LaceRowModel *m) { return m->generation; }

gsize lace_row_model_n_columns(LaceRowModel *m) { return m->num_columns; }

const LaceColumn *lace_row_model_get_column(LaceRowModel *m, gsize col) {
  return col < m->num_columns ? &m->columns[col] : NULL;
}

void lace_row_model_set_total(LaceRowModel *m, gsize total, gboolean known,
                              gboolean approximate) {
  if (known) {
    /* What was seen is the floor of any estimate. Not the extent: that
     * holds a page of room to scroll into while the count runs, and a
     * count below it (a table of 1500 rows after a full first page of
     * 1000) is right, the room is not. */
    if (total < m->seen)
      total = m->seen;
    m->total = total;
    m->total_known = TRUE;
    m->total_approximate = approximate;
    if (m->extent > m->total)
      m->extent = m->total;
  } else {
    m->total_known = FALSE;
    m->total_approximate = FALSE;
  }
  recompute_n_items(m);
}

gsize lace_row_model_get_total(LaceRowModel *m) {
  return m->total_known ? m->total : m->extent;
}

gboolean lace_row_model_total_known(LaceRowModel *m) { return m->total_known; }
gboolean lace_row_model_total_approximate(LaceRowModel *m) { return m->total_approximate; }
gsize lace_row_model_get_extent(LaceRowModel *m) { return m->extent; }

static void copy_columns(LaceRowModel *m, const LaceResult *r) {
  clear_columns(m);
  if (!r || r->num_columns == 0)
    return;
  m->columns = safe_calloc(r->num_columns, sizeof(LaceColumn));
  m->num_columns = r->num_columns;
  for (gsize i = 0; i < r->num_columns; i++) {
    const LaceColumn *c = &r->columns[i];
    m->columns[i].name = str_dup(c->name ? c->name : "");
    m->columns[i].type = c->type;
    m->columns[i].type_name = c->type_name ? str_dup(c->type_name) : NULL;
    m->columns[i].nullable = c->nullable;
    m->columns[i].primary_key = c->primary_key;
    m->columns[i].auto_increment = c->auto_increment;
    m->columns[i].generated = c->generated;
    m->columns[i].default_val = c->default_val ? str_dup(c->default_val) : NULL;
    m->columns[i].foreign_key = c->foreign_key ? str_dup(c->foreign_key) : NULL;
    m->columns[i].max_length = c->max_length;
  }
}

static void evict_if_needed(LaceRowModel *m) {
  while (g_hash_table_size(m->pages) > ROW_MODEL_MAX_PAGES) {
    GHashTableIter it;
    gpointer key, value;
    Page *oldest = NULL;
    g_hash_table_iter_init(&it, m->pages);
    while (g_hash_table_iter_next(&it, &key, &value)) {
      Page *p = value;
      if (!oldest || p->last_used < oldest->last_used)
        oldest = p;
    }
    if (!oldest)
      break;
    gsize start = oldest->page * m->page_size;
    gsize page_idx = oldest->page;
    g_hash_table_remove(m->pages, GSIZE_TO_POINTER(page_idx));
    /* Items on that page show placeholders again */
    for (gsize r = start; r < start + m->page_size && r < m->n_items; r++)
      item_changed(m, r);
  }
}

void lace_row_model_page_loaded(LaceRowModel *m, guint64 generation,
                                gsize page, LaceResult *result) {
  if (generation != m->generation || !result) {
    lace_result_free(result);
    return;
  }
  g_hash_table_remove(m->pending, GSIZE_TO_POINTER(page));
  if (m->num_columns == 0 && result->num_columns > 0) {
    copy_columns(m, result);
    g_signal_emit(m, model_signals[MODEL_COLUMNS_CHANGED], 0);
  }

  Page *p = g_new0(Page, 1);
  p->page = page;
  p->result = result;
  p->last_used = ++m->use_counter;
  g_hash_table_replace(m->pages, GSIZE_TO_POINTER(page), p);

  gsize start = page * m->page_size;
  gsize end = start + result->num_rows;
  if (end > m->seen)
    m->seen = end;
  if (end > m->extent)
    m->extent = end;
  if (result->num_rows < m->page_size) {
    /* A short page: the table ends here, whatever a count said */
    m->total = end;
    m->total_known = TRUE;
    m->total_approximate = FALSE;
  } else if (!m->total_known) {
    /* Full page while counting: let the view scroll one page further */
    m->extent = end + m->page_size;
  } else if (m->total_approximate && end >= m->total) {
    /* An estimate that undercounted: there is more */
    m->total = end + m->page_size;
  }
  if (m->total_known && m->extent > m->total)
    m->extent = m->total;
  recompute_n_items(m);
  evict_if_needed(m);
  for (gsize r = start; r < end && r < m->n_items; r++)
    item_changed(m, r);
}

void lace_row_model_page_failed(LaceRowModel *m, guint64 generation, gsize page) {
  if (generation != m->generation)
    return;
  g_hash_table_remove(m->pending, GSIZE_TO_POINTER(page));
}

void lace_row_model_request(LaceRowModel *m, gsize row) {
  gsize page = row / m->page_size;
  if (g_hash_table_contains(m->pages, GSIZE_TO_POINTER(page)) ||
      g_hash_table_contains(m->pending, GSIZE_TO_POINTER(page)))
    return;
  if (!m->fetch)
    return;
  g_hash_table_insert(m->pending, GSIZE_TO_POINTER(page), GINT_TO_POINTER(1));
  m->fetch(m, page, m->fetch_data);
}

void lace_row_model_mark_pending(LaceRowModel *m, gsize page) {
  if (!g_hash_table_contains(m->pages, GSIZE_TO_POINTER(page)))
    g_hash_table_insert(m->pending, GSIZE_TO_POINTER(page), GINT_TO_POINTER(1));
}

gboolean lace_row_model_page_loaded_p(LaceRowModel *m, gsize page) {
  return g_hash_table_contains(m->pages, GSIZE_TO_POINTER(page));
}

gboolean lace_row_model_page_pending(LaceRowModel *m, gsize page) {
  return g_hash_table_contains(m->pending, GSIZE_TO_POINTER(page));
}

LaceResult *lace_row_model_row_result(LaceRowModel *m, gsize row, gsize *in_page) {
  gsize page = row / m->page_size;
  Page *p = g_hash_table_lookup(m->pages, GSIZE_TO_POINTER(page));
  if (!p)
    return NULL;
  gsize idx = row - page * m->page_size;
  if (idx >= p->result->num_rows)
    return NULL;
  p->last_used = ++m->use_counter;
  if (in_page)
    *in_page = idx;
  return p->result;
}

gboolean lace_row_model_row_loaded(LaceRowModel *m, gsize row) {
  return lace_row_model_row_result(m, row, NULL) != NULL;
}

const LaceValue *lace_row_model_get_cell(LaceRowModel *m, gsize row, gsize col) {
  gsize idx;
  LaceResult *r = lace_row_model_row_result(m, row, &idx);
  if (!r || !r->rows || col >= r->rows[idx].num_cells)
    return NULL;
  return &r->rows[idx].cells[col];
}

void lace_row_model_set_cell(LaceRowModel *m, gsize row, gsize col, LaceValue value) {
  gsize idx;
  LaceResult *r = lace_row_model_row_result(m, row, &idx);
  if (!r || !r->rows || col >= r->rows[idx].num_cells) {
    lace_value_free(&value);
    return;
  }
  lace_value_free(&r->rows[idx].cells[col]);
  r->rows[idx].cells[col] = value;
  item_changed(m, row);
}

void lace_row_model_rows_removed(LaceRowModel *m, gsize from, gsize count) {
  (void)from; /* Rows from there on moved up: every page reloads */
  g_hash_table_remove_all(m->pages);
  g_hash_table_remove_all(m->pending);
  if (count > m->total)
    count = m->total;
  if (m->total_known)
    m->total -= count;
  m->seen = m->seen >= count ? m->seen - count : 0;
  if (m->extent >= count)
    m->extent -= count;
  else
    m->extent = 0;
  gsize n = m->n_items >= count ? m->n_items - count : 0;
  /* Items keep their positions: only the tail is gone from the view. The
   * rest show placeholders until their pages come back. */
  set_n_items(m, n);
  lace_row_model_invalidate(m);
}

void lace_row_model_invalidate(LaceRowModel *m) {
  g_hash_table_remove_all(m->pages);
  g_hash_table_remove_all(m->pending);
  /* Items re-request their pages when drawn; poke every live one */
  GList *keys = g_hash_table_get_keys(m->items);
  for (GList *l = keys; l; l = l->next) {
    gsize pos = GPOINTER_TO_SIZE(l->data);
    lace_row_model_request(m, pos);
    item_changed(m, pos);
  }
  g_list_free(keys);
}
