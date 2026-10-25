/* The connection form tab. Left: the saved connections tree and Recent
 * (ui/conn_picker.c); right: the URI line over the fields (ui/conn_form.c),
 * the open and manage buttons, a message line. Highlighting a saved
 * connection or a recent URL fills the fields, asking first when they hold
 * edits not saved. */
#include "ui/form_tab.h"
#include "config/connections.h"
#include "model/workspace.h"
#include "ui/conn_form.h"
#include "ui/conn_picker.h"
#include "ui/dialogs/confirm.h"
#include "ui/dialogs/pick.h"
#include "ui/dialogs/prompt.h"
#include "ui/grid.h"
#include "ui/input.h"
#include "util/mem.h"
#include "util/str.h"
#include <notcurses/notcurses.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef enum { FF_LIST, FF_URI, FF_FIELD, FF_BUTTON } FormFocus;
enum { B_OPEN, B_BACKGROUND, B_NEW_WS, B_TO_WS, B_SAVE, B_SAVE_AS, B_TEST, B_CLEAR, B_COUNT };
static const char *const button_labels[B_COUNT] = {
    "Open here", "Background tab", "New workspace", "To workspace\xe2\x80\xa6",
    "Save", "Save as new", "Test", "Clear"};
static const HotkeyAction button_actions[B_COUNT] = {
    HOTKEY_CONN_OPEN, HOTKEY_CONN_OPEN_BACKGROUND, HOTKEY_CONN_OPEN_NEW_WORKSPACE, HOTKEY_CONN_OPEN_TO_WORKSPACE,
    HOTKEY_CONN_SAVE, HOTKEY_CONN_SAVE_AS_NEW, HOTKEY_CONN_TEST, HOTKEY_CONN_CLEAR};

#define LABEL_COLS 19   /* the field labels' column ("Save SSH password") */
#define NARROW_COLS 70  /* below this the left panel is shown in the form's place, on request */

struct FormTab {
  ConnFields fields;
  TextInput uri;        /* the URI line as typed; rebuilt from the fields while not focused */
  bool uri_edited;      /* typed into since the fields were last applied to it */
  ConnPicker panel;
  bool panel_ready;     /* cp_init needs the app: done at the first draw or key */
  bool highlighted;     /* a row of the panel was picked */
  char *highlight_id;   /* the picked saved connection's id */
  char *highlight_name; /* and its name: the tab's label */
  FormFocus focus;
  int field, button;
  bool filter_focused;  /* the panel's filter line takes the keys */
  int filter_y, filter_cursor_x;
  /* Where the last draw put things (screen coordinates), for the mouse */
  int list_y, list_x, list_rows, list_cols;
  int uri_y, uri_x0, uri_x1;
  int field_y[CF_COUNT]; /* -1: not shown */
  int button_y[B_COUNT], button_x0[B_COUNT], button_x1[B_COUNT];
};

FormTab *form_tab_new(void) {
  FormTab *f = safe_calloc(1, sizeof *f);
  conn_fields_init(&f->fields);
  text_input_init(&f->uri, NULL, false);
  f->focus = FF_URI;
  f->field = CF_NAME;
  return f;
}

void form_tab_free(FormTab *f) {
  if (!f) return;
  conn_fields_free(&f->fields);
  text_input_free(&f->uri);
  if (f->panel_ready) cp_free(&f->panel);
  free(f->highlight_id);
  free(f->highlight_name);
  free(f);
}

const char *form_tab_label(const FormTab *f) {
  return f && f->highlight_name ? f->highlight_name : "New connection";
}

static void ready(NcApp *app, FormTab *f) {
  if (f->panel_ready) return;
  cp_init(app, &f->panel, false);
  f->panel_ready = true;
}

static const char *filter_text(const FormTab *f) { return f->panel.in.buf ? f->panel.in.buf : ""; }

/* The saved connection or folder behind a panel row */
static ConnectionItem *row_item(NcApp *app, FormTab *f, const ListRow *r) {
  if (!r || !app->connmgr || (r->kind != CPK_CONN && r->kind != CPK_FOLDER)) return NULL;
  return connmgr_get_filtered_item(app->connmgr, filter_text(f), (size_t)(intptr_t)r->data);
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
  if (!f->uri_edited) return true;
  if (!f->uri.len) { /* cleared: the fields stay as they are */
    sync_uri(f);
    return true;
  }
  if (!conn_fields_set_uri(&f->fields, f->uri.buf)) return false;
  sync_uri(f);
  return true;
}

/* Edits not saved: in the fields, or typed into the URI line */
static bool form_dirty(const FormTab *f) { return f->uri_edited || conn_fields_dirty(&f->fields); }

/* The panel row at pos becomes the highlight and fills the form */
static void apply_highlight(NcApp *app, FormTab *f, size_t pos) {
  ListWidget *l = &f->panel.list;
  if (pos >= l->num_shown) return;
  l->cursor = pos;
  f->highlighted = true;
  ListRow *r = list_selected(l);
  if (!r) return;
  ConnectionItem *it = row_item(app, f, r);
  if (r->kind == CPK_CONN && it) {
    conn_fields_fill(&f->fields, &it->connection);
    set_highlight(f, it->connection.id, it->connection.name);
  } else if (r->kind == CPK_RECENT && (size_t)(intptr_t)r->data < app->recent.count) {
    conn_fields_clear(&f->fields);
    conn_fields_set_uri(&f->fields, app->recent.items[(size_t)(intptr_t)r->data].url);
    conn_fields_mark_clean(&f->fields);
    set_highlight(f, NULL, NULL);
  } else { /* a folder: the form stays as it is */
    set_highlight(f, NULL, NULL);
  }
  sync_uri(f);
}

/* The tab with that uid, when it is still a form */
static FormTab *form_of(NcApp *app, uint64_t uid, ConnTab **tab) {
  ConnTab *t = app_find_tab(app, uid, NULL);
  if (tab) *tab = t;
  return t ? t->form : NULL;
}

typedef struct { uint64_t uid; size_t pos; } MoveCtx;

static void drop_confirmed(NcApp *app, bool yes, void *data) {
  MoveCtx *m = data;
  FormTab *f = form_of(app, m->uid, NULL);
  if (yes && f) apply_highlight(app, f, m->pos);
  free(m);
}

/* Highlight row pos: at once when the form holds no edits, else after asking */
static void request_highlight(NcApp *app, ConnTab *t, FormTab *f, size_t pos) {
  if (f->highlighted && pos == f->panel.list.cursor) return;
  if (!form_dirty(f)) {
    apply_highlight(app, f, pos);
    return;
  }
  MoveCtx *m = safe_malloc(sizeof *m);
  m->uid = t->uid;
  m->pos = pos;
  confirm_open(app, "Unsaved changes", "Drop the changes in the form?", "Drop", true, drop_confirmed, m);
}

static bool is_nav(const NcApp *app, const UiEvent *ev) {
  if (ev->key.is_special && !(ev->key.mods & (UI_MOD_CTRL | UI_MOD_ALT))) {
    int k = ev->key.key;
    if (k == UI_KEY_UP || k == UI_KEY_DOWN || k == UI_KEY_PAGEUP || k == UI_KEY_PAGEDOWN || k == UI_KEY_HOME ||
        k == UI_KEY_END)
      return true;
  }
  static const HotkeyAction nav[] = {HOTKEY_MOVE_UP, HOTKEY_MOVE_DOWN, HOTKEY_PAGE_UP, HOTKEY_PAGE_DOWN,
                                     HOTKEY_FIRST_ROW, HOTKEY_LAST_ROW};
  for (size_t i = 0; i < sizeof nav / sizeof *nav; i++)
    if (hotkey_matches(app->config, ev, nav[i])) return true;
  return false;
}

/* A navigation key or a mouse event on the panel moves the highlight (after
 * asking when the form holds edits); false for any other key */
static bool list_move(NcApp *app, ConnTab *t, FormTab *f, const UiEvent *ev) {
  ListWidget *l = &f->panel.list;
  size_t old = l->cursor;
  if (ev->type == UI_EVENT_KEY) {
    if (!is_nav(app, ev)) return false;
    if (!f->highlighted) { /* the first move picks the row the cursor waits on */
      if (list_selected(l)) request_highlight(app, t, f, old);
      return true;
    }
    list_key(l, ev);
  } else {
    ListAction a = list_mouse(l, ev, f->list_y, f->list_rows);
    if (a != LIST_ACTIVATE && a != LIST_CHANGED) return true;
  }
  size_t target = l->cursor;
  l->cursor = old;
  if (target != old || !f->highlighted) request_highlight(app, t, f, target);
  return true;
}

/* ---- actions ---- */

static void open_with(NcApp *app, ConnTab *t, FormTab *f, NcDestKind kind, const NcWorkspace *ws) {
  NcDest dest = {kind, t->uid, ws};
  if (!commit_uri(f)) return; /* what the line shows does not parse: nothing opens */
  if (f->highlighted && f->highlight_id && !conn_fields_dirty(&f->fields)) {
    char *id = str_dup(f->highlight_id); /* Open here frees the form */
    app_connect_saved(app, id, dest, NULL, NULL);
    free(id);
    return;
  }
  char *cs = conn_fields_connstr(&f->fields);
  if (!cs) return;
  const char *nm = f->fields.in[CF_NAME].buf;
  char *name = nm && *nm ? str_dup(nm) : NULL;
  app_connect_url(app, cs, NULL, name, dest, NULL, NULL);
  str_secure_free(cs);
  free(name);
}

typedef struct { uint64_t uid; NcWorkspace **wss; size_t n; } ToWs;

static void to_workspace_picked(NcApp *app, size_t index, void *data) {
  ToWs *c = data;
  ConnTab *t;
  FormTab *f = form_of(app, c->uid, &t);
  if (f && index < c->n) {
    bool live = false;
    for (size_t i = 0; i < app->num_workspaces; i++) live |= app->workspaces[i] == c->wss[index];
    if (live) open_with(app, t, f, NC_DEST_WORKSPACE, c->wss[index]);
  }
  free(c->wss);
  free(c);
}

static void to_workspace(NcApp *app, ConnTab *t, FormTab *f) {
  NcWorkspace *here = app_current_workspace(app);
  ToWs *c = safe_calloc(1, sizeof *c);
  c->uid = t->uid;
  c->wss = safe_calloc(app->num_workspaces ? app->num_workspaces : 1, sizeof *c->wss);
  char **names = safe_calloc(app->num_workspaces ? app->num_workspaces : 1, sizeof *names);
  for (size_t i = 0; i < app->num_workspaces; i++) {
    if (app->workspaces[i] == here) continue;
    c->wss[c->n] = app->workspaces[i];
    names[c->n++] = app->workspaces[i]->name;
  }
  if (!c->n) {
    conn_fields_set_message(&f->fields, true, "No other workspace");
    free(names);
    free(c->wss);
    free(c);
    return;
  }
  pick_open(app, "Open in workspace", names, c->n, to_workspace_picked, c);
  free(names);
}

static void test_done(NcApp *app, ConnTab *tab, const char *error, void *data) {
  (void)tab;
  uint64_t *uid = data;
  FormTab *f = form_of(app, *uid, NULL);
  if (f) {
    if (error) conn_fields_set_message(&f->fields, true, "%s", error);
    else conn_fields_set_message(&f->fields, false, "Connection OK");
  }
  free(uid);
}

static void test(NcApp *app, ConnTab *t, FormTab *f) {
  if (!commit_uri(f)) return;
  LaceSavedConnection *m = conn_fields_collect(&f->fields, true);
  char *cs = m ? conn_fields_connstr(&f->fields) : NULL;
  if (cs) {
    uint64_t *uid = safe_malloc(sizeof *uid);
    *uid = t->uid;
    conn_fields_set_message(&f->fields, false, "Testing\xe2\x80\xa6");
    app_test_connection(app, cs, m, test_done, uid);
  }
  str_secure_free(cs);
  if (m) lace_saved_conn_free(m);
}

static void clear(FormTab *f) {
  conn_fields_clear(&f->fields);
  set_highlight(f, NULL, NULL);
  f->highlighted = false;
  text_input_set(&f->uri, "");
  f->uri_edited = false;
  f->focus = FF_URI;
}

/* The panel's cursor onto the saved connection with that id (the list is rebuilt first) */
static void select_saved(NcApp *app, FormTab *f, const char *id) {
  cp_changed();
  cp_rebuild(app, &f->panel);
  ListWidget *l = &f->panel.list;
  for (size_t i = 0; i < l->num_shown; i++) {
    const ListRow *r = &l->rows[l->shown[i]];
    ConnectionItem *it = r->kind == CPK_CONN ? row_item(app, f, r) : NULL;
    if (it && it->connection.id && !strcmp(it->connection.id, id)) {
      l->cursor = i;
      f->highlighted = true;
      return;
    }
  }
}

static void replace_connection(LaceSavedConnection *dst, LaceSavedConnection *src) {
  char *id = dst->id;
  dst->id = NULL;
  connmgr_free_connection(dst);
  *dst = *src;
  dst->id = id;
  free(src);
}

/* Write c (taken over) to connections.json: over the highlighted entry, or as a new one */
static void store(NcApp *app, ConnTab *t, FormTab *f, LaceSavedConnection *c, bool as_new) {
  (void)t;
  if (!app->connmgr) {
    lace_saved_conn_free(c);
    conn_fields_set_message(&f->fields, true, "connections.json is not available");
    return;
  }
  ConnectionItem *it = !as_new && f->highlight_id ? connmgr_find_by_id(app->connmgr, f->highlight_id) : NULL;
  char *id;
  if (it && connmgr_is_connection(it)) {
    id = str_dup(it->connection.id);
    replace_connection(&it->connection, c);
  } else {
    /* next to the highlighted entry, or into the highlighted folder */
    ConnectionItem *parent = &app->connmgr->root;
    ConnectionItem *near = f->highlighted ? row_item(app, f, list_selected(&f->panel.list)) : NULL;
    if (near && connmgr_is_folder(near)) parent = near;
    else if (near && near->parent) parent = near->parent;
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
  if (!connmgr_save(app->connmgr, &err)) {
    conn_fields_set_message(&f->fields, true, "Cannot save: %s", err ? err : "unknown error");
    free(err);
    free(id);
    return;
  }
  it = connmgr_find_by_id(app->connmgr, id);
  const char *name = it ? it->connection.name : NULL;
  set_highlight(f, id, name);
  if (name && !f->fields.in[CF_NAME].len) text_input_set(&f->fields.in[CF_NAME], name);
  conn_fields_mark_clean(&f->fields);
  select_saved(app, f, id);
  conn_fields_set_message(&f->fields, false, "Saved %s", name ? name : "");
  app_set_status(app, false, "Saved %s", name ? name : "the connection");
  free(id);
}

typedef struct { uint64_t uid; bool as_new; LaceSavedConnection *c; } StoreCtx;

static void store_confirmed(NcApp *app, bool yes, void *data) {
  StoreCtx *s = data;
  ConnTab *t;
  FormTab *f = form_of(app, s->uid, &t);
  if (!f) {
    lace_saved_conn_free(s->c);
    free(s);
    return;
  }
  if (!yes) { /* saved without the passwords */
    s->c->save_password = s->c->save_ssh_password = false;
    str_secure_free(s->c->password);
    str_secure_free(s->c->ssh_password);
    s->c->password = str_dup("");
    s->c->ssh_password = str_dup("");
    f->fields.chk[CF_SAVEPW] = f->fields.chk[CF_SAVESSHPW] = false;
  }
  store(app, t, f, s->c, s->as_new);
  free(s);
}

static bool holds(const LaceSavedConnection *e, bool saved, const char *pw, const char *typed) {
  return e && saved && pw && !strcmp(pw, typed);
}

static void save(NcApp *app, ConnTab *t, FormTab *f, bool as_new) {
  if (!commit_uri(f)) return;
  LaceSavedConnection *c = conn_fields_collect(&f->fields, false);
  if (!c) return;
  ConnectionItem *it = !as_new && f->highlight_id && app->connmgr ? connmgr_find_by_id(app->connmgr, f->highlight_id)
                                                                  : NULL;
  const LaceSavedConnection *e = it && connmgr_is_connection(it) ? &it->connection : NULL;
  bool new_pw = c->save_password && c->password && *c->password &&
                !holds(e, e && e->save_password, e ? e->password : NULL, c->password);
  bool new_ssh = c->save_ssh_password && c->ssh_password && *c->ssh_password &&
                 !holds(e, e && e->save_ssh_password, e ? e->ssh_password : NULL, c->ssh_password);
  if (!new_pw && !new_ssh) {
    store(app, t, f, c, as_new);
    return;
  }
  StoreCtx *s = safe_malloc(sizeof *s);
  s->uid = t->uid;
  s->as_new = as_new;
  s->c = c;
  confirm_open(app, "Save password", "Store the password in connections.json (readable only by you)?", "Store",
               false, store_confirmed, s);
}

/* ---- the tree: folders, rename, duplicate, delete, move ---- */

static void focus_set(FormTab *f, FormFocus to);

/* The highlighted panel row's tree item */
static ConnectionItem *current_item(NcApp *app, FormTab *f) {
  return f->highlighted ? row_item(app, f, list_selected(&f->panel.list)) : NULL;
}

static bool persist(NcApp *app, FormTab *f) {
  char *err = NULL;
  bool ok = connmgr_save(app->connmgr, &err);
  if (!ok) conn_fields_set_message(&f->fields, true, "Cannot save connections.json: %s", err ? err : "unknown error");
  free(err);
  cp_changed();
  cp_rebuild(app, &f->panel);
  return ok;
}

/* A prompt or confirmation about the highlighted item: the tab, and the item
 * again by its panel index (the dialogs are modal: the tree does not change) */
typedef struct { uint64_t uid; size_t idx; char *id; } ItemCtx;

static ItemCtx *item_ctx(ConnTab *t, FormTab *f, const ConnectionItem *it) {
  ItemCtx *c = safe_calloc(1, sizeof *c);
  c->uid = t->uid;
  ListRow *r = list_selected(&f->panel.list);
  c->idx = r ? (size_t)(intptr_t)r->data : 0;
  c->id = it && connmgr_is_connection(it) && it->connection.id ? str_dup(it->connection.id) : NULL;
  return c;
}

static ConnectionItem *ctx_item(NcApp *app, FormTab *f, const ItemCtx *c) {
  if (!app->connmgr) return NULL;
  if (c->id) return connmgr_find_by_id(app->connmgr, c->id);
  return connmgr_get_filtered_item(app->connmgr, filter_text(f), c->idx);
}

static void item_ctx_free(ItemCtx *c) {
  free(c->id);
  free(c);
}

/* Where a new item goes: the highlighted folder, the folder of the highlighted connection, or the top */
static ConnectionItem *target_folder(NcApp *app, FormTab *f) {
  ConnectionItem *it = current_item(app, f);
  if (it && connmgr_is_folder(it)) return it;
  if (it && it->parent) return it->parent;
  return &app->connmgr->root;
}

static void folder_named(NcApp *app, char *text, void *data) {
  ItemCtx *c = data;
  ConnTab *t;
  FormTab *f = form_of(app, c->uid, &t);
  if (f && text && *text && app->connmgr) {
    ConnectionItem *parent = target_folder(app, f);
    if (connmgr_is_folder(parent)) parent->folder.expanded = true;
    if (!connmgr_add_folder(parent, connmgr_new_folder(text)))
      conn_fields_set_message(&f->fields, true, "Cannot add the folder");
    else
      persist(app, f);
  }
  free(text);
  item_ctx_free(c);
}

static void renamed(NcApp *app, char *text, void *data) {
  ItemCtx *c = data;
  ConnTab *t;
  FormTab *f = form_of(app, c->uid, &t);
  ConnectionItem *it = f ? ctx_item(app, f, c) : NULL;
  if (it && text && *text) {
    char **name = connmgr_is_folder(it) ? &it->folder.name : &it->connection.name;
    free(*name);
    *name = str_dup(text);
    if (c->id && f->highlight_id && !strcmp(c->id, f->highlight_id)) { /* the form shows it */
      bool clean = !conn_fields_dirty(&f->fields);
      set_highlight(f, c->id, text);
      text_input_set(&f->fields.in[CF_NAME], text);
      if (clean) conn_fields_mark_clean(&f->fields);
    }
    persist(app, f);
  }
  free(text);
  item_ctx_free(c);
}

static size_t count_connections(const ConnectionItem *it) {
  if (!connmgr_is_folder(it)) return 1;
  size_t n = 0;
  for (size_t i = 0; i < it->folder.num_children; i++) n += count_connections(&it->folder.children[i]);
  return n;
}

static void delete_confirmed(NcApp *app, bool yes, void *data) {
  ItemCtx *c = data;
  ConnTab *t;
  FormTab *f = form_of(app, c->uid, &t);
  ConnectionItem *it = yes && f ? ctx_item(app, f, c) : NULL;
  if (it) {
    bool shown = c->id && f->highlight_id && !strcmp(c->id, f->highlight_id);
    connmgr_remove_item(app->connmgr, it);
    persist(app, f);
    if (shown) { /* the form showed it: a clean form goes with it */
      bool clean = !conn_fields_dirty(&f->fields);
      set_highlight(f, NULL, NULL);
      f->highlighted = false;
      if (clean) conn_fields_clear(&f->fields);
    }
  }
  item_ctx_free(c);
}

static void duplicate(NcApp *app, FormTab *f, ConnectionItem *it) {
  LaceSavedConnection *copy = lace_saved_conn_copy(&it->connection);
  char *orig = str_dup(it->connection.id);
  free(copy->id);
  copy->id = lace_generate_uuid();
  char *name = str_printf("%s copy", it->connection.name ? it->connection.name : "connection");
  free(copy->name);
  copy->name = name;
  char *id = str_dup(copy->id);
  if (!connmgr_add_connection(it->parent, copy)) { /* it may move: found again by id */
    lace_saved_conn_free(copy);
    conn_fields_set_message(&f->fields, true, "Cannot add the copy");
  } else {
    ConnectionItem *o = connmgr_find_by_id(app->connmgr, orig), *n = connmgr_find_by_id(app->connmgr, id);
    if (o && n) connmgr_move_item(app->connmgr, n, o->parent, o); /* right after the original */
    persist(app, f);
  }
  free(orig);
  free(id);
}

typedef struct { ItemCtx *item; ConnectionItem **folders; size_t n; } MoveTo;

static void collect_folders(ConnectionItem *it, const ConnectionItem *skip, int depth, ConnectionItem ***out,
                            char ***names, size_t *n) {
  if (it == skip || !connmgr_is_folder(it)) return;
  *out = safe_realloc(*out, (*n + 1) * sizeof **out);
  *names = safe_realloc(*names, (*n + 1) * sizeof **names);
  (*out)[*n] = it;
  (*names)[*n] = str_printf("%*s%s", depth * 2, "", depth ? it->folder.name : "Connections");
  (*n)++;
  for (size_t i = 0; i < it->folder.num_children; i++) collect_folders(&it->folder.children[i], skip, depth + 1, out, names, n);
}

static void move_picked(NcApp *app, size_t index, void *data) {
  MoveTo *m = data;
  ConnTab *t;
  FormTab *f = form_of(app, m->item->uid, &t);
  ConnectionItem *it = f ? ctx_item(app, f, m->item) : NULL;
  if (it && index < m->n) {
    ConnectionItem *to = m->folders[index]; /* the dialog was modal: the tree is as it was */
    to->folder.expanded = true;
    ConnectionItem *last = to->folder.num_children ? &to->folder.children[to->folder.num_children - 1] : NULL;
    /* at the end of the folder (NULL: an empty folder's start); already there: nothing to do */
    if (last == it || connmgr_move_item(app->connmgr, it, to, last)) {
      persist(app, f);
      if (m->item->id) select_saved(app, f, m->item->id);
    } else {
      conn_fields_set_message(&f->fields, true, "Cannot move it there");
    }
  }
  item_ctx_free(m->item);
  free(m->folders);
  free(m);
}

static void move_item(NcApp *app, ConnTab *t, FormTab *f, ConnectionItem *it) {
  MoveTo *m = safe_calloc(1, sizeof *m);
  m->item = item_ctx(t, f, it);
  char **names = NULL;
  collect_folders(&app->connmgr->root, connmgr_is_folder(it) ? it : NULL, 0, &m->folders, &names, &m->n);
  pick_open(app, "Move to folder", names, m->n, move_picked, m);
  for (size_t i = 0; i < m->n; i++) free(names[i]);
  free(names);
}

/* The tree's keys (with the panel focused), also run by status hints */
static bool tree_action(NcApp *app, ConnTab *t, FormTab *f, HotkeyAction a) {
  if (a == HOTKEY_CONN_FILTER) {
    f->filter_focused = true;
    focus_set(f, FF_LIST);
    return true;
  }
  ListRow *r = f->highlighted ? list_selected(&f->panel.list) : NULL;
  if (a == HOTKEY_CONN_DELETE && r && r->kind == CPK_RECENT) { /* a recent URL is forgotten at once */
    size_t idx = (size_t)(intptr_t)r->data;
    if (idx < app->recent.count) {
      recent_remove(&app->recent, idx);
      char *err = NULL;
      if (!recent_save(&app->recent, &err)) conn_fields_set_message(&f->fields, true, "recent: %s", err ? err : "cannot save");
      free(err);
      f->highlighted = false;
      cp_changed();
      cp_rebuild(app, &f->panel);
    }
    return true;
  }
  if (!app->connmgr) {
    conn_fields_set_message(&f->fields, true, "connections.json is not available");
    return true;
  }
  ConnectionItem *it = current_item(app, f);
  switch (a) {
  case HOTKEY_CONN_NEW_FOLDER:
    prompt_open(app, "New folder", "Name", "", false, folder_named, item_ctx(t, f, it));
    return true;
  case HOTKEY_CONN_RENAME:
    if (!it) break;
    prompt_open(app, "Rename", "Name", connmgr_item_name(it), false, renamed, item_ctx(t, f, it));
    return true;
  case HOTKEY_CONN_DUPLICATE:
    if (!it || !connmgr_is_connection(it)) break;
    duplicate(app, f, it);
    return true;
  case HOTKEY_CONN_DELETE: {
    if (!it) break;
    char *msg = connmgr_is_folder(it)
                    ? str_printf("Delete the folder \"%s\" and the %zu connection%s in it?", connmgr_item_name(it),
                                 count_connections(it), count_connections(it) == 1 ? "" : "s")
                    : str_printf("Delete the saved connection \"%s\"?", connmgr_item_name(it));
    confirm_open(app, "Delete", msg, "Delete", true, delete_confirmed, item_ctx(t, f, it));
    free(msg);
    return true;
  }
  case HOTKEY_CONN_MOVE:
    if (!it) break;
    move_item(app, t, f, it);
    return true;
  default: return false;
  }
  conn_fields_set_message(&f->fields, true, "Pick a saved connection on the left first");
  return true;
}

static const HotkeyAction tree_actions[] = {HOTKEY_CONN_NEW_FOLDER, HOTKEY_CONN_RENAME, HOTKEY_CONN_DUPLICATE,
                                            HOTKEY_CONN_DELETE, HOTKEY_CONN_MOVE, HOTKEY_CONN_FILTER};

bool form_tab_do_action(NcApp *app, ConnTab *t, HotkeyAction a) {
  FormTab *f = t ? t->form : NULL;
  if (!f) return false;
  ready(app, f);
  switch (a) {
  case HOTKEY_CONN_OPEN: open_with(app, t, f, NC_DEST_HERE, NULL); return true;
  case HOTKEY_CONN_OPEN_BACKGROUND: open_with(app, t, f, NC_DEST_BACKGROUND, NULL); return true;
  case HOTKEY_CONN_OPEN_NEW_WORKSPACE: open_with(app, t, f, NC_DEST_NEW_WORKSPACE, NULL); return true;
  case HOTKEY_CONN_OPEN_TO_WORKSPACE: to_workspace(app, t, f); return true;
  case HOTKEY_CONN_SAVE: save(app, t, f, false); return true;
  case HOTKEY_CONN_SAVE_AS_NEW: save(app, t, f, true); return true;
  case HOTKEY_CONN_TEST: test(app, t, f); return true;
  case HOTKEY_CONN_CLEAR: case HOTKEY_CONN_NEW: clear(f); return true;
  default: return tree_action(app, t, f, a);
  }
}

/* ---- keys ---- */

static bool plain_char(const UiEvent *ev) {
  return !ev->key.is_special && !(ev->key.mods & (UI_MOD_CTRL | UI_MOD_ALT));
}

static bool special(const UiEvent *ev, int key) {
  return ev->key.is_special && ev->key.key == key && !(ev->key.mods & (UI_MOD_CTRL | UI_MOD_ALT));
}

static int first_field(const FormTab *f) { return conn_fields_next(&f->fields, CF_COUNT - 1, 1); }
static int last_field(const FormTab *f) { return conn_fields_next(&f->fields, 0, -1); }

static void focus_set(FormTab *f, FormFocus to) {
  if (f->focus == FF_URI && to != FF_URI && commit_uri(f)) sync_uri(f); /* the line shows what the fields read */
  f->focus = to;
}

/* Tab / Shift+Tab: panel, URI line, each field, each button, around */
static void focus_step(FormTab *f, int dir, bool panel_shown) {
  switch (f->focus) {
  case FF_LIST:
    if (dir > 0) focus_set(f, FF_URI);
    else { f->button = B_COUNT - 1; focus_set(f, FF_BUTTON); }
    break;
  case FF_URI:
    if (dir > 0) { f->field = first_field(f); focus_set(f, FF_FIELD); }
    else if (panel_shown) focus_set(f, FF_LIST);
    else { f->button = B_COUNT - 1; focus_set(f, FF_BUTTON); }
    break;
  case FF_FIELD:
    if (dir > 0 && f->field == last_field(f)) { f->button = 0; f->focus = FF_BUTTON; }
    else if (dir < 0 && f->field == first_field(f)) f->focus = FF_URI;
    else f->field = conn_fields_next(&f->fields, f->field, dir);
    break;
  case FF_BUTTON:
    if (dir > 0 && f->button == B_COUNT - 1) focus_set(f, panel_shown ? FF_LIST : FF_URI);
    else if (dir < 0 && f->button == 0) { f->field = last_field(f); f->focus = FF_FIELD; }
    else f->button += dir;
    break;
  }
}

static bool panel_shown(const NcApp *app) { return app->cols >= NARROW_COLS; }

bool form_tab_handle_key(NcApp *app, ConnTab *t, const UiEvent *ev) {
  FormTab *f = t ? t->form : NULL;
  if (!f || ev->type != UI_EVENT_KEY) return false;
  ready(app, f);
  if (f->filter_focused) { /* the panel's filter line */
    ConnPicker *p = &f->panel;
    if (special(ev, UI_KEY_ESCAPE)) {
      text_input_set(&p->in, "");
      f->filter_focused = false;
      cp_rebuild(app, p);
      return true;
    }
    if (special(ev, UI_KEY_ENTER) || special(ev, UI_KEY_DOWN) || special(ev, UI_KEY_TAB)) {
      f->filter_focused = false;
      return true;
    }
    char *before = str_dup(p->in.buf ? p->in.buf : "");
    bool used = nc_text_input_key(&p->in, ev);
    if (strcmp(before, p->in.buf ? p->in.buf : "") != 0) {
      f->highlighted = false; /* the rows changed under the highlight */
      cp_rebuild(app, p);
    }
    free(before);
    return used || plain_char(ev);
  }
  bool typing = f->focus == FF_URI || (f->focus == FF_FIELD && f->field != CF_DRIVER && !conn_fields_is_check(f->field));
  /* A focused button or a folder in the panel takes Enter for itself */
  if (special(ev, UI_KEY_ENTER) || (f->focus == FF_BUTTON && plain_char(ev) && ev->key.key == ' ')) {
    if (f->focus == FF_BUTTON) return form_tab_do_action(app, t, button_actions[f->button]);
    ListRow *r = f->focus == FF_LIST && f->highlighted ? list_selected(&f->panel.list) : NULL;
    ConnectionItem *it = r && r->kind == CPK_FOLDER ? row_item(app, f, r) : NULL;
    if (it) {
      connmgr_toggle_folder(it);
      cp_rebuild(app, &f->panel);
      return true;
    }
  }
  /* The buttons' keys; a plain letter bound to one types while a text field has the focus */
  for (int b = 0; b < B_COUNT; b++)
    if (!(typing && plain_char(ev)) && hotkey_matches(app->config, ev, button_actions[b]))
      return form_tab_do_action(app, t, button_actions[b]);
  if (special(ev, UI_KEY_TAB) || special(ev, UI_KEY_BTAB)) {
    focus_step(f, ev->key.key == UI_KEY_TAB ? 1 : -1, panel_shown(app));
    return true;
  }
  if (special(ev, UI_KEY_ESCAPE)) {
    if (f->focus == FF_LIST) return false;
    focus_set(f, FF_LIST);
    return true;
  }
  switch (f->focus) {
  case FF_LIST:
    if (hotkey_matches(app->config, ev, HOTKEY_CONN_NEW)) return form_tab_do_action(app, t, HOTKEY_CONN_NEW);
    for (size_t i = 0; i < sizeof tree_actions / sizeof *tree_actions; i++)
      if (hotkey_matches(app->config, ev, tree_actions[i])) return tree_action(app, t, f, tree_actions[i]);
    return list_move(app, t, f, ev);
  case FF_URI: {
    if (special(ev, UI_KEY_UP) || special(ev, UI_KEY_DOWN) || special(ev, UI_KEY_PAGEUP) ||
        special(ev, UI_KEY_PAGEDOWN)) {
      list_move(app, t, f, ev);
      return true;
    }
    /* An empty line leaves the keys no URI starts with to the global hotkeys */
    if (!f->uri.len && plain_char(ev) && strchr("?{}[]q,", ev->key.key) && ev->key.key) return false;
    char *before = str_dup(f->uri.buf ? f->uri.buf : "");
    bool used = nc_text_input_key(&f->uri, ev);
    if (strcmp(before, f->uri.buf ? f->uri.buf : "") != 0) f->uri_edited = true;
    str_secure_free(before);
    return used;
  }
  case FF_FIELD: {
    int fld = f->field;
    if (special(ev, UI_KEY_UP)) {
      if (fld == first_field(f)) f->focus = FF_URI;
      else f->field = conn_fields_next(&f->fields, fld, -1);
      return true;
    }
    if (special(ev, UI_KEY_DOWN)) {
      if (fld == last_field(f)) { f->button = 0; f->focus = FF_BUTTON; }
      else f->field = conn_fields_next(&f->fields, fld, 1);
      return true;
    }
    if (fld == CF_DRIVER) {
      bool right = special(ev, UI_KEY_RIGHT) || (plain_char(ev) && ev->key.key == ' ');
      if (right || special(ev, UI_KEY_LEFT)) {
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
    return nc_text_input_key(&f->fields.in[fld], ev) || plain_char(ev);
  }
  case FF_BUTTON:
    if (special(ev, UI_KEY_LEFT)) { f->button = (f->button + B_COUNT - 1) % B_COUNT; return true; }
    if (special(ev, UI_KEY_RIGHT)) { f->button = (f->button + 1) % B_COUNT; return true; }
    if (special(ev, UI_KEY_UP)) { f->field = last_field(f); f->focus = FF_FIELD; return true; }
    return false;
  }
  return false;
}

/* ---- mouse ---- */

#define DOUBLE_CLICK_MS 400 /* two presses on the same panel row within this are a double click (as the sidebar's) */
static struct { const FormTab *form; size_t row; int64_t at_ms; } g_click;

bool form_tab_handle_mouse(NcApp *app, ConnTab *t, const UiEvent *ev) {
  FormTab *f = t ? t->form : NULL;
  if (!f || ev->type != UI_EVENT_MOUSE) return false;
  ready(app, f);
  int y = ev->mouse.y, x = ev->mouse.x;
  bool press = ev->mouse.action == UI_MOUSE_PRESS && ev->mouse.button == UI_MOUSE_LEFT;
  bool wheel = ev->mouse.button == UI_MOUSE_SCROLL_UP || ev->mouse.button == UI_MOUSE_SCROLL_DOWN;
  if (f->list_cols > 0 && x >= f->list_x && x < f->list_x + f->list_cols && y >= f->list_y &&
      y < f->list_y + f->list_rows) {
    if (press || wheel) {
      focus_set(f, FF_LIST);
      list_move(app, t, f, ev); /* a press highlights the row (a dirty form asks first: the overlay takes the next press) */
    }
    if (press && f->highlighted) { /* a double click on a connection opens it here, on a folder toggles it */
      size_t row = f->panel.list.cursor;
      int64_t now = app_now_ms();
      bool dbl = g_click.form == f && g_click.row == row && now - g_click.at_ms <= DOUBLE_CLICK_MS;
      g_click.form = f;
      g_click.row = row;
      g_click.at_ms = dbl ? 0 : now; /* a third press starts over */
      ListRow *r = dbl ? list_selected(&f->panel.list) : NULL;
      ConnectionItem *it = r && r->kind == CPK_FOLDER ? row_item(app, f, r) : NULL;
      if (it) {
        connmgr_toggle_folder(it);
        cp_rebuild(app, &f->panel);
      } else if (r) {
        return form_tab_do_action(app, t, HOTKEY_CONN_OPEN); /* may free f */
      }
    }
    return true;
  }
  if (!press) return true;
  if (y == f->uri_y && x >= f->uri_x0 && x < f->uri_x1) {
    focus_set(f, FF_URI);
    return true;
  }
  for (int b = 0; b < B_COUNT; b++)
    if (y == f->button_y[b] && x >= f->button_x0[b] && x < f->button_x1[b]) {
      focus_set(f, FF_BUTTON);
      f->button = b;
      return form_tab_do_action(app, t, button_actions[b]); /* may free f */
    }
  for (int i = 0; i < CF_COUNT; i++) {
    if (f->field_y[i] != y) continue;
    focus_set(f, FF_FIELD);
    f->field = i;
    if (conn_fields_is_check(i)) f->fields.chk[i] = !f->fields.chk[i];
    return true;
  }
  return true;
}

/* ---- drawing ---- */

static void draw_list(NcApp *app, FormTab *f, int y, int x, int rows, int cols) {
  f->panel.list.hide_cursor = !f->highlighted;
  if (f->filter_focused || f->panel.in.len) { /* the filter line on top */
    nc_style(app->std, &app->theme, TC_ACCENT, TC_OVERLAY_BG, 0);
    nc_put(app->std, y, x, cols, "/");
    int cx = x + 2;
    nc_draw_input(app->std, &app->theme, y, x + 2, cols - 3, &f->panel.in, TC_FG, TC_EDIT_BG, &cx);
    f->filter_cursor_x = cx;
    f->filter_y = y;
    y++;
    rows--;
  }
  f->list_y = y;
  f->list_x = x;
  f->list_rows = rows;
  f->list_cols = cols;
  cp_draw_list(app, &f->panel, app->std, y, x, rows, cols);
}

bool form_tab_draw(NcApp *app, ConnTab *t, int y0, int rows, int *cursor_y, int *cursor_x) {
  FormTab *f = t ? t->form : NULL;
  if (!f) return false;
  ready(app, f);
  const Theme *th = &app->theme;
  struct ncplane *pl = app->std;
  bool cursor = false;
  for (int i = 0; i < CF_COUNT; i++) f->field_y[i] = -1;
  for (int b = 0; b < B_COUNT; b++) f->button_y[b] = -1;
  f->uri_y = -1;
  f->list_cols = 0;
  int x0 = 1;
  if (!panel_shown(app)) {
    if (f->focus == FF_LIST) { /* narrow: the panel in the form's place */
      draw_list(app, f, y0, 0, rows, app->cols);
      if (f->filter_focused) { *cursor_y = f->filter_y; *cursor_x = f->filter_cursor_x; }
      return f->filter_focused;
    }
  } else {
    int lw = app->cols / 3;
    if (lw < 24) lw = 24;
    if (lw > 40) lw = 40;
    draw_list(app, f, y0, 0, rows, lw);
    nc_style(pl, th, TC_DIM, TC_BG, 0);
    for (int r = 0; r < rows; r++) nc_put(pl, y0 + r, lw, 1, "\xe2\x94\x82");
    x0 = lw + 2;
  }
  int w = app->cols - x0 - 1;
  if (f->filter_focused) {
    cursor = true;
    *cursor_y = f->filter_y;
    *cursor_x = f->filter_cursor_x;
  }
  if (w < LABEL_COLS + 4) return cursor;
  int vw = w - LABEL_COLS;
  /* The URI line */
  if (f->focus != FF_URI) sync_uri(f);
  bool uf = f->focus == FF_URI;
  nc_style(pl, th, uf ? TC_ACCENT : TC_FG, TC_BG, uf ? NCSTYLE_BOLD : 0);
  nc_put(pl, y0, x0, LABEL_COLS, "URI");
  int cx = x0 + LABEL_COLS;
  if (!f->uri.len && !uf) {
    nc_style(pl, th, TC_DIM, TC_EDIT_BG, 0);
    nc_put(pl, y0, x0 + LABEL_COLS, vw, "a URL or a SQLite file, or pick on the left");
  } else {
    nc_draw_input(pl, th, y0, x0 + LABEL_COLS, vw, &f->uri, TC_FG, TC_EDIT_BG, &cx);
  }
  f->uri_y = y0;
  f->uri_x0 = x0;
  f->uri_x1 = x0 + w;
  if (uf && !f->filter_focused) {
    cursor = true;
    *cursor_y = y0;
    *cursor_x = cx;
  }
  if (rows > 1) {
    nc_style(pl, th, TC_DIM, TC_BG, 0);
    char *rule = safe_malloc((size_t)w * 3 + 1);
    for (int i = 0; i < w; i++) memcpy(rule + i * 3, "\xe2\x94\x80", 3);
    rule[w * 3] = 0;
    nc_put(pl, y0 + 1, x0, w, rule);
    free(rule);
  }
  /* The fields, scrolled so the focused one shows; 5 rows stay for the buttons and the message */
  int body = rows - 2 - 5;
  if (body < 1) body = 1;
  int nvis = 0, focus_row = 0;
  for (int i = 0; i < CF_COUNT; i++) {
    if (!conn_fields_visible(&f->fields, i)) continue;
    if (f->focus == FF_FIELD && i == f->field) focus_row = nvis;
    nvis++;
  }
  int first = focus_row >= body ? focus_row - body + 1 : 0;
  int n = 0, y = y0 + 2;
  for (int i = 0; i < CF_COUNT; i++) {
    if (!conn_fields_visible(&f->fields, i)) continue;
    int r = n++ - first;
    if (r < 0 || r >= body) continue;
    int fy = y + r;
    f->field_y[i] = fy;
    bool foc = f->focus == FF_FIELD && i == f->field;
    nc_style(pl, th, foc ? TC_ACCENT : TC_FG, TC_BG, foc ? NCSTYLE_BOLD : 0);
    nc_put(pl, fy, x0, LABEL_COLS, conn_fields_labels[i]);
    int vx = x0 + LABEL_COLS;
    int fw = vw < 48 ? vw : 48;
    if (i == CF_DRIVER) {
      char *d = str_printf("\xe2\x80\xb9 %s \xe2\x80\xba", conn_fields_driver_name(&f->fields, f->fields.driver));
      nc_style(pl, th, foc ? TC_CURSOR_FG : TC_FG, foc ? TC_CURSOR_BG : TC_EDIT_BG, 0);
      nc_put(pl, fy, vx, nc_text_width(d), d);
      free(d);
    } else if (conn_fields_is_check(i)) {
      nc_style(pl, th, foc ? TC_CURSOR_FG : TC_FG, foc ? TC_CURSOR_BG : TC_BG, 0);
      nc_put(pl, fy, vx, 3, f->fields.chk[i] ? "[x]" : "[ ]");
    } else {
      int fcx = vx;
      if (!f->fields.in[i].len && i == CF_PORT) {
        char *d = str_printf("%d", lace_driver_default_port(conn_fields_driver_name(&f->fields, f->fields.driver)));
        nc_style(pl, th, TC_DIM, TC_EDIT_BG, 0);
        nc_put(pl, fy, vx, fw, d);
        free(d);
      } else {
        nc_draw_input(pl, th, fy, vx, fw, &f->fields.in[i], TC_FG, TC_EDIT_BG, &fcx);
      }
      if (foc && !f->filter_focused) {
        cursor = true;
        *cursor_y = fy;
        *cursor_x = fcx;
      }
    }
  }
  int shown = nvis - first < body ? nvis - first : body;
  int by = y + shown + 1;
  /* The buttons: open actions, then manage actions */
  for (int row = 0; row < 2; row++) {
    int bx = x0;
    for (int b = row * 4; b < row * 4 + 4; b++) {
      char *label = str_printf(" %s ", button_labels[b]);
      int bw = nc_text_width(label);
      bool foc = f->focus == FF_BUTTON && f->button == b;
      if (bx + bw <= x0 + w && by + row < y0 + rows) {
        nc_style(pl, th, foc ? TC_CURSOR_FG : TC_FG, foc ? TC_CURSOR_BG : TC_EDIT_BG, b == B_OPEN ? NCSTYLE_BOLD : 0);
        nc_put(pl, by + row, bx, bw, label);
        f->button_y[b] = by + row;
        f->button_x0[b] = bx;
        f->button_x1[b] = bx + bw;
      }
      bx += bw + 2;
      free(label);
    }
  }
  if (f->fields.message && by + 3 < y0 + rows) {
    nc_style(pl, th, f->fields.message_error ? TC_ERROR_FG : TC_ACCENT, TC_BG, 0);
    nc_put(pl, by + 3, x0, w, f->fields.message);
  }
  return cursor;
}

bool form_tab_list_focused(const ConnTab *t) { return t && t->form && t->form->focus == FF_LIST; }

void form_tab_set_connstr(FormTab *f, const char *connstr) {
  conn_fields_clear(&f->fields);
  conn_fields_set_uri(&f->fields, connstr);
  conn_fields_mark_clean(&f->fields);
  set_highlight(f, NULL, NULL);
  f->highlighted = false;
  sync_uri(f);
}
