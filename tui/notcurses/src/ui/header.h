#ifndef LACE_NC_HEADER_H
#define LACE_NC_HEADER_H
#include <stddef.h>

struct View;

/* The header's left part, the path to the current view:
 *   "<workspace> ▸ <connection> ▸ <database> ▸ <view> (<i> of <n>)"
 * The other connection tabs are not listed (the tabs strip shows them). A
 * connection name that another tab of the workspace shares gets its position
 * among the equal names appended ("t.sqlite 2"), so names takes every tab.
 * When it does not fit in width columns the workspace name goes, then the
 * line is clipped. database NULL or empty: no database part (SQLite); view
 * NULL: no view part; workspace NULL: none. cur_off/cur_len and
 * db_off/db_len (each may be NULL) give the byte ranges of the connection
 * name and of the database in the result (len 0 when absent or clipped
 * away). Allocated. */
/* The same, also giving the byte range of the view's name */
char *header_format_ex(const char *workspace, const char *const *names, size_t num_names,
                       size_t current, const char *database, const char *view, size_t view_pos,
                       size_t view_count, int width, size_t *cur_off, size_t *cur_len,
                       size_t *db_off, size_t *db_len, size_t *view_off, size_t *view_len);
char *header_format(const char *workspace, const char *const *names, size_t num_names,
                    size_t current, const char *database, const char *view, size_t view_pos,
                    size_t view_count, int width, size_t *cur_off, size_t *cur_len,
                    size_t *db_off, size_t *db_len);
/* The right side for a table view: "3 of 1,200" (≈ before an estimate, + after
 * a lower bound; NULL while nothing is loaded) and "↑id ↓v  v = 42" */
char *header_position(const struct View *v);
char *header_sorts_filters(const struct View *v);
#endif
