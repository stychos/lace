#ifndef NC_CONFIRM_H
#define NC_CONFIRM_H
#include "app.h"
typedef void (*ConfirmFn)(NcApp *app, bool yes, void *data);
/* fn runs once: true after Yes, false after Cancel/Esc/click outside */
void confirm_open(NcApp *app, const char *title, const char *message, const char *yes_label,
                  bool destructive, ConfirmFn fn, void *data); /* default Cancel */
#endif
