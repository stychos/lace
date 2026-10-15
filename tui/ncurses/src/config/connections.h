/*
 * Lace
 * Connection Manager - Saved Connections Storage
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_CONFIG_CONNECTIONS_H
#define LACE_CONFIG_CONNECTIONS_H

#include "../../../liblace/include/config/saved_connection.h"
#include <stdbool.h>
#include <stddef.h>

/* Connection item types in the tree */
typedef enum { CONN_ITEM_FOLDER, CONN_ITEM_CONNECTION } ConnectionItemType;

/* Saved connection - use liblace type with alias for compatibility */
typedef LaceSavedConnection SavedConnection;

/* Forward declaration for tree structure */
struct ConnectionItem;

/* Folder containing connections and subfolders */
typedef struct ConnectionFolder {
  char *name;
  bool expanded;
  struct ConnectionItem *children;
  size_t num_children;
  size_t children_capacity;
} ConnectionFolder;

/* Tree node - either a folder or a connection */
typedef struct ConnectionItem {
  ConnectionItemType type;
  union {
    ConnectionFolder folder;
    SavedConnection connection;
  };
  struct ConnectionItem *parent;
} ConnectionItem;

/* Connection manager - owns all saved connections */
typedef struct ConnectionManager {
  ConnectionItem root; /* Root folder */
  bool modified;       /* Unsaved changes flag */
  char *file_path;     /* Path to connections.json */
  /* connections.json could not be loaded: what is shown is not what the
   * file holds, so connmgr_save() refuses (it would erase the file) */
  bool read_only;
  /* The file as loaded (size, mtime): a save refuses if another lace
   * process changed it since, instead of overwriting its changes */
  long long loaded_size;
  long long loaded_mtime_ns;
} ConnectionManager;

/* The tree as listed under a filter (NULL or "": the visible items):
 * names holding the text (ASCII case folded), the folders above them, and
 * everything inside a folder whose name matches; folders listed open */
size_t connmgr_count_filtered(ConnectionManager *mgr, const char *filter);
ConnectionItem *connmgr_get_filtered_item(ConnectionManager *mgr, const char *filter,
                                          size_t index);

/* Visit every connection in the tree, collapsed folders included */
void connmgr_for_each_connection(ConnectionManager *mgr,
                                 void (*fn)(ConnectionItem *item, void *ctx),
                                 void *ctx);

/* True for an id that is safe in file names (history/<id>.json) */
bool connmgr_id_valid(const char *id);

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/* Create a new empty connection manager */
ConnectionManager *connmgr_new(void);

/* Load connection manager from disk (or create empty if file doesn't exist) */
ConnectionManager *connmgr_load(char **error);

/* Load from a given file (connmgr_load() uses the config directory's) */
ConnectionManager *connmgr_load_file(const char *path, char **error);

/* Save connection manager to disk */
bool connmgr_save(ConnectionManager *mgr, char **error);

/* Free connection manager and all resources */
void connmgr_free(ConnectionManager *mgr);

/* ============================================================================
 * Connection CRUD
 * ============================================================================
 */

/* Create a new saved connection with generated UUID */
SavedConnection *connmgr_new_connection(void);

/* Add a connection to a folder (takes ownership; frees conn on success) */
bool connmgr_add_connection(ConnectionItem *folder, SavedConnection *conn);

/* Find a connection by ID */
ConnectionItem *connmgr_find_by_id(ConnectionManager *mgr, const char *id);

/* Remove an item from its parent (frees the item) */
bool connmgr_remove_item(ConnectionManager *mgr, ConnectionItem *item);

/* Move an item to a different folder at a specific position
 * insert_after: item to insert after, or NULL to insert at beginning
 *               If insert_after is not in new_parent, inserts at end */
bool connmgr_move_item(ConnectionManager *mgr, ConnectionItem *item,
                       ConnectionItem *new_parent,
                       ConnectionItem *insert_after);

/* ============================================================================
 * Folder CRUD
 * ============================================================================
 */

/* Create a new folder */
ConnectionFolder *connmgr_new_folder(const char *name);

/* Add a folder to a parent folder (takes ownership; frees folder on
 * success). Like every change to the tree, it may move items: pointers to
 * items other than the root are stale afterwards. */
bool connmgr_add_folder(ConnectionItem *parent, ConnectionFolder *folder);

/* ============================================================================
 * Tree Navigation
 * ============================================================================
 */

/* Count visible items in tree (expanded folders + their visible children) */
size_t connmgr_count_visible(ConnectionManager *mgr);

/* Get visible item at index (for UI list rendering) */
ConnectionItem *connmgr_get_visible_item(ConnectionManager *mgr, size_t idx);

/* Get tree depth of an item (root = 0) */
int connmgr_get_item_depth(ConnectionItem *item);

/* Toggle folder expand/collapse state */
void connmgr_toggle_folder(ConnectionItem *item);

/* ============================================================================
 * Connection Strings
 * ============================================================================
 */

/* Build a connection URL from saved connection */
char *connmgr_build_connstr(const SavedConnection *conn);

/* Parse a connection URL into a SavedConnection (for Save to List) */
SavedConnection *connmgr_parse_connstr(const char *url, char **error);

/* ============================================================================
 * Item Helpers
 * ============================================================================
 */

/* Get display name for an item (folder name or connection name) */
const char *connmgr_item_name(const ConnectionItem *item);

/* Check if item is a folder */
bool connmgr_is_folder(const ConnectionItem *item);

/* Check if item is a connection */
bool connmgr_is_connection(const ConnectionItem *item);

/* Free a single connection's fields (not the struct itself) */
void connmgr_free_connection(SavedConnection *conn);

/* Free a single folder's fields and children recursively */
void connmgr_free_folder(ConnectionFolder *folder);

#endif /* LACE_CONFIG_CONNECTIONS_H */
