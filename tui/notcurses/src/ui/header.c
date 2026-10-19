#include "ui/header.h"
#include "ui/tabstrip.h"
#include "app.h"
#include "ui/form_tab.h"
#include "core/utf8.h"
#include "model/workspace.h"
#include "util/mem.h"
#include "util/str.h"
#include <stdio.h>
#include <notcurses/notcurses.h>
#include <stdlib.h>
#include <string.h>

/* A growing string with the byte range of one piece remembered */
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

/* The name of tab k: repeated names get their position among equals */
static char *tab_label(const char *const *names, size_t k) {
  size_t same = 0;
  for (size_t i = 0; i < k; i++)
    if (!strcmp(names[i], names[k])) same++;
  return same ? str_printf("%s %zu", names[k], same + 1) : str_dup(names[k]);
}

/* toff/tlen (may be NULL, n entries) get the byte range of every tab's label */
static char *build(const char *workspace, const char *const *names, size_t n, size_t current,
                   const char *view, size_t vpos, size_t vcount, bool numbers, size_t *off,
                   size_t *len, size_t *toff, size_t *tlen) {
  Buf b = {0};
  put(&b, "");
  if (workspace) {
    put(&b, workspace);
    put(&b, " \xe2\x96\xb8 ");
  }
  if (n) {
    char *cur = tab_label(names, current < n ? current : 0);
    if (off) *off = b.len;
    put(&b, cur);
    if (len) *len = strlen(cur);
    size_t ci = current < n ? current : 0;
    if (toff) { toff[ci] = b.len - strlen(cur); tlen[ci] = strlen(cur); }
    free(cur);
    if (view && *view) {
      char *v = vcount ? str_printf(" \xe2\x96\xb8 %s (%zu of %zu)", view, vpos + 1, vcount)
                       : str_printf(" \xe2\x96\xb8 %s", view);
      put(&b, v);
      free(v);
    }
    for (size_t k = 0; k < n; k++) {
      if (k == current) continue;
      put(&b, " \xe2\x94\x82 ");
      char *l = numbers ? str_printf("%zu", k + 1) : tab_label(names, k);
      if (toff) { toff[k] = b.len; tlen[k] = strlen(l); }
      put(&b, l);
      free(l);
    }
  }
  return b.s;
}

char *header_format_spans(const char *workspace, const char *const *names, size_t num_names,
                          size_t current, const char *view, size_t view_pos, size_t view_count,
                          int width, size_t *cur_off, size_t *cur_len, size_t *toff, size_t *tlen) {
  size_t off = 0, len = 0;
  struct { bool numbers, ws; } steps[] = {{false, true}, {true, true}, {true, false}};
  char *out = NULL;
  for (int s = 0; s < 3; s++) {
    free(out);
    off = len = 0;
    for (size_t k = 0; toff && k < num_names; k++) toff[k] = tlen[k] = 0;
    out = build(steps[s].ws ? workspace : NULL, names, num_names, current, view, view_pos,
                view_count, steps[s].numbers, &off, &len, toff, tlen);
    if (utf8_width(out, strlen(out)) <= width) break;
  }
  size_t keep = utf8_fit(out, strlen(out), width > 0 ? width : 0, NULL);
  if (keep < strlen(out)) out[keep] = 0;
  if (off >= keep) off = len = 0;
  else if (off + len > keep) len = keep - off;
  for (size_t k = 0; toff && k < num_names; k++) { /* clipped tabs shrink or vanish */
    if (toff[k] >= keep) toff[k] = tlen[k] = 0;
    else if (toff[k] + tlen[k] > keep) tlen[k] = keep - toff[k];
  }
  if (cur_off) *cur_off = off;
  if (cur_len) *cur_len = len;
  return out;
}

char *header_format(const char *workspace, const char *const *names, size_t num_names,
                    size_t current, const char *view, size_t view_pos, size_t view_count,
                    int width, size_t *cur_off, size_t *cur_len) {
  return header_format_spans(workspace, names, num_names, current, view, view_pos, view_count,
                             width, cur_off, cur_len, NULL, NULL);
}

/* 12345 -> "12,345" */
static char *with_commas(size_t n) {
  char raw[32];
  snprintf(raw, sizeof raw, "%zu", n);
  size_t len = strlen(raw);
  char out[48];
  size_t o = 0;
  for (size_t i = 0; i < len; i++) {
    if (i && (len - i) % 3 == 0) out[o++] = ',';
    out[o++] = raw[i];
  }
  out[o] = 0;
  return str_dup(out);
}

/* "3 of 1,200", "1 of \xe2\x89\x88200,000" (estimate), "1 of 1,000+" (more may follow) */
char *header_position(const struct View *v) {
  const PageCache *pc = v->cache;
  size_t extent = page_cache_extent(pc);
  bool known = page_cache_total_known(pc);
  if (!extent && !known) return NULL;
  char *cur = with_commas(extent ? v->cursor_row + 1 : 0), *tot = with_commas(extent);
  char *out = str_printf("%s of %s%s%s", cur, known && page_cache_total_approximate(pc) ? "\xe2\x89\x88" : "",
                         tot, known ? "" : "+");
  free(cur);
  free(tot);
  return out;
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
    bool raw = f->op == LACE_FILTER_RAW || !f->column;
    char *text;
    if (raw) {
      text = str_printf("(%s)", f->value ? f->value : "");
    } else if (!lace_filter_op_needs_value(f->op)) {
      text = str_printf("%s %s", f->column, lace_filter_op_label(f->op));
    } else {
      const LaceColumn *c = NULL;
      for (size_t k = 0; v->schema && k < v->schema->num_columns; k++)
        if (v->schema->columns[k].name && !strcmp(v->schema->columns[k].name, f->column)) c = &v->schema->columns[k];
      bool quote = !c || !lace_type_is_numeric(c->type);
      const char *a = f->value ? f->value : "", *z = f->value2 ? f->value2 : "";
      if (lace_filter_op_two_values(f->op))
        text = quote ? str_printf("%s %s '%s' and '%s'", f->column, lace_filter_op_label(f->op), a, z)
                     : str_printf("%s %s %s and %s", f->column, lace_filter_op_label(f->op), a, z);
      else
        text = quote ? str_printf("%s %s '%s'", f->column, lace_filter_op_label(f->op), a)
                     : str_printf("%s %s %s", f->column, lace_filter_op_label(f->op), a);
    }
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
  size_t off = 0, len = 0;
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
  int pos_w = pos ? nc_text_width(pos) : 0;
  int right_max = app->cols / 2;
  char *right = NULL;
  if (pos || (sf && *sf)) {
    int sf_room = right_max - pos_w - (pos ? 2 : 0);
    char *sf_fit = NULL;
    if (sf && *sf && sf_room >= 4) {
      int w = nc_text_width(sf);
      if (w <= sf_room) {
        sf_fit = str_dup(sf);
      } else {
        size_t keep = utf8_fit(sf, strlen(sf), sf_room - 1, NULL);
        char *cut = str_ndup(sf, keep);
        sf_fit = str_printf("%s\xe2\x80\xa6", cut);
        free(cut);
      }
    }
    right = sf_fit && pos ? str_printf("%s  %s", sf_fit, pos) : sf_fit ? str_dup(sf_fit) : str_dup(pos ? pos : "");
    free(sf_fit);
  }
  int right_w = right ? nc_text_width(right) : 0;
  int left_w = app->cols - 1 - (right_w ? right_w + 2 : 0);
  size_t *toff = safe_calloc(ws->num_tabs, sizeof *toff), *tlen = safe_calloc(ws->num_tabs, sizeof *tlen);
  /* the view count only without the tabs strip, which shows the views */
  char *line = header_format_spans(wname, names, ws->num_tabs, ws->current, v ? view_title(v) : NULL,
                                   t ? t->current : 0, t && !tabstrip_rows(app) ? t->num_views : 0,
                                   left_w, &off, &len, toff, tlen);
  for (size_t k = 0; k < ws->num_tabs; k++) { /* where a click switches to tab k */
    if (!tlen[k]) continue;
    char *head = str_ndup(line, toff[k]), *lab = str_ndup(line + toff[k], tlen[k]);
    int x0 = 1 + nc_text_width(head);
    hit_add(&app->header_spans, x0, x0 + nc_text_width(lab), (int)k);
    free(head);
    free(lab);
  }
  free(toff);
  free(tlen);
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
