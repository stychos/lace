/*
 * Lace
 * The screen chrome's text: see chrome.h
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "chrome.h"
#include "utf8.h"
#include "../../../liblace/include/util/connstr.h"
#include "../../../liblace/include/util/mem.h"
#include "../../../liblace/include/util/str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A growing string */
typedef struct {
  char *s;
  size_t len, cap;
} Buf;

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
    if (!strcmp(names[i], names[k]))
      same++;
  return same ? str_printf("%s %zu", names[k], same + 1) : str_dup(names[k]);
}

static char *build(const char *workspace, const char *const *names, size_t n, size_t current, const char *database,
                   const char *view, size_t vpos, size_t vcount, size_t *off, size_t *len, size_t *dboff,
                   size_t *dblen, size_t *voff, size_t *vlen) {
  Buf b = {0};
  put(&b, "");
  *off = *len = *dboff = *dblen = *voff = *vlen = 0;
  if (workspace) {
    put(&b, workspace);
    put(&b, " \xe2\x96\xb8 ");
  }
  if (n) {
    char *cur = tab_label(names, current < n ? current : 0);
    *off = b.len;
    *len = strlen(cur);
    put(&b, cur);
    free(cur);
    if (database && *database) {
      put(&b, " \xe2\x96\xb8 ");
      *dboff = b.len;
      *dblen = strlen(database);
      put(&b, database);
    }
    if (view && *view) {
      put(&b, " \xe2\x96\xb8 ");
      *voff = b.len;
      *vlen = strlen(view);
      put(&b, view);
      if (vcount) {
        char *v = str_printf(" (%zu of %zu)", vpos + 1, vcount);
        put(&b, v);
        free(v);
      }
    }
  }
  return b.s;
}

/* A byte range of a line cut to keep bytes */
static void clip_range(size_t *off, size_t *len, size_t keep) {
  if (*off >= keep)
    *off = *len = 0;
  else if (*off + *len > keep)
    *len = keep - *off;
}

char *chrome_header_format(const char *workspace, const char *const *names, size_t num_names, size_t current,
                           const char *database, const char *view, size_t view_pos, size_t view_count, int width,
                           size_t *cur_off, size_t *cur_len, size_t *db_off, size_t *db_len, size_t *view_off,
                           size_t *view_len) {
  size_t off, len, dboff, dblen, voff, vlen;
  char *out = build(workspace, names, num_names, current, database, view, view_pos, view_count, &off, &len, &dboff,
                    &dblen, &voff, &vlen);
  if (workspace && utf8_width(out, strlen(out)) > width) { /* too wide: the workspace name goes */
    free(out);
    out = build(NULL, names, num_names, current, database, view, view_pos, view_count, &off, &len, &dboff, &dblen,
                &voff, &vlen);
  }
  size_t keep = utf8_fit(out, strlen(out), width > 0 ? width : 0, NULL);
  if (keep < strlen(out))
    out[keep] = 0;
  clip_range(&off, &len, keep);
  clip_range(&dboff, &dblen, keep);
  clip_range(&voff, &vlen, keep);
  if (cur_off)
    *cur_off = off;
  if (cur_len)
    *cur_len = len;
  if (db_off)
    *db_off = dboff;
  if (db_len)
    *db_len = dblen;
  if (view_off)
    *view_off = voff;
  if (view_len)
    *view_len = vlen;
  return out;
}

char *chrome_commas(size_t n) {
  char raw[32];
  snprintf(raw, sizeof raw, "%zu", n);
  size_t len = strlen(raw);
  char out[48];
  size_t o = 0;
  for (size_t i = 0; i < len; i++) {
    if (i && (len - i) % 3 == 0)
      out[o++] = ',';
    out[o++] = raw[i];
  }
  out[o] = 0;
  return str_dup(out);
}

char *chrome_position(size_t cursor, size_t extent, bool known, bool approximate) {
  if (!extent && !known)
    return NULL;
  char *cur = chrome_commas(extent ? cursor + 1 : 0), *tot = chrome_commas(extent);
  char *out = str_printf("%s of %s%s%s", cur, known && approximate ? "\xe2\x89\x88" : "", tot, known ? "" : "+");
  free(cur);
  free(tot);
  return out;
}

char *chrome_filter_text(const char *column, LaceFilterOp op, const char *value, const char *value2, bool numeric) {
  const char *a = value ? value : "", *z = value2 ? value2 : "";
  if (op == LACE_FILTER_RAW || !column || !*column)
    return str_printf("(%s)", a);
  const char *label = lace_filter_op_label(op);
  if (!lace_filter_op_needs_value(op))
    return str_printf("%s %s", column, label);
  if (lace_filter_op_two_values(op))
    return numeric ? str_printf("%s %s %s and %s", column, label, a, z)
                   : str_printf("%s %s '%s' and '%s'", column, label, a, z);
  return numeric ? str_printf("%s %s %s", column, label, a) : str_printf("%s %s '%s'", column, label, a);
}

char *chrome_header_right(const char *sf, const char *pos, int max_cols) {
  bool has_sf = sf && *sf;
  if (!pos && !has_sf)
    return NULL;
  int pos_w = pos ? utf8_width(pos, strlen(pos)) : 0;
  int sf_room = max_cols - pos_w - (pos ? 2 : 0);
  char *sf_fit = NULL;
  if (has_sf && sf_room >= 4) {
    int w = utf8_width(sf, strlen(sf));
    if (w <= sf_room) {
      sf_fit = str_dup(sf);
    } else {
      size_t keep = utf8_fit(sf, strlen(sf), sf_room - 1, NULL);
      char *cut = str_ndup(sf, keep);
      sf_fit = str_printf("%s\xe2\x80\xa6", cut);
      free(cut);
    }
  }
  char *right = sf_fit && pos ? str_printf("%s  %s", sf_fit, pos) : sf_fit ? str_dup(sf_fit) : str_dup(pos ? pos : "");
  free(sf_fit);
  return right;
}

size_t chrome_strip_layout(const int *widths, size_t n, size_t current, int width, int *x0, int *x1) {
  for (size_t i = 0; i < n; i++)
    x0[i] = x1[i] = 0;
  if (!n || width < 4)
    return 0;
  if (current >= n)
    current = n - 1;
  size_t first = 0;
  for (;;) {
    /* a scroll mark on the left when tabs are hidden there, one on the right when some follow */
    int x = first ? 2 : 0;
    bool fits = false;
    for (size_t i = first; i < n; i++) {
      int cell = widths[i] + 2;
      int limit = width - (i + 1 < n ? 2 : 0); /* room for the right mark */
      if (x + cell > limit)
        break;
      if (i == current)
        fits = true;
      x += cell + 1;
    }
    if (fits || first == current || first + 1 >= n)
      break;
    first++;
  }
  int x = first ? 2 : 0;
  for (size_t i = first; i < n; i++) {
    int cell = widths[i] + 2;
    int limit = width - (i + 1 < n ? 2 : 0);
    if (x + cell > limit)
      break;
    x0[i] = x;
    x1[i] = x + cell;
    x += cell + 1;
  }
  return first;
}

int chrome_strip_label_width(const char *label) {
  int w = label ? utf8_width(label, strlen(label)) : 0;
  return w > CHROME_TAB_LABEL_MAX ? CHROME_TAB_LABEL_MAX : w;
}

#define NOACT CHROME_NOACT
#define CTRL "\xe2\x8c\x83"
static const ChromeHint H_EDIT[] = {{"Enter save", NOACT}, {"Esc cancel", NOACT}, {CTRL "N NULL", NOACT},
                                    {CTRL "D empty", NOACT},   {CTRL "K copy", NOACT}, {CTRL "U paste", NOACT},
                                    {NULL, 0}};
static const ChromeHint H_EDITOR[] = {{CTRL "R run", HOTKEY_EXECUTE_QUERY},
                                      {CTRL "A run all", HOTKEY_EXECUTE_ALL},
                                      {CTRL "T transaction", HOTKEY_EXECUTE_TRANSACTION},
                                      {"Esc results", CHROME_FOCUS_RESULTS},
                                      {CTRL "\xe2\x86\x91/" CTRL "\xe2\x86\x93 split", NOACT},
                                      {NULL, 0}};
static const ChromeHint H_RESULTS[] = {{CTRL "P palette", HOTKEY_PALETTE}, {"Enter editor", CHROME_FOCUS_EDITOR},
                                       {"e edit", HOTKEY_EDIT_MODAL},      {"x delete", HOTKEY_DELETE_ROW},
                                       {"r refresh", HOTKEY_REFRESH},      {"p new query", HOTKEY_OPEN_QUERY},
                                       {"? keys", HOTKEY_HELP},            {NULL, 0}};
static const ChromeHint H_TABLE[] = {{CTRL "P palette", HOTKEY_PALETTE},  {"/ filter", HOTKEY_TOGGLE_FILTERS},
                                     {"o sort", HOTKEY_CYCLE_SORT},       {"Enter edit", HOTKEY_EDIT_INLINE},
                                     {"e editor", HOTKEY_EDIT_MODAL},     {"+ add", HOTKEY_ROW_ADD},
                                     {"x delete", HOTKEY_DELETE_ROW},     {"m actions", HOTKEY_TABLE_MENU},
                                     {"t tables", HOTKEY_TOGGLE_SIDEBAR}, {"? keys", HOTKEY_HELP},
                                     {NULL, 0}};
static const ChromeHint H_TABLE_FK[] = {{CTRL "P palette", HOTKEY_PALETTE},
                                        {"/ filter", HOTKEY_TOGGLE_FILTERS},
                                        {"o sort", HOTKEY_CYCLE_SORT},
                                        {"Enter edit", HOTKEY_EDIT_INLINE},
                                        {CTRL "Enter follow", HOTKEY_FOLLOW_FK},
                                        {"e editor", HOTKEY_EDIT_MODAL},
                                        {"+ add", HOTKEY_ROW_ADD},
                                        {"x delete", HOTKEY_DELETE_ROW},
                                        {"t tables", HOTKEY_TOGGLE_SIDEBAR},
                                        {"? keys", HOTKEY_HELP},
                                        {NULL, 0}};
static const ChromeHint H_RESULTS_FK[] = {{CTRL "P palette", HOTKEY_PALETTE},   {"Enter editor", CHROME_FOCUS_EDITOR},
                                          {CTRL "Enter follow", HOTKEY_FOLLOW_FK}, {"e edit", HOTKEY_EDIT_MODAL},
                                          {"x delete", HOTKEY_DELETE_ROW},     {"r refresh", HOTKEY_REFRESH},
                                          {"p new query", HOTKEY_OPEN_QUERY},  {"? keys", HOTKEY_HELP},
                                          {NULL, 0}};
static const ChromeHint H_FILTERS[] = {{"+ add", HOTKEY_ADD_FILTER},     {"- remove", HOTKEY_REMOVE_FILTER},
                                       {"X clear", HOTKEY_CLEAR_FILTERS}, {"Enter edit", NOACT},
                                       {"Tab next", NOACT},               {"Esc table", NOACT},
                                       {"/ close", HOTKEY_TOGGLE_FILTERS}, {NULL, 0}};
static const ChromeHint H_SIDEBAR[] = {{CTRL "P palette", HOTKEY_PALETTE}, {"/ search", HOTKEY_SIDEBAR_FILTER},
                                       {"Enter open", NOACT},              {"+ new tab", HOTKEY_NEW_TAB},
                                       {"Tab content", NOACT},             {"Esc back", NOACT},
                                       {"t hide", HOTKEY_TOGGLE_SIDEBAR},  {"? keys", HOTKEY_HELP},
                                       {NULL, 0}};
static const ChromeHint H_FORM[] = {{"Enter open", HOTKEY_CONN_OPEN},
                                    {CTRL "B background", HOTKEY_CONN_OPEN_BACKGROUND},
                                    {CTRL "N workspace", HOTKEY_CONN_OPEN_NEW_WORKSPACE},
                                    {CTRL "O to\xe2\x80\xa6", HOTKEY_CONN_OPEN_TO_WORKSPACE},
                                    {CTRL "S save", HOTKEY_CONN_SAVE},
                                    {CTRL "T test", HOTKEY_CONN_TEST},
                                    {CTRL "L clear", HOTKEY_CONN_CLEAR},
                                    {CTRL "P palette", HOTKEY_PALETTE},
                                    {NULL, 0}};
static const ChromeHint H_FORM_LIST[] = {{"Enter open", HOTKEY_CONN_OPEN},      {"N folder", HOTKEY_CONN_NEW_FOLDER},
                                         {"r rename", HOTKEY_CONN_RENAME},      {"d duplicate", HOTKEY_CONN_DUPLICATE},
                                         {"Space move", HOTKEY_CONN_MOVE},      {"x delete", HOTKEY_CONN_DELETE},
                                         {"/ filter", HOTKEY_CONN_FILTER},      {CTRL "S save", HOTKEY_CONN_SAVE},
                                         {CTRL "P palette", HOTKEY_PALETTE},    {NULL, 0}};
static const ChromeHint H_NONE[] = {{CTRL "P palette", HOTKEY_PALETTE}, {"? keys", HOTKEY_HELP}, {"q quit", HOTKEY_QUIT},
                                    {NULL, 0}};

const ChromeHint *chrome_hints(ChromeHintSet set) {
  static const ChromeHint *const sets[HINTS_COUNT] = {
      [HINTS_EDIT] = H_EDIT,         [HINTS_EDITOR] = H_EDITOR,   [HINTS_RESULTS] = H_RESULTS,
      [HINTS_RESULTS_FK] = H_RESULTS_FK, [HINTS_TABLE] = H_TABLE, [HINTS_TABLE_FK] = H_TABLE_FK,
      [HINTS_FILTERS] = H_FILTERS,   [HINTS_SIDEBAR] = H_SIDEBAR, [HINTS_FORM] = H_FORM,
      [HINTS_FORM_LIST] = H_FORM_LIST, [HINTS_NONE] = H_NONE,
  };
  return set >= 0 && set < HINTS_COUNT ? sets[set] : H_NONE;
}

char *chrome_hints_join(const ChromeHint *hs) {
  Buf b = {0};
  put(&b, "");
  for (; hs && hs->text; hs++) {
    if (b.len)
      put(&b, "  ");
    put(&b, hs->text);
  }
  return b.s;
}

const char *chrome_tx_label(bool open, bool failed, bool lost) {
  if (lost)
    return "[TX LOST] ";
  if (open && failed)
    return "[TX FAILED] ";
  if (open)
    return "[TX] ";
  return "";
}

char *chrome_connstr_short_name(const char *connstr) {
  ConnString *cs = connstr ? connstr_parse(connstr, NULL) : NULL;
  if (!cs)
    return str_dup("database");
  char *out;
  if (str_eq(cs->driver, "sqlite")) {
    const char *db = cs->database ? cs->database : "";
    const char *slash = strrchr(db, '/');
    out = str_dup(slash && slash[1] ? slash + 1 : db);
  } else {
    out = str_printf("%s on %s", cs->database ? cs->database : "db", cs->host && *cs->host ? cs->host : "localhost");
  }
  connstr_free(cs);
  return out;
}

const char *chrome_query_title(const char *text, char *buf, size_t cap) {
  if (!buf || cap == 0)
    return "";
  const char *s = text ? text : "";
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
    s++;
  if (!*s) {
    snprintf(buf, cap, "New query");
    return buf;
  }
  size_t len = 0;
  while (s[len] && s[len] != '\n')
    len++;
  while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r'))
    len--;
  size_t fit = utf8_fit(s, len, 40, NULL);
  if (fit > cap - 1)
    fit = utf8_truncate(s, cap - 1);
  memcpy(buf, s, fit);
  buf[fit] = '\0';
  return buf;
}

void chrome_filter_layout(int cols, bool two_values, ChromeFilterLayout *L) {
  bool narrow = cols < 60;
  L->col_x = 1;
  L->col_w = narrow ? 10 : 14;
  L->op_x = L->col_x + L->col_w + 2;
  L->op_w = narrow ? 9 : 12;
  L->val_x = L->op_x + L->op_w + 2;
  L->del_x = cols - 2;
  int vw = L->del_x - L->val_x - 1;
  if (vw < 4)
    vw = 4;
  if (two_values) {
    int half = (vw - 5) / 2;
    if (half < 3)
      half = 3;
    L->val_w = half;
    L->val2_x = L->val_x + half + 5; /* " and " */
    L->val2_w = vw - half - 5 > 3 ? vw - half - 5 : 3;
  } else {
    L->val_w = vw;
    L->val2_x = L->val_x + vw;
    L->val2_w = 0;
  }
}

typedef struct {
  char **v;
  size_t n;
} Lines;

static void push_line(Lines *l, const char *s, size_t n) {
  l->v = safe_realloc(l->v, (l->n + 1) * sizeof *l->v);
  l->v[l->n++] = str_ndup(s, n);
}

size_t chrome_wrap(const char *text, int width, char ***out) {
  Lines l = {0};
  if (width < 1)
    width = 1;
  const char *p = text ? text : "";
  while (*p) {
    const char *eol = strchr(p, '\n');
    size_t seg = eol ? (size_t)(eol - p) : strlen(p);
    size_t pos = 0;
    while (pos < seg) {
      size_t start = pos, last_break = 0;
      int w = 0;
      size_t i = pos;
      while (i < seg) {
        size_t nx = utf8_next(p, seg, i);
        if (nx <= i)
          nx = i + 1;
        int cw = utf8_width(p + i, nx - i);
        if (w + cw > width)
          break;
        w += cw;
        if (p[i] == ' ')
          last_break = i;
        i = nx;
      }
      if (i >= seg) {
        push_line(&l, p + start, seg - start);
        pos = seg;
        break;
      }
      size_t cut = last_break > start ? last_break : (i > start ? i : utf8_next(p, seg, start));
      push_line(&l, p + start, cut - start);
      pos = cut;
      while (pos < seg && p[pos] == ' ' && last_break > start)
        pos++;
    }
    if (seg == 0)
      push_line(&l, "", 0);
    p += seg;
    if (*p == '\n')
      p++;
  }
  if (!l.n)
    push_line(&l, "", 0);
  *out = l.v;
  return l.n;
}

void chrome_wrap_free(char **lines, size_t n) {
  for (size_t i = 0; i < n; i++)
    free(lines[i]);
  free(lines);
}
