/*
 * Lace
 * Sort by column: see sort_picker.h
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "sort_picker.h"
#include "list_draw.h"
#include "overlay.h"
#include "../tui_internal.h"
#include "../../../liblace/include/util/mem.h"
#include "../../../liblace/include/util/str.h"
#include <stdint.h>
#include <stdlib.h>

typedef struct {
  TuiState *state;
  Tab *tab;
  ListWidget *list;
  bool changed;
} Picker;

static void set_details(Picker *p) {
  Tab *tab = p->tab;
  for (size_t i = 0; i < p->list->num_rows; i++) {
    ListRow *r = &p->list->rows[i];
    free(r->detail);
    r->detail = NULL;
    size_t col = (size_t)(intptr_t)r->data;
    for (size_t k = 0; k < tab->num_sort_entries; k++) {
      if (tab->sort_entries[k].column != col)
        continue;
      const char *arrow = tab->sort_entries[k].direction == SORT_DESC ? "\xe2\x86\x93" : "\xe2\x86\x91";
      r->detail = tab->num_sort_entries > 1 ? str_printf("%s%zu", arrow, k + 1) : str_dup(arrow);
      break;
    }
  }
}

static bool cycle(ListRow *r, void *ctx) {
  Picker *p = ctx;
  if (tab_cycle_sort(p->tab, (size_t)(intptr_t)r->data))
    p->changed = true;
  else
    tui_set_status(p->state, "At most %d sort columns", MAX_SORT_COLUMNS);
  set_details(p);
  return false; /* stays open: Esc applies */
}

void tui_sort_picker_open(TuiState *state) {
  Tab *tab = TUI_TAB(state);
  if (!tab || tab->type != TAB_TYPE_TABLE || !tab_has_subview(tab, SUBVIEW_DATA))
    return;
  if (!tab->schema || !tab->schema->num_columns) {
    tui_set_status(state, "The table is still loading");
    return;
  }
  size_t n = tab->schema->num_columns;
  ListRow *rows = safe_calloc(n, sizeof *rows);
  for (size_t i = 0; i < n; i++)
    rows[i] = (ListRow){str_dup(tab->schema->columns[i].name ? tab->schema->columns[i].name : ""), NULL, NULL, 0,
                        (void *)(intptr_t)i, false};
  ListWidget list;
  list_init(&list, false);
  list_set_rows(&list, rows, n);
  list.detail_cols = 4;
  Picker p = {state, tab, &list, false};
  set_details(&p);

  int sr, sc;
  getmaxyx(stdscr, sr, sc);
  int h = (int)n < sr - 8 ? (int)n : sr - 8;
  TuiOverlay o;
  if (tui_overlay_open(&o, "Sort by column", "Enter cycle \xe2\x86\x91/\xe2\x86\x93/off  Esc apply", h,
                       sc - 8 < 44 ? sc - 8 : 44, -1, -1)) {
    tui_list_overlay_run(&o, &list, cycle, &p);
    tui_overlay_close(&o);
  }
  list_free(&list);
  tui_recreate_windows(state);
  if (p.changed)
    tui_refresh_table(state);
  tui_refresh(state);
}
