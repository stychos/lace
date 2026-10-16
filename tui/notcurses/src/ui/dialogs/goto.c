#include "app.h"
#include "ui/dialogs/prompt.h"
#include "ui/grid.h"
#include "util/mem.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static void goto_done(NcApp *app, char *text, void *data) {
  uint64_t uid = *(uint64_t *)data;
  free(data);
  if (!text) return;
  /* digits, with thousands separators and blanks allowed */
  unsigned long long n = 0;
  bool any = false, bad = false;
  for (const char *c = text; *c; c++) {
    if (isdigit((unsigned char)*c)) {
      if (n > (unsigned long long)(SIZE_MAX / 20)) { bad = true; break; }
      n = n * 10 + (unsigned)(*c - '0');
      any = true;
    } else if (*c != ',' && *c != ' ' && *c != '_') {
      bad = true;
    }
  }
  free(text);
  View *v = app_find_view(app, uid);
  if (!v) return;
  if (bad || !any || n == 0) {
    app_set_status(app, true, "Not a row number: rows start at 1");
    return;
  }
  grid_goto_row(app, v, (size_t)n - 1);
}

void goto_open(NcApp *app, View *v) {
  uint64_t *uid = safe_malloc(sizeof *uid);
  *uid = v->uid;
  prompt_open(app, "Go to row", "Row number (1-based)", "", false, goto_done, uid);
}
