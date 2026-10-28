/* SQL highlighting for the query editor (both TUIs): the next token of a text and its
 * class, found with liblace's lexer so strings, comments and quoted names
 * agree with the statement splitter. */
#ifndef LACE_CORE_SQLHL_H
#define LACE_CORE_SQLHL_H
#include "../../../liblace/include/lace.h"
#include <stddef.h>

typedef enum { HL_PLAIN, HL_KEYWORD, HL_STRING, HL_COMMENT, HL_NUMBER } HlClass;

/* Length (at least 1 unless s[pos] is NUL) and class of the token at pos */
size_t sqlhl_next(const char *s, size_t pos, LaceDriver d, HlClass *cls);
#endif
