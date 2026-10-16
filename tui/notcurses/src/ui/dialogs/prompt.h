#ifndef NC_PROMPT_H
#define NC_PROMPT_H
#include "app.h"
/* text is owned by the callee; NULL = cancelled */
typedef void (*PromptFn)(NcApp *app, char *text, void *data);
void prompt_open(NcApp *app, const char *title, const char *label, const char *initial, bool secret,
                 PromptFn fn, void *data);
#endif
