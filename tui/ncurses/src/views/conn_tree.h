/*
 * Lace
 * The saved connections tree as the connection form tab lists and changes
 * it (connect_tree.c): a filter, a highlighted row, moving an item
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_CONN_TREE_H
#define LACE_CONN_TREE_H

#include "../config/connections.h"
#include "../tui.h"
#include "text_input.h"

/* Move operation state - tracks folder and position for hierarchical
 * navigation */
typedef struct {
  bool active;                   /* Is move in progress? */
  ConnectionItem *source;        /* Item being moved */
  ConnectionItem *target_folder; /* Folder where item will be inserted */
  size_t insert_pos; /* Position within folder (0=before first, N=after last) */
} MoveState;

typedef struct ConnTree {
  TuiState *state;        /* for prompts and redraws */
  ConnectionManager *mgr; /* the process's (tui_connmgr) */
  const Config *config;
  TextInput filter;       /* filters the tree when not empty */
  size_t highlight;       /* the highlighted row's index in the (filtered) tree */
  MoveState move;
  char *error_msg, *success_msg; /* what an action has to say; the caller takes them */
} ConnTree;

/* One listed row: moving is the item being moved, where it would go */
typedef void (*ConnTreeRowFn)(void *ctx, bool moving, size_t index, ConnectionItem *item, int depth);

const char *conn_tree_filter(const ConnTree *t);
size_t connect_tree_count(ConnTree *t);
ConnectionItem *connect_tree_item(ConnTree *t, size_t index);
/* The tree's rows: as filtered, or during a move with the moved item where it would go */
void connect_tree_rows(ConnTree *t, ConnTreeRowFn add, void *ctx);

/* The actions, on the highlighted item (with none, the root); they set
 * mgr->modified, the caller writes the file */
void connect_tree_new_folder(ConnTree *t, ConnectionItem *at);
void connect_tree_rename(ConnTree *t);
void connect_tree_duplicate(ConnTree *t);
void connect_tree_delete(ConnTree *t);
/* Left/Right on a folder: close/open (Left on an item: to its folder) */
void connect_tree_left_right(ConnTree *t, bool right);
/* Start a move, or drop the item */
void connect_tree_move(ConnTree *t);
/* Keys during a move (Up/Down, drop, Esc); true if one was used */
bool connect_tree_move_key(ConnTree *t, const UiEvent *event);

#endif /* LACE_CONN_TREE_H */
