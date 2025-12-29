/*
 * Lace
 * Connection Manager - Saved Connections Storage
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "connections.h"
#include "../db/connstr.h"
#include "../platform/platform.h"
#include "../util/str.h"
#include <cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CONNECTIONS_FILE "connections.json"

/* ============================================================================
 * UUID Generation
 * ============================================================================
 */

/* Get cryptographically secure random bytes */
static bool secure_random_bytes(unsigned char *buf, size_t len) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
  /* Use arc4random_buf on BSD/macOS - always available, no seeding needed */
  arc4random_buf(buf, len);
  return true;
#else
  /* Fallback to /dev/urandom */
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0)
    return false;

  ssize_t bytes_read = read(fd, buf, len);
  close(fd);

  return bytes_read == (ssize_t)len;
#endif
}

/* Generate a simple UUID v4 string */
static char *generate_uuid(void) {
  char *uuid = malloc(37);
  if (!uuid)
    return NULL;

  unsigned char bytes[16];
  if (!secure_random_bytes(bytes, sizeof(bytes))) {
    /* Fallback to weak RNG if secure random fails */
    static bool seeded = false;
    if (!seeded) {
      srand((unsigned int)time(NULL) ^ (unsigned int)getpid());
      seeded = true;
    }
    for (int i = 0; i < 16; i++) {
      bytes[i] = (unsigned char)(rand() & 0xff);
    }
  }

  /* Set version (4) and variant bits */
  bytes[6] = (bytes[6] & 0x0f) | 0x40;
  bytes[8] = (bytes[8] & 0x3f) | 0x80;

  snprintf(uuid, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           bytes[0], bytes[1], bytes[2], bytes[3],
           bytes[4], bytes[5], bytes[6], bytes[7],
           bytes[8], bytes[9], bytes[10], bytes[11],
           bytes[12], bytes[13], bytes[14], bytes[15]);

  return uuid;
}

/* ============================================================================
 * Internal Helpers
 * ============================================================================
 */

static void set_error(char **err, const char *fmt, ...) {
  if (!err)
    return;

  va_list args;
  va_start(args, fmt);
  *err = str_vprintf(fmt, args);
  va_end(args);
}

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

/* Ensure folder has capacity for more children */
static bool folder_ensure_capacity(ConnectionFolder *folder, size_t needed) {
  if (folder->num_children + needed <= folder->children_capacity)
    return true;

  size_t new_cap = folder->children_capacity == 0 ? 8 : folder->children_capacity * 2;
  while (new_cap < folder->num_children + needed)
    new_cap *= 2;

  ConnectionItem *new_children = realloc(folder->children, new_cap * sizeof(ConnectionItem));
  if (!new_children)
    return false;

  folder->children = new_children;
  folder->children_capacity = new_cap;
  return true;
}

/* ============================================================================
 * JSON Parsing
 * ============================================================================
 */

static bool parse_connection(cJSON *json, SavedConnection *conn) {
  cJSON *id = cJSON_GetObjectItem(json, "id");
  cJSON *name = cJSON_GetObjectItem(json, "name");
  cJSON *driver = cJSON_GetObjectItem(json, "driver");
  cJSON *host = cJSON_GetObjectItem(json, "host");
  cJSON *database = cJSON_GetObjectItem(json, "database");
  cJSON *user = cJSON_GetObjectItem(json, "user");
  cJSON *password = cJSON_GetObjectItem(json, "password");
  cJSON *port = cJSON_GetObjectItem(json, "port");
  cJSON *save_password = cJSON_GetObjectItem(json, "save_password");

  /* Initialize all pointers to NULL for safe cleanup on failure */
  conn->id = NULL;
  conn->name = NULL;
  conn->driver = NULL;
  conn->host = NULL;
  conn->database = NULL;
  conn->user = NULL;
  conn->password = NULL;
  conn->port = cJSON_IsNumber(port) ? port->valueint : 0;
  conn->save_password =
      cJSON_IsBool(save_password) ? cJSON_IsTrue(save_password) : false;

  conn->id = str_dup(cJSON_IsString(id) ? id->valuestring : "");
  if (!conn->id)
    goto cleanup;
  conn->name = str_dup(cJSON_IsString(name) ? name->valuestring : "");
  if (!conn->name)
    goto cleanup;
  conn->driver = str_dup(cJSON_IsString(driver) ? driver->valuestring : "");
  if (!conn->driver)
    goto cleanup;
  conn->host = str_dup(cJSON_IsString(host) ? host->valuestring : "");
  if (!conn->host)
    goto cleanup;
  conn->database =
      str_dup(cJSON_IsString(database) ? database->valuestring : "");
  if (!conn->database)
    goto cleanup;
  conn->user = str_dup(cJSON_IsString(user) ? user->valuestring : "");
  if (!conn->user)
    goto cleanup;
  conn->password =
      str_dup(cJSON_IsString(password) ? password->valuestring : "");
  if (!conn->password)
    goto cleanup;

  return true;

cleanup:
  free(conn->id);
  conn->id = NULL;
  free(conn->name);
  conn->name = NULL;
  free(conn->driver);
  conn->driver = NULL;
  free(conn->host);
  conn->host = NULL;
  free(conn->database);
  conn->database = NULL;
  free(conn->user);
  conn->user = NULL;
  str_secure_free(conn->password);
  conn->password = NULL;
  return false;
}

static bool parse_item(cJSON *json, ConnectionItem *item, ConnectionItem *parent);

static bool parse_folder(cJSON *json, ConnectionFolder *folder) {
  cJSON *name = cJSON_GetObjectItem(json, "name");
  cJSON *expanded = cJSON_GetObjectItem(json, "expanded");
  cJSON *children = cJSON_GetObjectItem(json, "children");

  folder->name = str_dup(cJSON_IsString(name) ? name->valuestring : "");
  folder->expanded = cJSON_IsBool(expanded) ? cJSON_IsTrue(expanded) : true;
  folder->children = NULL;
  folder->num_children = 0;
  folder->children_capacity = 0;

  if (!folder->name)
    return false;

  if (cJSON_IsArray(children)) {
    size_t count = (size_t)cJSON_GetArraySize(children);
    if (count > 0) {
      if (!folder_ensure_capacity(folder, count))
        return false;

      /* Note: item->parent is set in parse_item, but we need a way to
       * pass the parent. We'll handle this in the caller. */
    }
  }

  return true;
}

static bool parse_item(cJSON *json, ConnectionItem *item, ConnectionItem *parent) {
  cJSON *type = cJSON_GetObjectItem(json, "type");
  const char *type_str = cJSON_IsString(type) ? type->valuestring : "";

  item->parent = parent;

  if (str_eq(type_str, "folder")) {
    item->type = CONN_ITEM_FOLDER;
    if (!parse_folder(json, &item->folder))
      return false;

    /* Parse children */
    cJSON *children = cJSON_GetObjectItem(json, "children");
    if (cJSON_IsArray(children)) {
      size_t count = (size_t)cJSON_GetArraySize(children);
      if (count > 0) {
        if (!folder_ensure_capacity(&item->folder, count))
          return false;

        cJSON *child;
        cJSON_ArrayForEach(child, children) {
          ConnectionItem *child_item = &item->folder.children[item->folder.num_children];
          if (!parse_item(child, child_item, item)) {
            /* Cleanup already parsed children */
            for (size_t i = 0; i < item->folder.num_children; i++) {
              if (connmgr_is_folder(&item->folder.children[i]))
                connmgr_free_folder(&item->folder.children[i].folder);
              else
                connmgr_free_connection(&item->folder.children[i].connection);
            }
            return false;
          }
          item->folder.num_children++;
        }
      }
    }
  } else if (str_eq(type_str, "connection")) {
    item->type = CONN_ITEM_CONNECTION;
    if (!parse_connection(json, &item->connection))
      return false;
  } else {
    return false;
  }

  return true;
}

/* ============================================================================
 * JSON Serialization
 * ============================================================================
 */

static cJSON *serialize_connection(const SavedConnection *conn) {
  cJSON *json = cJSON_CreateObject();
  if (!json)
    return NULL;

  cJSON_AddStringToObject(json, "type", "connection");
  cJSON_AddStringToObject(json, "id", conn->id ? conn->id : "");
  cJSON_AddStringToObject(json, "name", conn->name ? conn->name : "");
  cJSON_AddStringToObject(json, "driver", conn->driver ? conn->driver : "");
  cJSON_AddStringToObject(json, "host", conn->host ? conn->host : "");
  cJSON_AddNumberToObject(json, "port", conn->port);
  cJSON_AddStringToObject(json, "database", conn->database ? conn->database : "");
  cJSON_AddStringToObject(json, "user", conn->user ? conn->user : "");
  if (conn->save_password && conn->password) {
    cJSON_AddStringToObject(json, "password", conn->password);
  } else {
    cJSON_AddStringToObject(json, "password", "");
  }
  cJSON_AddBoolToObject(json, "save_password", conn->save_password);

  return json;
}

static cJSON *serialize_item(const ConnectionItem *item);

static cJSON *serialize_folder(const ConnectionFolder *folder) {
  cJSON *json = cJSON_CreateObject();
  if (!json)
    return NULL;

  cJSON_AddStringToObject(json, "type", "folder");
  cJSON_AddStringToObject(json, "name", folder->name ? folder->name : "");
  cJSON_AddBoolToObject(json, "expanded", folder->expanded);

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
  ConnectionManager *mgr = calloc(1, sizeof(ConnectionManager));
  if (!mgr)
    return NULL;

  init_folder_item(&mgr->root, "Connections");
  mgr->modified = false;
  mgr->file_path = get_connections_path();

  return mgr;
}

ConnectionManager *connmgr_load(char **error) {
  char *path = get_connections_path();
  if (!path) {
    set_error(error, "Failed to get config directory");
    return NULL;
  }

  /* Check if file exists */
  if (!platform_file_exists(path)) {
    /* No file yet - return empty manager */
    ConnectionManager *mgr = connmgr_new();
    free(path);
    return mgr;
  }

  /* Read file */
  FILE *f = fopen(path, "r");
  if (!f) {
    set_error(error, "Failed to open %s: %s", path, strerror(errno));
    free(path);
    return NULL;
  }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (size < 0) {
    fclose(f);
    set_error(error, "Failed to determine file size: %s", strerror(errno));
    free(path);
    return NULL;
  }

  if (size == 0 || size > 10 * 1024 * 1024) { /* Max 10MB */
    fclose(f);
    set_error(error, "Invalid file size");
    free(path);
    return NULL;
  }

  char *content = malloc((size_t)size + 1);
  if (!content) {
    fclose(f);
    set_error(error, "Out of memory");
    free(path);
    return NULL;
  }

  size_t read = fread(content, 1, (size_t)size, f);
  fclose(f);
  content[read] = '\0';

  /* Parse JSON */
  cJSON *json = cJSON_Parse(content);
  free(content);

  if (!json) {
    const char *err_ptr = cJSON_GetErrorPtr();
    set_error(error, "JSON parse error: %s", err_ptr ? err_ptr : "unknown");
    free(path);
    return NULL;
  }

  /* Create manager */
  ConnectionManager *mgr = calloc(1, sizeof(ConnectionManager));
  if (!mgr) {
    cJSON_Delete(json);
    set_error(error, "Out of memory");
    free(path);
    return NULL;
  }

  mgr->file_path = path;

  /* Parse root */
  cJSON *root = cJSON_GetObjectItem(json, "root");
  if (root && cJSON_IsObject(root)) {
    if (!parse_item(root, &mgr->root, NULL)) {
      cJSON_Delete(json);
      free(mgr->file_path);
      free(mgr);
      set_error(error, "Failed to parse root folder");
      return NULL;
    }
  } else {
    init_folder_item(&mgr->root, "Connections");
  }

  cJSON_Delete(json);
  return mgr;
}

bool connmgr_save(ConnectionManager *mgr, char **error) {
  if (!mgr || !mgr->file_path) {
    set_error(error, "Invalid connection manager");
    return false;
  }

  /* Ensure config directory exists */
  const char *config_dir = platform_get_config_dir();
  if (!config_dir) {
    set_error(error, "Failed to get config directory");
    return false;
  }

  if (!platform_dir_exists(config_dir)) {
    if (!platform_mkdir(config_dir)) {
      set_error(error, "Failed to create config directory");
      return false;
    }
  }

  /* Build JSON */
  cJSON *json = cJSON_CreateObject();
  if (!json) {
    set_error(error, "Out of memory");
    return false;
  }

  cJSON *root = serialize_item(&mgr->root);
  if (!root) {
    cJSON_Delete(json);
    set_error(error, "Failed to serialize connections");
    return false;
  }
  cJSON_AddItemToObject(json, "root", root);

  /* Write to file */
  char *content = cJSON_Print(json);
  cJSON_Delete(json);

  if (!content) {
    set_error(error, "Failed to serialize JSON");
    return false;
  }

  FILE *f = fopen(mgr->file_path, "w");
  if (!f) {
    free(content);
    set_error(error, "Failed to open %s for writing: %s", mgr->file_path, strerror(errno));
    return false;
  }

  /* Set file permissions to 0600 (owner read/write only) */
#ifndef LACE_OS_WINDOWS
  chmod(mgr->file_path, 0600);
#endif

  size_t len = strlen(content);
  size_t written = fwrite(content, 1, len, f);
  fclose(f);
  free(content);

  if (written != len) {
    set_error(error, "Failed to write all data");
    return false;
  }

  mgr->modified = false;
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
  SavedConnection *conn = calloc(1, sizeof(SavedConnection));
  if (!conn)
    return NULL;

  conn->id = generate_uuid();
  conn->name = str_dup("");
  conn->driver = str_dup("");
  conn->host = str_dup("");
  conn->database = str_dup("");
  conn->user = str_dup("");
  conn->password = str_dup("");
  conn->port = 0;
  conn->save_password = false;

  if (!conn->id || !conn->name || !conn->driver || !conn->host ||
      !conn->database || !conn->user || !conn->password) {
    connmgr_free_connection(conn);
    free(conn);
    return NULL;
  }

  return conn;
}

bool connmgr_add_connection(ConnectionItem *folder, SavedConnection *conn) {
  if (!folder || folder->type != CONN_ITEM_FOLDER || !conn)
    return false;

  if (!folder_ensure_capacity(&folder->folder, 1))
    return false;

  ConnectionItem *item = &folder->folder.children[folder->folder.num_children];
  item->type = CONN_ITEM_CONNECTION;
  item->connection = *conn;
  item->parent = folder;

  folder->folder.num_children++;
  return true;
}

static ConnectionItem *find_by_id_recursive(ConnectionItem *item, const char *id) {
  if (item->type == CONN_ITEM_CONNECTION) {
    if (item->connection.id && str_eq(item->connection.id, id)) {
      return item;
    }
  } else {
    for (size_t i = 0; i < item->folder.num_children; i++) {
      ConnectionItem *found = find_by_id_recursive(&item->folder.children[i], id);
      if (found)
        return found;
    }
  }
  return NULL;
}

ConnectionItem *connmgr_find_by_id(ConnectionManager *mgr, const char *id) {
  if (!mgr || !id)
    return NULL;
  return find_by_id_recursive(&mgr->root, id);
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

  mgr->modified = true;
  return true;
}

bool connmgr_move_item(ConnectionManager *mgr, ConnectionItem *item,
                       ConnectionItem *new_parent, ConnectionItem *insert_after) {
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

  /* If moving to a different folder that's in the SAME parent array as the source,
   * we need to track its index because removing the source will shift the array */
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
  size_t insert_idx = new_parent_folder->num_children;  /* Default: end */
  if (insert_after == NULL) {
    insert_idx = 0;  /* Insert at beginning */
  } else {
    for (size_t i = 0; i < new_parent_folder->num_children; i++) {
      if (&new_parent_folder->children[i] == insert_after) {
        insert_idx = i + 1;  /* Insert after this item */
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
    if (!folder_ensure_capacity(new_parent_folder, 1)) {
      /* Failed - try to restore in old parent */
      folder_ensure_capacity(old_parent_folder, 1);
      /* Shift to make room at old position */
      for (size_t i = old_parent_folder->num_children; i > old_idx; i--) {
        old_parent_folder->children[i] = old_parent_folder->children[i - 1];
      }
      old_parent_folder->children[old_idx] = item_copy;
      old_parent_folder->num_children++;
      return false;
    }

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

  mgr->modified = true;
  return true;
}

/* ============================================================================
 * Folder CRUD
 * ============================================================================
 */

ConnectionFolder *connmgr_new_folder(const char *name) {
  ConnectionFolder *folder = calloc(1, sizeof(ConnectionFolder));
  if (!folder)
    return NULL;

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

  if (!folder_ensure_capacity(&parent->folder, 1))
    return false;

  ConnectionItem *item = &parent->folder.children[parent->folder.num_children];
  item->type = CONN_ITEM_FOLDER;
  item->folder = *folder;
  item->parent = parent;

  parent->folder.num_children++;
  return true;
}

/* ============================================================================
 * Tree Navigation
 * ============================================================================
 */

static size_t count_visible_recursive(ConnectionItem *item) {
  size_t count = 1; /* Count self */

  if (item->type == CONN_ITEM_FOLDER && item->folder.expanded) {
    for (size_t i = 0; i < item->folder.num_children; i++) {
      count += count_visible_recursive(&item->folder.children[i]);
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
      count += count_visible_recursive(&mgr->root.folder.children[i]);
    }
  }
  return count;
}

static ConnectionItem *get_visible_item_recursive(ConnectionItem *item, size_t *idx, size_t target) {
  if (*idx == target)
    return item;
  (*idx)++;

  if (item->type == CONN_ITEM_FOLDER && item->folder.expanded) {
    for (size_t i = 0; i < item->folder.num_children; i++) {
      ConnectionItem *found = get_visible_item_recursive(&item->folder.children[i], idx, target);
      if (found)
        return found;
    }
  }

  return NULL;
}

ConnectionItem *connmgr_get_visible_item(ConnectionManager *mgr, size_t target) {
  if (!mgr)
    return NULL;

  /* Skip root, iterate its children */
  size_t idx = 0;
  if (mgr->root.folder.expanded) {
    for (size_t i = 0; i < mgr->root.folder.num_children; i++) {
      ConnectionItem *found = get_visible_item_recursive(&mgr->root.folder.children[i], &idx, target);
      if (found)
        return found;
    }
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
  if (!conn || !conn->driver)
    return NULL;

  return connstr_build(
      conn->driver,
      (conn->user && conn->user[0]) ? conn->user : NULL,
      (conn->password && conn->password[0]) ? conn->password : NULL,
      (conn->host && conn->host[0]) ? conn->host : NULL,
      conn->port,
      (conn->database && conn->database[0]) ? conn->database : NULL,
      NULL, NULL, 0);
}

SavedConnection *connmgr_parse_connstr(const char *url, char **error) {
  if (!url || !url[0]) {
    set_error(error, "Empty URL");
    return NULL;
  }

  ConnString *cs = connstr_parse(url, error);
  if (!cs)
    return NULL;

  SavedConnection *conn = connmgr_new_connection();
  if (!conn) {
    connstr_free(cs);
    set_error(error, "Out of memory");
    return NULL;
  }

  /* Copy fields */
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

  /* Generate a default name from the connection */
  free(conn->name);
  if (str_eq(cs->driver, "sqlite")) {
    /* Use filename for SQLite */
    const char *name = cs->database;
    const char *slash = strrchr(cs->database, '/');
    if (slash)
      name = slash + 1;
    conn->name = str_dup(name);
  } else {
    /* Use host/database for network DBs */
    conn->name = str_printf("%s/%s", cs->host ? cs->host : "localhost",
                            cs->database ? cs->database : "");
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
