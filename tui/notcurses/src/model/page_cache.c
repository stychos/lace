#include "model/page_cache.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdlib.h>
#include <string.h>

#define PC_SLOTS 8

typedef struct {
  bool used;
  size_t page;
  LaceResult *rows;
  uint64_t tick;
  bool stale; /* from before a soft reset: shown until its page is fetched again */
} Slot;

struct PageCache {
  size_t page_size;
  uint64_t gen, tick;
  Slot slots[PC_SLOTS];
  size_t *pending;
  size_t num_pending, cap_pending;
  size_t total;
  bool total_known, approximate;
  size_t highest_loaded_end;
  bool highest_full;
  LaceColumn *columns;
  size_t num_columns;
  PageCacheFetch fetch;
  void *fetch_data;
  bool stale_active;       /* a soft reset is waiting for its first page */
  size_t kept_total;       /* the totals of before it, for page_cache_unstale() */
  bool kept_known, kept_approx;
};

static void columns_free(PageCache *pc) {
  for (size_t i = 0; i < pc->num_columns; i++) {
    free(pc->columns[i].name);
    free(pc->columns[i].type_name);
    free(pc->columns[i].default_val);
    free(pc->columns[i].foreign_key);
  }
  free(pc->columns);
  pc->columns = NULL;
  pc->num_columns = 0;
}

static void columns_copy(PageCache *pc, const LaceResult *r) {
  if (r->num_columns == 0 || !r->columns) return; /* keep the previous columns */
  columns_free(pc);
  pc->columns = safe_calloc(r->num_columns, sizeof(LaceColumn));
  pc->num_columns = r->num_columns;
  for (size_t i = 0; i < r->num_columns; i++) {
    const LaceColumn *c = &r->columns[i];
    LaceColumn *d = &pc->columns[i];
    *d = *c;
    d->name = str_dup(c->name ? c->name : "");
    d->type_name = c->type_name ? str_dup(c->type_name) : NULL;
    d->default_val = c->default_val ? str_dup(c->default_val) : NULL;
    d->foreign_key = c->foreign_key ? str_dup(c->foreign_key) : NULL;
  }
}

static bool columns_same(const PageCache *pc, const LaceResult *r) {
  if (pc->num_columns != r->num_columns) return false;
  if (r->num_columns > 0 && !r->columns) return false;
  for (size_t i = 0; i < r->num_columns; i++) {
    const char *n = r->columns[i].name ? r->columns[i].name : "";
    if (strcmp(pc->columns[i].name, n) != 0) return false;
  }
  return true;
}

PageCache *page_cache_new(size_t page_size) {
  PageCache *pc = safe_calloc(1, sizeof *pc);
  pc->page_size = page_size ? page_size : 1;
  pc->gen = 1;
  return pc;
}

static void slot_clear(Slot *s) {
  if (s->used) lace_result_free(s->rows);
  memset(s, 0, sizeof *s);
}

static void pending_clear(PageCache *pc) { pc->num_pending = 0; }
static void highest_recompute(PageCache *pc);

void page_cache_free(PageCache *pc) {
  if (!pc) return;
  for (int i = 0; i < PC_SLOTS; i++) slot_clear(&pc->slots[i]);
  free(pc->pending);
  columns_free(pc);
  free(pc);
}

void page_cache_set_fetch(PageCache *pc, PageCacheFetch fn, void *data) {
  pc->fetch = fn;
  pc->fetch_data = data;
}

void page_cache_reset(PageCache *pc) {
  pc->stale_active = false;
  pc->gen++;
  for (int i = 0; i < PC_SLOTS; i++) slot_clear(&pc->slots[i]);
  pending_clear(pc);
  pc->total = 0;
  pc->total_known = pc->approximate = false;
  pc->highest_loaded_end = 0;
  pc->highest_full = false;
}

void page_cache_clear(PageCache *pc, size_t page_size) {
  page_cache_reset(pc);
  columns_free(pc);
  if (page_size) pc->page_size = page_size;
}

void page_cache_reset_soft(PageCache *pc) {
  if (!pc->stale_active) {
    pc->kept_total = pc->total;
    pc->kept_known = pc->total_known;
    pc->kept_approx = pc->approximate;
  }
  pc->stale_active = true;
  pc->gen++;
  for (int i = 0; i < PC_SLOTS; i++)
    if (pc->slots[i].used) pc->slots[i].stale = true;
  pending_clear(pc);
}

static void drop_stale(PageCache *pc) {
  for (int i = 0; i < PC_SLOTS; i++)
    if (pc->slots[i].used && pc->slots[i].stale) slot_clear(&pc->slots[i]);
  pc->stale_active = false;
}

bool page_cache_stale(const PageCache *pc) { return pc->stale_active; }

void page_cache_unstale(PageCache *pc) {
  if (!pc->stale_active) return;
  pc->gen++; /* whatever is still in flight belongs to the state given up */
  pending_clear(pc);
  for (int i = 0; i < PC_SLOTS; i++) pc->slots[i].stale = false;
  pc->stale_active = false;
  pc->total = pc->kept_total;
  pc->total_known = pc->kept_known;
  pc->approximate = pc->kept_approx;
  highest_recompute(pc);
}

uint64_t page_cache_generation(const PageCache *pc) { return pc->gen; }
size_t page_cache_page_size(const PageCache *pc) { return pc->page_size; }

void page_cache_set_total(PageCache *pc, size_t total, bool known, bool approximate) {
  pc->total = total;
  pc->total_known = known;
  pc->approximate = known && approximate;
}

size_t page_cache_total(const PageCache *pc) { return pc->total; }
bool page_cache_total_known(const PageCache *pc) { return pc->total_known; }
bool page_cache_total_approximate(const PageCache *pc) { return pc->approximate; }

size_t page_cache_extent(const PageCache *pc) {
  if (pc->total_known) return pc->total;
  return pc->highest_loaded_end + (pc->highest_full ? 1 : 0);
}

static Slot *slot_for(const PageCache *cpc, size_t page, bool bump) {
  PageCache *pc = (PageCache *)cpc;
  for (int i = 0; i < PC_SLOTS; i++) {
    if (pc->slots[i].used && pc->slots[i].page == page) {
      if (bump) pc->slots[i].tick = ++pc->tick;
      return &pc->slots[i];
    }
  }
  return NULL;
}

static bool fresh_slot(const PageCache *pc, size_t page) {
  Slot *s = slot_for(pc, page, true);
  return s && !s->stale;
}

static ptrdiff_t pending_find(const PageCache *pc, size_t page) {
  for (size_t i = 0; i < pc->num_pending; i++)
    if (pc->pending[i] == page) return (ptrdiff_t)i;
  return -1;
}

static void pending_remove(PageCache *pc, size_t page) {
  ptrdiff_t i = pending_find(pc, page);
  if (i < 0) return;
  pc->pending[i] = pc->pending[--pc->num_pending];
}

static void highest_recompute(PageCache *pc) {
  pc->highest_loaded_end = 0;
  pc->highest_full = false;
  for (int i = 0; i < PC_SLOTS; i++) {
    const Slot *s = &pc->slots[i];
    if (!s->used) continue;
    size_t end = s->page * pc->page_size + s->rows->num_rows;
    if (end > pc->highest_loaded_end || (end == pc->highest_loaded_end && !pc->highest_full)) {
      pc->highest_loaded_end = end;
      pc->highest_full = s->rows->num_rows >= pc->page_size;
    }
  }
}

void page_cache_page_loaded(PageCache *pc, uint64_t gen, size_t page, LaceResult *rows) {
  if (gen != pc->gen) {
    if (rows) lace_result_free(rows);
    return;
  }
  if (!rows) {
    pending_remove(pc, page);
    return;
  }
  pending_remove(pc, page);
  if (pc->num_columns == 0 || !columns_same(pc, rows)) columns_copy(pc, rows);

  if (pc->stale_active) { /* the first page of the new state: the old rows and total are gone */
    drop_stale(pc);
    pc->total_known = pc->approximate = false;
    pc->total = 0;
  }
  Slot *s = slot_for(pc, page, false);
  if (!s) {
    for (int i = 0; i < PC_SLOTS; i++)
      if (!pc->slots[i].used) { s = &pc->slots[i]; break; }
  }
  if (!s) {
    s = &pc->slots[0];
    for (int i = 1; i < PC_SLOTS; i++)
      if (pc->slots[i].tick < s->tick) s = &pc->slots[i];
  }
  slot_clear(s);
  s->used = true;
  s->page = page;
  s->rows = rows;
  s->tick = ++pc->tick;

  if (rows->num_rows < pc->page_size && !pc->total_known) {
    pc->total = page * pc->page_size + rows->num_rows;
    pc->total_known = true;
    pc->approximate = false;
  }
  highest_recompute(pc);
}

void page_cache_page_failed(PageCache *pc, uint64_t gen, size_t page) {
  if (gen != pc->gen) return;
  pending_remove(pc, page);
}

bool page_cache_page_loaded_p(const PageCache *pc, size_t page) {
  const Slot *s = slot_for(pc, page, false);
  return s && !s->stale;
}
bool page_cache_page_pending(const PageCache *pc, size_t page) { return pending_find(pc, page) >= 0; }

void page_cache_request(PageCache *pc, size_t row) {
  size_t page = row / pc->page_size;
  if (pc->total_known && row >= pc->total) return;
  if (fresh_slot(pc, page) || pending_find(pc, page) >= 0) return;
  if (pc->num_pending == pc->cap_pending) {
    pc->cap_pending = pc->cap_pending ? pc->cap_pending * 2 : 8;
    pc->pending = safe_realloc(pc->pending, pc->cap_pending * sizeof *pc->pending);
  }
  pc->pending[pc->num_pending++] = page;
  if (pc->fetch) pc->fetch(pc, page, pc->fetch_data);
}

bool page_cache_row_loaded(const PageCache *pc, size_t row) {
  const Slot *s = slot_for(pc, row / pc->page_size, false);
  return s && row % pc->page_size < s->rows->num_rows;
}

const LaceResult *page_cache_row_result(const PageCache *pc, size_t row, size_t *in_page) {
  Slot *s = slot_for(pc, row / pc->page_size, true);
  size_t i = row % pc->page_size;
  if (!s || i >= s->rows->num_rows) return NULL;
  if (in_page) *in_page = i;
  return s->rows;
}

const LaceValue *page_cache_cell(const PageCache *pc, size_t row, size_t col) {
  size_t i;
  const LaceResult *r = page_cache_row_result(pc, row, &i);
  if (!r || !r->rows) return NULL;
  const LaceRow *lr = &r->rows[i];
  if (!lr->cells || col >= lr->num_cells) return NULL;
  return &lr->cells[col];
}

void page_cache_set_cell(PageCache *pc, size_t row, size_t col, LaceValue v) {
  const LaceValue *cur = page_cache_cell(pc, row, col);
  if (!cur) {
    lace_value_free(&v);
    return;
  }
  LaceValue *dst = (LaceValue *)cur;
  lace_value_free(dst);
  *dst = v;
}

void page_cache_rows_removed(PageCache *pc, size_t from, size_t count) {
  size_t first = from / pc->page_size;
  for (int i = 0; i < PC_SLOTS; i++)
    if (pc->slots[i].used && pc->slots[i].page >= first) slot_clear(&pc->slots[i]);
  pending_clear(pc); /* every fetch in flight is stale now, as in reset */
  pc->gen++; /* fetches in flight predate the removal */
  if (pc->total_known) pc->total = count >= pc->total ? 0 : pc->total - count;
  highest_recompute(pc);
}

size_t page_cache_num_columns(const PageCache *pc) { return pc->num_columns; }
const LaceColumn *page_cache_column(const PageCache *pc, size_t col) {
  return col < pc->num_columns ? &pc->columns[col] : NULL;
}
