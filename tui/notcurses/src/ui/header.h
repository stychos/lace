#ifndef LACE_NC_HEADER_H
#define LACE_NC_HEADER_H
#include <stddef.h>

struct View;

/* The header's left part for the current tab:
 *   "<workspace> ▸ <connection> ▸ <view> (<i> of <n>) │ <other tabs>"
 * A tab name that repeats gets its position among the equal names appended
 * ("t.sqlite 2"). When it does not fit in width columns the other tabs
 * shrink to their numbers ("│ 2 │ 3"), then the workspace name goes, then the
 * line is clipped. view NULL: no view part; workspace NULL: none.
 * cur_off/cur_len (may be NULL) give the byte range of the current tab's
 * name in the result (len 0 when clipped away). Allocated. */
char *header_format(const char *workspace, const char *const *names, size_t num_names,
                    size_t current, const char *view, size_t view_pos, size_t view_count,
                    int width, size_t *cur_off, size_t *cur_len);
/* The same; toff/tlen (num_names entries) get the byte range of every tab's
 * label in the result (len 0: clipped away) */
char *header_format_spans(const char *workspace, const char *const *names, size_t num_names,
                          size_t current, const char *view, size_t view_pos, size_t view_count,
                          int width, size_t *cur_off, size_t *cur_len, size_t *toff, size_t *tlen);
/* The right side for a table view: "3 of 1,200" (≈ before an estimate, + after
 * a lower bound; NULL while nothing is loaded) and "↑id ↓v  v = 42" */
char *header_position(const struct View *v);
char *header_sorts_filters(const struct View *v);
#endif
