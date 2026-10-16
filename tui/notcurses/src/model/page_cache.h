/* Sparse OFFSET page cache: the backend-free model a grid draws from. Pages
 * of page_size rows, 8 LRU slots, a generation per reset (pages that land for
 * an older one are freed), and a fetch callback the UI wires to a job. */
#ifndef NC_PAGE_CACHE_H
#define NC_PAGE_CACHE_H

#include "types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Lifetime and threads: pointers returned by page_cache_row_result,
 * page_cache_cell and page_cache_column are valid only until the next
 * mutating call (page_loaded, reset, rows_removed, set_cell for that cell's
 * contents, or a column change). Reads bump the LRU tick even though they take
 * a const cache, so the cache belongs to one thread (the main loop). */
typedef struct PageCache PageCache;
typedef void (*PageCacheFetch)(PageCache *pc, size_t page, void *data);

PageCache *page_cache_new(size_t page_size); /* 8 slots */
void page_cache_free(PageCache *pc);
void page_cache_set_fetch(PageCache *pc, PageCacheFetch fn, void *data);
/* Like reset, and the columns are dropped too; page_size 0 keeps the size.
 * The generation moves on, so answers still in flight for the old contents land stale. */
void page_cache_clear(PageCache *pc, size_t page_size);
/* New generation: pages dropped, total unknown, columns kept until the next page */
void page_cache_reset(PageCache *pc);
/* New generation like reset, but the pages (and the total) stay visible, marked
 * stale, until the first page of the new generation lands: the rows do not
 * vanish while a sort or filter reloads. Stale pages are not "loaded" for
 * page_cache_request / page_cache_page_loaded_p, so the page is fetched again. */
void page_cache_reset_soft(PageCache *pc);
bool page_cache_stale(const PageCache *pc); /* a soft reset has not landed yet */
/* Give up a soft reset (the reload was cancelled): the old pages and total are
 * current again, and what is still in flight lands stale */
void page_cache_unstale(PageCache *pc);
uint64_t page_cache_generation(const PageCache *pc);
size_t page_cache_page_size(const PageCache *pc);
void page_cache_set_total(PageCache *pc, size_t total, bool known, bool approximate);
size_t page_cache_total(const PageCache *pc);
bool page_cache_total_known(const PageCache *pc);
bool page_cache_total_approximate(const PageCache *pc);
/* Rows the UI may address: total if known, else highest loaded row + 1
 * (+1 when the last loaded page was full) */
size_t page_cache_extent(const PageCache *pc);
/* Adopts rows; ignored (freed) for an old generation */
void page_cache_page_loaded(PageCache *pc, uint64_t gen, size_t page, LaceResult *rows);
void page_cache_page_failed(PageCache *pc, uint64_t gen, size_t page);
/* Fetch the page of row unless loaded or pending */
void page_cache_request(PageCache *pc, size_t row);
bool page_cache_page_loaded_p(const PageCache *pc, size_t page);
bool page_cache_page_pending(const PageCache *pc, size_t page);
bool page_cache_row_loaded(const PageCache *pc, size_t row);
const LaceResult *page_cache_row_result(const PageCache *pc, size_t row, size_t *in_page);
/* NULL when not loaded; a read counts as a use for the LRU */
const LaceValue *page_cache_cell(const PageCache *pc, size_t row, size_t col);
void page_cache_set_cell(PageCache *pc, size_t row, size_t col, LaceValue v); /* adopts v */
/* Drops every page from `from` on, lowers the total, and starts a new
 * generation so fetches in flight land stale (columns kept) */
void page_cache_rows_removed(PageCache *pc, size_t from, size_t count);
size_t page_cache_num_columns(const PageCache *pc);
const LaceColumn *page_cache_column(const PageCache *pc, size_t col);

#endif
