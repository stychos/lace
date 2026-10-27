/*
 * Lace
 * The connection form tab. Left: the saved connections tree and Recent;
 * right: the URI line over the fields (core/conn_fields.c, shared with the
 * notcurses frontend), the open and manage buttons, a message line.
 * Highlighting a saved connection or a recent URL fills the fields, asking
 * first when they hold edits not saved. Opening uses a connection already
 * open when there is one (app_find_open_connection), else connects behind
 * the progress dialog (connect_open.c), then places it (connect_place.c).
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "form_tab.h"
#include "conn_tree.h"
#include "connect_internal.h"
#include "connect_open.h"
#include "list_picker.h"
#include "text_input_draw.h"
#include "../core/conn_fields.h"
#include "../tui_internal.h"
#include "util/mem.h"
#include "util/connstr.h"
#include "util/str.h"
#include <lace.h>
#include <stdlib.h>
#include <string.h>

typedef struct FormTab FormTab;
typedef enum { FF_PANEL, FF_URI, FF_FIELD, FF_BUTTON } FormFocus;
enum { B_OPEN, B_BACKGROUND, B_NEW_WS, B_TO_WS, B_SAVE, B_SAVE_AS, B_TEST, B_CLEAR, B_COUNT };
static const char *const button_labels[B_COUNT] = {
    "Open here", "Background tab", "New workspace", "To workspace\xe2\x80\xa6",
    "Save", "Save as new", "Test", "Clear"};
static const HotkeyAction button_actions[B_COUNT] = {
    HOTKEY_CONN_OPEN, HOTKEY_CONN_OPEN_BACKGROUND, HOTKEY_CONN_OPEN_NEW_WORKSPACE, HOTKEY_CONN_OPEN_TO_WORKSPACE,
    HOTKEY_CONN_SAVE, HOTKEY_CONN_SAVE_AS_NEW, HOTKEY_CONN_TEST, HOTKEY_CONN_CLEAR};
static const HotkeyAction tree_actions[] = {HOTKEY_CONN_NEW_FOLDER, HOTKEY_CONN_RENAME, HOTKEY_CONN_DUPLICATE,
                                            HOTKEY_CONN_DELETE, HOTKEY_CONN_MOVE, HOTKEY_CONN_FILTER};

#define LABEL_COLS 19   /* the field labels' column ("Save SSH password") */
#define NARROW_COLS 64  /* below this main_win shows only the focused side */

/* A row of the left panel */
typedef enum { PR_HEADER, PR_NOTE, PR_TREE, PR_MOVING, PR_RECENT } PanelKind;
typedef struct {
  PanelKind kind;
  size_t index;          /* tree index (filtered) or recent index */
  ConnectionItem *item;  /* PR_TREE, PR_MOVING */
  int depth;
  const char *text;      /* PR_HEADER, PR_NOTE */
} PanelRow;

struct FormTab {
  ConnFields fields;
  TextInput uri;         /* the URI line as typed; rebuilt from the fields while not focused */
  bool uri_edited;       /* typed into since the fields were last applied to it */
  ConnTree tree;         /* the tree's filter, highlight and move */
  bool filter_focused;   /* the panel's filter line takes the keys */
  PanelKind pos_kind;    /* the panel's cursor: a tree or recent row */
  size_t pos_index;
  bool highlighted;      /* the cursor's row filled the form */
  char *highlight_id;    /* the highlighted saved connection's id */
  char *highlight_name;  /* and its name: the tab's label */
  FormFocus focus;
  int field, button;
  size_t scroll;
  PanelRow *rows;
  size_t num_rows, rows_cap;
  /* Where the last draw put things (main_win coordinates), for the mouse */
  int list_y, list_x, list_rows, list_cols;
  int uri_y, uri_x0, uri_x1;
  int field_y[CF_COUNT]; /* -1: not shown */
  int button_y[B_COUNT], button_x0[B_COUNT], button_x1[B_COUNT];
};

/* ---- the process's saved connections and Recent ---- */

ConnectionManager *tui_connmgr(TuiState *state) {
  if (!state->connmgr) {
    char *err = NULL;
    state->connmgr = connmgr_load(&err);
    if (!state->connmgr) { /* shown empty, and never written */
      state->connmgr = connmgr_new();
      state->connmgr->read_only = true;
      state->connmgr_error = err ? err : str_dup("connections.json could not be read");
      err = NULL;
    } else if (state->connmgr->modified && !state->connmgr->read_only) {
      connmgr_save(state->connmgr, NULL); /* ids repaired while loading */
    }
    free(err);
  }
  return state->connmgr;
}

RecentList *tui_recent(TuiState *state) {
  if (!state->recent) {
    state->recent = safe_calloc(1, sizeof *state->recent);
    recent_load(state->recent);
  }
  return state->recent;
}

void tui_connmgr_free(TuiState *state) {
  if (state->connmgr)
    connmgr_free(state->connmgr);
  state->connmgr = NULL;
  free(state->connmgr_error);
  state->connmgr_error = NULL;
  if (state->recent) {
    recent_free(state->recent);
    free(state->recent);
  }
  state->recent = NULL;
}

/* ---- state ---- */

static FormTab *form_new(TuiState *state) {
  FormTab *f = safe_calloc(1, sizeof *f);
  conn_fields_init(&f->fields);
  text_input_init(&f->uri, NULL, false);
  text_input_init(&f->tree.filter, NULL, false);
  f->tree.state = state;
  f->tree.config = state->app->config;
  f->focus = FF_URI;
  f->field = CF_NAME;
  f->pos_kind = PR_NOTE; /* nothing yet: the first selectable row */
  return f;
}

void form_tab_free(struct FormTab *f) {
  if (!f)
    return;
  conn_fields_free(&f->fields);
  text_input_free(&f->uri);
  text_input_free(&f->tree.filter);
  free(f->tree.error_msg);
  free(f->tree.success_msg);
  free(f->highlight_id);
  free(f->highlight_name);
  free(f->rows);
  free(f);
}

/* The tab's form, made when first needed; NULL for any other tab */
static FormTab *form_of(TuiState *state, Tab *tab) {
  if (!tab || tab->type != TAB_TYPE_FORM)
    return NULL;
  if (!tab->form)
    tab->form = form_new(state);
  FormTab *f = tab->form;
  f->tree.mgr = tui_connmgr(state);
  return f;
}

char *form_tab_label(const Tab *tab) {
  const FormTab *f = tab ? tab->form : NULL;
  return str_dup(f && f->highlight_name ? f->highlight_name : "New connection");
}

static void set_highlight(FormTab *f, const char *id, const char *name) {
  free(f->highlight_id);
  free(f->highlight_name);
  f->highlight_id = id ? str_dup(id) : NULL;
  f->highlight_name = name ? str_dup(name) : NULL;
}

static void sync_uri(FormTab *f) {
  char *u = conn_fields_uri(&f->fields);
  text_input_set(&f->uri, u);
  str_secure_free(u);
  f->uri_edited = false;
}

/* What was typed into the URI line goes into the fields: when the line is
 * left or an action runs, never while it is still being typed (a URI half
 * typed often parses, and would empty the fields). False (the message says
 * why, the text stays) when it does not parse. */
static bool commit_uri(FormTab *f) {
  if (!f->uri_edited)
    return true;
  if (!f->uri.len) { /* cleared: the fields stay as they are */
    sync_uri(f);
    return true;
  }
  if (!conn_fields_set_uri(&f->fields, f->uri.buf))
    return false;
  sync_uri(f);
  return true;
}

/* Edits not saved: in the fields, or typed into the URI line */
static bool form_dirty(const FormTab *f) { return f->uri_edited || conn_fields_dirty(&f->fields); }

/* Take what a tree action has to say */
static void take_tree_message(FormTab *f) {
  if (f->tree.error_msg)
    conn_fields_set_message(&f->fields, true, "%s", f->tree.error_msg);
  else if (f->tree.success_msg)
    conn_fields_set_message(&f->fields, false, "%s", f->tree.success_msg);
  free(f->tree.error_msg);
  free(f->tree.success_msg);
  f->tree.error_msg = f->tree.success_msg = NULL;
}

/* Write connections.json now, if a change is waiting */
static void persist(TuiState *state, FormTab *f) {
  ConnectionManager *mgr = tui_connmgr(state);
  if (!mgr->modified || mgr->read_only)
    return;
  char *err = NULL;
  if (!connmgr_save(mgr, &err))
    conn_fields_set_message(&f->fields, true, "Cannot save connections.json: %s", err ? err : "unknown error");
  free(err);
  mgr->modified = false; /* reported once */
}

/* ---- the left panel ---- */

static void add_row(FormTab *f, PanelKind kind, size_t index, ConnectionItem *item, int depth, const char *text) {
  if (f->num_rows == f->rows_cap) {
    f->rows_cap = f->rows_cap ? f->rows_cap * 2 : 32;
    f->rows = safe_reallocarray(f->rows, f->rows_cap, sizeof *f->rows);
  }
  f->rows[f->num_rows++] = (PanelRow){kind, index, item, depth, text};
}

static void add_tree_row(void *ctx, bool moving, size_t index, ConnectionItem *item, int depth) {
  add_row(ctx, moving ? PR_MOVING : PR_TREE, index, item, depth, NULL);
}

static bool matches(const char *haystack, const char *needle) {
  return haystack && needle && strcasestr(haystack, needle) != NULL;
}

/* A short name for a recent URL: a SQLite file's name, else "db on host" */
static char *short_name(const char *connstr) {
  ConnString *cs = connstr ? connstr_parse(connstr, NULL) : NULL;
  if (!cs)
    return str_dup(connstr ? connstr : "");
  char *out;
  if (cs->driver && str_eq(cs->driver, "sqlite")) {
    const char *db = cs->database ? cs->database : "";
    const char *slash = strrchr(db, '/');
    out = str_dup(slash && slash[1] ? slash + 1 : db);
  } else {
    const char *host = cs->host && cs->host[0] ? cs->host : "localhost";
    out = cs->database && cs->database[0] ? str_printf("%s on %s", cs->database, host) : str_dup(host);
  }
  connstr_free(cs);
  return out;
}

static void build_rows(TuiState *state, FormTab *f) {
  f->num_rows = 0;
  const char *filter = conn_tree_filter(&f->tree);
  add_row(f, PR_HEADER, 0, NULL, 0, "SAVED");
  if (connect_tree_count(&f->tree) > 0 || f->tree.move.active)
    connect_tree_rows(&f->tree, add_tree_row, f);
  else
    add_row(f, PR_NOTE, 0, NULL, 0,
            f->tree.mgr->read_only ? "(connections.json unreadable)" : filter ? "(nothing matches)" : "(none yet)");
  if (f->tree.move.active)
    return;
  RecentList *rl = tui_recent(state);
  size_t before = f->num_rows;
  for (size_t i = 0; i < rl->count; i++) {
    if (filter && !matches(rl->items[i].url, filter))
      continue;
    if (f->num_rows == before)
      add_row(f, PR_HEADER, 0, NULL, 0, "RECENT");
    add_row(f, PR_RECENT, i, NULL, 0, NULL);
  }
}

static bool selectable(const PanelRow *r) { return r->kind == PR_TREE || r->kind == PR_RECENT || r->kind == PR_MOVING; }

/* The row the panel's cursor is on (the first selectable one when it is
 * on nothing), SIZE_MAX when there is none */
static size_t cursor_row(FormTab *f) {
  for (size_t i = 0; i < f->num_rows; i++)
    if (f->tree.move.active ? f->rows[i].kind == PR_MOVING
                            : selectable(&f->rows[i]) && f->rows[i].kind == f->pos_kind &&
                                  f->rows[i].index == f->pos_index)
      return i;
  for (size_t i = 0; i < f->num_rows; i++)
    if (selectable(&f->rows[i]))
      return i;
  return SIZE_MAX;
}

static const PanelRow *highlighted_row(FormTab *f) {
  if (!f->highlighted)
    return NULL;
  size_t i = cursor_row(f);
  return i != SIZE_MAX ? &f->rows[i] : NULL;
}

/* The highlighted row's tree item, NULL for none or a recent one */
static ConnectionItem *highlighted_item(FormTab *f) {
  const PanelRow *r = highlighted_row(f);
  return r && r->kind == PR_TREE ? r->item : NULL;
}

/* The row at i becomes the highlight and fills the form */
static void apply_highlight(TuiState *state, FormTab *f, size_t i) {
  if (i >= f->num_rows || !selectable(&f->rows[i]))
    return;
  PanelRow *r = &f->rows[i];
  f->pos_kind = r->kind;
  f->pos_index = r->index;
  f->highlighted = true;
  if (r->kind == PR_TREE)
    f->tree.highlight = r->index;
  if (r->kind == PR_TREE && connmgr_is_connection(r->item)) {
    conn_fields_fill(&f->fields, &r->item->connection);
    set_highlight(f, r->item->connection.id, r->item->connection.name);
  } else if (r->kind == PR_RECENT) {
    RecentList *rl = tui_recent(state);
    conn_fields_clear(&f->fields);
    if (r->index < rl->count)
      conn_fields_set_uri(&f->fields, rl->items[r->index].url);
    conn_fields_mark_clean(&f->fields);
    set_highlight(f, NULL, NULL);
  } else { /* a folder: the form stays as it is */
    set_highlight(f, NULL, NULL);
  }
  sync_uri(f);
}

/* Highlight row i: at once when the form holds no edits, else after asking */
static void request_highlight(TuiState *state, FormTab *f, size_t i) {
  if (f->highlighted && i == cursor_row(f))
    return;
  if (form_dirty(f)) {
    bool drop = tui_show_confirm_dialog(state, "Drop the changes in the form?");
    tui_refresh(state);
    if (!drop)
      return;
  }
  apply_highlight(state, f, i);
}

/* Up/Down/PgUp/PgDn/Home/End or the movement hotkeys: the row they lead to
 * from the cursor (skipping headers), SIZE_MAX for another key */
static size_t nav_target(TuiState *state, FormTab *f, const UiEvent *ev) {
  const Config *c = state->app->config;
  int step = 0;
  bool edge = false;
  if (render_event_is_special(ev, UI_KEY_UP) || hotkey_matches(c, ev, HOTKEY_MOVE_UP))
    step = -1;
  else if (render_event_is_special(ev, UI_KEY_DOWN) || hotkey_matches(c, ev, HOTKEY_MOVE_DOWN))
    step = 1;
  else if (render_event_is_special(ev, UI_KEY_PAGEUP) || hotkey_matches(c, ev, HOTKEY_PAGE_UP))
    step = -(f->list_rows > 2 ? f->list_rows - 1 : 1);
  else if (render_event_is_special(ev, UI_KEY_PAGEDOWN) || hotkey_matches(c, ev, HOTKEY_PAGE_DOWN))
    step = f->list_rows > 2 ? f->list_rows - 1 : 1;
  else if (render_event_is_special(ev, UI_KEY_HOME) || hotkey_matches(c, ev, HOTKEY_FIRST_ROW))
    step = -1, edge = true;
  else if (render_event_is_special(ev, UI_KEY_END) || hotkey_matches(c, ev, HOTKEY_LAST_ROW))
    step = 1, edge = true;
  else
    return SIZE_MAX;
  build_rows(state, f);
  size_t at = cursor_row(f);
  if (at == SIZE_MAX)
    return SIZE_MAX;
  if (!f->highlighted) /* the first move picks the row the cursor waits on */
    return at;
  int dir = step < 0 ? -1 : 1;
  for (int n = edge ? (int)f->num_rows : abs(step); n > 0;) {
    size_t i = at;
    bool moved = false;
    while ((dir < 0 && i > 0) || (dir > 0 && i + 1 < f->num_rows)) {
      i = (size_t)((long)i + dir);
      if (selectable(&f->rows[i])) {
        moved = true;
        break;
      }
    }
    if (!moved)
      break;
    at = i;
    n--;
  }
  return at;
}

/* "1, 3": the workspaces with a tab on saved connection id ("" if none) */
static char *open_in(AppState *app, const char *id) {
  StringBuilder *sb = sb_new(16);
  for (size_t w = 0; id && w < app->num_workspaces; w++) {
    Workspace *ws = &app->workspaces[w];
    for (size_t t = 0; t < ws->num_tabs; t++) {
      Connection *c = app_get_connection(app, ws->tabs[t].connection_index);
      if (c && c->saved_conn_id && str_eq(c->saved_conn_id, id)) {
        sb_printf(sb, "%s%zu", sb->len ? ", " : "", w + 1);
        break;
      }
    }
  }
  return sb_finish(sb);
}

/* ---- actions ---- */

static void open_with(TuiState *state, Tab *tab, FormTab *f, PlaceKind kind, size_t target_ws) {
  if (!commit_uri(f))
    return; /* what the line shows does not parse: nothing opens */
  AppState *app = state->app;
  size_t ws_idx = app->current_workspace;
  Workspace *ws = TUI_WORKSPACE(state);
  size_t tab_idx = (size_t)(tab - ws->tabs);
  SavedConnection *model = NULL;
  bool own_model = false;
  char *saved_id = NULL, *connstr = NULL;
  /* The saved entry the form shows (a filter or a folder may have moved the
   * panel's cursor off it since), while the fields are as it filled them */
  ConnectionItem *it = f->highlight_id && !conn_fields_dirty(&f->fields)
                           ? connmgr_find_by_id(tui_connmgr(state), f->highlight_id)
                           : NULL;
  if (it && connmgr_is_connection(it)) {
    model = &it->connection;
    saved_id = str_dup(it->connection.id);
    connstr = tui_saved_connstr(model);
  } else {
    model = conn_fields_collect(&f->fields, true);
    own_model = model != NULL;
    connstr = model ? conn_fields_connstr(&f->fields) : NULL;
  }
  if (!connstr) {
    if (!f->fields.message)
      conn_fields_set_message(&f->fields, true, "These fields do not make a connection");
    goto done;
  }
  size_t idx = app_find_open_connection(app, saved_id, connstr);
  if (idx == SIZE_MAX) {
    char *final = NULL, *err = NULL;
    bool cancelled = false;
    DbConnection *db = tui_open_connection(state, model, connstr, &final, &err, &cancelled);
    if (!db) {
      if (cancelled) {
        conn_fields_set_message(&f->fields, false, "Connecting cancelled");
      } else {
        char *m = tui_connect_failure_message(err);
        char *flat = connect_flatten_message(m);
        conn_fields_set_message(&f->fields, true, "%s", flat ? flat : "Connection failed");
        free(flat);
        free(m);
      }
      free(err);
      goto done;
    }
    free(err);
    idx = tui_add_open_connection(state, db, final, saved_id);
    if (!saved_id) {
      RecentList *rl = tui_recent(state);
      if (recent_add(rl, final)) {
        char *rerr = NULL;
        if (!recent_save(rl, &rerr))
          tui_set_error(state, "recent.json: %s", rerr ? rerr : "cannot save");
        free(rerr);
      }
    }
    str_secure_free(final);
    if (idx == SIZE_MAX)
      goto done;
  } else if (!saved_id) {
    RecentList *rl = tui_recent(state);
    if (recent_add(rl, connstr))
      recent_save(rl, NULL);
  }
  /* Open here turns this tab (and frees this form): nothing of f after it */
  tui_place_connection(state, idx, kind, kind == PLACE_WORKSPACE ? target_ws : ws_idx, tab_idx);
done:
  str_secure_free(connstr);
  free(saved_id);
  if (own_model) {
    connmgr_free_connection(model);
    free(model);
  }
}

/* A workspace's name for a pick list */
static char *ws_label(AppState *app, size_t i) {
  Workspace *ws = &app->workspaces[i];
  return ws->name[0] ? str_printf("%zu  %s", i + 1, ws->name) : str_printf("%zu  Workspace %zu", i + 1, i + 1);
}

static void to_workspace(TuiState *state, Tab *tab, FormTab *f) {
  AppState *app = state->app;
  size_t n = 0;
  size_t *idx = safe_calloc(app->num_workspaces + 1, sizeof *idx);
  char **labels = safe_calloc(app->num_workspaces + 1, sizeof *labels);
  for (size_t i = 0; i < app->num_workspaces; i++) {
    if (i == app->current_workspace)
      continue;
    idx[n] = i;
    labels[n++] = ws_label(app, i);
  }
  if (!n) {
    conn_fields_set_message(&f->fields, true, "No other workspace");
  } else {
    int pick = tui_pick_from_list(app->config, "Open in workspace", (const char *const *)labels, n, 0,
                                  state->term_rows / 3, state->term_cols / 3, 30, 60, 16);
    tui_refresh(state);
    if (pick >= 0)
      open_with(state, tab, f, PLACE_WORKSPACE, idx[pick]);
  }
  for (size_t i = 0; i < n; i++)
    free(labels[i]);
  free(labels);
  free(idx);
}

static void test(TuiState *state, FormTab *f) {
  if (!commit_uri(f))
    return;
  LaceSavedConnection *m = conn_fields_collect(&f->fields, true);
  char *cs = m ? conn_fields_connstr(&f->fields) : NULL;
  if (cs) {
    char *msg = NULL;
    bool ok = tui_test_connection(state, m, cs, &msg);
    char *flat = connect_flatten_message(msg);
    conn_fields_set_message(&f->fields, !ok, "%s", flat ? flat : "");
    free(flat);
    free(msg);
  }
  str_secure_free(cs);
  if (m)
    lace_saved_conn_free(m);
}

static void clear_form(FormTab *f) {
  conn_fields_clear(&f->fields);
  set_highlight(f, NULL, NULL);
  f->highlighted = false;
  text_input_set(&f->uri, "");
  f->uri_edited = false;
  f->focus = FF_URI;
}

/* The panel's cursor onto the saved connection with that id */
static void select_saved(TuiState *state, FormTab *f, const char *id) {
  build_rows(state, f);
  for (size_t i = 0; i < f->num_rows; i++) {
    const PanelRow *r = &f->rows[i];
    if (r->kind == PR_TREE && connmgr_is_connection(r->item) && r->item->connection.id &&
        str_eq(r->item->connection.id, id)) {
      f->pos_kind = PR_TREE;
      f->pos_index = r->index;
      f->tree.highlight = r->index;
      f->highlighted = true;
      return;
    }
  }
}

static void replace_connection(SavedConnection *dst, SavedConnection *src) {
  char *id = dst->id;
  dst->id = NULL;
  connmgr_free_connection(dst);
  *dst = *src;
  dst->id = id;
  free(src);
}

/* Write c (taken over) to connections.json: over the highlighted entry, or
 * as a new one next to the highlighted entry or into the highlighted folder */
static void store(TuiState *state, FormTab *f, SavedConnection *c, bool as_new) {
  ConnectionManager *mgr = tui_connmgr(state);
  if (mgr->read_only) {
    lace_saved_conn_free(c);
    conn_fields_set_message(&f->fields, true, "Not saved: %s", state->connmgr_error ? state->connmgr_error
                                                                                    : "connections.json is read only");
    return;
  }
  ConnectionItem *it = !as_new && f->highlight_id ? connmgr_find_by_id(mgr, f->highlight_id) : NULL;
  char *id;
  if (it && connmgr_is_connection(it)) {
    id = str_dup(it->connection.id);
    replace_connection(&it->connection, c);
  } else {
    ConnectionItem *parent = &mgr->root;
    ConnectionItem *near = highlighted_item(f);
    if (near && connmgr_is_folder(near))
      parent = near;
    else if (near && near->parent)
      parent = near->parent;
    if (connmgr_is_folder(parent))
      parent->folder.expanded = true;
    free(c->id);
    c->id = lace_generate_uuid();
    id = str_dup(c->id);
    if (!connmgr_add_connection(parent, c)) {
      lace_saved_conn_free(c);
      free(id);
      conn_fields_set_message(&f->fields, true, "Cannot add the connection");
      return;
    }
  }
  char *err = NULL;
  if (!connmgr_save(mgr, &err)) {
    conn_fields_set_message(&f->fields, true, "Cannot save: %s", err ? err : "unknown error");
    free(err);
    free(id);
    return;
  }
  mgr->modified = false;
  it = connmgr_find_by_id(mgr, id);
  const char *name = it ? it->connection.name : NULL;
  set_highlight(f, id, name);
  if (name && !f->fields.in[CF_NAME].len)
    text_input_set(&f->fields.in[CF_NAME], name);
  conn_fields_mark_clean(&f->fields);
  select_saved(state, f, id);
  /* What was saved: no longer a recent one */
  char *url = conn_fields_connstr(&f->fields);
  char *clean = url ? recent_strip_passwords(url) : NULL;
  RecentList *rl = tui_recent(state);
  for (size_t i = 0; clean && i < rl->count; i++) {
    if (str_eq(rl->items[i].url, clean)) {
      recent_remove(rl, i);
      recent_save(rl, NULL);
      break;
    }
  }
  str_secure_free(url);
  str_secure_free(clean);
  conn_fields_set_message(&f->fields, false, "Saved %s", name ? name : "");
  free(id);
}

static bool holds(bool saved, const char *pw, const char *typed) { return saved && pw && str_eq(pw, typed); }

static void save(TuiState *state, FormTab *f, bool as_new) {
  if (!commit_uri(f))
    return;
  SavedConnection *c = conn_fields_collect(&f->fields, false);
  if (!c)
    return;
  ConnectionItem *it = !as_new && f->highlight_id ? connmgr_find_by_id(tui_connmgr(state), f->highlight_id) : NULL;
  const SavedConnection *e = it && connmgr_is_connection(it) ? &it->connection : NULL;
  bool new_pw = c->save_password && c->password && *c->password &&
                !(e && holds(e->save_password, e->password, c->password));
  bool new_ssh = c->save_ssh_password && c->ssh_password && *c->ssh_password &&
                 !(e && holds(e->save_ssh_password, e->ssh_password, c->ssh_password));
  if (new_pw || new_ssh) {
    bool yes = tui_show_confirm_dialog(state, "Store the password in connections.json (readable only by you)?");
    tui_refresh(state);
    if (!yes) { /* saved without the passwords */
      c->save_password = c->save_ssh_password = false;
      str_secure_free(c->password);
      str_secure_free(c->ssh_password);
      c->password = str_dup("");
      c->ssh_password = str_dup("");
      f->fields.chk[CF_SAVEPW] = f->fields.chk[CF_SAVESSHPW] = false;
    }
  }
  store(state, f, c, as_new);
}

/* After a tree change: the form follows what happened to the entry it shows */
static void tree_changed(TuiState *state, FormTab *f) {
  persist(state, f);
  take_tree_message(f);
  if (!f->highlight_id)
    return;
  ConnectionItem *it = connmgr_find_by_id(tui_connmgr(state), f->highlight_id);
  bool clean = !conn_fields_dirty(&f->fields);
  if (!it || !connmgr_is_connection(it)) { /* deleted: a clean form goes with it */
    set_highlight(f, NULL, NULL);
    f->highlighted = false;
    if (clean) {
      conn_fields_clear(&f->fields);
      sync_uri(f);
    }
    return;
  }
  if (!str_eq(it->connection.name, f->highlight_name)) { /* renamed */
    char *id = str_dup(f->highlight_id);
    set_highlight(f, id, it->connection.name);
    free(id);
    text_input_set(&f->fields.in[CF_NAME], it->connection.name);
    if (clean)
      conn_fields_mark_clean(&f->fields);
  }
}

/* The panel's own keys (N r d x Space /) */
static bool tree_action(TuiState *state, FormTab *f, HotkeyAction a) {
  if (a == HOTKEY_CONN_FILTER) {
    f->filter_focused = true;
    f->focus = FF_PANEL;
    return true;
  }
  build_rows(state, f);
  const PanelRow *r = highlighted_row(f);
  if (a == HOTKEY_CONN_DELETE && r && r->kind == PR_RECENT) { /* a recent URL is forgotten at once */
    RecentList *rl = tui_recent(state);
    if (r->index < rl->count) {
      recent_remove(rl, r->index);
      char *err = NULL;
      if (!recent_save(rl, &err))
        conn_fields_set_message(&f->fields, true, "recent.json: %s", err ? err : "cannot save");
      free(err);
      f->highlighted = false;
      f->pos_kind = PR_NOTE;
    }
    return true;
  }
  if (tui_connmgr(state)->read_only) {
    conn_fields_set_message(&f->fields, true, "Not saved: %s",
                            state->connmgr_error ? state->connmgr_error : "connections.json is read only");
    return true;
  }
  ConnectionItem *it = highlighted_item(f);
  if (r && r->kind == PR_TREE)
    f->tree.highlight = r->index;
  switch (a) {
  case HOTKEY_CONN_NEW_FOLDER:
    connect_tree_new_folder(&f->tree, it);
    break;
  case HOTKEY_CONN_RENAME:
    if (!it)
      goto pick;
    connect_tree_rename(&f->tree);
    break;
  case HOTKEY_CONN_DUPLICATE:
    if (!it || !connmgr_is_connection(it))
      goto pick;
    connect_tree_duplicate(&f->tree);
    break;
  case HOTKEY_CONN_DELETE:
    if (!it)
      goto pick;
    connect_tree_delete(&f->tree);
    f->pos_index = f->tree.highlight;
    break;
  case HOTKEY_CONN_MOVE:
    if (!it)
      goto pick;
    connect_tree_move(&f->tree);
    break;
  default:
    return false;
  }
  tree_changed(state, f);
  return true;
pick:
  conn_fields_set_message(&f->fields, true, "Pick a saved connection on the left first");
  return true;
}

bool form_tab_do_action(TuiState *state, Tab *tab, HotkeyAction a) {
  FormTab *f = form_of(state, tab);
  if (!f)
    return false;
  switch (a) {
  case HOTKEY_CONN_OPEN: open_with(state, tab, f, PLACE_HERE, 0); return true;
  case HOTKEY_CONN_OPEN_BACKGROUND: open_with(state, tab, f, PLACE_BACKGROUND, 0); return true;
  case HOTKEY_CONN_OPEN_NEW_WORKSPACE: open_with(state, tab, f, PLACE_NEW_WORKSPACE, 0); return true;
  case HOTKEY_CONN_OPEN_TO_WORKSPACE: to_workspace(state, tab, f); return true;
  case HOTKEY_CONN_SAVE: save(state, f, false); return true;
  case HOTKEY_CONN_SAVE_AS_NEW: save(state, f, true); return true;
  case HOTKEY_CONN_TEST: test(state, f); return true;
  case HOTKEY_CONN_CLEAR: clear_form(f); return true;
  default: return tree_action(state, f, a);
  }
}

/* ---- keys ---- */

static bool plain_char(const UiEvent *ev) {
  return ev->type == UI_EVENT_KEY && !ev->key.is_special && !(ev->key.mods & (UI_MOD_CTRL | UI_MOD_ALT)) &&
         ev->key.key >= ' ';
}

static int first_field(const FormTab *f) { return conn_fields_next(&f->fields, CF_COUNT - 1, 1); }
static int last_field(const FormTab *f) { return conn_fields_next(&f->fields, 0, -1); }

static void focus_set(FormTab *f, FormFocus to) {
  if (f->focus == FF_URI && to != FF_URI && commit_uri(f))
    sync_uri(f); /* the line shows what the fields read */
  f->focus = to;
}

/* Tab / Shift+Tab: panel, URI line, each field, each button, around */
static void focus_step(FormTab *f, int dir) {
  switch (f->focus) {
  case FF_PANEL:
    if (dir > 0)
      focus_set(f, FF_URI);
    else {
      f->button = B_COUNT - 1;
      focus_set(f, FF_BUTTON);
    }
    break;
  case FF_URI:
    if (dir > 0) {
      f->field = first_field(f);
      focus_set(f, FF_FIELD);
    } else {
      focus_set(f, FF_PANEL);
    }
    break;
  case FF_FIELD:
    if (dir > 0 && f->field == last_field(f)) {
      f->button = 0;
      f->focus = FF_BUTTON;
    } else if (dir < 0 && f->field == first_field(f)) {
      f->focus = FF_URI;
    } else {
      f->field = conn_fields_next(&f->fields, f->field, dir);
    }
    break;
  case FF_BUTTON:
    if (dir > 0 && f->button == B_COUNT - 1)
      focus_set(f, FF_PANEL);
    else if (dir < 0 && f->button == 0) {
      f->field = last_field(f);
      f->focus = FF_FIELD;
    } else {
      f->button += dir;
    }
    break;
  }
}

/* The keys of the panel's filter line */
static bool filter_key(TuiState *state, FormTab *f, const UiEvent *ev) {
  if (render_event_is_special(ev, UI_KEY_ESCAPE)) {
    text_input_set(&f->tree.filter, "");
    f->filter_focused = false;
    f->highlighted = false;
    return true;
  }
  if (render_event_is_special(ev, UI_KEY_ENTER) || render_event_is_special(ev, UI_KEY_DOWN) ||
      render_event_is_special(ev, UI_KEY_TAB)) {
    f->filter_focused = false;
    (void)state;
    return true;
  }
  char *before = str_dup(f->tree.filter.buf ? f->tree.filter.buf : "");
  bool used = text_input_handle_key(&f->tree.filter, ev);
  if (!str_eq(before, f->tree.filter.buf ? f->tree.filter.buf : "")) {
    f->highlighted = false; /* the rows changed under the highlight */
    f->pos_kind = PR_NOTE;
  }
  free(before);
  return used || plain_char(ev);
}

bool form_tab_handle_key(TuiState *state, Tab *tab, const UiEvent *ev) {
  FormTab *f = form_of(state, tab);
  if (!f || ev->type != UI_EVENT_KEY)
    return false;
  const Config *cfg = state->app->config;
  free(f->fields.message); /* a message lasts until the next key */
  f->fields.message = NULL;
  if (f->tree.move.active) { /* a move owns the keyboard until dropped or cancelled */
    connect_tree_move_key(&f->tree, ev);
    if (!f->tree.move.active) {
      f->pos_kind = PR_TREE;
      f->pos_index = f->tree.highlight;
      tree_changed(state, f);
    }
    return true;
  }
  if (f->filter_focused)
    return filter_key(state, f, ev);
  bool typing = f->focus == FF_URI || (f->focus == FF_FIELD && f->field != CF_DRIVER && !conn_fields_is_check(f->field));
  /* A focused button, or a folder in the panel, takes Enter for itself */
  if (render_event_is_special(ev, UI_KEY_ENTER) || (f->focus == FF_BUTTON && plain_char(ev) && ev->key.key == ' ')) {
    if (f->focus == FF_BUTTON)
      return form_tab_do_action(state, tab, button_actions[f->button]);
    build_rows(state, f);
    const PanelRow *r = f->focus == FF_PANEL ? highlighted_row(f) : NULL;
    if (r && r->kind == PR_TREE && connmgr_is_folder(r->item)) {
      connmgr_toggle_folder(r->item);
      tui_connmgr(state)->modified = true;
      persist(state, f);
      return true;
    }
  }
  /* The buttons' keys; a plain letter bound to one types while a text field has the focus */
  for (int b = 0; b < B_COUNT; b++)
    if (!(typing && plain_char(ev)) && hotkey_matches(cfg, ev, button_actions[b]))
      return form_tab_do_action(state, tab, button_actions[b]);
  if (render_event_is_special(ev, UI_KEY_TAB) || render_event_is_special(ev, UI_KEY_BTAB)) {
    focus_step(f, render_event_is_special(ev, UI_KEY_TAB) ? 1 : -1);
    return true;
  }
  if (render_event_is_special(ev, UI_KEY_ESCAPE)) {
    if (f->focus == FF_PANEL)
      return false;
    if (f->focus == FF_URI && f->uri_edited && f->uri.len) { /* the typed text goes, the fields stay */
      sync_uri(f);
      return true;
    }
    focus_set(f, f->focus == FF_URI ? FF_PANEL : FF_URI);
    return true;
  }
  switch (f->focus) {
  case FF_PANEL: {
    for (size_t i = 0; i < sizeof tree_actions / sizeof *tree_actions; i++)
      if (hotkey_matches(cfg, ev, tree_actions[i]))
        return tree_action(state, f, tree_actions[i]);
    if (render_event_is_special(ev, UI_KEY_LEFT) || render_event_is_special(ev, UI_KEY_RIGHT)) {
      build_rows(state, f);
      const PanelRow *r = highlighted_row(f);
      if (r && r->kind == PR_TREE) {
        f->tree.highlight = r->index;
        connect_tree_left_right(&f->tree, render_event_is_special(ev, UI_KEY_RIGHT));
        f->pos_index = f->tree.highlight;
        persist(state, f);
      }
      return true;
    }
    size_t to = nav_target(state, f, ev);
    if (to == SIZE_MAX)
      return false;
    request_highlight(state, f, to);
    return true;
  }
  case FF_URI: {
    if (render_event_is_special(ev, UI_KEY_UP) || render_event_is_special(ev, UI_KEY_DOWN) ||
        render_event_is_special(ev, UI_KEY_PAGEUP) || render_event_is_special(ev, UI_KEY_PAGEDOWN)) {
      size_t to = nav_target(state, f, ev);
      if (to != SIZE_MAX)
        request_highlight(state, f, to);
      return true;
    }
    /* An empty line leaves the keys no URI starts with to the global hotkeys */
    if (!f->uri.len && plain_char(ev) && ev->key.key < 128 && strchr("?{}[]q,w", ev->key.key))
      return false;
    char *before = str_dup(f->uri.buf ? f->uri.buf : "");
    bool used = text_input_handle_key(&f->uri, ev);
    if (!str_eq(before, f->uri.buf ? f->uri.buf : ""))
      f->uri_edited = true;
    str_secure_free(before);
    return used || plain_char(ev);
  }
  case FF_FIELD: {
    int fld = f->field;
    if (render_event_is_special(ev, UI_KEY_UP)) {
      if (fld == first_field(f))
        f->focus = FF_URI;
      else
        f->field = conn_fields_next(&f->fields, fld, -1);
      return true;
    }
    if (render_event_is_special(ev, UI_KEY_DOWN)) {
      if (fld == last_field(f)) {
        f->button = 0;
        f->focus = FF_BUTTON;
      } else {
        f->field = conn_fields_next(&f->fields, fld, 1);
      }
      return true;
    }
    if (fld == CF_DRIVER) {
      bool right = render_event_is_special(ev, UI_KEY_RIGHT) || (plain_char(ev) && ev->key.key == ' ');
      if (right || render_event_is_special(ev, UI_KEY_LEFT)) {
        int n = conn_fields_num_drivers(&f->fields);
        f->fields.driver = (f->fields.driver + (right ? 1 : n - 1)) % n;
        return true;
      }
      return plain_char(ev); /* letters do nothing here, and are not hotkeys */
    }
    if (conn_fields_is_check(fld)) {
      if (plain_char(ev) && ev->key.key == ' ') {
        f->fields.chk[fld] = !f->fields.chk[fld];
        return true;
      }
      return plain_char(ev);
    }
    return text_input_handle_key(&f->fields.in[fld], ev) || plain_char(ev);
  }
  case FF_BUTTON:
    if (render_event_is_special(ev, UI_KEY_LEFT)) {
      f->button = (f->button + B_COUNT - 1) % B_COUNT;
      return true;
    }
    if (render_event_is_special(ev, UI_KEY_RIGHT)) {
      f->button = (f->button + 1) % B_COUNT;
      return true;
    }
    if (render_event_is_special(ev, UI_KEY_UP)) {
      f->field = last_field(f);
      f->focus = FF_FIELD;
      return true;
    }
    return false;
  }
  return false;
}

/* ---- mouse ---- */

bool form_tab_handle_mouse(TuiState *state, Tab *tab, int y, int x, bool double_click, int wheel) {
  FormTab *f = form_of(state, tab);
  if (!f)
    return false;
  free(f->fields.message);
  f->fields.message = NULL;
  if (f->tree.move.active)
    return true;
  if (f->list_cols > 0 && x >= f->list_x && x < f->list_x + f->list_cols && y >= f->list_y &&
      y < f->list_y + f->list_rows) {
    focus_set(f, FF_PANEL);
    build_rows(state, f);
    if (wheel) {
      UiEvent ev = {.type = UI_EVENT_KEY, .key = {.key = wheel < 0 ? UI_KEY_UP : UI_KEY_DOWN, .is_special = true}};
      size_t to = nav_target(state, f, &ev);
      if (to != SIZE_MAX)
        request_highlight(state, f, to);
      return true;
    }
    size_t i = f->scroll + (size_t)(y - f->list_y);
    if (i >= f->num_rows || !selectable(&f->rows[i]))
      return true;
    request_highlight(state, f, i);
    if (double_click && f->highlighted && cursor_row(f) == i) {
      PanelRow *r = &f->rows[i];
      if (r->kind == PR_TREE && connmgr_is_folder(r->item)) {
        connmgr_toggle_folder(r->item);
        tui_connmgr(state)->modified = true;
        persist(state, f);
      } else {
        return form_tab_do_action(state, tab, HOTKEY_CONN_OPEN); /* may free f */
      }
    }
    return true;
  }
  if (wheel)
    return true;
  if (y == f->uri_y && x >= f->uri_x0 && x < f->uri_x1) {
    focus_set(f, FF_URI);
    return true;
  }
  for (int b = 0; b < B_COUNT; b++)
    if (y == f->button_y[b] && x >= f->button_x0[b] && x < f->button_x1[b]) {
      focus_set(f, FF_BUTTON);
      f->button = b;
      return form_tab_do_action(state, tab, button_actions[b]); /* may free f */
    }
  for (int i = 0; i < CF_COUNT; i++) {
    if (f->field_y[i] != y)
      continue;
    focus_set(f, FF_FIELD);
    f->field = i;
    if (conn_fields_is_check(i))
      f->fields.chk[i] = !f->fields.chk[i];
    return true;
  }
  return true;
}

/* ---- drawing ---- */

/* left text, and right text against the right edge (dropped when it does not fit) */
static void draw_two(WINDOW *win, int y, int x, int w, const char *left, const char *right, attr_t right_attr) {
  int rw = right && right[0] ? utf8_width(right, strlen(right)) : 0;
  if (rw > w / 2)
    rw = 0;
  tui_print_clip(win, y, x, rw ? w - rw - 1 : w, left);
  if (rw) {
    wattron(win, right_attr);
    tui_print_clip(win, y, x + w - rw, rw, right);
    wattroff(win, right_attr);
  }
}

static void draw_panel_row(TuiState *state, FormTab *f, WINDOW *win, const PanelRow *r, int y, int x, int w,
                           bool cur) {
  attr_t base = A_NORMAL;
  bool panel = f->focus == FF_PANEL && !f->filter_focused;
  if (cur && (f->highlighted || panel))
    base = panel ? (attr_t)(COLOR_PAIR(COLOR_SELECTED) | A_BOLD) : A_REVERSE;
  if (r->kind == PR_MOVING)
    base = COLOR_PAIR(COLOR_SELECTED) | A_BOLD;
  if (r->kind == PR_HEADER) {
    WITH_ATTR(win, COLOR_PAIR(COLOR_TITLE) | A_BOLD, tui_print_clip(win, y, x, w, r->text));
    return;
  }
  if (r->kind == PR_NOTE) {
    WITH_ATTR(win, A_DIM, tui_print_clip(win, y, x + 2, w - 2, r->text));
    return;
  }
  char *left = NULL, *right = NULL;
  attr_t right_attr = cur ? A_NORMAL : A_DIM;
  if (r->kind == PR_RECENT) {
    RecentList *rl = tui_recent(state);
    char *d = r->index < rl->count ? short_name(rl->items[r->index].url) : str_dup("");
    left = str_printf("  %s", d);
    free(d);
  } else {
    ConnectionItem *item = r->item;
    int indent = r->depth * 2;
    if (indent > w / 2)
      indent = w / 2;
    const char *name = connmgr_item_name(item);
    if (connmgr_is_folder(item)) {
      bool open = item->folder.expanded || conn_tree_filter(&f->tree);
      left = open ? str_printf("%*s\xE2\x96\xBE %s", indent, "", name ? name : "")
                  : str_printf("%*s\xE2\x96\xB8 %s (%zu)", indent, "", name ? name : "", item->folder.num_children);
    } else {
      left = str_printf("%*s%s %s", indent, "", r->kind == PR_MOVING ? "\xC2\xBB" : " ", name ? name : "");
      char *in = open_in(state->app, item->connection.id);
      if (in && in[0] && r->kind == PR_TREE) {
        right = str_printf("\xE2\x97\x8F open in %s", in);
        if (!cur)
          right_attr = COLOR_PAIR(COLOR_NUMBER);
      }
      free(in);
    }
  }
  wattron(win, base);
  mvwhline(win, y, x, ' ', w);
  draw_two(win, y, x, w, left ? left : "", right, right_attr);
  wattroff(win, base);
  free(left);
  free(right);
}

static void draw_panel(TuiState *state, FormTab *f, WINDOW *win, int y, int x, int rows, int cols) {
  if (f->filter_focused || f->tree.filter.len) { /* the filter line on top */
    WITH_ATTR(win, COLOR_PAIR(COLOR_TITLE) | A_BOLD, mvwaddch(win, y, x, '/'));
    text_input_draw(win, &f->tree.filter, y, x + 2, cols - 3, A_NORMAL, f->filter_focused);
    y++;
    rows--;
  }
  f->list_y = y;
  f->list_x = x;
  f->list_rows = rows;
  f->list_cols = cols;
  build_rows(state, f);
  size_t cur = cursor_row(f);
  size_t h = rows > 0 ? (size_t)rows : 1;
  if (cur != SIZE_MAX) {
    if (cur < f->scroll)
      f->scroll = cur;
    if (cur > 0 && cur - 1 < f->scroll && f->rows[cur - 1].kind == PR_HEADER)
      f->scroll = cur - 1;
    if (cur >= f->scroll + h)
      f->scroll = cur - h + 1;
  }
  if (f->scroll + h > f->num_rows)
    f->scroll = f->num_rows > h ? f->num_rows - h : 0;
  for (size_t i = 0; i < h && f->scroll + i < f->num_rows; i++)
    draw_panel_row(state, f, win, &f->rows[f->scroll + i], y + (int)i, x, cols, f->scroll + i == cur);
}

void form_tab_draw(TuiState *state, Tab *tab) {
  FormTab *f = form_of(state, tab);
  WINDOW *win = state->main_win;
  if (!f || !win)
    return;
  werase(win);
  int rows, w_all;
  getmaxyx(win, rows, w_all);
  for (int i = 0; i < CF_COUNT; i++)
    f->field_y[i] = -1;
  for (int b = 0; b < B_COUNT; b++)
    f->button_y[b] = -1;
  f->uri_y = -1;
  f->list_cols = 0;
  int x0 = 1;
  if (w_all < NARROW_COLS) {
    if (f->focus == FF_PANEL || f->tree.move.active) { /* narrow: the panel in the form's place */
      draw_panel(state, f, win, 0, 0, rows, w_all);
      wrefresh(win);
      return;
    }
  } else {
    int lw = w_all / 3;
    if (lw < 24)
      lw = 24;
    if (lw > 40)
      lw = 40;
    draw_panel(state, f, win, 0, 0, rows, lw);
    wattron(win, COLOR_PAIR(COLOR_BORDER));
    mvwvline(win, 0, lw, ACS_VLINE, rows);
    wattroff(win, COLOR_PAIR(COLOR_BORDER));
    x0 = lw + 2;
  }
  int w = w_all - x0 - 1;
  if (w < LABEL_COLS + 4) {
    wrefresh(win);
    return;
  }
  int vw = w - LABEL_COLS;
  /* The URI line */
  if (f->focus != FF_URI)
    sync_uri(f);
  bool uf = f->focus == FF_URI && !f->filter_focused;
  WITH_ATTR(win, uf ? (attr_t)(COLOR_PAIR(COLOR_TITLE) | A_BOLD) : A_NORMAL, tui_print_clip(win, 0, x0, LABEL_COLS, "URI"));
  if (!f->uri.len && !uf) {
    WITH_ATTR(win, A_DIM, tui_print_clip(win, 0, x0 + LABEL_COLS, vw, "a URL or a SQLite file, or pick on the left"));
  } else {
    text_input_draw(win, &f->uri, 0, x0 + LABEL_COLS, vw, A_UNDERLINE, uf);
  }
  f->uri_y = 0;
  f->uri_x0 = x0;
  f->uri_x1 = x0 + w;
  if (rows > 1) {
    wattron(win, COLOR_PAIR(COLOR_BORDER));
    mvwhline(win, 1, x0, ACS_HLINE, w);
    wattroff(win, COLOR_PAIR(COLOR_BORDER));
  }
  /* The fields, scrolled so the focused one shows; 5 rows stay for the buttons and the message */
  int body = rows - 2 - 5;
  if (body < 1)
    body = 1;
  int nvis = 0, focus_row = 0;
  for (int i = 0; i < CF_COUNT; i++) {
    if (!conn_fields_visible(&f->fields, i))
      continue;
    if (f->focus == FF_FIELD && i == f->field)
      focus_row = nvis;
    nvis++;
  }
  int first = focus_row >= body ? focus_row - body + 1 : 0;
  int n = 0, y = 2;
  for (int i = 0; i < CF_COUNT; i++) {
    if (!conn_fields_visible(&f->fields, i))
      continue;
    int r = n++ - first;
    if (r < 0 || r >= body)
      continue;
    int fy = y + r;
    f->field_y[i] = fy;
    bool foc = f->focus == FF_FIELD && i == f->field && !f->filter_focused;
    WITH_ATTR(win, foc ? (attr_t)(COLOR_PAIR(COLOR_TITLE) | A_BOLD) : A_NORMAL,
              tui_print_clip(win, fy, x0, LABEL_COLS, conn_fields_labels[i]));
    int vx = x0 + LABEL_COLS;
    int fw = vw < 48 ? vw : 48;
    if (i == CF_DRIVER) {
      char *d = str_printf("\xe2\x80\xb9 %s \xe2\x80\xba", conn_fields_driver_name(&f->fields, f->fields.driver));
      WITH_ATTR(win, foc ? A_REVERSE : A_UNDERLINE, tui_print_clip(win, fy, vx, fw, d));
      free(d);
    } else if (conn_fields_is_check(i)) {
      WITH_ATTR(win, foc ? A_REVERSE : A_NORMAL, tui_print_clip(win, fy, vx, 3, f->fields.chk[i] ? "[x]" : "[ ]"));
    } else if (!f->fields.in[i].len && i == CF_PORT && !foc) {
      char *d = str_printf("%d", lace_driver_default_port(conn_fields_driver_name(&f->fields, f->fields.driver)));
      WITH_ATTR(win, A_DIM | A_UNDERLINE, tui_print_fit(win, fy, vx, fw, d));
      free(d);
    } else {
      text_input_draw(win, &f->fields.in[i], fy, vx, fw, A_UNDERLINE, foc);
    }
  }
  int shown = nvis - first < body ? nvis - first : body;
  int by = y + shown + 1;
  /* The buttons: open actions, then manage actions */
  for (int row = 0; row < 2; row++) {
    int bx = x0;
    for (int b = row * 4; b < row * 4 + 4; b++) {
      char *label = str_printf("[%s]", button_labels[b]);
      int bw = utf8_width(label, strlen(label));
      bool foc = f->focus == FF_BUTTON && f->button == b;
      if (bx + bw <= x0 + w && by + row < rows) {
        attr_t a = foc ? A_REVERSE : b == B_OPEN ? A_BOLD : A_NORMAL;
        WITH_ATTR(win, a, tui_print_clip(win, by + row, bx, bw, label));
        f->button_y[b] = by + row;
        f->button_x0[b] = bx;
        f->button_x1[b] = bx + bw;
      }
      bx += bw + 1;
      free(label);
    }
  }
  if (by + 3 < rows) {
    const char *msg = f->fields.message;
    char *own = NULL;
    attr_t a = f->fields.message_error ? COLOR_PAIR(COLOR_ERROR) : COLOR_PAIR(COLOR_NUMBER);
    if (!msg && tui_connmgr(state)->read_only && state->connmgr_error) {
      msg = own = str_printf("connections.json: %s (changes will not be saved)", state->connmgr_error);
      a = COLOR_PAIR(COLOR_ERROR);
    } else if (!msg && f->tree.move.active) {
      ConnectionItem *t = f->tree.move.target_folder;
      msg = own = str_printf("Moving to %s: Up/Down choose the place, Space or Enter drops it, Esc cancels",
                             !t || t == &f->tree.mgr->root ? "the top level" : connmgr_item_name(t));
      a = A_DIM;
    }
    if (msg) {
      char *flat = connect_flatten_message(msg);
      WITH_ATTR(win, a, tui_print_clip(win, by + 3, x0, w, flat ? flat : ""));
      free(flat);
    }
    free(own);
  }
  wrefresh(win);
}

void form_tab_status(TuiState *state, Tab *tab, char *buf, size_t n) {
  FormTab *f = form_of(state, tab);
  if (!f || !n)
    return;
  const Config *c = state->app->config;
  if (f->focus == FF_PANEL && !f->filter_focused) {
    char a[16], b[16], d[16], e[16], g[16], h[16];
    snprintf(a, sizeof a, "%s", connect_key_hint(c, HOTKEY_CONN_NEW_FOLDER, "N"));
    snprintf(b, sizeof b, "%s", connect_key_hint(c, HOTKEY_CONN_RENAME, "r"));
    snprintf(d, sizeof d, "%s", connect_key_hint(c, HOTKEY_CONN_DUPLICATE, "d"));
    snprintf(e, sizeof e, "%s", connect_key_hint(c, HOTKEY_CONN_DELETE, "x"));
    snprintf(g, sizeof g, "%s", connect_key_hint(c, HOTKEY_CONN_MOVE, "Space"));
    snprintf(h, sizeof h, "%s", connect_key_hint(c, HOTKEY_CONN_FILTER, "/"));
    snprintf(buf, n, "Enter open  %s folder  %s rename  %s duplicate  %s delete  %s move  %s filter  Tab form", a, b,
             d, e, g, h);
    return;
  }
  char k[B_COUNT][16];
  for (int i = 0; i < B_COUNT; i++)
    snprintf(k[i], sizeof k[i], "%s", connect_key_hint(c, button_actions[i], ""));
  snprintf(buf, n, "%s open  %s background  %s new workspace  %s to workspace  %s save  %s save as new  %s test  %s clear",
           k[B_OPEN], k[B_BACKGROUND], k[B_NEW_WS], k[B_TO_WS], k[B_SAVE], k[B_SAVE_AS], k[B_TEST], k[B_CLEAR]);
}

void form_tab_set_connstr(TuiState *state, Tab *tab, const char *connstr) {
  FormTab *f = form_of(state, tab);
  if (!f)
    return;
  conn_fields_clear(&f->fields);
  if (connstr && *connstr)
    conn_fields_set_uri(&f->fields, connstr);
  conn_fields_mark_clean(&f->fields);
  set_highlight(f, NULL, NULL);
  f->highlighted = false;
  sync_uri(f);
}
