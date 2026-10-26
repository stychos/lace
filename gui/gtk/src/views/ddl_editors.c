/*
 * Lace GTK frontend
 * The structure editors and the table actions
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "ddl_editors.h"
#include "ddl_helpers.h"
#include "ddl_ops.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>

#define SQLITE_WHY "SQLite cannot alter this; create a new table and copy the rows"
#define SQLITE_FK_WHY "SQLite cannot add or drop a foreign key on an existing table; create a new table and copy the rows"

static gboolean is_mysql(LaceDriver d) { return d == LACE_DRIVER_MYSQL || d == LACE_DRIVER_MARIADB; }
static LaceDriver driver_of(LaceTableView *v) { return lace_connection_get_driver(lace_table_view_get_connection(v)); }
static const char *or_null(const char *s) { return s && *s ? s : NULL; }

static const char *object_word(LaceObjectKind k) {
  switch (k) {
  case LACE_OBJ_VIEW: return "view";
  case LACE_OBJ_ROUTINE: return "routine";
  case LACE_OBJ_TRIGGER: return "trigger";
  default: return "table";
  }
}

/* "orders (12,400 rows)" from what the view knows */
static char *rows_text(LaceTableView *v) {
  gboolean known = FALSE, approx = FALSE;
  gint64 total = lace_table_view_get_total(v, &known, &approx);
  gsize nf = 0;
  lace_table_view_get_filters(v, &nf);
  return lace_ddl_rows_text(lace_table_view_get_table(v), total, known && !nf, approx);
}

/* The view is still part of a window with a connection */
static gboolean alive(LaceTableView *v) {
  return v && LACE_IS_TABLE_VIEW(v) && lace_table_view_get_connection(v) && gtk_widget_get_root(GTK_WIDGET(v));
}

/* ==========================================================================
 * The editor dialog: rows of fields read back by key
 * ========================================================================== */

typedef enum { F_ENTRY, F_SWITCH, F_COMBO, F_PICK } FieldKind;

typedef struct {
  char *key;
  FieldKind kind;
  GtkWidget *row;
  GtkStringList *options; /* COMBO, PICK (owned) */
  GtkWidget *dropdown;    /* PICK: the suffix */
  gboolean append;        /* PICK: a choice is appended (", name") rather than set */
  gboolean ignore;        /* PICK: the list is being filled; the drop-down's own selection is not a pick */
} Field;

typedef struct Editor Editor;
typedef void (*EditorSubmit)(Editor *e);

struct Editor {
  AdwDialog *dialog;
  GtkWidget *banner, *save, *group;
  GPtrArray *fields;
  LaceTableView *view; /* weak */
  EditorSubmit submit;
  char *target; /* the column, index or key edited; NULL for a new one */
};

static Editor *g_editor; /* the open one, for the script driver */

static void field_free(gpointer p) {
  Field *f = p;
  g_free(f->key);
  g_clear_object(&f->options);
  g_free(f);
}

static void editor_closed(AdwDialog *d, Editor *e) {
  (void)d;
  if (g_editor == e)
    g_editor = NULL;
  if (e->view)
    g_object_remove_weak_pointer(G_OBJECT(e->view), (gpointer *)&e->view);
  g_ptr_array_free(e->fields, TRUE);
  g_free(e->target);
  g_free(e);
}

static void on_save(GtkButton *b, Editor *e) {
  (void)b;
  if (!alive(e->view)) {
    adw_dialog_close(e->dialog);
    return;
  }
  adw_banner_set_revealed(ADW_BANNER(e->banner), FALSE);
  e->submit(e);
}

static void on_cancel(GtkButton *b, Editor *e) {
  (void)b;
  adw_dialog_close(e->dialog);
}

static Editor *editor_new(LaceTableView *v, const char *title, EditorSubmit submit) {
  Editor *e = g_new0(Editor, 1);
  e->view = v;
  g_object_add_weak_pointer(G_OBJECT(v), (gpointer *)&e->view);
  e->submit = submit;
  e->fields = g_ptr_array_new_with_free_func(field_free);
  e->dialog = adw_dialog_new();
  adw_dialog_set_title(e->dialog, title);
  adw_dialog_set_content_width(e->dialog, 560);
  adw_dialog_set_content_height(e->dialog, 560);
  GtkWidget *tv = adw_toolbar_view_new();
  GtkWidget *header = adw_header_bar_new();
  adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
  adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
  GtkWidget *cancel = gtk_button_new_with_label("Cancel");
  g_signal_connect(cancel, "clicked", G_CALLBACK(on_cancel), e);
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), cancel);
  e->save = gtk_button_new_with_label("Save");
  gtk_widget_add_css_class(e->save, "suggested-action");
  g_signal_connect(e->save, "clicked", G_CALLBACK(on_save), e);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), e->save);
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), header);
  e->banner = adw_banner_new("");
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(tv), e->banner);
  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  GtkWidget *clamp = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 640);
  e->group = adw_preferences_group_new();
  gtk_widget_set_margin_top(e->group, 12);
  gtk_widget_set_margin_bottom(e->group, 24);
  gtk_widget_set_margin_start(e->group, 12);
  gtk_widget_set_margin_end(e->group, 12);
  adw_clamp_set_child(ADW_CLAMP(clamp), e->group);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), clamp);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(tv), scrolled);
  adw_dialog_set_child(e->dialog, tv);
  adw_dialog_set_default_widget(e->dialog, e->save);
  g_signal_connect(e->dialog, "closed", G_CALLBACK(editor_closed), e);
  return e;
}

static void editor_present(Editor *e) {
  g_editor = e;
  adw_dialog_present(e->dialog, GTK_WIDGET(e->view));
  for (guint i = 0; i < e->fields->len; i++) {
    Field *f = g_ptr_array_index(e->fields, i);
    if (gtk_widget_get_sensitive(f->row) && (f->kind == F_ENTRY || f->kind == F_PICK)) {
      gtk_widget_grab_focus(f->row);
      break;
    }
  }
}

static Field *field_add(Editor *e, const char *key, FieldKind kind, GtkWidget *row) {
  Field *f = g_new0(Field, 1);
  f->key = g_strdup(key);
  f->kind = kind;
  f->row = row;
  g_ptr_array_add(e->fields, f);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(e->group), row);
  return f;
}

static void on_entry_activated(AdwEntryRow *r, Editor *e) {
  (void)r;
  gtk_widget_activate(e->save);
}

static Field *editor_entry(Editor *e, const char *key, const char *title, const char *text, const char *hint) {
  GtkWidget *row = adw_entry_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  char *t = hint ? g_strdup_printf("%s  ·  %s", title, hint) : g_strdup(title);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), t);
  g_free(t);
  if (text)
    gtk_editable_set_text(GTK_EDITABLE(row), text);
  g_signal_connect(row, "entry-activated", G_CALLBACK(on_entry_activated), e);
  return field_add(e, key, F_ENTRY, row);
}

static Field *editor_switch(Editor *e, const char *key, const char *title, gboolean on) {
  GtkWidget *row = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  adw_switch_row_set_active(ADW_SWITCH_ROW(row), on);
  return field_add(e, key, F_SWITCH, row);
}

/* A closed list; current: the option selected first (NULL: the first) */
static Field *editor_combo(Editor *e, const char *key, const char *title, const char *const *options, size_t n,
                           const char *current) {
  GtkWidget *row = adw_combo_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  Field *f = field_add(e, key, F_COMBO, row);
  f->options = gtk_string_list_new(NULL);
  guint sel = 0;
  for (size_t i = 0; i < n; i++) {
    gtk_string_list_append(f->options, options[i]);
    if (current && g_str_equal(options[i], current))
      sel = (guint)i;
  }
  adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(f->options));
  adw_combo_row_set_selected(ADW_COMBO_ROW(row), sel);
  return f;
}

static void on_pick_selected(GObject *dd, GParamSpec *ps, Field *f) {
  (void)ps;
  guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
  if (sel == GTK_INVALID_LIST_POSITION || f->ignore)
    return;
  const char *name = gtk_string_list_get_string(f->options, sel);
  if (!name)
    return;
  const char *cur = gtk_editable_get_text(GTK_EDITABLE(f->row));
  if (f->append && cur && *cur) {
    char *t = g_strdup_printf("%s, %s", cur, name);
    gtk_editable_set_text(GTK_EDITABLE(f->row), t);
    g_free(t);
  } else {
    gtk_editable_set_text(GTK_EDITABLE(f->row), name);
  }
  gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), GTK_INVALID_LIST_POSITION);
}

/* The drop-down selects the first item when its list fills: not a pick */
static void on_options_filling(GListModel *m, guint pos, guint removed, guint added, Field *f) {
  (void)m; (void)pos; (void)removed; (void)added;
  f->ignore = TRUE;
}

static void on_options_filled(GListModel *m, guint pos, guint removed, guint added, Field *f) {
  (void)m; (void)pos; (void)removed; (void)added;
  gtk_drop_down_set_selected(GTK_DROP_DOWN(f->dropdown), GTK_INVALID_LIST_POSITION);
  f->ignore = FALSE;
}

/* An entry with a list of choices at its end: free text, or a choice
 * (append: each choice is added to a comma-separated list, in order) */
static Field *editor_pick(Editor *e, const char *key, const char *title, const char *text, const char *hint,
                          const char *const *options, size_t n, gboolean append) {
  Field *f = editor_entry(e, key, title, text, hint);
  f->kind = F_PICK;
  f->append = append;
  f->options = gtk_string_list_new(NULL);
  for (size_t i = 0; i < n; i++)
    gtk_string_list_append(f->options, options[i]);
  g_signal_connect(f->options, "items-changed", G_CALLBACK(on_options_filling), f); /* before the drop-down's handler */
  f->dropdown = gtk_drop_down_new(G_LIST_MODEL(g_object_ref(f->options)), NULL);
  g_signal_connect(f->options, "items-changed", G_CALLBACK(on_options_filled), f);   /* after it */
  gtk_drop_down_set_selected(GTK_DROP_DOWN(f->dropdown), GTK_INVALID_LIST_POSITION);
  gtk_widget_set_valign(f->dropdown, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(f->dropdown, "flat");
  gtk_widget_set_tooltip_text(f->dropdown, append ? "Add a column" : "Pick one");
  g_signal_connect(f->dropdown, "notify::selected", G_CALLBACK(on_pick_selected), f);
  adw_entry_row_add_suffix(ADW_ENTRY_ROW(f->row), f->dropdown);
  return f;
}

static Field *editor_field(Editor *e, const char *key) {
  for (guint i = 0; i < e->fields->len; i++) {
    Field *f = g_ptr_array_index(e->fields, i);
    if (g_str_equal(f->key, key))
      return f;
  }
  return NULL;
}

static void editor_disable(Editor *e, const char *key, const char *why) {
  Field *f = editor_field(e, key);
  if (!f)
    return;
  gtk_widget_set_sensitive(f->row, FALSE);
  const char *title = adw_preferences_row_get_title(ADW_PREFERENCES_ROW(f->row));
  char *t = g_strdup_printf("%s  ·  %s", title, why);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(f->row), t);
  g_free(t);
  gtk_widget_set_tooltip_text(f->row, why);
}

/* A field's text, trimmed (g_free); "" when empty or not a text field */
static char *editor_text(Editor *e, const char *key) {
  Field *f = editor_field(e, key);
  if (!f)
    return g_strdup("");
  if (f->kind == F_COMBO) {
    guint sel = adw_combo_row_get_selected(ADW_COMBO_ROW(f->row));
    const char *s = sel == GTK_INVALID_LIST_POSITION ? NULL : gtk_string_list_get_string(f->options, sel);
    return g_strdup(s ? s : "");
  }
  if (f->kind == F_SWITCH)
    return g_strdup(adw_switch_row_get_active(ADW_SWITCH_ROW(f->row)) ? "on" : "");
  return g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(f->row))));
}

static gboolean editor_on(Editor *e, const char *key) {
  Field *f = editor_field(e, key);
  return f && f->kind == F_SWITCH && adw_switch_row_get_active(ADW_SWITCH_ROW(f->row));
}

static void editor_error(Editor *e, const char *msg) {
  adw_banner_set_title(ADW_BANNER(e->banner), msg ? msg : "");
  adw_banner_set_revealed(ADW_BANNER(e->banner), TRUE);
}

/* The change goes to the confirmation; the editor closes when it ran */
static void editor_confirm(Editor *e, const char *title, const char *summary, char *sql, gboolean destructive,
                           LaceDdlAfter after, char *arg) {
  LaceDdlChange c = {.title = title, .summary = summary, .sql = sql, .destructive = destructive, .after = after,
                     .arg = arg, .editor = e->dialog, .editor_banner = e->banner};
  lace_ddl_confirm(e->view, &c);
}

gboolean lace_ddl_editor_set_field(const char *key, const char *value) {
  Field *f = g_editor ? editor_field(g_editor, key) : NULL;
  if (!f)
    return FALSE;
  switch (f->kind) {
  case F_SWITCH:
    adw_switch_row_set_active(ADW_SWITCH_ROW(f->row), value && (g_str_equal(value, "on") || g_str_equal(value, "1") ||
                                                                g_str_equal(value, "true") || g_str_equal(value, "yes")));
    return TRUE;
  case F_COMBO:
    for (guint i = 0; i < g_list_model_get_n_items(G_LIST_MODEL(f->options)); i++)
      if (g_strcmp0(gtk_string_list_get_string(f->options, i), value) == 0) {
        adw_combo_row_set_selected(ADW_COMBO_ROW(f->row), i);
        return TRUE;
      }
    return FALSE;
  default:
    gtk_editable_set_text(GTK_EDITABLE(f->row), value ? value : "");
    return TRUE;
  }
}

gboolean lace_ddl_editor_save(void) {
  if (!g_editor)
    return FALSE;
  gtk_widget_activate(g_editor->save);
  return TRUE;
}

/* ==========================================================================
 * Columns
 * ========================================================================== */

static const LaceColumn *column_named(LaceTableView *v, const char *name) {
  const LaceSchema *s = lace_table_view_get_schema(v);
  for (size_t i = 0; s && name && i < s->num_columns; i++)
    if (s->columns[i].name && g_str_equal(s->columns[i].name, name))
      return &s->columns[i];
  return NULL;
}

/* The column names as a NULL-terminated vector of the schema's strings (g_free the vector) */
static const char **column_names(LaceTableView *v, size_t *n) {
  const LaceSchema *s = lace_table_view_get_schema(v);
  size_t count = s ? s->num_columns : 0;
  const char **names = g_new0(const char *, count + 1);
  for (size_t i = 0; i < count; i++)
    names[i] = s->columns[i].name ? s->columns[i].name : "";
  *n = count;
  return names;
}

static void column_submit(Editor *e) {
  LaceTableView *v = e->view;
  LaceDriver d = driver_of(v);
  const char *table = lace_table_view_get_table(v);
  char *name = editor_text(e, "name"), *type = editor_text(e, "type"), *dflt = editor_text(e, "default");
  char *coll = editor_text(e, "collation"), *comment = editor_text(e, "comment"), *after = editor_text(e, "after");
  if (!*name) {
    editor_error(e, "A name is needed");
    goto out;
  }
  LaceDdlColumn w = {.name = name, .type = or_null(type), .nullable = editor_on(e, "nullable"),
                     .default_sql = or_null(dflt), .collation = or_null(coll),
                     .comment = d == LACE_DRIVER_SQLITE ? NULL : comment, .auto_increment = editor_on(e, "auto_increment"),
                     .after = NULL};
  if (is_mysql(d) && *after)
    w.after = g_str_equal(after, "(first)") ? "" : g_str_equal(after, "(end)") || g_str_equal(after, "(in place)") ? NULL : after;
  char *err = NULL, *sql = NULL, *summary = NULL;
  const char *title;
  if (!e->target) {
    if (!w.comment || !*w.comment)
      w.comment = NULL;
    sql = lace_ddl_add_column(d, table, &w, &err);
    summary = g_strdup_printf("Add column %s to %s", name, table);
    title = "Add column";
  } else {
    const LaceColumn *cur = column_named(v, e->target);
    if (!cur) {
      editor_error(e, "The column is gone (the structure was reloaded)");
      goto out;
    }
    w.name = cur->name;
    if (w.comment && g_strcmp0(w.comment, cur->comment ? cur->comment : "") == 0)
      w.comment = NULL; /* unchanged */
    char *alter = lace_ddl_alter_column(d, table, cur, &w, &err);
    gboolean no_change = !alter && err && g_str_equal(err, "No change");
    if (no_change) {
      free(err);
      err = NULL;
    }
    char *rename = NULL;
    if (g_strcmp0(name, cur->name) != 0 && (alter || no_change))
      rename = lace_ddl_rename_column(d, table, cur->name, name, &err);
    if (alter && rename)
      sql = g_strdup_printf("%s;\n%s", alter, rename); /* the alter names the old column */
    else if (alter)
      sql = g_strdup(alter);
    else if (rename)
      sql = g_strdup(rename);
    free(alter);
    free(rename);
    if (!sql && !err) {
      editor_error(e, "No change");
      goto out;
    }
    summary = rename ? g_strdup_printf("Change column %s of %s and rename it to %s", cur->name, table, name)
                     : g_strdup_printf("Change column %s of %s", cur->name, table);
    title = "Change column";
    if (sql) { /* liblace's allocation for the confirmation */
      char *copy = strdup(sql);
      g_free(sql);
      sql = copy;
    }
  }
  if (!sql) {
    lace_ddl_refused(GTK_WIDGET(v), e->banner, err);
    err = NULL;
  } else {
    editor_confirm(e, title, summary, sql, FALSE, LACE_DDL_AFTER_SCHEMA, NULL);
  }
  free(err);
  g_free(summary);
out:
  g_free(name);
  g_free(type);
  g_free(dflt);
  g_free(coll);
  g_free(comment);
  g_free(after);
}

/* cur NULL: a new column */
static void open_column_editor(LaceTableView *v, const LaceColumn *cur) {
  LaceDriver d = driver_of(v);
  gboolean mysql = is_mysql(d), sqlite = d == LACE_DRIVER_SQLITE;
  LaceConnection *conn = lace_table_view_get_connection(v);
  char *title = cur ? g_strdup_printf("Column %s", cur->name) : g_strdup_printf("Add column to %s", lace_table_view_get_table(v));
  Editor *e = editor_new(v, title, column_submit);
  g_free(title);
  e->target = cur ? g_strdup(cur->name) : NULL;
  editor_entry(e, "name", "Name", cur ? cur->name : NULL, NULL);
  Field *type = editor_pick(e, "type", "Type", cur ? cur->type_name : NULL, NULL, NULL, 0, FALSE);
  lace_ddl_fetch_names(conn, G_OBJECT(e->dialog), type->options, "types", NULL);
  editor_switch(e, "nullable", "Nullable", cur ? cur->nullable : TRUE);
  editor_entry(e, "default", "Default", cur ? cur->default_val : NULL, "SQL: 'text', 0, CURRENT_TIMESTAMP");
  Field *coll = editor_pick(e, "collation", "Collation", cur ? cur->collation : NULL, NULL, NULL, 0, FALSE);
  lace_ddl_fetch_names(conn, G_OBJECT(e->dialog), coll->options, "collations", NULL);
  editor_entry(e, "comment", "Comment", cur ? cur->comment : NULL, NULL);
  editor_switch(e, "auto_increment", "Auto-increment", cur && (cur->auto_increment || cur->identity));
  if (mysql) {
    size_t n = 0;
    const char **names = column_names(v, &n);
    const char **opts = g_new0(const char *, n + 3);
    size_t k = 0;
    opts[k++] = cur ? "(in place)" : "(end)";
    opts[k++] = "(first)";
    for (size_t i = 0; i < n; i++)
      if (!cur || g_strcmp0(names[i], cur->name) != 0)
        opts[k++] = names[i];
    editor_combo(e, "after", cur ? "Move after" : "After", opts, k, NULL);
    g_free(opts);
    g_free(names);
  }
  if (sqlite) {
    editor_disable(e, "comment", "SQLite has no column comments");
    if (cur) {
      editor_disable(e, "type", SQLITE_WHY);
      editor_disable(e, "nullable", SQLITE_WHY);
      editor_disable(e, "default", SQLITE_WHY);
      editor_disable(e, "collation", SQLITE_WHY);
      editor_disable(e, "auto_increment", SQLITE_WHY);
    } else {
      editor_disable(e, "auto_increment", "SQLite: only INTEGER PRIMARY KEY AUTOINCREMENT at CREATE TABLE");
    }
  } else if (cur && cur->generated) {
    editor_disable(e, "type", "A generated column: drop it and add it again");
    editor_disable(e, "default", "A generated column: drop it and add it again");
  }
  editor_present(e);
}

static void drop_column(LaceTableView *v, const LaceColumn *c) {
  char *err = NULL, *sql = lace_ddl_drop_column(driver_of(v), lace_table_view_get_table(v), c->name, &err);
  if (!sql) {
    lace_ddl_refused(GTK_WIDGET(v), NULL, err);
    return;
  }
  char *rows = rows_text(v), *uses = lace_ddl_column_uses(lace_table_view_get_schema(v), c->name);
  char *summary = *uses ? g_strdup_printf("Drop column %s from %s.\nIt is part of %s: PostgreSQL drops those with it, "
                                          "MySQL and SQLite may refuse.", c->name, rows, uses)
                        : g_strdup_printf("Drop column %s from %s.", c->name, rows);
  LaceDdlChange ch = {.title = "Drop column", .summary = summary, .sql = sql, .destructive = TRUE,
                      .after = LACE_DDL_AFTER_SCHEMA};
  lace_ddl_confirm(v, &ch);
  g_free(summary);
  g_free(rows);
  g_free(uses);
}

/* ---- prompts: a view and a target name ---- */

typedef struct {
  LaceTableView *view;
  char *target;
  int what; /* which prompt */
} Prompt;

enum { P_RENAME_COLUMN, P_RENAME_INDEX, P_RENAME_FK, P_RENAME_OBJECT, P_AUTOINC, P_COMMENT, P_NEW_TABLE, P_NEW_VIEW,
       P_NEW_FUNCTION };

static Prompt *prompt_new(LaceTableView *v, const char *target, int what) {
  Prompt *p = g_new0(Prompt, 1);
  p->view = g_object_ref(v);
  p->target = g_strdup(target);
  p->what = what;
  return p;
}

static void prompt_free(Prompt *p) {
  g_object_unref(p->view);
  g_free(p->target);
  g_free(p);
}

static void prompt_done(const char *text, gpointer data);

static void open_prompt(LaceTableView *v, const char *target, int what, const char *heading, const char *body,
                        const char *initial, const char *action) {
  lace_prompt(GTK_WIDGET(v), heading, body, initial, action, FALSE, prompt_done, prompt_new(v, target, what));
}

/* ==========================================================================
 * The primary key, moving a column
 * ========================================================================== */

static void pk_submit(Editor *e) {
  LaceTableView *v = e->view;
  const char *table = lace_table_view_get_table(v);
  char *text = editor_text(e, "columns");
  char **names = lace_ddl_split_names(text);
  g_free(text);
  size_t n = g_strv_length(names);
  for (size_t i = 0; i < n; i++)
    if (!column_named(v, names[i])) {
      char *msg = g_strdup_printf("%s is not a column of %s", names[i], table);
      editor_error(e, msg);
      g_free(msg);
      g_strfreev(names);
      return;
    }
  char *err = NULL;
  char *sql = lace_ddl_set_primary_key(driver_of(v), table, (const char *const *)names, n,
                                       lace_ddl_primary_key_name(lace_table_view_get_schema(v)), &err);
  if (!sql) {
    lace_ddl_refused(GTK_WIDGET(v), e->banner, err);
  } else {
    char *cols = g_strjoinv(", ", names);
    char *summary = n ? g_strdup_printf("Set the primary key of %s to (%s)", table, cols)
                      : g_strdup_printf("Drop the primary key of %s", table);
    editor_confirm(e, n ? "Set primary key" : "Drop primary key", summary, sql, n == 0, LACE_DDL_AFTER_SCHEMA, NULL);
    g_free(summary);
    g_free(cols);
  }
  g_strfreev(names);
}

static void open_pk_editor(LaceTableView *v) {
  char *current = lace_ddl_key_columns(lace_table_view_get_schema(v));
  char *title = g_strdup_printf("Primary key of %s", lace_table_view_get_table(v));
  Editor *e = editor_new(v, title, pk_submit);
  g_free(title);
  size_t n = 0;
  const char **names = column_names(v, &n);
  editor_pick(e, "columns", "Key columns", current, "in order; none drops the key", names, n, TRUE);
  g_free(names);
  g_free(current);
  if (driver_of(v) == LACE_DRIVER_SQLITE)
    editor_disable(e, "columns", SQLITE_WHY);
  editor_present(e);
}

static void move_submit(Editor *e) {
  LaceTableView *v = e->view;
  const LaceColumn *cur = column_named(v, e->target);
  if (!cur) {
    editor_error(e, "The column is gone");
    return;
  }
  char *after = editor_text(e, "after");
  LaceDdlColumn w = {.name = cur->name, .type = cur->type_name, .nullable = cur->nullable,
                     .default_sql = cur->default_val, .collation = cur->collation,
                     .auto_increment = cur->auto_increment || cur->identity != 0,
                     .after = g_str_equal(after, "(first)") ? "" : after};
  char *err = NULL, *sql = lace_ddl_alter_column(driver_of(v), lace_table_view_get_table(v), cur, &w, &err);
  if (!sql) {
    lace_ddl_refused(GTK_WIDGET(v), e->banner, err);
  } else {
    char *summary = *w.after ? g_strdup_printf("Move column %s of %s after %s", cur->name, lace_table_view_get_table(v), after)
                             : g_strdup_printf("Move column %s of %s first", cur->name, lace_table_view_get_table(v));
    editor_confirm(e, "Move column", summary, sql, FALSE, LACE_DDL_AFTER_SCHEMA, NULL);
    g_free(summary);
  }
  g_free(after);
}

static void open_move_editor(LaceTableView *v, const LaceColumn *c) {
  if (!is_mysql(driver_of(v))) {
    lace_toast(GTK_WIDGET(v), "Only MySQL can reorder columns; the others keep the creation order");
    return;
  }
  char *title = g_strdup_printf("Move column %s", c->name);
  Editor *e = editor_new(v, title, move_submit);
  g_free(title);
  e->target = g_strdup(c->name);
  size_t n = 0;
  const char **names = column_names(v, &n);
  const char **opts = g_new0(const char *, n + 2);
  size_t k = 0;
  opts[k++] = "(first)";
  for (size_t i = 0; i < n; i++)
    if (g_strcmp0(names[i], c->name) != 0)
      opts[k++] = names[i];
  editor_combo(e, "after", "After", opts, k, NULL);
  g_free(opts);
  g_free(names);
  editor_present(e);
}

/* ==========================================================================
 * Indexes and checks
 * ========================================================================== */

static const LaceIndex *index_named(LaceTableView *v, const char *name) {
  const LaceSchema *s = lace_table_view_get_schema(v);
  for (size_t i = 0; s && name && i < s->num_indexes; i++)
    if (s->indexes[i].name && g_str_equal(s->indexes[i].name, name))
      return &s->indexes[i];
  return NULL;
}

static const char *kind_word(const LaceIndex *ix) {
  switch (ix->kind) {
  case LACE_INDEX_CHECK: return "check constraint";
  case LACE_INDEX_EXCLUSION: return "exclusion constraint";
  case LACE_INDEX_UNIQUE: return "unique index";
  default: return "index";
  }
}

static void index_submit(Editor *e) {
  LaceTableView *v = e->view;
  const char *table = lace_table_view_get_table(v);
  char *name = editor_text(e, "name"), *cols = editor_text(e, "columns"), *method = editor_text(e, "method");
  char *where = editor_text(e, "where");
  char **names = lace_ddl_split_names(cols);
  size_t n = g_strv_length(names);
  gboolean unique = editor_on(e, "unique");
  if (!*name) {
    editor_error(e, "A name is needed");
  } else if (!n) {
    editor_error(e, "At least one column is needed");
  } else {
    char *err = NULL;
    char *sql = lace_ddl_create_index(driver_of(v), table, name, unique, (const char *const *)names, n, or_null(method),
                                      or_null(where), &err);
    if (!sql) {
      lace_ddl_refused(GTK_WIDGET(v), e->banner, err);
    } else {
      char *joined = g_strjoinv(", ", names), *rows = rows_text(v);
      char *summary = g_strdup_printf("Create %sindex %s on %s (%s).\nLocks the table while it builds; Esc cancels.",
                                      unique ? "unique " : "", name, rows, joined);
      editor_confirm(e, "Create index", summary, sql, FALSE, LACE_DDL_AFTER_SCHEMA, NULL);
      g_free(summary);
      g_free(joined);
      g_free(rows);
    }
  }
  g_strfreev(names);
  g_free(name);
  g_free(cols);
  g_free(method);
  g_free(where);
}

static void open_index_editor(LaceTableView *v) {
  LaceDriver d = driver_of(v);
  static const char *const pg_methods[] = {"btree", "hash", "gin", "gist", "brin", "spgist"};
  static const char *const my_methods[] = {"BTREE", "HASH", "FULLTEXT", "SPATIAL"};
  char *title = g_strdup_printf("New index on %s", lace_table_view_get_table(v));
  Editor *e = editor_new(v, title, index_submit);
  g_free(title);
  editor_entry(e, "name", "Name", NULL, NULL);
  editor_switch(e, "unique", "Unique", FALSE);
  size_t n = 0;
  const char **names = column_names(v, &n);
  editor_pick(e, "columns", "Columns", NULL, "in order", names, n, TRUE);
  g_free(names);
  if (d == LACE_DRIVER_POSTGRES)
    editor_pick(e, "method", "Method", NULL, NULL, pg_methods, 6, FALSE);
  else if (is_mysql(d))
    editor_pick(e, "method", "Method", NULL, NULL, my_methods, 4, FALSE);
  else
    editor_pick(e, "method", "Method", NULL, NULL, NULL, 0, FALSE);
  editor_entry(e, "where", "Partial (WHERE)", NULL, "a condition: status = 'open'");
  if (d == LACE_DRIVER_SQLITE)
    editor_disable(e, "method", "SQLite indexes have no method");
  if (is_mysql(d))
    editor_disable(e, "where", "MySQL has no partial indexes");
  editor_present(e);
}

static void check_submit(Editor *e) {
  LaceTableView *v = e->view;
  const char *table = lace_table_view_get_table(v);
  char *name = editor_text(e, "name"), *cond = editor_text(e, "condition");
  if (!*cond) {
    editor_error(e, "A condition is needed");
  } else {
    char *err = NULL, *sql = lace_ddl_add_check(driver_of(v), table, or_null(name), cond, &err);
    if (!sql) {
      lace_ddl_refused(GTK_WIDGET(v), e->banner, err);
    } else {
      char *summary = g_strdup_printf("Add the check constraint %s(%s) to %s.\nEvery row must satisfy it already.", name,
                                      cond, table);
      editor_confirm(e, "Add check constraint", summary, sql, FALSE, LACE_DDL_AFTER_SCHEMA, NULL);
      g_free(summary);
    }
  }
  g_free(name);
  g_free(cond);
}

static void open_check_editor(LaceTableView *v) {
  if (driver_of(v) == LACE_DRIVER_SQLITE) {
    lace_toast(GTK_WIDGET(v), "SQLite takes check constraints only at CREATE TABLE; create a new table and copy the rows");
    return;
  }
  char *title = g_strdup_printf("New check constraint on %s", lace_table_view_get_table(v));
  Editor *e = editor_new(v, title, check_submit);
  g_free(title);
  editor_entry(e, "name", "Name", NULL, "optional");
  editor_entry(e, "condition", "Condition", NULL, "total >= 0");
  editor_present(e);
}

static void drop_index(LaceTableView *v, const LaceIndex *ix) {
  if (ix->kind == LACE_INDEX_PRIMARY) {
    lace_toast(GTK_WIDGET(v), "The primary key is changed from Structure (the key editor)");
    return;
  }
  LaceDriver d = driver_of(v);
  const char *table = lace_table_view_get_table(v);
  char *err = NULL, *sql;
  if (ix->kind == LACE_INDEX_CHECK || ix->kind == LACE_INDEX_EXCLUSION)
    sql = lace_ddl_drop_constraint(d, table, ix->name, &err);
  else
    sql = lace_ddl_drop_index(d, table, ix->name, ix->constraint, &err);
  if (!sql) {
    lace_ddl_refused(GTK_WIDGET(v), NULL, err);
    return;
  }
  char *summary = g_strdup_printf("Drop the %s %s of %s.", kind_word(ix), ix->name ? ix->name : "", table);
  LaceDdlChange ch = {.title = "Drop index", .summary = summary, .sql = sql, .destructive = TRUE, .after = LACE_DDL_AFTER_SCHEMA};
  lace_ddl_confirm(v, &ch);
  g_free(summary);
}

/* ==========================================================================
 * Foreign keys
 * ========================================================================== */

static const char *const ACTIONS[] = {"", "NO ACTION", "RESTRICT", "CASCADE", "SET NULL", "SET DEFAULT"};

static const LaceForeignKey *fk_named(LaceTableView *v, const char *name) {
  const LaceSchema *s = lace_table_view_get_schema(v);
  for (size_t i = 0; s && name && i < s->num_foreign_keys; i++)
    if (s->foreign_keys[i].name && g_str_equal(s->foreign_keys[i].name, name))
      return &s->foreign_keys[i];
  return NULL;
}

/* The ADD statement (MySQL's index before it when the columns have none; *note says so) */
static char *fk_add_sql(LaceTableView *v, const LaceDdlForeignKey *fk, char **note, char **err) {
  LaceDriver d = driver_of(v);
  const char *table = lace_table_view_get_table(v);
  char *add = lace_ddl_add_foreign_key(d, table, fk, err);
  if (!add)
    return NULL;
  *note = NULL;
  if (is_mysql(d) && !lace_ddl_index_covers(lace_table_view_get_schema(v), (char *const *)fk->columns, fk->num_columns)) {
    char *ixname = g_strdup_printf("fk_%s_%s", table, fk->columns[0]);
    char *ix = lace_ddl_create_index(d, table, ixname, false, fk->columns, fk->num_columns, NULL, NULL, err);
    if (!ix) {
      free(add);
      g_free(ixname);
      return NULL;
    }
    char *joined = g_strjoinv(", ", (char **)fk->columns);
    *note = g_strdup_printf("\nAnd an index %s on (%s), which MySQL requires.", ixname, joined);
    char *both = g_strdup_printf("%s;\n%s", ix, add);
    free(ix);
    free(add);
    g_free(ixname);
    g_free(joined);
    add = strdup(both);
    g_free(both);
  }
  return add;
}

static void fk_submit(Editor *e) {
  LaceTableView *v = e->view;
  LaceDriver d = driver_of(v);
  const char *table = lace_table_view_get_table(v);
  char *name = editor_text(e, "name"), *cols = editor_text(e, "columns"), *rt = editor_text(e, "ref_table");
  char *rcols = editor_text(e, "ref_columns"), *od = editor_text(e, "on_delete"), *ou = editor_text(e, "on_update");
  char **names = lace_ddl_split_names(cols), **rnames = lace_ddl_split_names(rcols);
  size_t n = g_strv_length(names), rn = g_strv_length(rnames);
  if (!n || !*rt || rn != n) {
    editor_error(e, !n ? "At least one column is needed"
                       : !*rt ? "The referenced table is needed" : "As many referenced columns as columns are needed");
  } else {
    LaceDdlForeignKey fk = {.name = or_null(name), .columns = (const char *const *)names, .num_columns = n,
                            .ref_table = rt, .ref_columns = (const char *const *)rnames, .num_ref_columns = rn,
                            .on_delete = or_null(od), .on_update = or_null(ou),
                            .deferrable = editor_on(e, "deferrable"), .initially_deferred = editor_on(e, "initially_deferred")};
    char *err = NULL, *note = NULL, *sql = fk_add_sql(v, &fk, &note, &err);
    if (sql && e->target) { /* a change: the old key goes first */
      char *drop = lace_ddl_drop_foreign_key(d, table, e->target, &err);
      if (drop) {
        char *both = g_strdup_printf("%s;\n%s", drop, sql);
        free(sql);
        free(drop);
        sql = strdup(both);
        g_free(both);
      } else {
        free(sql);
        sql = NULL;
      }
    }
    if (!sql) {
      lace_ddl_refused(GTK_WIDGET(v), e->banner, err);
    } else {
      char *joined = g_strjoinv(", ", names), *rjoined = g_strjoinv(", ", rnames);
      char *summary = e->target
          ? g_strdup_printf("Change the foreign key %s of %s: (%s) references %s (%s), by dropping it and adding it again.%s",
                            e->target, table, joined, rt, rjoined, note ? note : "")
          : g_strdup_printf("Add a foreign key to %s: (%s) references %s (%s).\nEvery row must satisfy it already.%s",
                            table, joined, rt, rjoined, note ? note : "");
      editor_confirm(e, e->target ? "Change foreign key" : "Add foreign key", summary, sql, e->target != NULL,
                     LACE_DDL_AFTER_SCHEMA, NULL);
      g_free(summary);
      g_free(joined);
      g_free(rjoined);
    }
    g_free(note);
  }
  g_strfreev(names);
  g_strfreev(rnames);
  g_free(name);
  g_free(cols);
  g_free(rt);
  g_free(rcols);
  g_free(od);
  g_free(ou);
}

static void open_fk_editor(LaceTableView *v, const LaceForeignKey *cur) {
  LaceDriver d = driver_of(v);
  if (d == LACE_DRIVER_SQLITE) {
    lace_toast(GTK_WIDGET(v), SQLITE_FK_WHY);
    return;
  }
  char *title = cur ? g_strdup_printf("Foreign key %s", cur->name ? cur->name : "")
                    : g_strdup_printf("New foreign key on %s", lace_table_view_get_table(v));
  Editor *e = editor_new(v, title, fk_submit);
  g_free(title);
  e->target = cur && cur->name ? g_strdup(cur->name) : NULL;
  editor_entry(e, "name", "Name", cur ? cur->name : NULL, "optional");
  char *cols = cur ? catalog_join(cur->columns, cur->num_columns) : NULL;
  char *rcols = cur ? catalog_join(cur->ref_columns, cur->num_ref_columns) : NULL;
  size_t n = 0;
  const char **names = column_names(v, &n);
  editor_pick(e, "columns", "Columns", cols, "in order", names, n, TRUE);
  g_free(names);
  GListModel *tables = lace_connection_get_tables(lace_table_view_get_connection(v));
  guint nt = g_list_model_get_n_items(tables);
  const char **tnames = g_new0(const char *, nt + 1);
  for (guint i = 0; i < nt; i++)
    tnames[i] = gtk_string_list_get_string(GTK_STRING_LIST(tables), i);
  editor_pick(e, "ref_table", "References", cur ? cur->ref_table : NULL, NULL, tnames, nt, FALSE);
  g_free(tnames);
  editor_entry(e, "ref_columns", "Referenced columns", rcols, "comma-separated, in order");
  editor_combo(e, "on_delete", "ON DELETE", ACTIONS, 6, cur ? cur->on_delete : NULL);
  editor_combo(e, "on_update", "ON UPDATE", ACTIONS, 6, cur ? cur->on_update : NULL);
  editor_switch(e, "deferrable", "Deferrable", cur && cur->deferrable);
  editor_switch(e, "initially_deferred", "Initially deferred", cur && cur->initially_deferred);
  if (d != LACE_DRIVER_POSTGRES) {
    editor_disable(e, "deferrable", "Only PostgreSQL has deferrable constraints");
    editor_disable(e, "initially_deferred", "Only PostgreSQL has deferrable constraints");
  }
  free(cols);
  free(rcols);
  editor_present(e);
}

static void drop_fk(LaceTableView *v, const LaceForeignKey *fk) {
  if (driver_of(v) == LACE_DRIVER_SQLITE) {
    lace_toast(GTK_WIDGET(v), SQLITE_FK_WHY);
    return;
  }
  if (!fk->name) {
    lace_toast(GTK_WIDGET(v), "The key has no name to drop it by");
    return;
  }
  char *err = NULL, *sql = lace_ddl_drop_foreign_key(driver_of(v), lace_table_view_get_table(v), fk->name, &err);
  if (!sql) {
    lace_ddl_refused(GTK_WIDGET(v), NULL, err);
    return;
  }
  char *summary = g_strdup_printf("Drop the foreign key %s of %s (references %s).", fk->name, lace_table_view_get_table(v),
                                  fk->ref_table ? fk->ref_table : "");
  LaceDdlChange ch = {.title = "Drop foreign key", .summary = summary, .sql = sql, .destructive = TRUE,
                      .after = LACE_DDL_AFTER_SCHEMA};
  lace_ddl_confirm(v, &ch);
  g_free(summary);
}

/* ==========================================================================
 * Information: the counter, the comment, the engine, the charset, maintenance
 * ========================================================================== */

void lace_ddl_info_actions(CatalogRows *rows, LaceTableView *v) {
  LaceDriver d = driver_of(v);
  gboolean mysql = is_mysql(d), pg = d == LACE_DRIVER_POSTGRES, sqlite = d == LACE_DRIVER_SQLITE;
  catalog_rows_blank(rows);
  catalog_rows_heading(rows, "Actions");
  if (lace_table_view_get_object_kind(v) != LACE_OBJ_TABLE) {
    catalog_rows_span(rows, LACE_SV_TAG_EDIT_DEFINITION, "Edit in a query view");
    return;
  }
  const LaceAutoInc *a = lace_table_view_get_autoinc(v);
  if (a && a->column)
    catalog_rows_span(rows, LACE_SV_TAG_AUTOINC, a->has_counter ? "Set the next auto-increment value…"
                                                                : "Set the next auto-increment value (no counter: the maximum + 1)");
  if (pg || mysql)
    catalog_rows_span(rows, LACE_SV_TAG_COMMENT, "Set the comment…");
  if (mysql) {
    catalog_rows_span(rows, LACE_SV_TAG_ENGINE, "Set the engine…");
    catalog_rows_span(rows, LACE_SV_TAG_CHARSET, "Set the charset and collation…");
  }
  catalog_rows_span(rows, LACE_SV_TAG_ANALYZE, "Analyze (update the statistics)");
  if (pg)
    catalog_rows_span(rows, LACE_SV_TAG_VACUUM, "Vacuum (VACUUM ANALYZE the table)");
  if (sqlite)
    catalog_rows_span(rows, LACE_SV_TAG_VACUUM, "Vacuum (the whole file: SQLite has no per-table VACUUM)");
  if (mysql)
    catalog_rows_span(rows, LACE_SV_TAG_OPTIMIZE, "Optimize (OPTIMIZE TABLE)");
}

static void engine_submit(Editor *e) {
  LaceTableView *v = e->view;
  char *engine = editor_text(e, "engine");
  if (!*engine) {
    editor_error(e, "An engine is needed");
    g_free(engine);
    return;
  }
  char *err = NULL, *sql = lace_ddl_set_table_option(driver_of(v), lace_table_view_get_table(v), LACE_DDL_OPT_ENGINE, engine, NULL, &err);
  if (!sql) {
    lace_ddl_refused(GTK_WIDGET(v), e->banner, err);
  } else {
    char *summary = g_strdup_printf("Change the engine of %s to %s.\nThe table is rebuilt: it is locked meanwhile.",
                                    lace_table_view_get_table(v), engine);
    editor_confirm(e, "Set engine", summary, sql, FALSE, LACE_DDL_AFTER_SCHEMA, NULL);
    g_free(summary);
  }
  g_free(engine);
}

static void open_engine_editor(LaceTableView *v) {
  const LaceTableInfo *info = lace_table_view_get_info(v);
  char *title = g_strdup_printf("Engine of %s", lace_table_view_get_table(v));
  Editor *e = editor_new(v, title, engine_submit);
  g_free(title);
  Field *f = editor_pick(e, "engine", "Engine", info ? info->engine : NULL, NULL, NULL, 0, FALSE);
  lace_ddl_fetch_names(lace_table_view_get_connection(v), G_OBJECT(e->dialog), f->options, "engines", NULL);
  editor_present(e);
}

static void charset_submit(Editor *e) {
  LaceTableView *v = e->view;
  char *cs = editor_text(e, "charset"), *coll = editor_text(e, "collation");
  if (!*cs) {
    editor_error(e, "A charset is needed");
  } else {
    char *err = NULL, *sql = lace_ddl_set_table_option(driver_of(v), lace_table_view_get_table(v), LACE_DDL_OPT_CHARSET, cs,
                                                       or_null(coll), &err);
    if (!sql) {
      lace_ddl_refused(GTK_WIDGET(v), e->banner, err);
    } else {
      char *summary = g_strdup_printf("Set the default charset of %s to %s%s%s (existing columns keep theirs)",
                                      lace_table_view_get_table(v), cs, *coll ? " with collation " : "", coll);
      editor_confirm(e, "Set charset", summary, sql, FALSE, LACE_DDL_AFTER_SCHEMA, NULL);
      g_free(summary);
    }
  }
  g_free(cs);
  g_free(coll);
}

static void open_charset_editor(LaceTableView *v) {
  const LaceTableInfo *info = lace_table_view_get_info(v);
  char *title = g_strdup_printf("Charset of %s", lace_table_view_get_table(v));
  Editor *e = editor_new(v, title, charset_submit);
  g_free(title);
  editor_entry(e, "charset", "Charset", info ? info->charset : NULL, "utf8mb4");
  Field *f = editor_pick(e, "collation", "Collation", info ? info->collation : NULL, NULL, NULL, 0, FALSE);
  lace_ddl_fetch_names(lace_table_view_get_connection(v), G_OBJECT(e->dialog), f->options, "collations",
                       info ? info->charset : NULL);
  editor_present(e);
}

static void maintenance(LaceTableView *v, LaceDdlMaintenance kind) {
  LaceDriver d = driver_of(v);
  char *err = NULL, *sql = lace_ddl_maintenance(d, lace_table_view_get_table(v), kind, &err);
  if (!sql) {
    lace_ddl_refused(GTK_WIDGET(v), NULL, err);
    return;
  }
  const char *title = kind == LACE_DDL_ANALYZE ? "Analyze" : kind == LACE_DDL_VACUUM ? "Vacuum" : "Optimize";
  char *rows = rows_text(v);
  char *summary = kind == LACE_DDL_VACUUM && d == LACE_DRIVER_SQLITE
                      ? g_strdup("Rebuild the whole database file (SQLite's VACUUM is not per table).\nLocks the database while it runs; Cancel stops it.")
                      : g_strdup_printf("%s %s.\nLocks the table while it runs; Cancel stops it.", title, rows);
  LaceDdlChange ch = {.title = title, .summary = summary, .sql = sql, .after = LACE_DDL_AFTER_SCHEMA};
  lace_ddl_confirm(v, &ch);
  g_free(summary);
  g_free(rows);
}

/* ==========================================================================
 * The table's actions
 * ========================================================================== */

static void truncate_table(LaceTableView *v) {
  char *err = NULL, *sql = lace_ddl_truncate(driver_of(v), lace_table_view_get_table(v), &err);
  if (!sql) {
    lace_ddl_refused(GTK_WIDGET(v), NULL, err);
    return;
  }
  char *rows = rows_text(v);
  char *summary = g_strdup_printf("Delete every row of %s.", rows);
  LaceDdlChange ch = {.title = "Truncate table", .summary = summary, .sql = sql, .destructive = TRUE,
                      .after = LACE_DDL_AFTER_SCHEMA};
  lace_ddl_confirm(v, &ch);
  g_free(summary);
  g_free(rows);
}

/* "order_items (fk_items_order), invoices" */
static char *deps_text(LaceTableView *v) {
  size_t n = 0;
  gboolean loaded = FALSE;
  const LaceDependent *deps = lace_table_view_get_dependents(v, &n, &loaded);
  GString *s = g_string_new(NULL);
  for (size_t i = 0; loaded && i < n; i++)
    g_string_append_printf(s, "%s%s%s%s%s", s->len ? ", " : "", deps[i].table ? deps[i].table : "",
                           deps[i].name ? " (" : "", deps[i].name ? deps[i].name : "", deps[i].name ? ")" : "");
  return g_string_free(s, FALSE);
}

static void drop_cascade_confirmed(gboolean yes, gpointer data) {
  LaceTableView *v = data;
  if (yes && alive(v)) {
    char *err = NULL, *sql = lace_ddl_drop_table(LACE_DRIVER_POSTGRES, lace_table_view_get_table(v), true, &err);
    if (!sql) {
      lace_ddl_refused(GTK_WIDGET(v), NULL, err);
    } else {
      char *deps = deps_text(v);
      char *summary = g_strdup_printf("Drop table %s and the foreign keys of %s with it (CASCADE).",
                                      lace_table_view_get_table(v), deps);
      LaceDdlChange ch = {.title = "Drop table", .summary = summary, .sql = sql, .destructive = TRUE,
                          .after = LACE_DDL_AFTER_DROP};
      lace_ddl_confirm(v, &ch);
      g_free(summary);
      g_free(deps);
    }
  }
  g_object_unref(v);
}

/* The dependents are known: the confirmation(s) */
static void drop_now(LaceTableView *v) {
  LaceDriver d = driver_of(v);
  LaceObjectKind kind = lace_table_view_get_object_kind(v);
  const char *table = lace_table_view_get_table(v), *word = object_word(kind);
  char *rows = rows_text(v), *deps = deps_text(v);
  size_t nd = 0;
  gboolean loaded = FALSE;
  lace_table_view_get_dependents(v, &nd, &loaded);
  char *err = NULL, *sql = NULL;
  switch (kind) {
  case LACE_OBJ_VIEW: sql = lace_ddl_drop_view(d, table, &err); break;
  case LACE_OBJ_ROUTINE: {
    LaceObject o = {.kind = LACE_OBJ_ROUTINE, .name = (char *)table, .detail = (char *)lace_table_view_get_object_detail(v)};
    sql = lace_ddl_drop_routine(d, &o, &err);
    break;
  }
  case LACE_OBJ_TRIGGER: sql = lace_ddl_drop_trigger(d, table, lace_table_view_get_object_table(v), &err); break;
  default: sql = lace_ddl_drop_table(d, table, false, &err); break;
  }
  if (!sql) {
    lace_ddl_refused(GTK_WIDGET(v), NULL, err);
  } else if (kind == LACE_OBJ_TABLE && d == LACE_DRIVER_POSTGRES && loaded && nd) {
    /* PostgreSQL refuses while others reference it: CASCADE, after a second confirmation */
    free(sql);
    char *msg = g_strdup_printf("Drop table %s?\nReferenced by %s: PostgreSQL drops the table only with CASCADE, "
                                "which also drops those foreign keys. The next confirmation asks for it.", rows, deps);
    lace_confirm(GTK_WIDGET(v), "Drop table", msg, "Continue", TRUE, drop_cascade_confirmed, g_object_ref(v));
    g_free(msg);
  } else {
    char *summary = kind == LACE_OBJ_TABLE
        ? (*deps ? g_strdup_printf("Drop table %s.\nReferenced by %s: the server may refuse, or drop their rows' keys.", rows, deps)
                 : g_strdup_printf("Drop table %s.", rows))
        : g_strdup_printf("Drop the %s %s.", word, table);
    char *title = g_strdup_printf("Drop %s", word);
    LaceDdlChange ch = {.title = title, .summary = summary, .sql = sql, .destructive = TRUE, .after = LACE_DDL_AFTER_DROP};
    lace_ddl_confirm(v, &ch);
    g_free(title);
    g_free(summary);
  }
  g_free(rows);
  g_free(deps);
}

static void on_deps_for_drop(LaceTableView *v, gpointer data) {
  (void)data;
  g_signal_handlers_disconnect_by_func(v, on_deps_for_drop, NULL);
  if (alive(v))
    drop_now(v);
}

static void drop_object(LaceTableView *v) {
  size_t n = 0;
  gboolean loaded = FALSE;
  lace_table_view_get_dependents(v, &n, &loaded);
  if (lace_table_view_get_object_kind(v) == LACE_OBJ_TABLE && !loaded) {
    g_signal_connect(v, "dependents-loaded", G_CALLBACK(on_deps_for_drop), NULL);
    lace_table_view_load_dependents(v);
    lace_toast(GTK_WIDGET(v), "Checking what references %s…", lace_table_view_get_table(v));
    return;
  }
  drop_now(v);
}

static void edit_definition(LaceTableView *v) {
  const char *def = lace_table_view_get_definition(v);
  if (!def) {
    lace_toast(GTK_WIDGET(v), "The definition is still loading: try again in a moment");
    return;
  }
  lace_ddl_open_query(v, def);
}

/* ---- the prompts' answers ---- */

static void prompt_done(const char *text, gpointer data) {
  Prompt *p = data;
  LaceTableView *v = p->view;
  char *name = text ? g_strstrip(g_strdup(text)) : NULL;
  if (!name || !*name || !alive(v)) {
    g_free(name);
    prompt_free(p);
    return;
  }
  LaceDriver d = driver_of(v);
  const char *table = lace_table_view_get_table(v);
  char *err = NULL, *sql = NULL, *summary = NULL;
  const char *title = NULL;
  LaceDdlAfter after = LACE_DDL_AFTER_SCHEMA;
  char *arg = NULL;
  switch (p->what) {
  case P_RENAME_COLUMN:
    if (g_str_equal(name, p->target))
      break;
    sql = lace_ddl_rename_column(d, table, p->target, name, &err);
    summary = g_strdup_printf("Rename column %s of %s to %s", p->target, table, name);
    title = "Rename column";
    break;
  case P_RENAME_INDEX: {
    const LaceIndex *ix = index_named(v, p->target);
    if (!ix || g_str_equal(name, p->target))
      break;
    if (ix->kind == LACE_INDEX_CHECK || ix->kind == LACE_INDEX_EXCLUSION)
      sql = lace_ddl_rename_constraint(d, table, ix->name, name, &err);
    else
      sql = lace_ddl_rename_index(d, table, ix, name, &err);
    summary = g_strdup_printf("Rename the %s %s of %s to %s%s", kind_word(ix), ix->name, table, name,
                              sql && strchr(sql, ';') ? " (dropped and created again)" : "");
    title = "Rename index";
    break;
  }
  case P_RENAME_FK: {
    const LaceForeignKey *fk = fk_named(v, p->target);
    if (!fk || g_str_equal(name, p->target))
      break;
    if (d == LACE_DRIVER_POSTGRES) {
      sql = lace_ddl_rename_constraint(d, table, fk->name, name, &err);
    } else {
      LaceDdlForeignKey nk = {.name = name, .columns = (const char *const *)fk->columns, .num_columns = fk->num_columns,
                              .ref_table = fk->ref_table, .ref_columns = (const char *const *)fk->ref_columns,
                              .num_ref_columns = fk->num_ref_columns, .on_delete = fk->on_delete, .on_update = fk->on_update};
      char *note = NULL, *add = fk_add_sql(v, &nk, &note, &err);
      char *drop = add ? lace_ddl_drop_foreign_key(d, table, fk->name, &err) : NULL;
      if (add && drop) {
        char *both = g_strdup_printf("%s;\n%s", drop, add);
        sql = strdup(both);
        g_free(both);
      }
      free(add);
      free(drop);
      g_free(note);
    }
    summary = g_strdup_printf("Rename the foreign key %s of %s to %s%s", fk->name, table, name,
                              d == LACE_DRIVER_POSTGRES ? "" : " (dropped and added again)");
    title = "Rename foreign key";
    break;
  }
  case P_RENAME_OBJECT: {
    if (g_str_equal(name, table))
      break;
    LaceObjectKind kind = lace_table_view_get_object_kind(v);
    sql = kind == LACE_OBJ_VIEW ? lace_ddl_rename_view(d, table, name, &err) : lace_ddl_rename_table(d, table, name, &err);
    summary = g_strdup_printf("Rename the %s %s to %s.\nOpen tabs follow; query views keep their text.", object_word(kind), table, name);
    title = kind == LACE_OBJ_VIEW ? "Rename view" : "Rename table";
    after = LACE_DDL_AFTER_RENAME;
    arg = g_strdup(name);
    break;
  }
  case P_AUTOINC: {
    const LaceAutoInc *a = lace_table_view_get_autoinc(v);
    char *end = NULL;
    long long n = strtoll(name, &end, 10);
    if (!a || !end || *end) {
      lace_toast(GTK_WIDGET(v), "Not a number: %s", name);
      break;
    }
    sql = lace_ddl_set_autoincrement(d, table, a, n, &err);
    summary = g_strdup_printf("The next %s of %s becomes %lld%s", a->column, table, n,
                              a->max >= 0 && n <= a->max ? " (MySQL refuses a value below the maximum; the rest allow it)" : "");
    title = "Set auto-increment";
    break;
  }
  case P_COMMENT:
    sql = lace_ddl_set_table_option(d, table, LACE_DDL_OPT_COMMENT, g_str_equal(name, "-") ? "" : name, NULL, &err);
    summary = g_str_equal(name, "-") ? g_strdup_printf("Clear the comment of %s", table)
                                     : g_strdup_printf("Set the comment of %s to \"%s\"", table, name);
    title = "Set comment";
    break;
  case P_NEW_TABLE:
    sql = lace_ddl_create_table_with_id(d, name, &err);
    summary = g_strdup_printf("Create the table %s with one column, id, then open it on Structure.", name);
    title = "New table";
    after = LACE_DDL_AFTER_CREATE_TABLE;
    arg = g_strdup(name);
    break;
  case P_NEW_VIEW:
  case P_NEW_FUNCTION: {
    char *tpl = p->what == P_NEW_FUNCTION ? lace_ddl_routine_template(d, name, &err) : lace_ddl_view_template(d, name, &err);
    if (!tpl) {
      lace_ddl_refused(GTK_WIDGET(v), NULL, err);
      err = NULL;
    } else {
      lace_ddl_open_query(v, tpl);
      lace_toast(GTK_WIDGET(v), "Edit the statement and run it (Ctrl+Enter); the sidebar reloads then");
    }
    free(tpl);
    break;
  }
  default: break;
  }
  if (title && !sql) {
    lace_ddl_refused(GTK_WIDGET(v), NULL, err);
    err = NULL;
    g_free(arg);
  } else if (sql) {
    LaceDdlChange ch = {.title = title, .summary = summary, .sql = sql, .after = after, .arg = arg};
    lace_ddl_confirm(v, &ch);
  }
  free(err);
  g_free(summary);
  g_free(name);
  prompt_free(p);
}

/* ==========================================================================
 * Dispatch
 * ========================================================================== */

static const LaceColumn *tag_column(LaceTableView *v, int tag) {
  const LaceSchema *s = lace_table_view_get_schema(v);
  if (tag < CATALOG_TAG_COLUMN || tag >= CATALOG_TAG_INDEX || !s)
    return NULL;
  size_t i = (size_t)(tag - CATALOG_TAG_COLUMN);
  return i < s->num_columns ? &s->columns[i] : NULL;
}

static const LaceIndex *tag_index(LaceTableView *v, int tag) {
  const LaceSchema *s = lace_table_view_get_schema(v);
  if (tag < CATALOG_TAG_INDEX || tag >= LACE_SV_TAG_AUTOINC || !s)
    return NULL;
  size_t i = (size_t)(tag - CATALOG_TAG_INDEX);
  return i < s->num_indexes ? &s->indexes[i] : NULL;
}

static const LaceForeignKey *tag_fk(LaceTableView *v, int tag) {
  const LaceSchema *s = lace_table_view_get_schema(v);
  if (tag < CATALOG_TAG_FK || tag >= CATALOG_TAG_DEP || !s)
    return NULL;
  size_t i = (size_t)(tag - CATALOG_TAG_FK);
  return i < s->num_foreign_keys ? &s->foreign_keys[i] : NULL;
}

gboolean lace_ddl_action_applies(LaceTableView *v, const char *action) {
  LaceObjectKind kind = lace_table_view_get_object_kind(v);
  LaceSubView sv = lace_table_view_get_subview(v);
  if (g_str_has_prefix(action, "struct-")) {
    if (sv == LACE_SUBVIEW_DATA)
      return FALSE;
    if (kind != LACE_OBJ_TABLE)
      return g_str_equal(action, "struct-edit"); /* a definition: edit in a query view */
    if (sv == LACE_SUBVIEW_INFORMATION)
      return g_str_equal(action, "struct-edit");
    if (g_str_equal(action, "struct-key") || g_str_equal(action, "struct-move"))
      return sv == LACE_SUBVIEW_STRUCTURE;
    if (g_str_equal(action, "struct-check"))
      return sv == LACE_SUBVIEW_INDEXES;
    return TRUE;
  }
  if (g_str_equal(action, "truncate"))
    return kind == LACE_OBJ_TABLE;
  if (g_str_equal(action, "rename-object"))
    return kind == LACE_OBJ_TABLE || kind == LACE_OBJ_VIEW;
  if (g_str_equal(action, "edit-definition"))
    return kind != LACE_OBJ_TABLE;
  if (g_str_equal(action, "new-function"))
    return driver_of(v) != LACE_DRIVER_SQLITE;
  return TRUE;
}

void lace_ddl_activate(LaceTableView *v, int tag) {
  if (!alive(v))
    return;
  switch (tag) {
  case LACE_SV_TAG_AUTOINC: {
    const LaceAutoInc *a = lace_table_view_get_autoinc(v);
    if (!a || !a->column)
      return;
    if (!a->has_counter) {
      lace_toast(GTK_WIDGET(v), "A plain INTEGER PRIMARY KEY has no counter: the next rowid is the maximum + 1");
      return;
    }
    char *initial = a->next >= 0 ? g_strdup_printf("%" G_GINT64_FORMAT, (gint64)a->next) : g_strdup("1");
    open_prompt(v, NULL, P_AUTOINC, "Next auto-increment value", "The value the database assigns next", initial, "_Set");
    g_free(initial);
    return;
  }
  case LACE_SV_TAG_COMMENT: {
    const LaceTableInfo *info = lace_table_view_get_info(v);
    char *heading = g_strdup_printf("Comment on %s", lace_table_view_get_table(v));
    open_prompt(v, NULL, P_COMMENT, heading, "A single - clears it", info ? info->comment : NULL, "_Set");
    g_free(heading);
    return;
  }
  case LACE_SV_TAG_ENGINE: open_engine_editor(v); return;
  case LACE_SV_TAG_CHARSET: open_charset_editor(v); return;
  case LACE_SV_TAG_ANALYZE: maintenance(v, LACE_DDL_ANALYZE); return;
  case LACE_SV_TAG_VACUUM: maintenance(v, LACE_DDL_VACUUM); return;
  case LACE_SV_TAG_OPTIMIZE: maintenance(v, LACE_DDL_OPTIMIZE); return;
  case LACE_SV_TAG_EDIT_DEFINITION: edit_definition(v); return;
  default: break;
  }
  const LaceColumn *c = tag_column(v, tag);
  if (c) {
    open_column_editor(v, c);
    return;
  }
  const LaceIndex *ix = tag_index(v, tag);
  if (ix) {
    if (ix->kind == LACE_INDEX_PRIMARY)
      lace_toast(GTK_WIDGET(v), "The primary key is changed from Structure (the key editor)");
    else {
      char *heading = g_strdup_printf("Rename %s", ix->name ? ix->name : "index");
      open_prompt(v, ix->name, P_RENAME_INDEX, heading, NULL, ix->name, "_Rename");
      g_free(heading);
    }
    return;
  }
  const LaceForeignKey *fk = tag_fk(v, tag);
  if (fk)
    open_fk_editor(v, fk);
}

void lace_ddl_action(LaceTableView *v, const char *action, int tag) {
  if (!alive(v))
    return;
  if (!lace_ddl_action_applies(v, action)) {
    lace_toast(GTK_WIDGET(v), "%s does not apply here", action);
    return;
  }
  LaceObjectKind kind = lace_table_view_get_object_kind(v);
  LaceSubView sv = lace_table_view_get_subview(v);
  const char *table = lace_table_view_get_table(v);
  if (g_str_has_prefix(action, "struct-")) {
    if (kind != LACE_OBJ_TABLE || sv == LACE_SUBVIEW_INFORMATION) { /* struct-edit: the row's action */
      if (kind != LACE_OBJ_TABLE && sv == LACE_SUBVIEW_STRUCTURE)
        edit_definition(v);
      else
        lace_ddl_activate(v, tag);
      return;
    }
    if (!lace_table_view_get_schema(v)) {
      lace_toast(GTK_WIDGET(v), "The structure is still loading");
      return;
    }
    const LaceColumn *c = sv == LACE_SUBVIEW_STRUCTURE ? tag_column(v, tag) : NULL;
    const LaceIndex *ix = sv == LACE_SUBVIEW_INDEXES ? tag_index(v, tag) : NULL;
    const LaceForeignKey *fk = sv == LACE_SUBVIEW_FOREIGN_KEYS ? tag_fk(v, tag) : NULL;
    const char *pick = sv == LACE_SUBVIEW_STRUCTURE ? "Pick a column first"
                       : sv == LACE_SUBVIEW_INDEXES ? "Pick an index first" : "Pick a foreign key first";
    gboolean has = c || ix || fk;
    if (g_str_equal(action, "struct-add")) {
      if (sv == LACE_SUBVIEW_STRUCTURE) open_column_editor(v, NULL);
      else if (sv == LACE_SUBVIEW_INDEXES) open_index_editor(v);
      else open_fk_editor(v, NULL);
    } else if (g_str_equal(action, "struct-check")) {
      open_check_editor(v);
    } else if (g_str_equal(action, "struct-key")) {
      open_pk_editor(v);
    } else if (!has) {
      lace_toast(GTK_WIDGET(v), "%s", pick);
    } else if (g_str_equal(action, "struct-edit")) {
      if (c) open_column_editor(v, c);
      else if (fk) open_fk_editor(v, fk);
      else lace_ddl_activate(v, tag); /* an index: rename */
    } else if (g_str_equal(action, "struct-drop")) {
      if (c) drop_column(v, c);
      else if (ix) drop_index(v, ix);
      else drop_fk(v, fk);
    } else if (g_str_equal(action, "struct-rename")) {
      if (c) {
        char *heading = g_strdup_printf("Rename column %s", c->name);
        open_prompt(v, c->name, P_RENAME_COLUMN, heading, NULL, c->name, "_Rename");
        g_free(heading);
      } else if (ix) {
        lace_ddl_activate(v, tag);
      } else if (driver_of(v) == LACE_DRIVER_SQLITE) {
        lace_toast(GTK_WIDGET(v), SQLITE_FK_WHY);
      } else if (!fk->name) {
        lace_toast(GTK_WIDGET(v), "The key has no name");
      } else {
        char *heading = g_strdup_printf("Rename %s", fk->name);
        open_prompt(v, fk->name, P_RENAME_FK, heading, NULL, fk->name, "_Rename");
        g_free(heading);
      }
    } else if (g_str_equal(action, "struct-move")) {
      if (c) open_move_editor(v, c);
    }
    return;
  }
  if (g_str_equal(action, "rename-object")) {
    char *heading = g_strdup_printf("Rename %s %s", object_word(kind), table);
    open_prompt(v, table, P_RENAME_OBJECT, heading, NULL, table, "_Rename");
    g_free(heading);
  } else if (g_str_equal(action, "truncate")) {
    truncate_table(v);
  } else if (g_str_equal(action, "drop-object")) {
    drop_object(v);
  } else if (g_str_equal(action, "new-table")) {
    open_prompt(v, NULL, P_NEW_TABLE, "New table", "Its name; it gets one column, id", NULL, "_Create");
  } else if (g_str_equal(action, "new-view")) {
    open_prompt(v, NULL, P_NEW_VIEW, "New view", "Its name; a query view opens with the template", NULL, "_Continue");
  } else if (g_str_equal(action, "new-function")) {
    open_prompt(v, NULL, P_NEW_FUNCTION, "New function", "Its name; a query view opens with the template", NULL, "_Continue");
  } else if (g_str_equal(action, "edit-definition")) {
    edit_definition(v);
  } else {
    lace_toast(GTK_WIDGET(v), "Unknown action %s", action);
  }
}
