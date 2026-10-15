/*
 * Lace
 * Connection Manager - Saved Connections Storage
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "connections.h"
#include "util/connstr.h"
#include "../platform/platform.h"
#include "util/json_helpers.h"
#include "../../liblace/include/util/mem.h"
#include "../../liblace/include/util/str.h"
#include <cjson/cJSON.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <sys/stat.h>

#define CONNECTIONS_FILE "connections.json"

/* Maximum folder nesting depth to prevent stack overflow in recursive
 * functions.
 * TODO: Revise this limit after testing with the connections editor to ensure
 * it works well with deeply nested folder structures created by users. */
#define MAX_FOLDER_DEPTH 100

/* UUID generation is provided by liblace: lace_generate_uuid() */

/* ============================================================================
 * Internal Helpers
 * ============================================================================
 */

/* Get path to connections.json file */
static char *get_connections_path(void) {
  const char *config_dir = platform_get_config_dir();
  if (!config_dir)
    return NULL;

  return str_printf("%s%s%s", config_dir, LACE_PATH_SEP_STR, CONNECTIONS_FILE);
}

/* Initialize a connection item as a folder */
static void init_folder_item(ConnectionItem *item, const char *name) {
  item->type = CONN_ITEM_FOLDER;
  item->folder.name = str_dup(name);
  item->folder.expanded = true;
  item->folder.children = NULL;
  item->folder.num_children = 0;
  item->folder.children_capacity = 0;
  item->parent = NULL;
}

/* Children live by value in their folder's array, so growing or shifting
 * an array moves items, and every parent pointer into it goes stale. After
 * any change to the tree, re-link the whole of it from the root. */
static void relink_children(ConnectionItem *folder, int depth) {
  if (!folder || folder->type != CONN_ITEM_FOLDER || depth > MAX_FOLDER_DEPTH)
    return;
  for (size_t i = 0; i < folder->folder.num_children; i++) {
    ConnectionItem *child = &folder->folder.children[i];
    child->parent = folder;
    relink_children(child, depth + 1);
  }
}

/* The root above item: its ancestors were not moved by a change below them,
 * so the path up is valid before the change */
static ConnectionItem *tree_root(ConnectionItem *item) {
  int guard = 0;
  while (item && item->parent && guard++ <= MAX_FOLDER_DEPTH)
    item = item->parent;
  return item;
}

/* Ensure folder has capacity for more children */
static void folder_ensure_capacity(ConnectionFolder *folder, size_t needed) {
  if (!folder)
    return;

  size_t required = folder->num_children + needed;
  if (folder->children_capacity >= required)
    return;

  size_t new_cap = folder->children_capacity == 0 ? 8 : folder->children_capacity;
  while (new_cap < required) {
    new_cap *= 2;
  }

  size_t old_cap = folder->children_capacity;
  folder->children = safe_reallocarray(folder->children, new_cap, sizeof(ConnectionItem));
  memset(&folder->children[old_cap], 0, (new_cap - old_cap) * sizeof(ConnectionItem));
  folder->children_capacity = new_cap;
}

/* ============================================================================
 * JSON Parsing
 * ============================================================================
 */

static bool parse_connection(cJSON *json, SavedConnection *conn) {
  /* Initialize all pointers to NULL for safe cleanup on failure */
  conn->id = NULL;
  conn->name = NULL;
  conn->driver = NULL;
  conn->host = NULL;
  conn->database = NULL;
  conn->user = NULL;
  conn->password = NULL;
  conn->port = json_get_int(json, "port", 0);
  conn->save_password = json_get_bool(json, "save_password", false);

  /* SSH tunnel fields */
  conn->ssh = json_get_bool(json, "ssh", false);
  conn->ssh_host = NULL;
  conn->ssh_user = NULL;
  conn->ssh_password = NULL;
  conn->ssh_key = NULL;
  conn->ssh_port = json_get_int(json, "ssh_port", 0);
  conn->save_ssh_password = json_get_bool(json, "save_ssh_password", false);

  conn->id = json_dup_string_or(json, "id", "");
  if (!conn->id)
    goto cleanup;
  conn->name = json_dup_string_or(json, "name", "");
  if (!conn->name)
    goto cleanup;
  conn->driver = json_dup_string_or(json, "driver", "");
  if (!conn->driver)
    goto cleanup;
  conn->host = json_dup_string_or(json, "host", "");
  if (!conn->host)
    goto cleanup;
  conn->database = json_dup_string_or(json, "database", "");
  if (!conn->database)
    goto cleanup;
  conn->user = json_dup_string_or(json, "user", "");
  if (!conn->user)
    goto cleanup;
  conn->password = json_dup_string_or(json, "password", "");
  if (!conn->password)
    goto cleanup;

  /* SSH fields (optional) */
  if (conn->ssh) {
    conn->ssh_host = json_dup_string_or(json, "ssh_host", "");
    if (!conn->ssh_host)
      goto cleanup;
    conn->ssh_user = json_dup_string_or(json, "ssh_user", "");
    if (!conn->ssh_user)
      goto cleanup;
    conn->ssh_password = json_dup_string_or(json, "ssh_password", "");
    if (!conn->ssh_password)
      goto cleanup;
    conn->ssh_key = json_dup_string_or(json, "ssh_key", "");
    if (!conn->ssh_key)
      goto cleanup;
  }

  return true;

cleanup:
  FREE_NULL(conn->id);
  FREE_NULL(conn->name);
  FREE_NULL(conn->driver);
  FREE_NULL(conn->host);
  FREE_NULL(conn->database);
  FREE_NULL(conn->user);
  str_secure_free(conn->password);
  conn->password = NULL;
  FREE_NULL(conn->ssh_host);
  FREE_NULL(conn->ssh_user);
  str_secure_free(conn->ssh_password);
  conn->ssh_password = NULL;
  FREE_NULL(conn->ssh_key);
  return false;
}

typedef enum { PARSE_OK, PARSE_SKIP, PARSE_FAIL } ParseResult;

static ParseResult parse_item(cJSON *json, ConnectionItem *item,
                              ConnectionItem *parent, int depth);

static ParseResult parse_item(cJSON *json, ConnectionItem *item,
                              ConnectionItem *parent, int depth) {
  const char *type_str = json_get_string(json, "type", "");
  item->parent = parent;

  if (str_eq(type_str, "folder")) {
    /* The tree code stops at MAX_FOLDER_DEPTH: deeper items could be
     * neither shown nor found, so such a file is refused */
    if (depth > MAX_FOLDER_DEPTH)
      return PARSE_FAIL;
    item->type = CONN_ITEM_FOLDER;
    item->folder.name = json_dup_string_or(json, "name", "");
    item->folder.expanded = json_get_bool(json, "expanded", true);
    item->folder.children = NULL;
    item->folder.num_children = 0;
    item->folder.children_capacity = 0;
    if (!item->folder.name)
      return PARSE_FAIL;
    cJSON *children = json_get_array(json, "children");
    size_t count = children ? (size_t)json_array_size(children) : 0;
    if (count > 0) {
      folder_ensure_capacity(&item->folder, count);
      cJSON *child;
      cJSON_ArrayForEach(child, children) {
        if (item->folder.num_children >= count)
          break;
        ConnectionItem *child_item =
            &item->folder.children[item->folder.num_children];
        ParseResult r = parse_item(child, child_item, item, depth + 1);
        if (r == PARSE_FAIL) {
          connmgr_free_folder(&item->folder); /* Name, children, array */
          memset(&item->folder, 0, sizeof(item->folder));
          return PARSE_FAIL;
        }
        if (r == PARSE_OK)
          item->folder.num_children++;
      }
    }
    return PARSE_OK;
  }
  if (str_eq(type_str, "connection")) {
    item->type = CONN_ITEM_CONNECTION;
    return parse_connection(json, &item->connection) ? PARSE_OK : PARSE_FAIL;
  }
  /* An item of a kind this version does not know (a newer lace, a hand
   * edit): left out of the tree, the rest still loads */
  return PARSE_SKIP;
}

bool connmgr_id_valid(const char *id) {
  if (!id || !id[0] || strlen(id) > 64)
    return false;
  for (const char *c = id; *c; c++) {
    if (!isalnum((unsigned char)*c) && *c != '-')
      return false;
  }
  return true;
}

static void for_each_connection(ConnectionItem *item, int depth,
                                void (*fn)(ConnectionItem *, void *), void *ctx) {
  if (depth > MAX_FOLDER_DEPTH)
    return;
  if (item->type == CONN_ITEM_CONNECTION) {
    fn(item, ctx);
    return;
  }
  for (size_t i = 0; i < item->folder.num_children; i++)
    for_each_connection(&item->folder.children[i], depth + 1, fn, ctx);
}

void connmgr_for_each_connection(ConnectionManager *mgr,
                                 void (*fn)(ConnectionItem *item, void *ctx),
                                 void *ctx) {
  if (mgr && fn)
    for_each_connection(&mgr->root, 0, fn, ctx);
}

/* Missing, unsafe or repeated ids get a new one (and the file is saved
 * with it): an id names the history file and finds the connection again
 * when a session is restored */
typedef struct {
  char **seen;
  size_t n, cap;
  bool changed;
} IdCheck;

static void repair_id(ConnectionItem *item, void *ctx) {
  IdCheck *c = ctx;
  SavedConnection *conn = &item->connection;
  bool dup = false;
  for (size_t i = 0; i < c->n && !dup; i++)
    dup = conn->id && str_eq(c->seen[i], conn->id);
  if (dup || !connmgr_id_valid(conn->id)) {
    free(conn->id);
    conn->id = lace_generate_uuid();
    c->changed = true;
  }
  if (c->n == c->cap) {
    c->cap = c->cap ? c->cap * 2 : 16;
    c->seen = safe_reallocarray(c->seen, c->cap, sizeof(char *));
  }
  c->seen[c->n++] = conn->id;
}

/* ============================================================================
 * JSON Serialization
 * ============================================================================
 */

static cJSON *serialize_connection(const SavedConnection *conn) {
  cJSON *json = cJSON_CreateObject();
  if (!json)
    return NULL;

  JSON_ADD_STR(json, "type", "connection");
  JSON_ADD_STR(json, "id", conn->id);
  JSON_ADD_STR(json, "name", conn->name);
  JSON_ADD_STR(json, "driver", conn->driver);
  JSON_ADD_STR(json, "host", conn->host);
  JSON_ADD_INT(json, "port", conn->port);
  JSON_ADD_STR(json, "database", conn->database);
  JSON_ADD_STR(json, "user", conn->user);
  JSON_ADD_STR(json, "password",
               (conn->save_password && conn->password) ? conn->password : "");
  JSON_ADD_BOOL(json, "save_password", conn->save_password);

  /* SSH tunnel fields */
  if (conn->ssh) {
    JSON_ADD_BOOL(json, "ssh", true);
    JSON_ADD_STR(json, "ssh_host", conn->ssh_host ? conn->ssh_host : "");
    JSON_ADD_STR(json, "ssh_user", conn->ssh_user ? conn->ssh_user : "");
    JSON_ADD_STR(json, "ssh_password",
                 (conn->save_ssh_password && conn->ssh_password) ? conn->ssh_password : "");
    JSON_ADD_INT(json, "ssh_port", conn->ssh_port);
    JSON_ADD_BOOL(json, "save_ssh_password", conn->save_ssh_password);
    JSON_ADD_STR(json, "ssh_key", conn->ssh_key ? conn->ssh_key : "");
  }

  return json;
}

static cJSON *serialize_item(const ConnectionItem *item);

static cJSON *serialize_folder(const ConnectionFolder *folder) {
  cJSON *json = cJSON_CreateObject();
  if (!json)
    return NULL;

  JSON_ADD_STR(json, "type", "folder");
  JSON_ADD_STR(json, "name", folder->name);
  JSON_ADD_BOOL(json, "expanded", folder->expanded);

  cJSON *children = cJSON_CreateArray();
  if (!children) {
    cJSON_Delete(json);
    return NULL;
  }

  for (size_t i = 0; i < folder->num_children; i++) {
    cJSON *child = serialize_item(&folder->children[i]);
    if (!child) {
      cJSON_Delete(children);
      cJSON_Delete(json);
      return NULL;
    }
    cJSON_AddItemToArray(children, child);
  }

  cJSON_AddItemToObject(json, "children", children);
  return json;
}

static cJSON *serialize_item(const ConnectionItem *item) {
  if (item->type == CONN_ITEM_FOLDER) {
    return serialize_folder(&item->folder);
  } else {
    return serialize_connection(&item->connection);
  }
}

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

ConnectionManager *connmgr_new(void) {
  ConnectionManager *mgr = safe_calloc(1, sizeof(ConnectionManager));

  init_folder_item(&mgr->root, "Connections");
  mgr->modified = false;
  mgr->file_path = get_connections_path();

  return mgr;
}

/* Size and modification time of path (0, 0 if it does not exist) */
static void file_signature(const char *path, long long *size, long long *mtime_ns) {
  struct stat st;
  *size = 0;
  *mtime_ns = 0;
  if (path && stat(path, &st) == 0) {
    *size = (long long)st.st_size;
#ifdef __APPLE__
    *mtime_ns = (long long)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
    *mtime_ns = (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
  }
}

ConnectionManager *connmgr_load(char **error) {
  char *path = get_connections_path();
  if (!path) {
    err_setf(error, "Failed to get config directory");
    return NULL;
  }
  ConnectionManager *mgr = connmgr_load_file(path, error);
  free(path);
  return mgr;
}

ConnectionManager *connmgr_load_file(const char *file, char **error) {
  char *path = str_dup(file);

  /* Check if file exists */
  if (!platform_file_exists(path)) {
    /* No file yet - return empty manager */
    ConnectionManager *mgr = connmgr_new();
    free(mgr->file_path);
    mgr->file_path = path;
    return mgr;
  }

  /* Read file */
  char *content = platform_read_file(path, 10 * 1024 * 1024); /* Max 10MB */
  if (!content || !content[0]) {
    if (content)
      err_setf(error, "Empty file: %s", path);
    else
      err_setf(error, "Failed to read %s: %s", path, strerror(errno));
    str_secure_free(content);
    free(path);
    return NULL;
  }

  /* Parse JSON */
  cJSON *json = cJSON_Parse(content);
  if (!json) {
    /* The error pointer points into content: report its offset (never the
     * text, which holds passwords) before freeing it */
    const char *err_ptr = cJSON_GetErrorPtr();
    err_setf(error, "JSON parse error near byte %ld",
             err_ptr ? (long)(err_ptr - content) : -1L);
    str_secure_free(content);
    free(path);
    return NULL;
  }
  str_secure_free(content); /* Holds saved passwords */

  /* Create manager */
  ConnectionManager *mgr = safe_calloc(1, sizeof(ConnectionManager));

  mgr->file_path = path;

  /* Parse root */
  cJSON *root = json_get_object(json, "root");
  if (root) {
    /* The root must be a folder: everything else reads it as one */
    if (!str_eq(json_get_string(root, "type", ""), "folder") ||
        parse_item(root, &mgr->root, NULL, 0) != PARSE_OK) {
      cJSON_Delete(json);
      free(mgr->file_path);
      free(mgr);
      err_setf(error, "connections.json is not a valid connection list");
      return NULL;
    }
  } else {
    init_folder_item(&mgr->root, "Connections");
  }
  cJSON_Delete(json);

  IdCheck ids = {0};
  connmgr_for_each_connection(mgr, repair_id, &ids);
  free(ids.seen);
  mgr->modified = ids.changed;
  file_signature(mgr->file_path, &mgr->loaded_size, &mgr->loaded_mtime_ns);
  return mgr;
}

bool connmgr_save(ConnectionManager *mgr, char **error) {
  if (!mgr || !mgr->file_path) {
    err_setf(error, "Invalid connection manager");
    return false;
  }
  if (mgr->read_only) {
    err_setf(error, "connections.json could not be read, so it is not "
                    "overwritten: fix or move the file, then reopen");
    return false;
  }
  long long size, mtime;
  file_signature(mgr->file_path, &size, &mtime);
  if (size != mgr->loaded_size || mtime != mgr->loaded_mtime_ns) {
    err_setf(error, "connections.json was changed by another lace meanwhile: "
                    "reopen the connection window to see it");
    return false;
  }

  /* Ensure config directory exists */
  const char *config_dir = platform_get_config_dir();
  if (!config_dir) {
    err_setf(error, "Failed to get config directory");
    return false;
  }

  if (!platform_dir_exists(config_dir)) {
    if (!platform_mkdir(config_dir)) {
      err_setf(error, "Failed to create config directory");
      return false;
    }
  }

  /* Build JSON */
  cJSON *json = cJSON_CreateObject();
  if (!json) {
    err_setf(error, "Out of memory");
    return false;
  }

  cJSON *root = serialize_item(&mgr->root);
  if (!root) {
    cJSON_Delete(json);
    err_setf(error, "Failed to serialize connections");
    return false;
  }
  cJSON_AddItemToObject(json, "root", root);

  /* Write to file */
  char *content = cJSON_Print(json);
  cJSON_Delete(json);

  if (!content) {
    err_setf(error, "Failed to serialize JSON");
    return false;
  }

  bool ok = platform_write_file_private(mgr->file_path, content,
                                        strlen(content));
  int e = errno;
  str_secure_free(content); /* Content may contain passwords */
  if (!ok) {
    err_setf(error, "Failed to write %s: %s", mgr->file_path, strerror(e));
    return false;
  }

  mgr->modified = false;
  file_signature(mgr->file_path, &mgr->loaded_size, &mgr->loaded_mtime_ns);
  return true;
}

void connmgr_free_connection(SavedConnection *conn) {
  if (!conn)
    return;
  free(conn->id);
  free(conn->name);
  free(conn->driver);
  free(conn->host);
  free(conn->database);
  free(conn->user);
  str_secure_free(conn->password);
  free(conn->ssh_host);
  free(conn->ssh_user);
  str_secure_free(conn->ssh_password);
  free(conn->ssh_key);
}

void connmgr_free_folder(ConnectionFolder *folder) {
  if (!folder)
    return;

  free(folder->name);

  for (size_t i = 0; i < folder->num_children; i++) {
    ConnectionItem *child = &folder->children[i];
    if (child->type == CONN_ITEM_FOLDER) {
      connmgr_free_folder(&child->folder);
    } else {
      connmgr_free_connection(&child->connection);
    }
  }
  free(folder->children);
}

void connmgr_free(ConnectionManager *mgr) {
  if (!mgr)
    return;

  connmgr_free_folder(&mgr->root.folder);
  free(mgr->file_path);
  free(mgr);
}

/* ============================================================================
 * Connection CRUD
 * ============================================================================
 */

SavedConnection *connmgr_new_connection(void) {
  SavedConnection *conn = safe_calloc(1, sizeof(SavedConnection));

  conn->id = lace_generate_uuid();
  if (!conn->id) {
    free(conn);
    return NULL;
  }

  conn->name = str_dup("");
  conn->driver = str_dup("");
  conn->host = str_dup("");
  conn->database = str_dup("");
  conn->user = str_dup("");
  conn->password = str_dup("");
  conn->port = 0;
  conn->save_password = false;

  conn->ssh = false;
  conn->ssh_host = str_dup("");
  conn->ssh_user = str_dup("");
  conn->ssh_password = str_dup("");
  conn->ssh_port = 0;
  conn->save_ssh_password = false;
  conn->ssh_key = str_dup("");

  return conn;
}

bool connmgr_add_connection(ConnectionItem *folder, SavedConnection *conn) {
  if (!folder || folder->type != CONN_ITEM_FOLDER || !conn)
    return false;

  ConnectionItem *root = tree_root(folder);
  folder_ensure_capacity(&folder->folder, 1);

  ConnectionItem *item = &folder->folder.children[folder->folder.num_children];
  item->type = CONN_ITEM_CONNECTION;
  item->connection = *conn;
  item->parent = folder;
  free(conn); /* Its contents now belong to the tree */

  folder->folder.num_children++;
  relink_children(root, 0);
  return true;
}

static ConnectionItem *find_by_id_recursive(ConnectionItem *item,
                                            const char *id, int depth) {
  if (depth > MAX_FOLDER_DEPTH)
    return NULL; /* Prevent stack overflow */

  if (item->type == CONN_ITEM_CONNECTION) {
    if (item->connection.id && str_eq(item->connection.id, id)) {
      return item;
    }
  } else {
    for (size_t i = 0; i < item->folder.num_children; i++) {
      ConnectionItem *found =
          find_by_id_recursive(&item->folder.children[i], id, depth + 1);
      if (found)
        return found;
    }
  }
  return NULL;
}

ConnectionItem *connmgr_find_by_id(ConnectionManager *mgr, const char *id) {
  if (!mgr || !id)
    return NULL;
  return find_by_id_recursive(&mgr->root, id, 0);
}

bool connmgr_remove_item(ConnectionManager *mgr, ConnectionItem *item) {
  if (!mgr || !item || !item->parent)
    return false;

  ConnectionFolder *parent = &item->parent->folder;

  /* Find item index in parent */
  size_t idx = 0;
  bool found = false;
  for (size_t i = 0; i < parent->num_children; i++) {
    if (&parent->children[i] == item) {
      idx = i;
      found = true;
      break;
    }
  }

  if (!found)
    return false;

  /* Free item contents */
  if (item->type == CONN_ITEM_FOLDER) {
    connmgr_free_folder(&item->folder);
  } else {
    connmgr_free_connection(&item->connection);
  }

  /* Shift remaining items */
  for (size_t i = idx; i < parent->num_children - 1; i++) {
    parent->children[i] = parent->children[i + 1];
  }
  parent->num_children--;
  relink_children(&mgr->root, 0);

  mgr->modified = true;
  return true;
}

bool connmgr_move_item(ConnectionManager *mgr, ConnectionItem *item,
                       ConnectionItem *new_parent,
                       ConnectionItem *insert_after) {
  if (!mgr || !item || !new_parent || !item->parent) {
    return false;
  }

  /* Can't move to non-folder */
  if (new_parent->type != CONN_ITEM_FOLDER) {
    return false;
  }

  /* Can't move to self or descendant (for folders) */
  if (item == new_parent) {
    return false;
  }

  if (item->type == CONN_ITEM_FOLDER) {
    /* Check if new_parent is a descendant of item */
    ConnectionItem *p = new_parent;
    while (p) {
      if (p == item) {
        return false;
      }
      p = p->parent;
    }
  }

  ConnectionFolder *old_parent_folder = &item->parent->folder;
  ConnectionItem *old_parent_item = item->parent;

  /* Find item index in old parent */
  size_t old_idx = 0;
  bool found = false;
  for (size_t i = 0; i < old_parent_folder->num_children; i++) {
    if (&old_parent_folder->children[i] == item) {
      old_idx = i;
      found = true;
      break;
    }
  }

  if (!found) {
    return false;
  }

  /* Check if moving within same folder */
  bool same_folder = (item->parent == new_parent);

  /* If moving to a different folder that's in the SAME parent array as the
   * source, we need to track its index because removing the source will shift
   * the array */
  size_t new_parent_idx = 0;
  bool new_parent_in_same_array = false;
  if (!same_folder && old_parent_item == new_parent->parent) {
    /* new_parent is a sibling of item - find its index */
    for (size_t i = 0; i < old_parent_folder->num_children; i++) {
      if (&old_parent_folder->children[i] == new_parent) {
        new_parent_idx = i;
        new_parent_in_same_array = true;
        break;
      }
    }
  }

  /* Get folder pointer - will be updated after shift if needed */
  ConnectionFolder *new_parent_folder = &new_parent->folder;

  /* Find insertion position in new parent */
  size_t insert_idx = new_parent_folder->num_children; /* Default: end */
  if (insert_after == NULL) {
    insert_idx = 0; /* Insert at beginning */
  } else {
    for (size_t i = 0; i < new_parent_folder->num_children; i++) {
      if (&new_parent_folder->children[i] == insert_after) {
        insert_idx = i + 1; /* Insert after this item */
        break;
      }
    }
  }

  /* Make a copy of the item data */
  ConnectionItem item_copy = *item;

  if (same_folder) {
    /* Moving within same folder - just reorder */
    if (insert_idx > old_idx) {
      /* Moving down: shift items up */
      for (size_t i = old_idx; i < insert_idx - 1; i++) {
        old_parent_folder->children[i] = old_parent_folder->children[i + 1];
      }
      old_parent_folder->children[insert_idx - 1] = item_copy;
    } else if (insert_idx < old_idx) {
      /* Moving up: shift items down */
      for (size_t i = old_idx; i > insert_idx; i--) {
        old_parent_folder->children[i] = old_parent_folder->children[i - 1];
      }
      old_parent_folder->children[insert_idx] = item_copy;
    }
    /* If insert_idx == old_idx, no change needed */

    /* After reordering, update parent pointers for ALL folders' children
     * because items moved to new addresses */
    for (size_t i = 0; i < old_parent_folder->num_children; i++) {
      ConnectionItem *child = &old_parent_folder->children[i];
      if (child->type == CONN_ITEM_FOLDER) {
        for (size_t j = 0; j < child->folder.num_children; j++) {
          child->folder.children[j].parent = child;
        }
      }
    }
  } else {
    /* Moving to different folder */
    /* Remove from old parent (shift remaining) */
    for (size_t i = old_idx; i < old_parent_folder->num_children - 1; i++) {
      old_parent_folder->children[i] = old_parent_folder->children[i + 1];
    }
    old_parent_folder->num_children--;

    /* Update parent pointers for shifted items in old folder */
    for (size_t i = old_idx; i < old_parent_folder->num_children; i++) {
      ConnectionItem *child = &old_parent_folder->children[i];
      if (child->type == CONN_ITEM_FOLDER) {
        for (size_t j = 0; j < child->folder.num_children; j++) {
          child->folder.children[j].parent = child;
        }
      }
    }

    /* If new_parent was in the same array and AFTER the removed item,
     * its position shifted - update the pointer */
    if (new_parent_in_same_array && new_parent_idx > old_idx) {
      new_parent_idx--;
      new_parent = &old_parent_folder->children[new_parent_idx];
      new_parent_folder = &new_parent->folder;
    }

    /* Ensure capacity in new parent */
    folder_ensure_capacity(new_parent_folder, 1);

    /* Shift items to make room at insert position */
    for (size_t i = new_parent_folder->num_children; i > insert_idx; i--) {
      new_parent_folder->children[i] = new_parent_folder->children[i - 1];
    }

    /* Insert at position */
    new_parent_folder->children[insert_idx] = item_copy;
    new_parent_folder->children[insert_idx].parent = new_parent;
    new_parent_folder->num_children++;

    /* Update parent pointers for shifted items in new folder */
    for (size_t i = insert_idx; i < new_parent_folder->num_children; i++) {
      ConnectionItem *child = &new_parent_folder->children[i];
      if (child->type == CONN_ITEM_FOLDER) {
        for (size_t j = 0; j < child->folder.num_children; j++) {
          child->folder.children[j].parent = child;
        }
      }
    }
  }

  relink_children(&mgr->root, 0);
  mgr->modified = true;
  return true;
}

/* ============================================================================
 * Folder CRUD
 * ============================================================================
 */

ConnectionFolder *connmgr_new_folder(const char *name) {
  ConnectionFolder *folder = safe_calloc(1, sizeof(ConnectionFolder));

  folder->name = str_dup(name ? name : "New Folder");
  folder->expanded = true;
  folder->children = NULL;
  folder->num_children = 0;
  folder->children_capacity = 0;

  if (!folder->name) {
    free(folder);
    return NULL;
  }

  return folder;
}

bool connmgr_add_folder(ConnectionItem *parent, ConnectionFolder *folder) {
  if (!parent || parent->type != CONN_ITEM_FOLDER || !folder)
    return false;

  ConnectionItem *root = tree_root(parent);
  folder_ensure_capacity(&parent->folder, 1);

  ConnectionItem *item = &parent->folder.children[parent->folder.num_children];
  item->type = CONN_ITEM_FOLDER;
  item->folder = *folder;
  item->parent = parent;
  free(folder); /* Its contents now belong to the tree */

  parent->folder.num_children++;
  relink_children(root, 0);
  return true;
}

/* ============================================================================
 * Tree Navigation
 * ============================================================================
 */

static size_t count_visible_recursive(ConnectionItem *item, int depth) {
  if (depth > MAX_FOLDER_DEPTH)
    return 0; /* Prevent stack overflow */

  size_t count = 1; /* Count self */

  if (item->type == CONN_ITEM_FOLDER && item->folder.expanded) {
    for (size_t i = 0; i < item->folder.num_children; i++) {
      count += count_visible_recursive(&item->folder.children[i], depth + 1);
    }
  }

  return count;
}

size_t connmgr_count_visible(ConnectionManager *mgr) {
  if (!mgr)
    return 0;

  /* Don't count root itself, just its visible contents */
  size_t count = 0;
  if (mgr->root.folder.expanded) {
    for (size_t i = 0; i < mgr->root.folder.num_children; i++) {
      count += count_visible_recursive(&mgr->root.folder.children[i], 0);
    }
  }
  return count;
}

static ConnectionItem *get_visible_item_recursive(ConnectionItem *item,
                                                  size_t *idx, size_t target,
                                                  int depth) {
  if (depth > MAX_FOLDER_DEPTH)
    return NULL; /* Prevent stack overflow */

  if (*idx == target)
    return item;
  (*idx)++;

  if (item->type == CONN_ITEM_FOLDER && item->folder.expanded) {
    for (size_t i = 0; i < item->folder.num_children; i++) {
      ConnectionItem *found = get_visible_item_recursive(
          &item->folder.children[i], idx, target, depth + 1);
      if (found)
        return found;
    }
  }

  return NULL;
}

ConnectionItem *connmgr_get_visible_item(ConnectionManager *mgr,
                                         size_t target) {
  if (!mgr)
    return NULL;

  /* Skip root, iterate its children */
  size_t idx = 0;
  if (mgr->root.folder.expanded) {
    for (size_t i = 0; i < mgr->root.folder.num_children; i++) {
      ConnectionItem *found = get_visible_item_recursive(
          &mgr->root.folder.children[i], &idx, target, 0);
      if (found)
        return found;
    }
  }
  return NULL;
}

/* ---- Filtered view: items whose names hold the filter text ---- */

static bool name_has(const char *name, const char *filter) {
  if (!name)
    return false;
  size_t n = strlen(filter);
  for (const char *p = name; *p; p++) {
    if (strncasecmp(p, filter, n) == 0)
      return true; /* ASCII case folded; other text matches as is */
  }
  return n == 0;
}

/* Does item, or anything below it, match? */
static bool subtree_matches(ConnectionItem *item, const char *filter, int depth) {
  if (depth > MAX_FOLDER_DEPTH)
    return false;
  if (name_has(connmgr_item_name(item), filter))
    return true;
  if (item->type == CONN_ITEM_FOLDER) {
    for (size_t i = 0; i < item->folder.num_children; i++)
      if (subtree_matches(&item->folder.children[i], filter, depth + 1))
        return true;
  }
  return false;
}

/* Walk the filtered list: an item is listed if it or something below it
 * matches, or a folder above it matched (its whole content shows); folders
 * are shown open. Returns the item at target (counting in *idx). */
static ConnectionItem *walk_filtered(ConnectionItem *item, const char *filter,
                                     bool inside_match, size_t *idx,
                                     size_t target, int depth) {
  if (depth > MAX_FOLDER_DEPTH)
    return NULL;
  bool self = name_has(connmgr_item_name(item), filter);
  if (!inside_match && !subtree_matches(item, filter, depth))
    return NULL;
  if (*idx == target)
    return item;
  (*idx)++;
  if (item->type == CONN_ITEM_FOLDER) {
    for (size_t i = 0; i < item->folder.num_children; i++) {
      ConnectionItem *found =
          walk_filtered(&item->folder.children[i], filter, inside_match || self,
                        idx, target, depth + 1);
      if (found)
        return found;
    }
  }
  return NULL;
}

size_t connmgr_count_filtered(ConnectionManager *mgr, const char *filter) {
  if (!mgr)
    return 0;
  if (!filter || !filter[0])
    return connmgr_count_visible(mgr);
  size_t idx = 0;
  for (size_t i = 0; i < mgr->root.folder.num_children; i++)
    walk_filtered(&mgr->root.folder.children[i], filter, false, &idx, SIZE_MAX, 0);
  return idx;
}

ConnectionItem *connmgr_get_filtered_item(ConnectionManager *mgr, const char *filter,
                                          size_t target) {
  if (!mgr)
    return NULL;
  if (!filter || !filter[0])
    return connmgr_get_visible_item(mgr, target);
  size_t idx = 0;
  for (size_t i = 0; i < mgr->root.folder.num_children; i++) {
    ConnectionItem *found =
        walk_filtered(&mgr->root.folder.children[i], filter, false, &idx, target, 0);
    if (found)
      return found;
  }
  return NULL;
}

int connmgr_get_item_depth(ConnectionItem *item) {
  int depth = 0;
  ConnectionItem *p = item ? item->parent : NULL;
  while (p) {
    depth++;
    p = p->parent;
  }
  /* Subtract 1 because root shouldn't count as depth */
  return depth > 0 ? depth - 1 : 0;
}

void connmgr_toggle_folder(ConnectionItem *item) {
  if (item && item->type == CONN_ITEM_FOLDER) {
    item->folder.expanded = !item->folder.expanded;
  }
}

/* ============================================================================
 * Connection Strings
 * ============================================================================
 */

char *connmgr_build_connstr(const SavedConnection *conn) {
  /* Use liblace's connection string builder */
  return lace_saved_conn_to_connstr(conn);
}

SavedConnection *connmgr_parse_connstr(const char *url, char **error) {
  if (!url || !url[0]) {
    err_setf(error, "Empty URL");
    return NULL;
  }

  ConnString *cs = connstr_parse(url, error);
  if (!cs)
    return NULL;

  SavedConnection *conn = connmgr_new_connection();
  if (!conn) {
    connstr_free(cs);
    err_setf(error, "Out of memory");
    return NULL;
  }

  /* Replace default empty strings with actual values */
  free(conn->driver);
  conn->driver = str_dup(cs->driver ? cs->driver : "");
  free(conn->host);
  conn->host = str_dup(cs->host ? cs->host : "");
  free(conn->database);
  conn->database = str_dup(cs->database ? cs->database : "");
  free(conn->user);
  conn->user = str_dup(cs->user ? cs->user : "");
  str_secure_free(conn->password);
  conn->password = str_dup(cs->password ? cs->password : "");

  conn->port = cs->port;
  conn->save_password = (cs->password && cs->password[0]);

  /* SSH tunnel fields */
  conn->ssh = cs->ssh;
  free(conn->ssh_host); /* The new connection's empty defaults */
  conn->ssh_host = str_dup(cs->ssh_host ? cs->ssh_host : "");
  free(conn->ssh_user);
  conn->ssh_user = str_dup(cs->ssh_user ? cs->ssh_user : "");
  str_secure_free(conn->ssh_password);
  conn->ssh_password = str_dup(cs->ssh_password ? cs->ssh_password : "");
  conn->ssh_port = cs->ssh_port;
  conn->save_ssh_password = (cs->ssh_password && cs->ssh_password[0]);
  free(conn->ssh_key);
  const char *key = connstr_get_option(cs, "ssh_key");
  conn->ssh_key = str_dup(key ? key : "");

  /* Generate a default name from the connection */
  free(conn->name);
  if (str_eq(cs->driver, "sqlite")) {
    /* Use filename for SQLite */
    const char *name = cs->database ? cs->database : "database";
    if (cs->database) {
      const char *slash = strrchr(cs->database, '/');
      if (slash)
        name = slash + 1;
    }
    conn->name = str_dup(name);
  } else if (cs->ssh) {
    /* For SSH connections: show ssh_host -> host/database */
    conn->name = str_printf("%s/%s (via %s)",
                            cs->host ? cs->host : "localhost",
                            cs->database ? cs->database : "",
                            cs->ssh_host ? cs->ssh_host : "ssh");
    if (!conn->name) {
      connmgr_free_connection(conn);
      free(conn);
      connstr_free(cs);
      err_setf(error, "Out of memory");
      return NULL;
    }
  } else {
    /* Use host/database for network DBs */
    conn->name = str_printf("%s/%s", cs->host ? cs->host : "localhost",
                            cs->database ? cs->database : "");
    if (!conn->name) {
      connmgr_free_connection(conn);
      free(conn);
      connstr_free(cs);
      err_setf(error, "Out of memory");
      return NULL;
    }
  }

  connstr_free(cs);
  return conn;
}

/* ============================================================================
 * Item Helpers
 * ============================================================================
 */

const char *connmgr_item_name(const ConnectionItem *item) {
  if (!item)
    return "";
  if (item->type == CONN_ITEM_FOLDER) {
    return item->folder.name ? item->folder.name : "";
  } else {
    return item->connection.name ? item->connection.name : "";
  }
}

bool connmgr_is_folder(const ConnectionItem *item) {
  return item && item->type == CONN_ITEM_FOLDER;
}

bool connmgr_is_connection(const ConnectionItem *item) {
  return item && item->type == CONN_ITEM_CONNECTION;
}
