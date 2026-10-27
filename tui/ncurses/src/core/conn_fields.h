/* The connection form's fields, without any drawing: what the connection tab
 * edits. Filled from a saved connection or a URI, turned back into a saved
 * connection, a connection string or the (masked) URI line. */
#ifndef LACE_CONN_FIELDS_H
#define LACE_CONN_FIELDS_H
#include "views/text_input.h"
#include <config/saved_connection.h>
#include <stdbool.h>

enum {
  CF_NAME, CF_DRIVER, CF_FILE, CF_HOST, CF_PORT, CF_DB, CF_USER, CF_PASS, CF_SAVEPW, CF_SSH, CF_SSHHOST,
  CF_SSHPORT, CF_SSHUSER, CF_SSHKEY, CF_SSHPASS, CF_SAVESSHPW, CF_COUNT
};
#define CF_NUM_DRIVERS 4 /* sqlite, postgres, mysql, mariadb; a fifth is a saved driver this build does not know */
#define CF_PASSWORD_MASK "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2"

typedef struct ConnFields {
  TextInput in[CF_COUNT]; /* the text fields (CF_DRIVER and the checkboxes leave theirs empty) */
  bool chk[CF_COUNT];     /* the checkboxes */
  int driver;             /* index into the drivers; 0 is SQLite */
  char *custom;           /* the unknown driver's name (driver == CF_NUM_DRIVERS) */
  char *baseline;         /* the snapshot at the last fill, clear or mark_clean */
  char *message;          /* validation or parse error, test result */
  bool message_error;
} ConnFields;

extern const char *const conn_fields_labels[CF_COUNT];
void conn_fields_init(ConnFields *f);  /* empty, driver postgres */
void conn_fields_free(ConnFields *f);  /* wipes what was typed */
/* Every field from c: a password only when it is saved, never one left from
 * before; the result is clean */
void conn_fields_fill(ConnFields *f, const LaceSavedConnection *c);
void conn_fields_clear(ConnFields *f); /* as init; clean */
int conn_fields_num_drivers(const ConnFields *f);
const char *conn_fields_driver_name(const ConnFields *f, int i);
bool conn_fields_is_check(int field);
bool conn_fields_visible(const ConnFields *f, int field);
int conn_fields_next(const ConnFields *f, int from, int dir); /* the next visible field, wrapping */
/* Whether anything (passwords included) differs from the last fill/clear/mark_clean */
bool conn_fields_dirty(const ConnFields *f);
void conn_fields_mark_clean(ConnFields *f);
void conn_fields_set_message(ConnFields *f, bool error, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
/* A text field's text ("" for a field out of range or with none) and setting
 * it (for callers that cannot reach the array, Swift) */
const char *conn_fields_text(const ConnFields *f, int field);
void conn_fields_set_text(ConnFields *f, int field, const char *text);
/* The saved connection the fields describe; NULL (message set) when they do
 * not validate. with_secrets keeps typed passwords whatever the save boxes say. */
LaceSavedConnection *conn_fields_collect(ConnFields *f, bool with_secrets);
/* The string to connect with (typed passwords in); NULL (message set) when invalid */
char *conn_fields_connstr(ConnFields *f);
/* The URI line: the connection string with every password shown as
 * CF_PASSWORD_MASK; "" when the fields do not make one */
char *conn_fields_uri(const ConnFields *f);
/* Typed into the URI line: a URL or a SQLite file. On success every field but
 * the name follows it (a password equal to the mask keeps the field's);
 * otherwise the fields stay and the message says why. */
bool conn_fields_set_uri(ConnFields *f, const char *text);

/* Validation: an error message or NULL. port: empty or 1..65535; host: no "/ ? # @" or whitespace */
char *connect_check_port(const char *text, const char *what);
char *connect_check_host(const char *text, const char *what);

/* What was typed or given on the command line as a connection string: a URL
 * as it is, an existing SQLite file (a leading ~/ is $HOME) made a URL;
 * NULL and *error otherwise */
char *conn_target_to_connstr(const char *text, char **error);
/* Whether two connections are the same database: by saved id when either
 * has one, else by the connection string without passwords */
bool conn_key_matches(const char *saved_a, const char *connstr_a, const char *saved_b, const char *connstr_b);
#endif
