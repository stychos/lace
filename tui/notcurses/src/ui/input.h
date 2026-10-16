#ifndef LACE_NC_INPUT_H
#define LACE_NC_INPUT_H
#include "core/ui_types.h"
#include "views/text_input.h"
#include <stdbool.h>
#include <stdint.h>
#include "ui/theme.h"

struct ncinput;
/* Translate one notcurses input event; false: ignore it */
bool input_translate(const struct ncinput *ni, uint32_t id, UiEvent *out);
/* text_input_handle_key() plus whole multi-byte characters: a non-ASCII key
 * carries its UTF-8 text in key.text, which the shared line editor ignores
 * (it inserts only the first byte) */
bool nc_text_input_key(TextInput *in, const UiEvent *ev);
/* Ctrl+<letter> (upper case) */
static inline bool nc_key_ctrl(const UiEvent *ev, char letter) {
  return ev->type == UI_EVENT_KEY && !ev->key.is_special && ev->key.key == letter &&
         (ev->key.mods & UI_MOD_CTRL) && !(ev->key.mods & UI_MOD_ALT);
}
static inline bool nc_key_special(const UiEvent *ev, int key) {
  return ev->type == UI_EVENT_KEY && ev->key.is_special && ev->key.key == key;
}
/* The text a plain printable key types (whole UTF-8 character), or NULL; buf
 * needs 8 bytes */
const char *nc_key_typed(const UiEvent *ev, char buf[8]);
/* Draw a text input in width columns at (y, x) of the plane, scrolled so the
 * cursor shows, on the edit background (focused) or the plain one; returns the
 * cursor's column offset inside the field */
int nc_text_input_draw(struct ncplane *p, const Theme *t, const TextInput *in, int y, int x,
                       int width, bool focused);
#endif
