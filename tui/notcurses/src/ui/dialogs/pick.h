#ifndef NC_PICK_H
#define NC_PICK_H
#include "app.h"
/* index is the picked item's, SIZE_MAX when the overlay was cancelled */
typedef void (*PickFn)(NcApp *app, size_t index, void *data);
/* A list overlay of items (copied) to pick one from; fn runs exactly once */
void pick_open(NcApp *app, const char *title, char **items, size_t n, PickFn fn, void *data);
#endif
