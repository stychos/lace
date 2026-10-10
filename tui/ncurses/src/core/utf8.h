/*
 * Lace
 * UTF-8 helpers for text editing and display
 *
 * Keys arrive from ncurses one byte at a time, so a multi-byte character is
 * inserted byte by byte and is complete once its last byte arrives. Cursor
 * movement and deletion step over whole characters; positions are byte
 * offsets that always sit on a character boundary.
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_UTF8_H
#define LACE_UTF8_H

#include <stdbool.h>
#include <stddef.h>

/* True for a key that inserts text: printable ASCII or any byte of a
 * multi-byte UTF-8 character (0x80-0xFF). */
bool utf8_is_text_key(int ch);

/* Bytes in the character that starts with lead byte c (1 for ASCII or an
 * invalid byte). */
size_t utf8_seq_len(unsigned char c);

/* Largest n' <= n that does not split a character (s must be readable up
 * to s[n]). Use it when cutting a string to fit a buffer. */
size_t utf8_truncate(const char *s, size_t n);

/* Offset of the character before pos (0 if pos is 0). */
size_t utf8_prev(const char *s, size_t pos);

/* Offset of the character after pos (len if pos >= len). */
size_t utf8_next(const char *s, size_t len, size_t pos);

/* Number of characters in the first n bytes of s (e.g. for masking a
 * password with one '*' per character). */
size_t utf8_count(const char *s, size_t n);

/* Display width of the first n bytes of s, in terminal columns.
 * Invalid bytes and control characters count as one column. */
int utf8_width(const char *s, size_t n);

/* Number of bytes of s (at most n) whose characters fit in max_cols
 * columns, never splitting a character. *cols receives their width. */
size_t utf8_fit(const char *s, size_t n, int max_cols, int *cols);

/* Byte offset at which display column col starts (or n if past the end). */
size_t utf8_offset_at_col(const char *s, size_t n, int col);

#endif /* LACE_UTF8_H */
