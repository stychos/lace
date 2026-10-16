/*
 * Lace
 * Connection string display (no ncurses)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_CONNSTR_DISPLAY_H
#define LACE_CONNSTR_DISPLAY_H

/* What a connection string points at, never a password (allocated):
 * "sqlite: /path" or "driver://user@host:port/db  via ssh user@host".
 * NULL for a string that does not parse. */
char *connstr_display(const char *connstr);

#endif /* LACE_CONNSTR_DISPLAY_H */
