/*
 * Lace
 * A form dialog: one row per field (text, on/off, a pick from a list, a
 * list of names picked one at a time), the structure editors' dialog.
 * Synchronous, like tui_prompt: the caller builds the fields, runs the
 * dialog (F2 or the Save button answers true) and reads the values back by
 * field id. Every line field is a TextInput.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_FORM_DIALOG_H
#define LACE_FORM_DIALOG_H

#include "../config/config.h"
#include "text_input.h"
#include <stdbool.h>
#include <stddef.h>

typedef enum {
  FD_TEXT,   /* free text */
  FD_TOGGLE, /* on/off: Enter or Space flips */
  FD_PICK,   /* one of the options (Enter opens the list); typing too when free_text */
  FD_LIST    /* comma-separated names: Enter picks one from the options and appends it */
} FdKind;

typedef struct {
  int id;
  const char *label;
  FdKind kind;
  char *text;  /* TEXT, PICK, LIST: the value (owned) */
  bool on;     /* TOGGLE */
  const char *const *options; /* PICK, LIST (not owned) */
  size_t num_options;
  bool free_text;
  bool disabled; /* shown dim, not editable; hint says why */
  const char *hint;
  TextInput in; /* private: live while the dialog runs */
} FdField;

typedef struct {
  const Config *cfg;
  const char *title;
  FdField *fields;
  size_t n, cap;
  char *message; /* shown under the fields (a refusal); owned */
} FormDialog;

void fd_init(FormDialog *f, const Config *cfg, const char *title);
void fd_add_text(FormDialog *f, int id, const char *label, const char *text, const char *hint);
void fd_add_toggle(FormDialog *f, int id, const char *label, bool on);
void fd_add_pick(FormDialog *f, int id, const char *label, const char *text, const char *const *options,
                 size_t num_options, bool free_text);
void fd_add_list(FormDialog *f, int id, const char *label, const char *text, const char *const *options,
                 size_t num_options);
void fd_disable(FormDialog *f, int id, const char *why);
void fd_set_message(FormDialog *f, const char *msg);
/* Runs the dialog; true on Save (F2, the button). The values are in the
 * fields either way; run it again to show a refusal (fd_set_message) */
bool form_dialog_run(FormDialog *f);
void form_dialog_free(FormDialog *f);
const char *fd_text(const FormDialog *f, int id); /* trimmed; "" when empty */
bool fd_on(const FormDialog *f, int id);
/* The names of a list field (or a text split at commas), trimmed; caller
 * frees the strings and the array */
size_t fd_names(const FormDialog *f, int id, char ***out);
size_t fd_split_names(const char *text, char ***out);

#endif /* LACE_FORM_DIALOG_H */
