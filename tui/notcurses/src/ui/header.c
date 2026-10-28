#include "ui/header.h"
#include "ui/tabstrip.h"
#include "app.h"
#include "ui/form_tab.h"
#include "core/chrome.h"
#include "core/utf8.h"
#include "model/workspace.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdio.h>
#include <notcurses/notcurses.h>
#include <stdlib.h>
#include <string.h>

/* A growing string */
typedef struct { char *s; size_t len, cap; } Buf;

static void put(Buf *b, const char *text) {
  size_t n = strlen(text);
  if (b->len + n + 1 > b->cap) {
    b->cap = (b->len + n + 1) * 2;
    b->s = safe_realloc(b->s, b->cap);
  }
  memcpy(b->s + b->len, text, n + 1);
  b->len += n;
}

char *header_format_ex(const char *workspace, const char *const *names, size_t num_names,
                       size_t current, const char *database, const char *view, size_t view_pos,
                       size_t view_count, int width, size_t *cur_off, size_t *cur_len,
                       size_t *db_off, size_t *db_len, size_t *view_off, size_t *view_len) {
  return chrome_header_format(workspace, names, num_names, current, database, view, view_pos, view_count, width,
                              cur_off, cur_len, db_off, db_len, view_off, view_len);
}

char *header_format(const char *workspace, const char *const *names, size_t num_names,
                    size_t current, const char *database, const char *view, size_t view_pos,
                    size_t view_count, int width, size_t *cur_off, size_t *cur_len,
                    size_t *db_off, size_t *db_len) {
  return chrome_header_format(workspace, names, num_names, current, database, view, view_pos, view_count, width,
                              cur_off, cur_len, db_off, db_len, NULL, NULL);
}

/* "3 of 1,200", "1 of \xe2\x89\x88200,000" (estimate), "1 of 1,000+" (more may follow) */
char *header_position(const struct View *v) {
  const PageCache *pc = v->cache;
  return chrome_position(v->cursor_row, page_cache_extent(pc), page_cache_total_known(pc),
                         page_cache_total_approximate(pc));
}

/* "\xe2\x86\x91id \xe2\x86\x93v  v = 42  status contains 'x'" */
char *header_sorts_filters(const struct View *v) {
  Buf b = {0};
  put(&b, "");
  for (size_t i = 0; i < v->num_sorts; i++) {
    if (b.len) put(&b, " ");
    put(&b, v->sorts[i].desc ? "\xe2\x86\x93" : "\xe2\x86\x91");
    put(&b, v->sorts[i].column);
  }
  for (size_t i = 0; i < v->num_filters; i++) {
    const LaceFilter *f = &v->filters[i];
    if (!view_filter_active(f)) continue; /* the panel's blank rows */
    if (b.len) put(&b, "  ");
    const LaceColumn *c = NULL;
    for (size_t k = 0; f->column && v->schema && k < v->schema->num_columns; k++)
      if (v->schema->columns[k].name && !strcmp(v->schema->columns[k].name, f->column)) c = &v->schema->columns[k];
    char *text = chrome_filter_text(f->column, f->op, f->value, f->value2, c && lace_type_is_numeric(c->type));
    put(&b, text);
    free(text);
  }
  return b.s;
}

void header_draw(NcApp *app) {
  app->header_spans.n = 0;
  NcWorkspace *ws = app->current_ws < app->num_workspaces ? app->workspaces[app->current_ws] : NULL;
  const char *wname = ws && ws->name ? ws->name : "Workspace";
  nc_style(app->std, &app->theme, TC_HEADER_FG, TC_HEADER_BG, NCSTYLE_BOLD);
  nc_put(app->std, 0, 0, app->cols, "");
  if (!ws || !ws->num_tabs) {
    nc_put(app->std, 0, 1, app->cols - 1, wname);
    return;
  }
  const char **names = safe_calloc(ws->num_tabs, sizeof *names);
  for (size_t i = 0; i < ws->num_tabs; i++) {
    const ConnTab *ti = ws->tabs[i];
    names[i] = ti->conn ? nc_connection_name(ti->conn) : ti->form ? form_tab_label(ti->form) : "?";
  }
  ConnTab *t = workspace_current_tab(ws);
  View *v = conntab_current_view(t);
  size_t off = 0, len = 0, db_off = 0, db_len = 0, v_off = 0, v_len = 0;
  const char *database = t && t->conn ? nc_connection_database(t->conn) : NULL;
  /* The right side: sorts and filters (cut first), then the cursor position */
  char *pos = NULL;
  if (v && v->kind == VIEW_TABLE) {
    pos = header_position(v);
  } else if (v && v->kind == VIEW_QUERY && !v->running) {
    if (v->affected >= 0) {
      pos = str_printf("%lld %s affected", (long long)v->affected, v->affected == 1 ? "row" : "rows");
    } else if (page_cache_num_columns(v->cache) > 0) {
      char *p = header_position(v);
      if (p) {
        pos = str_printf("%s %s", p, page_cache_total_known(v->cache) && page_cache_extent(v->cache) == 1 ? "row" : "rows");
        free(p);
      }
    }
  }
  char *sf = v && v->kind == VIEW_TABLE ? header_sorts_filters(v) : NULL;
  char *right = chrome_header_right(sf, pos, app->cols / 2);
  int right_w = right ? nc_text_width(right) : 0;
  int left_w = app->cols - 1 - (right_w ? right_w + 2 : 0);
  /* the view count only without the tabs strip, which shows the views */
  char *line = header_format_ex(wname, names, ws->num_tabs, ws->current, database, v ? view_title(v) : NULL,
                                t ? t->current : 0, t && !tabstrip_rows(app) ? t->num_views : 0,
                                left_w, &off, &len, &db_off, &db_len, &v_off, &v_len);
  if (db_len) { /* a click on the database segment switches databases */
    char *head = str_ndup(line, db_off), *seg = str_ndup(line + db_off, db_len);
    int x0 = 1 + nc_text_width(head);
    hit_add(&app->header_spans, x0, x0 + nc_text_width(seg), 0);
    free(head);
    free(seg);
  }
  if (v_len && v && v->kind == VIEW_TABLE) { /* a click on the view's name opens the table's actions */
    char *head = str_ndup(line, v_off), *seg = str_ndup(line + v_off, v_len);
    int x0 = 1 + nc_text_width(head);
    hit_add(&app->header_spans, x0, x0 + nc_text_width(seg), 1);
    free(head);
    free(seg);
  }
  nc_style(app->std, &app->theme, TC_HEADER_FG, TC_HEADER_BG, NCSTYLE_BOLD);
  nc_put(app->std, 0, 1, left_w, line);
  if (right_w) {
    nc_style(app->std, &app->theme, TC_HEADER_FG, TC_HEADER_BG, 0);
    nc_put_right(app->std, 0, app->cols - 1 - right_w, right_w, right);
  }
  free(right);
  free(pos);
  free(sf);
  if (len) {
    char *head = str_ndup(line, off), *cur = str_ndup(line + off, len);
    nc_style(app->std, &app->theme, TC_ACCENT, TC_HEADER_BG, NCSTYLE_BOLD);
    nc_put(app->std, 0, 1 + nc_text_width(head), nc_text_width(cur), cur);
    free(head);
    free(cur);
  }
  free(line);
  free(names);
}
