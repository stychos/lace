/*
 * lace - Database Viewer and Manager
 * Byte buffer for network protocol handling
 */

#ifndef LACE_BUFFER_H
#define LACE_BUFFER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Dynamic byte buffer */
typedef struct {
    uint8_t    *data;
    size_t      len;        /* Current data length */
    size_t      cap;        /* Allocated capacity */
    size_t      pos;        /* Read position */
} Buffer;

/* Buffer lifecycle */
Buffer *buf_new(size_t initial_cap);
Buffer *buf_new_from(const uint8_t *data, size_t len);
void buf_free(Buffer *buf);
void buf_reset(Buffer *buf);
void buf_clear(Buffer *buf);

/* Capacity management */
bool buf_reserve(Buffer *buf, size_t additional);
bool buf_grow(Buffer *buf, size_t min_cap);
void buf_shrink(Buffer *buf);

/* Writing - append data at end */
bool buf_write_u8(Buffer *buf, uint8_t val);
bool buf_write_u16_be(Buffer *buf, uint16_t val);
bool buf_write_u16_le(Buffer *buf, uint16_t val);
bool buf_write_u32_be(Buffer *buf, uint32_t val);
bool buf_write_u32_le(Buffer *buf, uint32_t val);
bool buf_write_u64_be(Buffer *buf, uint64_t val);
bool buf_write_u64_le(Buffer *buf, uint64_t val);
bool buf_write_i8(Buffer *buf, int8_t val);
bool buf_write_i16_be(Buffer *buf, int16_t val);
bool buf_write_i16_le(Buffer *buf, int16_t val);
bool buf_write_i32_be(Buffer *buf, int32_t val);
bool buf_write_i32_le(Buffer *buf, int32_t val);
bool buf_write_i64_be(Buffer *buf, int64_t val);
bool buf_write_i64_le(Buffer *buf, int64_t val);
bool buf_write_bytes(Buffer *buf, const uint8_t *data, size_t len);
bool buf_write_str(Buffer *buf, const char *str);
bool buf_write_str_len(Buffer *buf, const char *str, size_t len);
bool buf_write_cstr(Buffer *buf, const char *str);  /* With null terminator */
bool buf_write_zeros(Buffer *buf, size_t count);

/* Reading - from current position */
bool buf_read_u8(Buffer *buf, uint8_t *val);
bool buf_read_u16_be(Buffer *buf, uint16_t *val);
bool buf_read_u16_le(Buffer *buf, uint16_t *val);
bool buf_read_u32_be(Buffer *buf, uint32_t *val);
bool buf_read_u32_le(Buffer *buf, uint32_t *val);
bool buf_read_u64_be(Buffer *buf, uint64_t *val);
bool buf_read_u64_le(Buffer *buf, uint64_t *val);
bool buf_read_i8(Buffer *buf, int8_t *val);
bool buf_read_i16_be(Buffer *buf, int16_t *val);
bool buf_read_i16_le(Buffer *buf, int16_t *val);
bool buf_read_i32_be(Buffer *buf, int32_t *val);
bool buf_read_i32_le(Buffer *buf, int32_t *val);
bool buf_read_i64_be(Buffer *buf, int64_t *val);
bool buf_read_i64_le(Buffer *buf, int64_t *val);
bool buf_read_bytes(Buffer *buf, uint8_t *out, size_t len);
char *buf_read_str(Buffer *buf, size_t len);          /* Returns allocated string */
char *buf_read_cstr(Buffer *buf);                     /* Read null-terminated string */
bool buf_skip(Buffer *buf, size_t count);

/* Peeking - read without advancing position */
bool buf_peek_u8(Buffer *buf, uint8_t *val);
bool buf_peek_u16_be(Buffer *buf, uint16_t *val);
bool buf_peek_u32_be(Buffer *buf, uint32_t *val);

/* Position management */
size_t buf_remaining(Buffer *buf);
size_t buf_tell(Buffer *buf);
bool buf_seek(Buffer *buf, size_t pos);
bool buf_rewind(Buffer *buf);
const uint8_t *buf_ptr(Buffer *buf);            /* Pointer to current position */
const uint8_t *buf_data(Buffer *buf);           /* Pointer to start */

/* Utility */
void buf_compact(Buffer *buf);                   /* Remove read data */
Buffer *buf_slice(Buffer *buf, size_t len);      /* Extract a slice as new buffer */

/* Debug */
void buf_dump(Buffer *buf, const char *label);

#endif /* LACE_BUFFER_H */
