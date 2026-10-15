/*
 * Lace
 * Workspaces window: the saved connections tree (its rows, its actions and
 * moving items)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "connect_internal.h"

/* ============================================================================
 * The tree as listed
 * ============================================================================
 */

const char *connect_filter(DialogState *ds) {
  return ds->input.len > 0 ? ds->input.buf : NULL;
}

size_t connect_tree_count(DialogState *ds) {
  return connmgr_count_filtered(ds->mgr, connect_filter(ds));
}

ConnectionItem *connect_tree_item(DialogState *ds, size_t index) {
  return connmgr_get_filtered_item(ds->mgr, connect_filter(ds), index);
}

static ConnectionItem *highlighted(DialogState *ds) {
  return connect_tree_item(ds, ds->tree_highlight);
}

/* ============================================================================
 * Moving items: a folder and a position in it, one folder level at a time
 * ============================================================================
 */

/* Count children in folder excluding the source item */
static size_t count_children_excluding_source(ConnectionItem *folder,
                                              ConnectionItem *source) {
  if (!folder || folder->type != CONN_ITEM_FOLDER)
    return 0;
  size_t count = 0;
  for (size_t i = 0; i < folder->folder.num_children; i++) {
    if (&folder->folder.children[i] != source)
      count++;
  }
  return count;
}

/* Get child at index (excluding source) */
static ConnectionItem *get_child_excluding_source(ConnectionItem *folder,
                                                  size_t idx,
                                                  ConnectionItem *source) {
  if (!folder || folder->type != CONN_ITEM_FOLDER)
    return NULL;
  size_t count = 0;
  for (size_t i = 0; i < folder->folder.num_children; i++) {
    ConnectionItem *child = &folder->folder.children[i];
    if (child == source)
      continue;
    if (count == idx)
      return child;
    count++;
  }
  return NULL;
}

/* Find index of item in parent folder (excluding source) */
static size_t find_index_in_parent(ConnectionItem *item,
                                   ConnectionItem *source) {
  if (!item || !item->parent || item->parent->type != CONN_ITEM_FOLDER)
    return 0;
  ConnectionFolder *folder = &item->parent->folder;
  size_t idx = 0;
  for (size_t i = 0; i < folder->num_children; i++) {
    ConnectionItem *child = &folder->children[i];
    if (child == source)
      continue;
    if (child == item)
      return idx;
    idx++;
  }
  return idx;
}

/* The item to insert after for the current position (NULL: at the start) */
static ConnectionItem *get_insert_after_for_pos(DialogState *ds) {
  if (!ds->move.active || !ds->move.target_folder || ds->move.insert_pos == 0)
    return NULL;
  return get_child_excluding_source(ds->move.target_folder,
                                    ds->move.insert_pos - 1, ds->move.source);
}

/* The depth the item would have where it is now */
static int get_move_target_depth(DialogState *ds) {
  if (!ds->move.active || !ds->move.target_folder ||
      ds->move.target_folder == &ds->mgr->root)
    return 0;
  return connmgr_get_item_depth(ds->move.target_folder) + 1;
}

static void clear_move_state(MoveState *move) { memset(move, 0, sizeof(*move)); }

/* Check if an item is a descendant of an ancestor */
static bool is_descendant_of(ConnectionItem *item, ConnectionItem *ancestor) {
  if (!item || !ancestor)
    return false;
  for (ConnectionItem *p = item->parent; p; p = p->parent)
    if (p == ancestor)
      return true;
  return false;
}

/* Where, among the listed items without the source, the source goes */
static ssize_t compute_source_visual_position(DialogState *ds) {
  if (!ds->move.active || !ds->move.target_folder)
    return -1;

  ConnectionItem *target = ds->move.target_folder;
  size_t insert_pos = ds->move.insert_pos;
  size_t visible_count = connect_tree_count(ds);

  /* At the start of a folder: right after its header */
  if (insert_pos == 0) {
    if (target == &ds->mgr->root)
      return 0;
    size_t idx = 0;
    for (size_t i = 0; i < visible_count; i++) {
      ConnectionItem *item = connect_tree_item(ds, i);
      if (item == ds->move.source)
        continue;
      if (item == target)
        return (ssize_t)(idx + 1);
      idx++;
    }
    return -1;
  }

  /* After a child (and everything listed inside it), or at the folder's
   * end */
  ConnectionItem *after = get_child_excluding_source(target, insert_pos - 1, ds->move.source);
  ConnectionItem *anchor = after ? after : target;
  size_t idx = 0;
  ssize_t anchor_idx = -1, last = -1;
  for (size_t i = 0; i < visible_count; i++) {
    ConnectionItem *item = connect_tree_item(ds, i);
    if (item == ds->move.source)
      continue;
    if (item == anchor) {
      anchor_idx = last = (ssize_t)idx;
    } else if (anchor_idx >= 0) {
      if (!is_descendant_of(item, anchor))
        break;
      last = (ssize_t)idx;
    }
    idx++;
  }
  return last >= 0 ? last + 1 : -1;
}

void connect_tree_rows(DialogState *ds) {
  size_t n = connect_tree_count(ds);
  if (!ds->move.active) {
    for (size_t i = 0; i < n; i++) {
      ConnectionItem *item = connect_tree_item(ds, i);
      if (item)
        connect_add_row(ds, ROW_TREE, i, item, connmgr_get_item_depth(item), NULL);
    }
    return;
  }
  /* During a move: everything else, and the item where it would go */
  ssize_t at = compute_source_visual_position(ds);
  int depth = get_move_target_depth(ds);
  size_t shown = 0;
  bool placed = false;
  for (size_t i = 0; i < n; i++) {
    ConnectionItem *item = connect_tree_item(ds, i);
    if (!item || item == ds->move.source)
      continue;
    if (!placed && (ssize_t)shown == at) {
      connect_add_row(ds, ROW_MOVING, 0, ds->move.source, depth, NULL);
      placed = true;
    }
    connect_add_row(ds, ROW_TREE, i, item, connmgr_get_item_depth(item), NULL);
    shown++;
  }
  if (!placed)
    connect_add_row(ds, ROW_MOVING, 0, ds->move.source, depth, NULL);
}

/* ============================================================================
 * Moving
 * ============================================================================
 */

static void set_msg(char **slot, const char *msg) {
  free(*slot);
  *slot = str_dup(msg);
}

static void drop_item(DialogState *ds) {
  ConnectionItem *target = ds->move.target_folder;
  ConnectionItem *insert_after = get_insert_after_for_pos(ds);
  ConnectionItem *source = ds->move.source;

  /* The moved item is found again by name (pointers shift in a move) */
  char *source_name = source ? str_dup(connmgr_item_name(source)) : NULL;
  bool is_folder = source && connmgr_is_folder(source);
  bool moved = false;
  if (target && source && target != source) {
    /* Never into itself or a folder inside it */
    bool valid = true;
    if (connmgr_is_folder(source)) {
      for (ConnectionItem *p = target; p; p = p->parent)
        if (p == source)
          valid = false;
    }
    /* Open the target first: the move shifts items, so target may point at
     * another one afterwards */
    bool was_expanded = target->type == CONN_ITEM_FOLDER && target->folder.expanded;
    if (valid && target->type == CONN_ITEM_FOLDER)
      target->folder.expanded = true;
    if (valid && connmgr_move_item(ds->mgr, source, target, insert_after)) {
      set_msg(&ds->success_msg, "Moved");
      moved = true;
    } else {
      if (valid && target->type == CONN_ITEM_FOLDER)
        target->folder.expanded = was_expanded;
      set_msg(&ds->error_msg, "Cannot move here");
    }
  }
  clear_move_state(&ds->move);

  size_t visible = connect_tree_count(ds);
  if (moved && source_name) {
    for (size_t i = 0; i < visible; i++) {
      ConnectionItem *item = connect_tree_item(ds, i);
      const char *name = item ? connmgr_item_name(item) : NULL;
      if (name && connmgr_is_folder(item) == is_folder && strcmp(name, source_name) == 0) {
        ds->tree_highlight = i;
        break;
      }
    }
  }
  free(source_name);
  if (ds->tree_highlight >= visible && visible > 0)
    ds->tree_highlight = visible - 1;
}

void connect_tree_move(DialogState *ds) {
  if (ds->move.active) {
    drop_item(ds);
    return;
  }
  ConnectionItem *current = highlighted(ds);
  if (current && connect_filter(ds)) {
    /* Moving works on the whole tree: drop the filter, open the folders
     * above the item and keep the highlight on it */
    text_input_set(&ds->input, "");
    for (ConnectionItem *p = current->parent; p && p->parent; p = p->parent) {
      if (!p->folder.expanded) {
        p->folder.expanded = true;
        ds->mgr->modified = true;
      }
    }
    size_t n = connect_tree_count(ds);
    for (size_t i = 0; i < n; i++) {
      if (connect_tree_item(ds, i) == current) {
        ds->tree_highlight = i;
        break;
      }
    }
  }
  if (!current || !current->parent || current->parent->type != CONN_ITEM_FOLDER)
    return;
  ds->move.active = true;
  ds->move.source = current;
  ds->move.target_folder = current->parent;
  /* After removing the source, insert_pos K means "after K items": the
   * source's own index puts it back where it was */
  ConnectionFolder *pf = &current->parent->folder;
  for (size_t i = 0; i < pf->num_children; i++) {
    if (&pf->children[i] == current) {
      ds->move.insert_pos = i;
      break;
    }
  }
  ds->input_focused = false;
}

bool connect_tree_move_key(DialogState *ds, const UiEvent *event) {
  if (!ds->move.active)
    return false;
  ConnectionItem *target = ds->move.target_folder;
  size_t pos = ds->move.insert_pos;
  size_t num_children = count_children_excluding_source(target, ds->move.source);

  if (hotkey_matches(ds->config, event, HOTKEY_MOVE_UP)) {
    if (pos > 0) {
      ds->move.insert_pos--;
      /* Now after an open folder: into it, at its end */
      ConnectionItem *prev =
          get_child_excluding_source(target, ds->move.insert_pos, ds->move.source);
      if (prev && connmgr_is_folder(prev) && prev->folder.expanded) {
        ds->move.target_folder = prev;
        ds->move.insert_pos = count_children_excluding_source(prev, ds->move.source);
      }
    } else if (target != &ds->mgr->root && target->parent) {
      /* At the start of a folder: out of it, before it */
      ds->move.insert_pos = find_index_in_parent(target, ds->move.source);
      ds->move.target_folder = target->parent;
    }
  } else if (hotkey_matches(ds->config, event, HOTKEY_MOVE_DOWN)) {
    if (pos < num_children) {
      /* Before an open folder: into it, at its start */
      ConnectionItem *curr = get_child_excluding_source(target, pos, ds->move.source);
      if (curr && connmgr_is_folder(curr) && curr->folder.expanded) {
        ds->move.target_folder = curr;
        ds->move.insert_pos = 0;
      } else {
        ds->move.insert_pos++;
      }
    } else if (target != &ds->mgr->root && target->parent) {
      /* At the end of a folder: out of it, after it */
      ds->move.insert_pos = find_index_in_parent(target, ds->move.source) + 1;
      ds->move.target_folder = target->parent;
    }
  } else if (hotkey_matches(ds->config, event, HOTKEY_CONN_MOVE) ||
             render_event_is_special(event, UI_KEY_ENTER)) {
    drop_item(ds);
  } else if (render_event_is_special(event, UI_KEY_ESCAPE)) {
    clear_move_state(&ds->move);
    set_msg(&ds->success_msg, "Move cancelled");
  }
  /* Every other key is ignored: deleting or adding an item would leave
   * move.source and move.target_folder pointing at a freed or shifted
   * item */
  return true;
}

/* ============================================================================
 * Actions
 * ============================================================================
 */

void connect_tree_left_right(DialogState *ds, bool right) {
  ConnectionItem *item = highlighted(ds);
  if (!item)
    return;
  if (connmgr_is_folder(item) && item->folder.expanded != right) {
    connmgr_toggle_folder(item);
    ds->mgr->modified = true;
  } else if (!right && item->parent && item->parent->parent) {
    /* To the folder it is in */
    size_t n = connect_tree_count(ds);
    for (size_t i = 0; i < n; i++) {
      if (connect_tree_item(ds, i) == item->parent) {
        ds->tree_highlight = i;
        break;
      }
    }
  }
}

/* Where something new goes: into the given folder, else next to the given
 * item, else at the top */
static ConnectionItem *new_parent(DialogState *ds, ConnectionItem *at) {
  if (at && connmgr_is_folder(at)) {
    at->folder.expanded = true;
    return at;
  }
  if (at && at->parent)
    return at->parent;
  return &ds->mgr->root;
}

void connect_tree_new_folder(DialogState *ds, ConnectionItem *at) {
  char *name = connect_prompt_text(ds->win, "New Folder", "Name:", "");
  connect_redraw(ds);
  if (!name)
    return;
  ConnectionFolder *folder = connmgr_new_folder(name);
  free(name);
  if (!folder)
    return;
  if (connmgr_add_folder(new_parent(ds, at), folder)) {
    ds->mgr->modified = true;
    set_msg(&ds->success_msg, "Folder created");
  } else {
    set_msg(&ds->error_msg, "Failed to create folder");
    free(folder->name);
    free(folder);
  }
}

void connect_tree_new_connection(DialogState *ds, ConnectionItem *at) {
  char *err = NULL;
  if (connect_connection_form(ds, NULL, new_parent(ds, at), &err)) {
    set_msg(&ds->success_msg, "Connection created");
  } else if (err) {
    free(ds->error_msg);
    ds->error_msg = err;
  }
  connect_redraw(ds);
}

void connect_tree_edit(DialogState *ds) {
  ConnectionItem *item = highlighted(ds);
  if (!item)
    return;
  if (connmgr_is_folder(item)) {
    connect_tree_rename(ds);
    return;
  }
  char *err = NULL;
  if (connect_connection_form(ds, item, NULL, &err)) {
    set_msg(&ds->success_msg, "Connection updated");
  } else if (err) {
    free(ds->error_msg);
    ds->error_msg = err;
  }
  connect_redraw(ds);
}

void connect_tree_rename(DialogState *ds) {
  ConnectionItem *item = highlighted(ds);
  if (!item)
    return;
  char *name = connect_prompt_text(ds->win, connmgr_is_folder(item) ? "Rename Folder" : "Rename",
                                   "Name:", connmgr_item_name(item));
  connect_redraw(ds);
  if (!name)
    return;
  if (connmgr_is_folder(item)) {
    free(item->folder.name);
    item->folder.name = name;
  } else {
    free(item->connection.name);
    item->connection.name = name;
  }
  ds->mgr->modified = true;
}

void connect_tree_duplicate(DialogState *ds) {
  ConnectionItem *item = highlighted(ds);
  if (!item || !connmgr_is_connection(item) || !item->parent) {
    set_msg(&ds->error_msg, "Select a connection to duplicate");
    return;
  }
  SavedConnection *copy = lace_saved_conn_copy(&item->connection);
  if (!copy)
    return;
  free(copy->id);
  copy->id = lace_generate_uuid(); /* Its own history and session entry */
  free(copy->name);
  copy->name = str_printf("%s (copy)", item->connection.name ? item->connection.name : "");
  if (connmgr_add_connection(item->parent, copy)) { /* Takes copy */
    ds->mgr->modified = true;
    set_msg(&ds->success_msg, "Duplicated: edit it with e");
  } else {
    connmgr_free_connection(copy);
    free(copy);
  }
}

void connect_tree_delete(DialogState *ds) {
  ConnectionItem *item = highlighted(ds);
  if (!item || !item->parent)
    return;
  char *msg;
  if (connmgr_is_folder(item) && item->folder.num_children > 0)
    msg = str_printf("Delete folder \"%s\" and the %zu item%s in it?",
                     connmgr_item_name(item), item->folder.num_children,
                     item->folder.num_children == 1 ? "" : "s");
  else
    msg = str_printf("Delete \"%s\"?", connmgr_item_name(item));
  bool yes = connect_confirm(ds->win, "Confirm Delete", msg);
  free(msg);
  connect_redraw(ds);
  if (!yes)
    return;
  connmgr_remove_item(ds->mgr, item);
  ds->mgr->modified = true;
  /* Stay on the same row (now the next item), or the last one */
  size_t visible = connect_tree_count(ds);
  if (ds->tree_highlight >= visible)
    ds->tree_highlight = visible > 0 ? visible - 1 : 0;
  set_msg(&ds->success_msg, "Deleted");
}
