/*
 * Lace
 * Connection Manager dialog: the saved connections tree (drawing, moving items, keys)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "connect_internal.h"

/* ============================================================================
 * Tree Drawing
 * ============================================================================
 */

static void draw_tree_item(WINDOW *win, int y, int x, int width,
                           ConnectionItem *item, bool selected, bool focused,
                           bool is_move_source) {
  int depth = connmgr_get_item_depth(item);
  int indent = depth * 2;

  /* Clear line */
  mvwhline(win, y, x, ' ', width);

  /* Highlight if selected */
  if (is_move_source) {
    /* Item being moved - show dimmed */
    wattron(win, A_DIM);
  } else if (selected) {
    if (focused) {
      wattron(win, COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
    } else {
      wattron(win, A_REVERSE);
    }
  }

  mvwhline(win, y, x, ' ', width);

  /* Draw indent */
  wmove(win, y, x + indent);

  /* Draw name and folder indicator */
  const char *name = connmgr_item_name(item);
  int name_width = width - indent - 2;

  if (connmgr_is_folder(item)) {
    /* Folder: name followed by expand/collapse arrow */
    const char *arrow = item->folder.expanded
                            ? " \xE2\x96\xBC"  /* ▼ UTF-8 (expanded) */
                            : " \xE2\x96\xB6"; /* ▶ UTF-8 (collapsed) */
    int arrow_width = 2;                       /* space + arrow */
    int max_name = name_width - arrow_width;
    if (max_name > 0) {
      if ((int)strlen(name) > max_name) {
        waddnstr(win, name, max_name - 1);
        waddch(win, '~');
      } else {
        waddstr(win, name);
      }
      waddstr(win, arrow);
    }
  } else {
    /* Connection: show cut marker if move source, then name */
    if (is_move_source) {
      waddstr(win, "~ ");
      name_width -= 2;
    }
    if (name_width > 0) {
      if ((int)strlen(name) > name_width) {
        waddnstr(win, name, name_width - 1);
        waddch(win, '~');
      } else {
        waddstr(win, name);
      }
    }
  }

  if (is_move_source) {
    wattroff(win, A_DIM);
  } else if (selected) {
    if (focused) {
      wattroff(win, COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
    } else {
      wattroff(win, A_REVERSE);
    }
  }
}

/*
 * Move system helpers - hierarchical folder+position navigation
 *
 * Navigation moves one folder level at a time for natural tree traversal.
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

/* Get the insert_after item for current position (NULL = insert at beginning)
 */
static ConnectionItem *get_insert_after_for_pos(DialogState *ds) {
  if (!ds->move.active || !ds->move.target_folder)
    return NULL;
  if (ds->move.insert_pos == 0)
    return NULL;
  return get_child_excluding_source(ds->move.target_folder,
                                    ds->move.insert_pos - 1, ds->move.source);
}

/* Get the depth for current target position */
static int get_move_target_depth(DialogState *ds) {
  if (!ds->move.active || !ds->move.target_folder)
    return 0;
  /* Items in root are at depth 0 */
  if (ds->move.target_folder == &ds->mgr->root)
    return 0;
  /* Items in any subfolder are at folder's depth + 1 */
  int folder_depth = connmgr_get_item_depth(ds->move.target_folder);
  return folder_depth + 1;
}

/* Helper: clear move state */
static void clear_move_state(MoveState *move) {
  move->active = false;
  move->source = NULL;
  move->source_idx = 0;
  move->target_folder = NULL;
  move->insert_pos = 0;
}

/* Helper: draw the moving item at its target position */
static void draw_moving_item_at_depth(WINDOW *win, DialogState *ds, int y,
                                      int x, int width, int depth) {
  if (!ds->move.source)
    return;
  const char *name = connmgr_item_name(ds->move.source);
  if (!name)
    name = "(unnamed)";
  wattron(win, COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
  mvwhline(win, y, x, ' ', width);
  if (connmgr_is_folder(ds->move.source)) {
    const char *arrow =
        ds->move.source->folder.expanded ? " \xE2\x96\xBC" : " \xE2\x96\xB6";
    mvwprintw(win, y, x + depth * 2, "%s%s", name, arrow);
  } else {
    mvwprintw(win, y, x + depth * 2, "%s", name);
  }
  wattroff(win, COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
}

/* Helper: check if item is the source being moved */
static bool is_source_item(DialogState *ds, ConnectionItem *item) {
  return ds->move.active && item == ds->move.source;
}

/* Check if an item is a descendant of an ancestor */
static bool is_descendant_of(ConnectionItem *item, ConnectionItem *ancestor) {
  if (!item || !ancestor)
    return false;
  ConnectionItem *p = item->parent;
  while (p) {
    if (p == ancestor)
      return true;
    p = p->parent;
  }
  return false;
}

/* Compute the visual index where source should appear (excluding source from
 * count) */
static ssize_t compute_source_visual_position(DialogState *ds) {
  if (!ds->move.active || !ds->move.target_folder)
    return -1;

  ConnectionItem *target = ds->move.target_folder;
  size_t insert_pos = ds->move.insert_pos;
  size_t visible_count = connmgr_count_visible(ds->mgr);

  /* Case 1: Insert at beginning of folder */
  if (insert_pos == 0) {
    if (target == &ds->mgr->root) {
      return 0; /* Very first position */
    }
    /* Find the target folder in the visible list */
    size_t idx = 0;
    for (size_t i = 0; i < visible_count; i++) {
      ConnectionItem *item = connmgr_get_visible_item(ds->mgr, i);
      if (item == ds->move.source)
        continue;
      if (item == target) {
        return (ssize_t)(idx + 1); /* Right after the folder header */
      }
      idx++;
    }
    return -1;
  }

  /* Case 2: Insert after a specific child */
  ConnectionItem *after_child =
      get_child_excluding_source(target, insert_pos - 1, ds->move.source);
  if (!after_child) {
    /* Fallback: insert at end of folder, find folder's last visible descendant
     */
    size_t idx = 0;
    ssize_t folder_idx = -1;
    ssize_t last_descendant_idx = -1;
    for (size_t i = 0; i < visible_count; i++) {
      ConnectionItem *item = connmgr_get_visible_item(ds->mgr, i);
      if (item == ds->move.source)
        continue;
      if (item == target) {
        folder_idx = (ssize_t)idx;
        last_descendant_idx = (ssize_t)idx;
      } else if (folder_idx >= 0 && is_descendant_of(item, target)) {
        last_descendant_idx = (ssize_t)idx;
      } else if (folder_idx >= 0) {
        break;
      }
      idx++;
    }
    return (last_descendant_idx >= 0) ? last_descendant_idx + 1 : -1;
  }

  /* Find after_child and its last visible descendant */
  size_t idx = 0;
  ssize_t after_child_idx = -1;
  ssize_t last_descendant_idx = -1;

  for (size_t i = 0; i < visible_count; i++) {
    ConnectionItem *item = connmgr_get_visible_item(ds->mgr, i);
    if (item == ds->move.source)
      continue;

    if (item == after_child) {
      after_child_idx = (ssize_t)idx;
      last_descendant_idx = (ssize_t)idx;
    } else if (after_child_idx >= 0) {
      if (is_descendant_of(item, after_child)) {
        last_descendant_idx = (ssize_t)idx;
      } else {
        break; /* No longer a descendant */
      }
    }
    idx++;
  }

  return (last_descendant_idx >= 0) ? last_descendant_idx + 1 : -1;
}

/* Get first key display for a hotkey action (returns static buffer, max 8
 * chars) */
static const char *get_first_key_hint(const Config *config,
                                      HotkeyAction action) {
  static char buf[16];
  buf[0] = '\0';

  if (!config)
    return buf;

  char *display = hotkey_get_display(config, action);
  if (display) {
    /* Take only first key (before comma) */
    char *comma = strchr(display, ',');
    if (comma) {
      size_t len = (size_t)(comma - display);
      if (len > sizeof(buf) - 1)
        len = sizeof(buf) - 1;
      strncpy(buf, display, len);
      buf[len] = '\0';
    } else {
      strncpy(buf, display, sizeof(buf) - 1);
      buf[sizeof(buf) - 1] = '\0';
    }
    free(display);
  }
  return buf;
}

void connect_draw_tree(WINDOW *win, DialogState *ds, int start_y,
                            int start_x, int height, int width) {
  /* Panel header */
  wattron(win, A_BOLD);
  mvwprintw(win, start_y, start_x, "Saved Connections");
  wattroff(win, A_BOLD);

  /* Show move target folder info */
  if (ds->move.active && ds->move.target_folder) {
    const char *folder_name = (ds->move.target_folder == &ds->mgr->root)
                                  ? "(root)"
                                  : connmgr_item_name(ds->move.target_folder);
    wattron(win, COLOR_PAIR(COLOR_NUMBER));
    mvwprintw(win, start_y, start_x + 18, " -> %s", folder_name);
    wattroff(win, COLOR_PAIR(COLOR_NUMBER));
  }

  int tree_y = start_y + 1;
  int tree_height = height - 4;

  size_t visible_count = connmgr_count_visible(ds->mgr);

  if (visible_count == 0 || (ds->move.active && visible_count == 1)) {
    wattron(win, A_DIM);
    mvwprintw(win, tree_y + 1, start_x + 1, "(no saved connections)");
    wattroff(win, A_DIM);

    /* If moving and tree is empty (only source), show the item */
    if (ds->move.active) {
      int depth = get_move_target_depth(ds);
      draw_moving_item_at_depth(win, ds, tree_y + 2, start_x, width, depth);
    }
  } else if (!ds->move.active) {
    /* Normal mode - draw items in order */
    if (ds->tree_highlight >= ds->tree_scroll + (size_t)tree_height) {
      ds->tree_scroll = ds->tree_highlight - (size_t)tree_height + 1;
    }
    if (ds->tree_highlight < ds->tree_scroll) {
      ds->tree_scroll = ds->tree_highlight;
    }

    int draw_y = tree_y;
    for (size_t i = ds->tree_scroll;
         i < visible_count && draw_y < tree_y + tree_height; i++) {
      ConnectionItem *item = connmgr_get_visible_item(ds->mgr, i);
      if (!item)
        continue;
      bool is_cursor = (i == ds->tree_highlight);
      draw_tree_item(win, draw_y, start_x, width, item, is_cursor,
                     ds->focus == FOCUS_TREE, false);
      draw_y++;
    }
  } else {
    /* Move mode - draw tree with source at computed visual position */
    int draw_y = tree_y;
    bool source_drawn = false;
    ssize_t source_visual_pos = compute_source_visual_position(ds);
    size_t idx_excluding_source = 0;

    /* Handle insert at very beginning (position 0) */
    if (source_visual_pos == 0) {
      int depth = get_move_target_depth(ds);
      draw_moving_item_at_depth(win, ds, draw_y, start_x, width, depth);
      draw_y++;
      source_drawn = true;
    }

    for (size_t i = 0; i < visible_count && draw_y < tree_y + tree_height;
         i++) {
      ConnectionItem *item = connmgr_get_visible_item(ds->mgr, i);
      if (!item)
        continue;

      /* Skip the source item (it's drawn at target position) */
      if (is_source_item(ds, item))
        continue;

      /* Check if source should be drawn BEFORE this item */
      if (!source_drawn && (ssize_t)idx_excluding_source == source_visual_pos) {
        int depth = get_move_target_depth(ds);
        draw_moving_item_at_depth(win, ds, draw_y, start_x, width, depth);
        draw_y++;
        source_drawn = true;
        if (draw_y >= tree_y + tree_height)
          break;
      }

      /* Draw the regular item */
      draw_tree_item(win, draw_y, start_x, width, item, false, false, false);
      draw_y++;
      idx_excluding_source++;
    }

    /* If source not yet drawn, draw at end */
    if (!source_drawn && draw_y < tree_y + tree_height) {
      int depth = get_move_target_depth(ds);
      draw_moving_item_at_depth(win, ds, draw_y, start_x, width, depth);
    }
  }

  /* Draw shortcut hints at bottom */
  int hint_y = start_y + height - 2;
  wattron(win, A_DIM);
  if (ds->move.active) {
    char move_key[16];
    strncpy(move_key, get_first_key_hint(ds->config, HOTKEY_CONN_MOVE),
            sizeof(move_key) - 1);
    move_key[sizeof(move_key) - 1] = '\0';
    mvwprintw(win, hint_y, start_x, "%s:drop Esc:cancel",
              move_key[0] ? move_key : "Space");
  } else {
    /* Build hints from configured hotkeys (copy to avoid static buffer reuse)
     */
    char new_key[16], folder_key[16], edit_key[16], del_key[16], rename_key[16],
        move_key[16];
    strncpy(new_key, get_first_key_hint(ds->config, HOTKEY_CONN_NEW),
            sizeof(new_key) - 1);
    new_key[sizeof(new_key) - 1] = '\0';
    strncpy(folder_key, get_first_key_hint(ds->config, HOTKEY_CONN_NEW_FOLDER),
            sizeof(folder_key) - 1);
    folder_key[sizeof(folder_key) - 1] = '\0';
    strncpy(edit_key, get_first_key_hint(ds->config, HOTKEY_CONN_EDIT),
            sizeof(edit_key) - 1);
    edit_key[sizeof(edit_key) - 1] = '\0';
    strncpy(del_key, get_first_key_hint(ds->config, HOTKEY_CONN_DELETE),
            sizeof(del_key) - 1);
    del_key[sizeof(del_key) - 1] = '\0';
    strncpy(rename_key, get_first_key_hint(ds->config, HOTKEY_CONN_RENAME),
            sizeof(rename_key) - 1);
    rename_key[sizeof(rename_key) - 1] = '\0';
    strncpy(move_key, get_first_key_hint(ds->config, HOTKEY_CONN_MOVE),
            sizeof(move_key) - 1);
    move_key[sizeof(move_key) - 1] = '\0';

    mvwprintw(win, hint_y, start_x, "%s:new %s:folder %s:edit",
              new_key[0] ? new_key : "n", folder_key[0] ? folder_key : "N",
              edit_key[0] ? edit_key : "e");
    mvwprintw(win, hint_y + 1, start_x, "%s:move %s:del %s:rename",
              move_key[0] ? move_key : "Space", del_key[0] ? del_key : "d",
              rename_key[0] ? rename_key : "r");
  }
  wattroff(win, A_DIM);
}

/* ============================================================================
 * Input Handling
 * ============================================================================
 */

bool connect_tree_handle_key(DialogState *ds, const UiEvent *event) {
  size_t visible_count = connmgr_count_visible(ds->mgr);
  int key_char = render_event_get_char(event);

  /* In move mode, hierarchical navigation through folders */
  if (ds->move.active) {
    ConnectionItem *target = ds->move.target_folder;
    size_t pos = ds->move.insert_pos;
    size_t num_children =
        count_children_excluding_source(target, ds->move.source);

    if (hotkey_matches(ds->config, event, HOTKEY_MOVE_UP)) {
      if (pos > 0) {
        /* Move up within folder */
        ds->move.insert_pos--;
        /* Check if we're now after an expanded folder - enter it at end */
        ConnectionItem *prev = get_child_excluding_source(
            target, ds->move.insert_pos, ds->move.source);
        if (prev && connmgr_is_folder(prev) && prev->folder.expanded) {
          ds->move.target_folder = prev;
          ds->move.insert_pos =
              count_children_excluding_source(prev, ds->move.source);
        }
      } else {
        /* At start of folder - exit to parent (before this folder) */
        if (target != &ds->mgr->root && target->parent) {
          size_t folder_idx = find_index_in_parent(target, ds->move.source);
          ds->move.target_folder = target->parent;
          ds->move.insert_pos = folder_idx;
        }
      }
      return true;
    }

    if (hotkey_matches(ds->config, event, HOTKEY_MOVE_DOWN)) {
      if (pos < num_children) {
        /* Check if item at current position is an expanded folder - enter it */
        ConnectionItem *curr =
            get_child_excluding_source(target, pos, ds->move.source);
        if (curr && connmgr_is_folder(curr) && curr->folder.expanded) {
          ds->move.target_folder = curr;
          ds->move.insert_pos = 0;
        } else {
          /* Move down within folder */
          ds->move.insert_pos++;
        }
      } else {
        /* At end of folder - exit to parent (after this folder) */
        if (target != &ds->mgr->root && target->parent) {
          size_t folder_idx = find_index_in_parent(target, ds->move.source);
          ds->move.target_folder = target->parent;
          ds->move.insert_pos = folder_idx + 1;
        }
      }
      return true;
    }
  } else {
    /* Normal navigation */
    if (hotkey_matches(ds->config, event, HOTKEY_MOVE_UP)) {
      if (ds->tree_highlight > 0) {
        ds->tree_highlight--;
      }
      return true;
    }

    if (hotkey_matches(ds->config, event, HOTKEY_MOVE_DOWN)) {
      if (visible_count > 0 && ds->tree_highlight < visible_count - 1) {
        ds->tree_highlight++;
      } else if (visible_count == 0 ||
                 ds->tree_highlight >= visible_count - 1) {
        /* At bottom of tree, move to buttons panel */
        ds->prev_panel_focus = FOCUS_TREE;
        ds->focus = FOCUS_BUTTONS;
      }
      return true;
    }
  }

  /* Spatial navigation: 'l' moves focus to URL panel */
  if (key_char == 'l') {
    ds->focus = FOCUS_URL;
    return true;
  }

  /* Move item: select item for move or drop item at current location */
  if (hotkey_matches(ds->config, event, HOTKEY_CONN_MOVE)) {
    if (ds->move.active) {
      /* Drop the item at target position */
      ConnectionItem *target = ds->move.target_folder;
      ConnectionItem *insert_after = get_insert_after_for_pos(ds);
      ConnectionItem *source = ds->move.source;

      /* Get the name to find item after move (pointers may become invalid) */
      char *source_name = source ? str_dup(connmgr_item_name(source)) : NULL;
      bool is_folder = source && connmgr_is_folder(source);

      bool moved = false;
      if (target && source && target != source) {
        /* Check if trying to move into self or descendant */
        bool valid = true;
        if (connmgr_is_folder(source)) {
          ConnectionItem *p = target;
          while (p) {
            if (p == source) {
              valid = false;
              break;
            }
            p = p->parent;
          }
        }

        /* Expand the target first: the move shifts items, so target may
         * point at another one afterwards */
        bool was_expanded = target->type == CONN_ITEM_FOLDER && target->folder.expanded;
        if (valid && target->type == CONN_ITEM_FOLDER)
          target->folder.expanded = true;
        if (valid && connmgr_move_item(ds->mgr, source, target, insert_after)) {
          ds->success_msg = str_dup("Item moved!");
          moved = true;
        } else {
          if (valid && target->type == CONN_ITEM_FOLDER)
            target->folder.expanded = was_expanded;
          ds->error_msg = str_dup("Cannot move here");
        }
      }

      /* Clear move state */
      clear_move_state(&ds->move);

      /* Find and highlight the moved item by name */
      if (moved && source_name) {
        size_t visible = connmgr_count_visible(ds->mgr);
        for (size_t i = 0; i < visible; i++) {
          ConnectionItem *item = connmgr_get_visible_item(ds->mgr, i);
          if (item && connmgr_is_folder(item) == is_folder) {
            const char *name = connmgr_item_name(item);
            if (name && strcmp(name, source_name) == 0) {
              ds->tree_highlight = i;
              break;
            }
          }
        }
      }
      free(source_name);

      /* Ensure highlight is valid */
      size_t visible = connmgr_count_visible(ds->mgr);
      if (ds->tree_highlight >= visible && visible > 0) {
        ds->tree_highlight = visible - 1;
      }
    } else {
      /* Start move - initialize target_folder and insert_pos from source
       * position */
      ConnectionItem *current =
          connmgr_get_visible_item(ds->mgr, ds->tree_highlight);
      if (current && current->parent &&
          current->parent->type == CONN_ITEM_FOLDER) {
        ds->move.active = true;
        ds->move.source = current;
        ds->move.source_idx = ds->tree_highlight;
        ds->move.target_folder = current->parent;
        /* Find actual index of source in parent - this becomes the insert
         * position because after removing source, insert_pos=K means "after K
         * items" */
        ConnectionFolder *pf = &current->parent->folder;
        size_t src_idx = 0;
        for (size_t i = 0; i < pf->num_children; i++) {
          if (&pf->children[i] == current) {
            src_idx = i;
            break;
          }
        }
        ds->move.insert_pos = src_idx;
        ds->success_msg = str_dup("Move with arrows, Space to drop");
      }
    }
    return true;
  }

  /* Escape cancels move mode */
  if (render_event_is_special(event, UI_KEY_ESCAPE) && ds->move.active) {
    clear_move_state(&ds->move);
    ds->success_msg = str_dup("Move cancelled");
    return true;
  }

  /* Move mode takes only its own keys (navigation above, drop, Escape, and
   * Left/Right below): deleting or adding an item would leave move.source
   * and move.target_folder pointing at a freed or shifted item */
  if (ds->move.active && !render_event_is_special(event, UI_KEY_RIGHT) &&
      !render_event_is_special(event, UI_KEY_LEFT)) {
    return true;
  }

  /* Right arrow: expand folder or move to URL panel */
  if (render_event_is_special(event, UI_KEY_RIGHT)) {
    ConnectionItem *item =
        connmgr_get_visible_item(ds->mgr, ds->tree_highlight);
    if (item && connmgr_is_folder(item) && !item->folder.expanded) {
      connmgr_toggle_folder(item);
      ds->mgr->modified = true;
    } else if (!ds->move.active) {
      /* Move to URL panel only if not in move mode */
      ds->focus = FOCUS_URL;
    }
    return true;
  }

  if (render_event_is_special(event, UI_KEY_LEFT)) {
    ConnectionItem *item =
        connmgr_get_visible_item(ds->mgr, ds->tree_highlight);
    if (item) {
      if (connmgr_is_folder(item) && item->folder.expanded) {
        /* Collapse folder */
        connmgr_toggle_folder(item);
        ds->mgr->modified = true;
      } else if (item->parent && item->parent->parent) {
        /* Move to parent folder */
        for (size_t i = 0; i < connmgr_count_visible(ds->mgr); i++) {
          if (connmgr_get_visible_item(ds->mgr, i) == item->parent) {
            ds->tree_highlight = i;
            break;
          }
        }
      }
    }
    return true;
  }

  /* New folder - adds to selected folder or parent of selected item */
  if (ds->config && hotkey_matches(ds->config, event, HOTKEY_CONN_NEW_FOLDER)) {
    char *name = connect_prompt_text(ds->dialog_win, "New Folder", "Name:", "");
    if (name) {
      ConnectionFolder *folder = connmgr_new_folder(name);
      if (folder) {
        /* Determine parent: selected folder, or parent of selected connection
         */
        ConnectionItem *selected =
            connmgr_get_visible_item(ds->mgr, ds->tree_highlight);
        ConnectionItem *parent = &ds->mgr->root;

        if (selected) {
          if (connmgr_is_folder(selected)) {
            parent = selected;
            /* Expand parent so new folder is visible */
            selected->folder.expanded = true;
          } else if (selected->parent) {
            parent = selected->parent;
          }
        }

        if (connmgr_add_folder(parent, folder)) {
          ds->mgr->modified = true;
          ds->success_msg = str_dup("Folder created!");
        } else {
          ds->error_msg = str_dup("Failed to create folder");
          free(folder->name);
          free(folder);
        }
      }
      free(name);
    }
    return true;
  }

  /* New connection - adds to selected folder or parent of selected item */
  if (ds->config && hotkey_matches(ds->config, event, HOTKEY_CONN_NEW)) {
    ConnectionItem *selected =
        connmgr_get_visible_item(ds->mgr, ds->tree_highlight);
    ConnectionItem *parent = &ds->mgr->root;

    if (selected) {
      if (connmgr_is_folder(selected)) {
        parent = selected;
        selected->folder.expanded = true;
      } else if (selected->parent) {
        parent = selected->parent;
      }
    }

    char *err = NULL;
    if (connect_connection_form(ds->dialog_win, ds->mgr, NULL, parent, &err)) {
      ds->success_msg = str_dup("Connection created!");
    } else if (err) {
      ds->error_msg = err;
    }
    return true;
  }

  /* Edit connection */
  if (ds->config && hotkey_matches(ds->config, event, HOTKEY_CONN_EDIT)) {
    ConnectionItem *item =
        connmgr_get_visible_item(ds->mgr, ds->tree_highlight);
    if (item && connmgr_is_connection(item)) {
      char *err = NULL;
      if (connect_connection_form(ds->dialog_win, ds->mgr, item, NULL, &err)) {
        ds->success_msg = str_dup("Connection updated!");
      } else if (err) {
        ds->error_msg = err;
      }
    } else if (item && connmgr_is_folder(item)) {
      /* For folders, just rename */
      const char *current_name = connmgr_item_name(item);
      char *new_name = connect_prompt_text(ds->dialog_win, "Rename Folder",
                                         "Name:", current_name);
      if (new_name) {
        free(item->folder.name);
        item->folder.name = new_name;
        ds->mgr->modified = true;
      }
    }
    return true;
  }

  /* Rename */
  if (ds->config && hotkey_matches(ds->config, event, HOTKEY_CONN_RENAME)) {
    ConnectionItem *item =
        connmgr_get_visible_item(ds->mgr, ds->tree_highlight);
    if (item) {
      const char *current_name = connmgr_item_name(item);
      char *new_name =
          connect_prompt_text(ds->dialog_win, "Rename", "Name:", current_name);
      if (new_name) {
        if (connmgr_is_folder(item)) {
          free(item->folder.name);
          item->folder.name = new_name;
        } else {
          free(item->connection.name);
          item->connection.name = new_name;
        }
        ds->mgr->modified = true;
      }
    }
    return true;
  }

  /* Delete */
  if (ds->config && hotkey_matches(ds->config, event, HOTKEY_CONN_DELETE)) {
    ConnectionItem *item =
        connmgr_get_visible_item(ds->mgr, ds->tree_highlight);
    if (item && item->parent) {
      const char *name = connmgr_item_name(item);
      char msg[128];
      snprintf(msg, sizeof(msg), "Delete '%s'?", name ? name : "item");

      if (connect_confirm(ds->dialog_win, "Confirm Delete", msg)) {
        connmgr_remove_item(ds->mgr, item);
        if (ds->tree_highlight > 0) {
          ds->tree_highlight--;
        }
      }
    }
    return true;
  }

  return false;
}

