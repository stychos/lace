/* A multi-line UTF-8 text buffer with a cursor: the editing core of the cell
 * editor and (Task 11) the query editor. Positions are byte offsets on
 * character boundaries; columns are display columns (a tab counts 4). */
#ifndef NC_TEXTBUF_H
#define NC_TEXTBUF_H
#include <lace.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
  char *text;      /* always NUL-terminated */
  size_t len, cap, cursor;
  size_t want_col; /* the display column vertical moves aim for */
} TextBuf;

void textbuf_init(TextBuf *tb, const char *initial); /* copies; cursor at the end */
/* View a heap string (malloc'd, NUL-terminated) as a buffer; take tb->text back afterwards */
void textbuf_adopt(TextBuf *tb, char *text, size_t cursor, size_t want_col);
void textbuf_free(TextBuf *tb);                       /* wipes: it may hold a password */
void textbuf_set(TextBuf *tb, const char *text);      /* cursor at the end */
void textbuf_insert(TextBuf *tb, const char *s, size_t n); /* at the cursor, newlines kept */
void textbuf_delete(TextBuf *tb, bool backward);      /* one character */
void textbuf_move_char(TextBuf *tb, int dir);         /* -1 / +1, across line breaks */
void textbuf_home(TextBuf *tb);                       /* start of the line */
void textbuf_end(TextBuf *tb);                        /* end of the line */
void textbuf_move_line(TextBuf *tb, int dir);         /* keeps want_col */
size_t textbuf_num_lines(const TextBuf *tb);
void textbuf_line_col(const TextBuf *tb, size_t *line, size_t *col); /* col in display columns */
size_t textbuf_line_start(const TextBuf *tb, size_t line);           /* byte offset; clamps */
size_t textbuf_line_end(const TextBuf *tb, size_t line);             /* before the newline */
/* Byte offset of display column col on a line (clamped to the line's end) */
size_t textbuf_cursor_at(const TextBuf *tb, size_t line, size_t col);
/* Byte range [*start, *end) of the statement at the cursor (lace_sql_statement_at);
 * returns its length, 0 when there is none */
size_t textbuf_statement_at(const TextBuf *tb, LaceDriver d, size_t *start, size_t *end);
#endif
