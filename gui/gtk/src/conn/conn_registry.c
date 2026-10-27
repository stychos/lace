/*
 * Lace GTK frontend
 * The open database connections and their holders
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "conn_registry.h"
#include "core/conn_fields.h"

typedef struct {
  LaceConnection *conn;
  guint holders;
} Entry;

struct _LaceConnRegistry {
  GArray *entries; /* Entry */
};

LaceConnRegistry *lace_conn_registry_new(void) {
  LaceConnRegistry *r = g_new0(LaceConnRegistry, 1);
  r->entries = g_array_new(FALSE, TRUE, sizeof(Entry));
  return r;
}

void lace_conn_registry_free(LaceConnRegistry *r) {
  if (!r)
    return;
  for (guint i = 0; i < r->entries->len; i++)
    g_object_unref(g_array_index(r->entries, Entry, i).conn);
  g_array_unref(r->entries);
  g_free(r);
}

static gint index_of(LaceConnRegistry *r, LaceConnection *c) {
  for (guint i = 0; i < r->entries->len; i++)
    if (g_array_index(r->entries, Entry, i).conn == c)
      return (gint)i;
  return -1;
}

LaceConnection *lace_conn_registry_find(LaceConnRegistry *r, const char *saved_id, const char *connstr) {
  gboolean by_id = saved_id && *saved_id;
  for (guint i = 0; i < r->entries->len; i++) {
    LaceConnection *c = g_array_index(r->entries, Entry, i).conn;
    if (lace_connection_is_closed(c))
      continue;
    const char *sid = lace_connection_get_saved_id(c);
    if (by_id ? sid && g_str_equal(sid, saved_id) &&
                    (!connstr || conn_key_matches(NULL, connstr, NULL, lace_connection_get_connstr(c)))
              : conn_key_matches(NULL, connstr, sid, lace_connection_get_connstr(c)))
      return c;
  }
  return NULL;
}

void lace_conn_registry_add(LaceConnRegistry *r, LaceConnection *c) {
  if (index_of(r, c) >= 0)
    return;
  Entry e = {g_object_ref(c), 0};
  g_array_append_val(r->entries, e);
}

void lace_conn_registry_hold(LaceConnRegistry *r, LaceConnection *c) {
  gint i = index_of(r, c);
  if (i < 0) {
    lace_conn_registry_add(r, c);
    i = (gint)r->entries->len - 1;
  }
  g_array_index(r->entries, Entry, i).holders++;
}

gboolean lace_conn_registry_release(LaceConnRegistry *r, LaceConnection *c) {
  gint i = index_of(r, c);
  if (i < 0)
    return FALSE;
  Entry *e = &g_array_index(r->entries, Entry, i);
  if (e->holders > 1) {
    e->holders--;
    return FALSE;
  }
  lace_connection_close(c);
  g_array_remove_index(r->entries, (guint)i);
  g_object_unref(c);
  return TRUE;
}

guint lace_conn_registry_holders(LaceConnRegistry *r, LaceConnection *c) {
  gint i = index_of(r, c);
  return i < 0 ? 0 : g_array_index(r->entries, Entry, i).holders;
}

guint lace_conn_registry_count(LaceConnRegistry *r) { return r->entries->len; }
