/*
 * Lace GTK frontend
 * The structure editors and the table actions: AdwDialogs of entry, switch
 * and combo rows (a column, the primary key, an index, a check constraint,
 * a foreign key, MySQL's charset), prompts (rename, the counter, the
 * comment) and the confirmations (drop, truncate, maintenance), each ending
 * in lace_ddl_confirm.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_GTK_DDL_EDITORS_H
#define LACE_GTK_DDL_EDITORS_H

#include "core/catalog_rows.h"
#include "table_view.h"

G_BEGIN_DECLS

/* The Information list's action rows, appended by the table view's builder;
 * their tags, above every shared range */
enum {
  LACE_SV_TAG_AUTOINC = 90001, LACE_SV_TAG_COMMENT, LACE_SV_TAG_ENGINE, LACE_SV_TAG_CHARSET,
  LACE_SV_TAG_ANALYZE, LACE_SV_TAG_VACUUM, LACE_SV_TAG_OPTIMIZE, LACE_SV_TAG_EDIT_DEFINITION
};
void lace_ddl_info_actions(CatalogRows *rows, LaceTableView *v);

/* Run an action on the view. The structure editors' ("struct-add",
 * "struct-edit", "struct-drop", "struct-rename", "struct-key", "struct-move",
 * "struct-check") act on the current sub-view with the selected row's tag
 * (CATALOG_TAG_NONE: none); the table's ("rename-object", "truncate",
 * "drop-object", "new-table", "new-view", "new-function", "edit-definition")
 * on the object. Unknown names say so in a toast. */
void lace_ddl_action(LaceTableView *v, const char *action, int tag);
/* Enter or a double click on a list row with that tag: a column or key
 * opens its editor, an index its rename, an Information action runs */
void lace_ddl_activate(LaceTableView *v, int tag);
/* Whether an action is listed for the view's object and sub-view */
gboolean lace_ddl_action_applies(LaceTableView *v, const char *action);

/* The script driver: set a field of the open editor by its key ("name",
 * "type", "nullable", "default", "collation", "comment", "auto_increment",
 * "after", "columns", "unique", "method", "where", "condition",
 * "ref_table", "ref_columns", "on_delete", "on_update", "deferrable",
 * "initially_deferred", "charset"); false when none is open or has it */
gboolean lace_ddl_editor_set_field(const char *key, const char *value);
gboolean lace_ddl_editor_save(void); /* press the open editor's Save */

G_END_DECLS

#endif /* LACE_GTK_DDL_EDITORS_H */
