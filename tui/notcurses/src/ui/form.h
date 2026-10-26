/* A form overlay: one row per field (text, on/off, a pick from a list, a
 * list of names picked one at a time), the structure editors' dialog. The
 * caller builds the fields, opens the form with a submit callback (F2) and
 * reads the values back by field id; a form in flight is found again by its
 * uid, never by pointer (Esc may have closed it). */
#ifndef NC_FORM_H
#define NC_FORM_H
#include "app.h"
#include "views/text_input.h"

typedef enum {
  FORM_TEXT,   /* free text */
  FORM_TOGGLE, /* on/off: Enter or Space flips */
  FORM_PICK,   /* one of the options (Enter opens the list); typing too when free_text */
  FORM_LIST    /* comma-separated names: Enter picks one from the options and appends it */
} FormKind;

typedef struct {
  int id;
  char *label;
  FormKind kind;
  char *text; /* TEXT, PICK, LIST */
  bool on;    /* TOGGLE */
  char **options;
  size_t num_options;
  bool free_text;
  bool disabled; /* shown dim, not editable; hint says why */
  char *hint;    /* dim text after an empty value */
} FormField;

typedef struct { FormField *v; size_t n, cap; } FormFields;
void form_fields_text(FormFields *ff, int id, const char *label, const char *text, const char *hint);
void form_fields_toggle(FormFields *ff, int id, const char *label, bool on);
void form_fields_pick(FormFields *ff, int id, const char *label, const char *text, bool free_text);
void form_fields_list(FormFields *ff, int id, const char *label, const char *text);
void form_fields_options(FormFields *ff, int id, const char *const *names, size_t n); /* copied */
void form_fields_disable(FormFields *ff, int id, const char *why);
void form_fields_free(FormFields *ff);

typedef struct Form Form;
/* F2: the caller validates, builds its statement and runs it (the form stays
 * open: form_close it on success, form_set_message on a refusal) */
typedef void (*FormSubmitFn)(NcApp *app, Form *f, void *data);

struct Form {
  uint64_t uid;
  FormField *fields;
  size_t n, cur, scroll;
  bool editing;
  TextInput in;
  char *message;
  bool error, busy;
  Overlay *overlay;
  FormSubmitFn submit;
  void *data;
  void (*free_data)(void *);
  int label_w;
};

/* Opens the overlay; the fields are taken over (ff is emptied) */
Form *form_open(NcApp *app, const char *title, FormFields *ff, FormSubmitFn submit, void *data,
                void (*free_data)(void *));
Form *form_find(NcApp *app, uint64_t uid); /* NULL once closed */
void form_close(NcApp *app, Form *f);
const FormField *form_field(const Form *f, int id);
const char *form_text(const Form *f, int id); /* trimmed; "" when empty or not a text field */
bool form_on(const Form *f, int id);
/* A FORM_LIST's names (or a text split at commas), trimmed, empty ones left
 * out; caller frees the strings and the array */
size_t form_names(const Form *f, int id, char ***out);
void form_set_options(Form *f, int id, char **names, size_t n); /* adopts */
void form_set_message(Form *f, const char *msg, bool error);
void form_set_busy(Form *f, bool busy, const char *msg);

/* The pure parts (unit tests): split "a, b ,,c" into trimmed names */
size_t form_split_names(const char *text, char ***out);
#endif
