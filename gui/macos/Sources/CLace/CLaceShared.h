/*
 * Lace macOS frontend
 * Accessors for the TUI's saved-connections tree that Swift cannot reach
 * itself: ConnectionItem holds its folder or connection in an anonymous
 * union, and the children live in a growable array by value.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_MACOS_CLACE_SHARED_H
#define LACE_MACOS_CLACE_SHARED_H

#include "../../../../tui/ncurses/src/config/connections.h"

static inline ConnectionItem *lace_connmgr_root(ConnectionManager *mgr) {
  return &mgr->root;
}

static inline LaceSavedConnection *lace_item_connection(ConnectionItem *item) {
  return item && item->type == CONN_ITEM_CONNECTION ? &item->connection : NULL;
}

static inline ConnectionFolder *lace_item_folder(ConnectionItem *item) {
  return item && item->type == CONN_ITEM_FOLDER ? &item->folder : NULL;
}

static inline ConnectionItem *lace_folder_child(ConnectionFolder *folder, size_t index) {
  return folder && index < folder->num_children ? &folder->children[index] : NULL;
}

#endif /* LACE_MACOS_CLACE_SHARED_H */
