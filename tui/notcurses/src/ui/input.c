#include "input.h"
#include "core/utf8.h"
#include <notcurses/notcurses.h>
#include <stdio.h>
#include <string.h>

static bool special(uint32_t id, int *out) {
  switch (id) {
  case NCKEY_UP: *out = UI_KEY_UP; return true;
  case NCKEY_DOWN: *out = UI_KEY_DOWN; return true;
  case NCKEY_LEFT: *out = UI_KEY_LEFT; return true;
  case NCKEY_RIGHT: *out = UI_KEY_RIGHT; return true;
  case NCKEY_HOME: *out = UI_KEY_HOME; return true;
  case NCKEY_END: *out = UI_KEY_END; return true;
  case NCKEY_PGUP: *out = UI_KEY_PAGEUP; return true;
  case NCKEY_PGDOWN: *out = UI_KEY_PAGEDOWN; return true;
  case NCKEY_BACKSPACE: *out = UI_KEY_BACKSPACE; return true;
  case NCKEY_DEL: *out = UI_KEY_DELETE; return true;
  case NCKEY_INS: *out = UI_KEY_INSERT; return true;
  case NCKEY_ENTER: *out = UI_KEY_ENTER; return true;
  case NCKEY_ESC: *out = UI_KEY_ESCAPE; return true;
  case NCKEY_TAB: *out = UI_KEY_TAB; return true;
  default: break;
  }
  if (id >= NCKEY_F01 && id <= NCKEY_F12) {
    *out = UI_KEY_F1 + (int)(id - NCKEY_F01);
    return true;
  }
  if (id == 0x7f) { *out = UI_KEY_BACKSPACE; return true; } /* legacy DEL byte */
  return false;
}

/* The modifier mask: `modifiers`, plus the deprecated bools, which are the
 * only place a legacy ESC-prefixed Alt shows up */
static unsigned modifiers_of(const struct ncinput *ni) {
  unsigned m = 0;
  if (ni->modifiers & NCKEY_MOD_CTRL) m |= UI_MOD_CTRL;
  if (ni->modifiers & NCKEY_MOD_ALT) m |= UI_MOD_ALT;
  if (ni->modifiers & NCKEY_MOD_SHIFT) m |= UI_MOD_SHIFT;
  if (ni->modifiers & NCKEY_MOD_SUPER) m |= UI_MOD_SUPER; /* Cmd on macOS, when the terminal forwards it */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  if (ni->ctrl) m |= UI_MOD_CTRL;
  if (ni->alt) m |= UI_MOD_ALT;
  if (ni->shift) m |= UI_MOD_SHIFT;
#pragma clang diagnostic pop
  return m;
}

bool input_translate(const struct ncinput *ni, uint32_t id, UiEvent *out) {
  memset(out, 0, sizeof *out);
  /* A held key repeats: under the kitty protocol its repeats come typed as
   * such and count as presses (so Backspace, letters and the arrows all
   * repeat in a field, as on a legacy terminal); releases mean nothing
   * except for the mouse buttons */
  if (ni->evtype == NCTYPE_RELEASE && !nckey_mouse_p(id)) return false;
  if (id == NCKEY_RESIZE) { out->type = UI_EVENT_RESIZE; return true; }
  if (id == NCKEY_EOF) { out->type = UI_EVENT_QUIT; return true; }
  unsigned mods = modifiers_of(ni);
  if (nckey_mouse_p(id)) {
    out->type = UI_EVENT_MOUSE;
    out->mouse.x = ni->x;
    out->mouse.y = ni->y;
    out->mouse.mods = (UiKeyMod)mods;
    out->mouse.action = ni->evtype == NCTYPE_RELEASE ? UI_MOUSE_RELEASE : UI_MOUSE_PRESS;
    switch (id) {
    case NCKEY_BUTTON1: out->mouse.button = UI_MOUSE_LEFT; break;
    case NCKEY_BUTTON2: out->mouse.button = UI_MOUSE_MIDDLE; break;
    case NCKEY_BUTTON3: out->mouse.button = UI_MOUSE_RIGHT; break;
    case NCKEY_SCROLL_UP: out->mouse.button = UI_MOUSE_SCROLL_UP; break;
    case NCKEY_SCROLL_DOWN: out->mouse.button = UI_MOUSE_SCROLL_DOWN; break;
    default: return false;
    }
    return true;
  }
  out->type = UI_EVENT_KEY;
  int code;
  if (id == NCKEY_TAB && (mods & UI_MOD_SHIFT)) {
    out->key.key = UI_KEY_BTAB;
    out->key.is_special = true;
    out->key.mods = (UiKeyMod)(mods & ~(unsigned)UI_MOD_SHIFT);
    return true;
  }
  if (special(id, &code)) {
    out->key.key = code;
    out->key.is_special = true;
    out->key.mods = (UiKeyMod)mods;
    return true;
  }
  if (id >= NCKEY_INVALID) return false; /* a synthesized id we do not know (F13+, ...) */
  if (id < 32 && !(mods & UI_MOD_CTRL)) { /* a raw control byte: Ctrl+letter on a legacy terminal */
    if (id == 0) return false;
    out->key.key = 'A' + (int)id - 1;
    out->key.mods = (UiKeyMod)(mods | UI_MOD_CTRL);
    return true;
  }
  if (id >= 'a' && id <= 'z' && (mods & (UI_MOD_CTRL | UI_MOD_ALT | UI_MOD_SHIFT | UI_MOD_SUPER)))
    id = id - 'a' + 'A';
  /* On a character Shift is its case, unless Ctrl, Alt or Cmd is held too:
   * then it stays a modifier (Ctrl+Shift+P), the rule config.c's parser applies */
  if ((mods & UI_MOD_SHIFT) && !(mods & (UI_MOD_CTRL | UI_MOD_ALT | UI_MOD_SUPER))) mods &= ~(unsigned)UI_MOD_SHIFT;
  /* Under the kitty keyboard protocol the id is the key's base character
   * ('1' for Shift+1, '[' for '{') and the text the press produces, with
   * Shift and the layout applied, is only in eff_text: that is what was
   * typed, and what a character hotkey like '{' or '?' is bound to. An
   * older notcurses (3.0.7, Ubuntu 24.04) has no eff_text: there the id
   * is all there is */
#ifdef NCINPUT_MAX_EFF_TEXT_CODEPOINTS
  if (ni->eff_text[0] && !(mods & (UI_MOD_CTRL | UI_MOD_ALT | UI_MOD_SUPER))) {
    unsigned n = 0;
    while (n < NCINPUT_MAX_EFF_TEXT_CODEPOINTS && ni->eff_text[n]) n++;
    unsigned char utf8[sizeof out->key.text];
    int len = notcurses_ucs32_to_utf8(ni->eff_text, n, utf8, sizeof utf8 - 1);
    if (len > 0) {
      utf8[len] = 0;
      out->key.key = ni->eff_text[0] >= 32 && ni->eff_text[0] <= 0x7e && n == 1
                         ? (int)ni->eff_text[0] : utf8[0];
      out->key.mods = (UiKeyMod)mods;
      out->key.is_special = false;
      if (out->key.key > 0x7e || n > 1) memcpy(out->key.text, utf8, (size_t)len + 1);
      return true;
    }
  }
#endif
  if (id > 0x7f) { /* text: first byte for the ncurses-style byte path, text for the rest */
    out->key.key = (unsigned char)ni->utf8[0];
    out->key.mods = (UiKeyMod)mods;
    out->key.is_special = false;
    snprintf(out->key.text, sizeof out->key.text, "%s", ni->utf8);
    return true;
  }
  out->key.key = (int)id;
  out->key.mods = (UiKeyMod)mods;
  out->key.is_special = false;
  return true;
}

bool nc_text_input_key(TextInput *in, const UiEvent *ev) {
  if (ev->type == UI_EVENT_KEY && !ev->key.is_special && ev->key.text[0] &&
      !(ev->key.mods & (UI_MOD_CTRL | UI_MOD_ALT))) {
    text_input_insert(in, ev->key.text);
    return true;
  }
  return text_input_handle_key(in, ev);
}

int nc_text_input_draw(struct ncplane *p, const Theme *t, const TextInput *in, int y, int x,
                       int width, bool focused) {
  const char *buf = in->buf ? in->buf : "";
  size_t len = in->len, cur = in->cursor < len ? in->cursor : len;
  size_t start = 0;
  while (start < cur && utf8_width(buf + start, cur - start) > width - 1)
    start = utf8_next(buf, len, start);
  nc_style(p, t, TC_OVERLAY_FG, focused ? TC_EDIT_BG : TC_OVERLAY_BG, focused ? 0 : NCSTYLE_UNDERLINE);
  nc_put(p, y, x, width, buf + start);
  return utf8_width(buf + start, cur - start);
}

const char *nc_key_typed(const UiEvent *ev, char buf[8]) {
  if (ev->type != UI_EVENT_KEY || ev->key.is_special || (ev->key.mods & (UI_MOD_CTRL | UI_MOD_ALT)))
    return NULL;
  if (ev->key.text[0]) {
    memcpy(buf, ev->key.text, 8);
    buf[7] = 0;
    return buf;
  }
  if (ev->key.key < 32 || ev->key.key > 0x7e) return NULL;
  buf[0] = (char)ev->key.key;
  buf[1] = 0;
  return buf;
}
