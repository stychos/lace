/*
 * Lace
 * One-line prompt dialog (names, passwords)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_PROMPT_H
#define LACE_PROMPT_H

#include <stdbool.h>

typedef struct {
  const char *title;
  const char *label;
  const char *initial;   /* Starting text (NULL = empty) */
  const char *error_msg; /* Shown under the field (NULL = none) */
  bool secret;           /* Mask the text; wipe it from memory afterwards */
  bool allow_empty;      /* OK on empty text returns ""; else it cancels */
} PromptOptions;

/* Ask for one line of text in a centered dialog: input, OK and Cancel, Tab
 * and Shift+Tab move between them. Returns the text (caller frees; use
 * str_secure_free for a secret one), or NULL if cancelled. */
char *tui_prompt(const PromptOptions *opts);

#endif /* LACE_PROMPT_H */
