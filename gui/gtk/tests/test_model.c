/*
 * Lace GTK frontend
 * Unit tests: the row model (GLib only, no display)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "test.h"
#include "model/row_model.h"
#include <stdlib.h>

static int g_fetched[64];
static int g_fetch_count;

static void fetch(LaceRowModel *m, gsize page, gpointer data) {
  (void)m;
  (void)data;
  if (page < 64)
    g_fetched[page]++;
  g_fetch_count++;
}

static LaceResult *page_result(gsize start, gsize n) {
  LaceResult *r = calloc(1, sizeof(*r));
  r->num_columns = 2;
  r->columns = calloc(2, sizeof(LaceColumn));
  r->columns[0].name = strdup("id");
  r->columns[0].type = LACE_TYPE_INT;
  r->columns[1].name = strdup("name");
  r->columns[1].type = LACE_TYPE_TEXT;
  r->num_rows = n;
  r->rows = calloc(n ? n : 1, sizeof(LaceRow));
  for (gsize i = 0; i < n; i++) {
    r->rows[i].num_cells = 2;
    r->rows[i].cells = calloc(2, sizeof(LaceValue));
    r->rows[i].cells[0] = lace_value_int((int64_t)(start + i));
    r->rows[i].cells[1] = lace_value_text("x");
  }
  return r;
}

static int g_changes;
static guint g_last_pos, g_last_removed, g_last_added;
static void on_items_changed(GListModel *m, guint pos, guint removed, guint added, gpointer d) {
  (void)m;
  (void)d;
  g_changes++;
  g_last_pos = pos;
  g_last_removed = removed;
  g_last_added = added;
}

/* Let the model's chunked reveal finish */
static void settle(void) {
  for (int i = 0; i < 40; i++) {
    while (g_main_context_iteration(NULL, FALSE))
      ;
    g_usleep(5000);
  }
}

static int g_item_changed;
static void on_item_changed(LaceRowItem *item, gpointer d) {
  (void)item;
  (void)d;
  g_item_changed++;
}

int main(void) {
  LaceRowModel *m = lace_row_model_new(10);
  lace_row_model_set_fetch(m, fetch, NULL);
  g_signal_connect(m, "items-changed", G_CALLBACK(on_items_changed), NULL);

  CHECK(g_list_model_get_n_items(G_LIST_MODEL(m)) == 0);
  guint64 gen = lace_row_model_get_generation(m);

  /* First page requested explicitly, then lands full: one page shown plus
   * one page of room to scroll */
  lace_row_model_request(m, 0);
  CHECK(g_fetched[0] == 1);
  CHECK(lace_row_model_page_pending(m, 0));
  lace_row_model_request(m, 3); /* same page: no second fetch */
  CHECK(g_fetched[0] == 1);
  lace_row_model_page_loaded(m, gen, 0, page_result(0, 10));
  CHECK(lace_row_model_n_columns(m) == 2);
  CHECK_STR(lace_row_model_get_column(m, 1)->name, "name");
  CHECK(g_list_model_get_n_items(G_LIST_MODEL(m)) == 20);
  CHECK(!lace_row_model_total_known(m));
  CHECK(g_changes == 1 && g_last_pos == 0 && g_last_added == 20);

  /* Getting an item past the loaded page fetches its page */
  LaceRowItem *item = g_list_model_get_item(G_LIST_MODEL(m), 15);
  CHECK(item != NULL);
  CHECK(!lace_row_item_is_loaded(item));
  CHECK(g_fetched[1] == 1);
  g_signal_connect(item, "changed", G_CALLBACK(on_item_changed), NULL);
  /* The same item object comes back for the same position */
  LaceRowItem *again = g_list_model_get_item(G_LIST_MODEL(m), 15);
  CHECK(again == item);
  g_object_unref(again);

  /* A short page makes the total known and notifies the item */
  lace_row_model_page_loaded(m, gen, 1, page_result(10, 3));
  CHECK(lace_row_model_total_known(m));
  CHECK(lace_row_model_get_total(m) == 13);
  CHECK(g_list_model_get_n_items(G_LIST_MODEL(m)) == 13);
  CHECK(lace_row_item_is_loaded(item) == FALSE); /* position 15 is past the end now */
  const LaceValue *v = lace_row_model_get_cell(m, 12, 0);
  CHECK(v && v->int_val == 12);
  g_object_unref(item);

  /* Cell replacement notifies the row's item */
  item = g_list_model_get_item(G_LIST_MODEL(m), 12);
  g_signal_connect(item, "changed", G_CALLBACK(on_item_changed), NULL);
  g_item_changed = 0;
  lace_row_model_set_cell(m, 12, 1, lace_value_text("y"));
  CHECK(g_item_changed == 1);
  v = lace_row_item_get_cell(item, 1);
  CHECK(v && strcmp(v->text.data, "y") == 0);
  g_object_unref(item);

  /* A page for an older generation is dropped */
  lace_row_model_reset(m);
  CHECK(g_list_model_get_n_items(G_LIST_MODEL(m)) == 0);
  CHECK(lace_row_model_n_columns(m) == 0);
  lace_row_model_page_loaded(m, gen, 0, page_result(0, 10));
  CHECK(g_list_model_get_n_items(G_LIST_MODEL(m)) == 0);
  CHECK(!lace_row_model_page_loaded_p(m, 0));

  /* A known total from a count caps and extends the view */
  gen = lace_row_model_get_generation(m);
  lace_row_model_request(m, 0);
  lace_row_model_page_loaded(m, gen, 0, page_result(0, 10));
  lace_row_model_set_total(m, 1000, TRUE, TRUE);
  /* Revealed in chunks: 50 at once, the rest over the next frames */
  CHECK(g_list_model_get_n_items(G_LIST_MODEL(m)) == 70);
  settle();
  CHECK(g_list_model_get_n_items(G_LIST_MODEL(m)) == 1000);
  CHECK(lace_row_model_total_approximate(m));
  /* An estimate below what was seen is raised to the extent */
  lace_row_model_page_loaded(m, gen, 50, page_result(500, 10));
  lace_row_model_set_total(m, 100, TRUE, TRUE);
  CHECK(lace_row_model_get_total(m) == 510);

  /* LRU: loading many pages evicts the least recently used one */
  for (gsize p = 1; p < 40; p++)
    lace_row_model_page_loaded(m, gen, p, page_result(p * 10, 10));
  CHECK(!lace_row_model_page_loaded_p(m, 0));
  CHECK(lace_row_model_page_loaded_p(m, 39));

  /* Deleting rows drops pages and shrinks the model */
  g_changes = 0;
  lace_row_model_rows_removed(m, 5, 2);
  CHECK(g_changes == 1 && g_last_pos == 508 && g_last_removed == 2 && g_last_added == 0);
  CHECK(lace_row_model_get_total(m) == 508);
  CHECK(!lace_row_model_page_loaded_p(m, 39));

  /* Invalidate keeps the total but reloads pages */
  g_fetch_count = 0;
  item = g_list_model_get_item(G_LIST_MODEL(m), 1);
  lace_row_model_page_loaded(m, gen, 0, page_result(0, 10));
  CHECK(lace_row_item_is_loaded(item));
  lace_row_model_invalidate(m);
  CHECK(!lace_row_item_is_loaded(item));
  CHECK(lace_row_model_page_pending(m, 0));
  CHECK(g_fetch_count == 2); /* get_item(1) and the invalidate */
  CHECK(lace_row_model_get_total(m) == 508);
  g_object_unref(item);

  g_object_unref(m);
  TEST_MAIN_END();
}
