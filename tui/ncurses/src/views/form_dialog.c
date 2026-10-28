/*
 * Lace
 * A form dialog (see form_dialog.h)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#include "theme.h"
#include "overlay.h"
#include "form_dialog.h"
#include "list_picker.h"
#include "text_input_draw.h"
#include "../render_helpers.h"
#include "../tui_internal.h"
#include "../../../liblace/include/util/mem.h"
#include "../../../liblace/include/util/str.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

void fd_init(FormDialog *f, const Config *cfg, const char *title) {
  memset(f, 0, sizeof *f);
  f->cfg = cfg;
  f->title = title;
}

static FdField *add(FormDialog *f, int id, const char *label, FdKind kind) {
  if (f->n == f->cap) {
    f->cap = f->cap ? f->cap * 2 : 8;
    f->fields = safe_realloc(f->fields, f->cap * sizeof *f->fields);
  }
  FdField *fl = &f->fields[f->n++];
  memset(fl, 0, sizeof *fl);
  fl->id = id;
  fl->label = label;
  fl->kind = kind;
  return fl;
}

static FdField *find(const FormDialog *f, int id) {
  for (size_t i = 0; i < f->n; i++)
    if (f->fields[i].id == id)
      return &f->fields[i];
  return NULL;
}

void fd_add_text(FormDialog *f, int id, const char *label, const char *text, const char *hint) {
  FdField *fl = add(f, id, label, FD_TEXT);
  fl->text = text && *text ? str_dup(text) : NULL;
  fl->hint = hint;
}

void fd_add_toggle(FormDialog *f, int id, const char *label, bool on) { add(f, id, label, FD_TOGGLE)->on = on; }

void fd_add_pick(FormDialog *f, int id, const char *label, const char *text, const char *const *options,
                 size_t num_options, bool free_text) {
  FdField *fl = add(f, id, label, FD_PICK);
  fl->text = text && *text ? str_dup(text) : NULL;
  fl->options = options;
  fl->num_options = num_options;
  fl->free_text = free_text;
  fl->hint = free_text ? "Enter picks, or type" : "Enter picks";
}

void fd_add_list(FormDialog *f, int id, const char *label, const char *text, const char *const *options,
                 size_t num_options) {
  FdField *fl = add(f, id, label, FD_LIST);
  fl->text = text && *text ? str_dup(text) : NULL;
  fl->options = options;
  fl->num_options = num_options;
  fl->free_text = true;
  fl->hint = "Enter picks, in order";
}

void fd_disable(FormDialog *f, int id, const char *why) {
  FdField *fl = find(f, id);
  if (!fl)
    return;
  fl->disabled = true;
  fl->hint = why;
}

void fd_set_message(FormDialog *f, const char *msg) {
  free(f->message);
  f->message = msg ? str_dup(msg) : NULL;
}

void form_dialog_free(FormDialog *f) {
  for (size_t i = 0; i < f->n; i++)
    free(f->fields[i].text);
  free(f->fields);
  free(f->message);
  memset(f, 0, sizeof *f);
}

/* ---- values ---- */

static char *trimmed(const char *s) {
  if (!s)
    return str_dup("");
  while (*s && isspace((unsigned char)*s))
    s++;
  size_t n = strlen(s);
  while (n && isspace((unsigned char)s[n - 1]))
    n--;
  return str_ndup(s, n);
}

const char *fd_text(const FormDialog *f, int id) {
  static char bufs[8][4096]; /* a ring: several values read in a row stay valid */
  static unsigned next;
  FdField *fl = find(f, id);
  if (!fl || !fl->text)
    return "";
  char *buf = bufs[next++ % 8];
  char *t = trimmed(fl->text);
  snprintf(buf, 4096, "%s", t);
  free(t);
  return buf;
}

bool fd_on(const FormDialog *f, int id) {
  FdField *fl = find(f, id);
  return fl && fl->on;
}

size_t fd_split_names(const char *text, char ***out) {
  *out = NULL;
  if (!text)
    return 0;
  size_t n = 0, cap = 0;
  const char *p = text;
  for (;;) {
    const char *e = strchr(p, ',');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    char *part = str_ndup(p, len), *t = trimmed(part);
    free(part);
    if (*t) {
      if (n == cap) {
        cap = cap ? cap * 2 : 4;
        *out = safe_realloc(*out, cap * sizeof **out);
      }
      (*out)[n++] = t;
    } else {
      free(t);
    }
    if (!e)
      break;
    p = e + 1;
  }
  return n;
}

size_t fd_names(const FormDialog *f, int id, char ***out) {
  FdField *fl = find(f, id);
  return fd_split_names(fl ? fl->text : NULL, out);
}

/* ---- the dialog ---- */

enum { BTN_SAVE, BTN_CANCEL };

static bool editable(const FdField *fl) {
  return !fl->disabled && (fl->kind == FD_TEXT || fl->kind == FD_LIST || (fl->kind == FD_PICK && fl->free_text));
}

/* Enter on a pick or list field: a popup of its options under the field */
static void pick_option(FormDialog *f, FdField *fl, int y, int x) {
  if (!fl->num_options) {
    fd_set_message(f, fl->free_text ? "No list to pick from: type the value" : "Nothing to pick from");
    return;
  }
  size_t cur = 0;
  for (size_t i = 0; fl->kind == FD_PICK && i < fl->num_options; i++)
    if (fl->in.buf && !strcmp(fl->in.buf, fl->options[i]))
      cur = i;
  int screen_h, screen_w;
  getmaxyx(stdscr, screen_h, screen_w);
  int sel = tui_pick_from_list(f->cfg, fl->label, fl->options, fl->num_options, cur, y, x, 24, screen_w - 4,
                               screen_h - 4);
  if (sel < 0)
    return;
  const char *name = fl->options[sel];
  if (fl->kind == FD_LIST && fl->in.len) {
    char *t = str_printf("%s, %s", fl->in.buf, name);
    text_input_set(&fl->in, t);
    free(t);
  } else {
    text_input_set(&fl->in, name);
  }
}

bool form_dialog_run(FormDialog *f) {
  int screen_h, screen_w;
  getmaxyx(stdscr, screen_h, screen_w);
  int height = (int)f->n + 6;
  if (height < 8)
    height = 8; /* room for the message, the buttons and one field */
  if (height > screen_h - 2)
    height = screen_h - 2;
  int width = screen_w - 4 < 72 ? screen_w - 4 : 72;
  if (height < 8 || width < 30)
    return false;
  int y = (screen_h - height) / 2, x = (screen_w - width) / 2;
  WINDOW *dlg = newwin(height, width, y > 0 ? y : 0, x > 0 ? x : 0);
  if (!dlg)
    return false;
  keypad(dlg, TRUE);

  for (size_t i = 0; i < f->n; i++)
    text_input_init(&f->fields[i].in, f->fields[i].text ? f->fields[i].text : "", false);
  int lw = 0;
  for (size_t i = 0; i < f->n; i++) {
    int w = utf8_width(f->fields[i].label, strlen(f->fields[i].label));
    if (w > lw)
      lw = w;
  }
  if (lw > 20)
    lw = 20;
  int fx = 2 + lw + 2, fw = width - fx - 2;
  int vis = height - 6; /* field rows shown at once */
  size_t focus = 0, scroll = 0;
  int button = -1; /* -1: a field has the focus; else BTN_SAVE / BTN_CANCEL */
  bool running = true, saved = false;
  static const char *const labels[] = {"Save", "Cancel"};

  while (running) {
    if (focus < scroll)
      scroll = focus;
    if (focus >= scroll + (size_t)vis)
      scroll = focus - (size_t)vis + 1;
    werase(dlg);
    tui_frame_window(dlg, f->title, "\xe2\x86\x91\xe2\x86\x93 field  Enter edit/pick  F2 save  Esc cancel");
    int cursor_x = -1, cursor_y = -1;
    for (int i = 0; i < vis; i++) {
      size_t k = scroll + (size_t)i;
      if (k >= f->n)
        break;
      FdField *fl = &f->fields[k];
      int ry = 2 + i;
      bool cur = button < 0 && k == focus;
      attr_t lattr = fl->disabled ? A_DIM : cur ? A_BOLD : A_NORMAL;
      WITH_ATTR(dlg, lattr, tui_print_fit(dlg, ry, 2, lw, fl->label));
      if (fl->kind == FD_TOGGLE) {
        attr_t a = fl->disabled ? A_DIM : cur ? A_REVERSE : A_NORMAL;
        WITH_ATTR(dlg, a, tui_print_fit(dlg, ry, fx, fw, fl->on ? "[X] yes" : "[ ] no"));
        if (fl->disabled && fl->hint)
          WITH_ATTR(dlg, A_DIM, tui_print_clip(dlg, ry, fx + 9, fw - 9, fl->hint));
        continue;
      }
      if (fl->disabled) {
        char *shown = fl->in.len ? str_printf("%s  (%s)", fl->in.buf, fl->hint ? fl->hint : "") : str_dup(fl->hint ? fl->hint : "");
        WITH_ATTR(dlg, A_DIM, tui_print_fit(dlg, ry, fx, fw, shown));
        free(shown);
        continue;
      }
      if (!fl->in.len && !cur && fl->hint) {
        WITH_ATTR(dlg, A_DIM, tui_print_fit(dlg, ry, fx, fw, fl->hint));
      } else {
        int cx = text_input_draw(dlg, &fl->in, ry, fx, fw - 1, cur ? COLOR_PAIR(COLOR_SELECTED) : A_NORMAL, cur);
        if (cur && editable(fl) && cx >= 0) {
          cursor_x = cx;
          cursor_y = ry;
        }
      }
      if (fl->kind != FD_TEXT)
        mvwaddstr(dlg, ry, fx + fw - 1, "\xe2\x96\xbe"); /* a list opens here */
    }
    if (f->message)
      WITH_ATTR(dlg, COLOR_PAIR(COLOR_ERROR_TEXT), tui_print_clip(dlg, height - 4, 2, width - 4, f->message));
    WITH_ATTR(dlg, COLOR_PAIR(COLOR_BORDER), mvwaddch(dlg, height - 3, 0, ACS_LTEE);
              mvwhline(dlg, height - 3, 1, ACS_HLINE, width - 2); mvwaddch(dlg, height - 3, width - 1, ACS_RTEE));
    int bx = width / 2 - 10;
    WITH_ATTR(dlg, button == BTN_SAVE ? A_REVERSE : A_NORMAL, mvwprintw(dlg, height - 2, bx, "[ %s ]", labels[0]));
    WITH_ATTR(dlg, button == BTN_CANCEL ? A_REVERSE : A_NORMAL, mvwprintw(dlg, height - 2, bx + 9, "[ %s ]", labels[1]));
    if (cursor_x >= 0) {
      wmove(dlg, cursor_y, cursor_x);
      curs_set(1);
    } else {
      curs_set(0);
    }
    wrefresh(dlg);

    int ch = wgetch(dlg);
    if (ch == KEY_RESIZE)
      continue;
    UiEvent ev;
    if (!render_translate_key(ch, &ev) || ev.type != UI_EVENT_KEY)
      continue;
    FdField *fl = button < 0 ? &f->fields[focus] : NULL;
    if (render_event_is_special(&ev, UI_KEY_ESCAPE)) {
      running = false;
    } else if (hotkey_matches(f->cfg, &ev, HOTKEY_ROW_SAVE) || (ev.key.mods & UI_MOD_CTRL && !ev.key.is_special && ev.key.key == 'S')) {
      saved = true;
      running = false;
    } else if (render_event_is_special(&ev, UI_KEY_TAB) || render_event_is_special(&ev, UI_KEY_DOWN)) {
      if (button < 0 && focus + 1 < f->n) focus++;
      else if (button < 0) button = BTN_SAVE;
      else if (button == BTN_SAVE) button = BTN_CANCEL;
      else { button = -1; focus = 0; }
    } else if (render_event_is_special(&ev, UI_KEY_BTAB) || render_event_is_special(&ev, UI_KEY_UP)) {
      if (button == BTN_CANCEL) button = BTN_SAVE;
      else if (button == BTN_SAVE) { button = -1; focus = f->n ? f->n - 1 : 0; }
      else if (focus > 0) focus--;
      else button = BTN_CANCEL;
    } else if (button >= 0) {
      if (render_event_is_special(&ev, UI_KEY_LEFT)) button = BTN_SAVE;
      else if (render_event_is_special(&ev, UI_KEY_RIGHT)) button = BTN_CANCEL;
      else if (render_event_is_special(&ev, UI_KEY_ENTER)) {
        saved = button == BTN_SAVE;
        running = false;
      }
    } else if (render_event_is_special(&ev, UI_KEY_ENTER)) {
      if (fl->disabled) {
        fd_set_message(f, fl->hint);
      } else if (fl->kind == FD_TOGGLE) {
        fl->on = !fl->on;
      } else if (fl->kind == FD_TEXT) {
        if (focus + 1 < f->n) focus++;
        else button = BTN_SAVE;
      } else {
        int by, bxx;
        getbegyx(dlg, by, bxx);
        pick_option(f, fl, by + 2 + (int)(focus - scroll) + 1, bxx + fx);
      }
    } else if (fl->disabled) {
      /* nothing: the hint says why */
    } else if (fl->kind == FD_TOGGLE) {
      if (!ev.key.is_special && ev.key.key == ' ') fl->on = !fl->on;
    } else if (fl->kind == FD_PICK && !fl->free_text) {
      if (!ev.key.is_special && !(ev.key.mods & (UI_MOD_CTRL | UI_MOD_ALT))) {
        int by, bxx;
        getbegyx(dlg, by, bxx);
        pick_option(f, fl, by + 2 + (int)(focus - scroll) + 1, bxx + fx);
      }
    } else {
      text_input_handle_key(&fl->in, &ev);
    }
  }

  for (size_t i = 0; i < f->n; i++) {
    size_t cur;
    free(f->fields[i].text);
    f->fields[i].text = text_input_release(&f->fields[i].in, &cur);
    if (f->fields[i].text && !*f->fields[i].text) {
      free(f->fields[i].text);
      f->fields[i].text = NULL;
    }
  }
  curs_set(0);
  delwin(dlg);
  touchwin(stdscr);
  return saved;
}
