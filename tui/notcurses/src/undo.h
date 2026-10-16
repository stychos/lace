/* Undo in this frontend: the view's stack (core/undo_stack.h, `View.undo`)
 * and the writes that revert its last step. A reverted edit is a verified
 * UPDATE back to the old value (refused when the row changed since), a
 * reverted delete an INSERT of every row as it was on screen, followed by a
 * look at the tables that reference this one: rows the database removed or
 * changed with the delete (ON DELETE CASCADE, SET NULL, SET DEFAULT) do not
 * come back, and the status says so. */
#ifndef NC_UNDO_H
#define NC_UNDO_H
#include "app.h"
#include "core/undo_stack.h"
#include "model/view.h"
#include <stdbool.h>

/* Revert the view's last step: queues the write(s); the status says what
 * happened. false when there is nothing to undo. */
bool undo_last(NcApp *app, View *v);
#endif
