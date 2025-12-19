/*
 * lace - Database Viewer and Manager
 * Byte buffer implementation
 */

#include "buffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAP 256
#define GROWTH_FACTOR 2

Buffer *buf_new(size_t initial_cap) {
  Buffer *buf = malloc(sizeof(Buffer));
  if (!buf)
    return NULL;

  if (initial_cap == 0)
    initial_cap = INITIAL_CAP;

  buf->data = malloc(initial_cap);
  if (!buf->data) {
    free(buf);
    return NULL;
  }

  buf->len = 0;
  buf->cap = initial_cap;
  buf->pos = 0;
  return buf;
}

Buffer *buf_new_from(const uint8_t *data, size_t len) {
  Buffer *buf = buf_new(len);
  if (!buf)
    return NULL;

  memcpy(buf->data, data, len);
  buf->len = len;
  return buf;
}

void buf_free(Buffer *buf) {
  if (buf) {
    free(buf->data);
    free(buf);
  }
}

void buf_reset(Buffer *buf) {
  buf->len = 0;
  buf->pos = 0;
}

void buf_clear(Buffer *buf) {
  buf->len = 0;
  buf->pos = 0;
  memset(buf->data, 0, buf->cap);
}

bool buf_reserve(Buffer *buf, size_t additional) {
  /* Check for overflow */
  if (additional > SIZE_MAX - buf->len)
    return false;
  size_t needed = buf->len + additional;
  if (needed <= buf->cap)
    return true;
  return buf_grow(buf, needed);
}

bool buf_grow(Buffer *buf, size_t min_cap) {
  size_t new_cap = buf->cap;
  while (new_cap < min_cap) {
    /* Check for overflow before multiplying */
    if (new_cap > SIZE_MAX / GROWTH_FACTOR) {
      /* Would overflow, try exact allocation */
      new_cap = min_cap;
      break;
    }
    new_cap *= GROWTH_FACTOR;
  }

  uint8_t *new_data = realloc(buf->data, new_cap);
  if (!new_data)
    return false;

  buf->data = new_data;
  buf->cap = new_cap;
  return true;
}

void buf_shrink(Buffer *buf) {
  if (buf->len < buf->cap / 4 && buf->cap > INITIAL_CAP) {
    size_t new_cap = buf->cap / 2;
    if (new_cap < buf->len)
      new_cap = buf->len;
    if (new_cap < INITIAL_CAP)
      new_cap = INITIAL_CAP;

    uint8_t *new_data = realloc(buf->data, new_cap);
    if (new_data) {
      buf->data = new_data;
      buf->cap = new_cap;
    }
  }
}

/* Writing functions */

bool buf_write_u8(Buffer *buf, uint8_t val) {
  if (!buf_reserve(buf, 1))
    return false;
  buf->data[buf->len++] = val;
  return true;
}

bool buf_write_u16_be(Buffer *buf, uint16_t val) {
  if (!buf_reserve(buf, 2))
    return false;
  buf->data[buf->len++] = (val >> 8) & 0xFF;
  buf->data[buf->len++] = val & 0xFF;
  return true;
}

bool buf_write_u16_le(Buffer *buf, uint16_t val) {
  if (!buf_reserve(buf, 2))
    return false;
  buf->data[buf->len++] = val & 0xFF;
  buf->data[buf->len++] = (val >> 8) & 0xFF;
  return true;
}

bool buf_write_u32_be(Buffer *buf, uint32_t val) {
  if (!buf_reserve(buf, 4))
    return false;
  buf->data[buf->len++] = (val >> 24) & 0xFF;
  buf->data[buf->len++] = (val >> 16) & 0xFF;
  buf->data[buf->len++] = (val >> 8) & 0xFF;
  buf->data[buf->len++] = val & 0xFF;
  return true;
}

bool buf_write_u32_le(Buffer *buf, uint32_t val) {
  if (!buf_reserve(buf, 4))
    return false;
  buf->data[buf->len++] = val & 0xFF;
  buf->data[buf->len++] = (val >> 8) & 0xFF;
  buf->data[buf->len++] = (val >> 16) & 0xFF;
  buf->data[buf->len++] = (val >> 24) & 0xFF;
  return true;
}

bool buf_write_u64_be(Buffer *buf, uint64_t val) {
  if (!buf_reserve(buf, 8))
    return false;
  buf->data[buf->len++] = (val >> 56) & 0xFF;
  buf->data[buf->len++] = (val >> 48) & 0xFF;
  buf->data[buf->len++] = (val >> 40) & 0xFF;
  buf->data[buf->len++] = (val >> 32) & 0xFF;
  buf->data[buf->len++] = (val >> 24) & 0xFF;
  buf->data[buf->len++] = (val >> 16) & 0xFF;
  buf->data[buf->len++] = (val >> 8) & 0xFF;
  buf->data[buf->len++] = val & 0xFF;
  return true;
}

bool buf_write_u64_le(Buffer *buf, uint64_t val) {
  if (!buf_reserve(buf, 8))
    return false;
  buf->data[buf->len++] = val & 0xFF;
  buf->data[buf->len++] = (val >> 8) & 0xFF;
  buf->data[buf->len++] = (val >> 16) & 0xFF;
  buf->data[buf->len++] = (val >> 24) & 0xFF;
  buf->data[buf->len++] = (val >> 32) & 0xFF;
  buf->data[buf->len++] = (val >> 40) & 0xFF;
  buf->data[buf->len++] = (val >> 48) & 0xFF;
  buf->data[buf->len++] = (val >> 56) & 0xFF;
  return true;
}

bool buf_write_i8(Buffer *buf, int8_t val) {
  return buf_write_u8(buf, (uint8_t)val);
}

bool buf_write_i16_be(Buffer *buf, int16_t val) {
  return buf_write_u16_be(buf, (uint16_t)val);
}

bool buf_write_i16_le(Buffer *buf, int16_t val) {
  return buf_write_u16_le(buf, (uint16_t)val);
}

bool buf_write_i32_be(Buffer *buf, int32_t val) {
  return buf_write_u32_be(buf, (uint32_t)val);
}

bool buf_write_i32_le(Buffer *buf, int32_t val) {
  return buf_write_u32_le(buf, (uint32_t)val);
}

bool buf_write_i64_be(Buffer *buf, int64_t val) {
  return buf_write_u64_be(buf, (uint64_t)val);
}

bool buf_write_i64_le(Buffer *buf, int64_t val) {
  return buf_write_u64_le(buf, (uint64_t)val);
}

bool buf_write_bytes(Buffer *buf, const uint8_t *data, size_t len) {
  if (!buf_reserve(buf, len))
    return false;
  memcpy(buf->data + buf->len, data, len);
  buf->len += len;
  return true;
}

bool buf_write_str(Buffer *buf, const char *str) {
  return buf_write_bytes(buf, (const uint8_t *)str, strlen(str));
}

bool buf_write_str_len(Buffer *buf, const char *str, size_t len) {
  return buf_write_bytes(buf, (const uint8_t *)str, len);
}

bool buf_write_cstr(Buffer *buf, const char *str) {
  size_t len = strlen(str) + 1;
  return buf_write_bytes(buf, (const uint8_t *)str, len);
}

bool buf_write_zeros(Buffer *buf, size_t count) {
  if (!buf_reserve(buf, count))
    return false;
  memset(buf->data + buf->len, 0, count);
  buf->len += count;
  return true;
}

/* Reading functions */

bool buf_read_u8(Buffer *buf, uint8_t *val) {
  if (buf_remaining(buf) < 1)
    return false;
  *val = buf->data[buf->pos++];
  return true;
}

bool buf_read_u16_be(Buffer *buf, uint16_t *val) {
  if (buf_remaining(buf) < 2)
    return false;
  *val = ((uint16_t)buf->data[buf->pos] << 8) |
         ((uint16_t)buf->data[buf->pos + 1]);
  buf->pos += 2;
  return true;
}

bool buf_read_u16_le(Buffer *buf, uint16_t *val) {
  if (buf_remaining(buf) < 2)
    return false;
  *val = ((uint16_t)buf->data[buf->pos + 1] << 8) |
         ((uint16_t)buf->data[buf->pos]);
  buf->pos += 2;
  return true;
}

bool buf_read_u32_be(Buffer *buf, uint32_t *val) {
  if (buf_remaining(buf) < 4)
    return false;
  *val = ((uint32_t)buf->data[buf->pos] << 24) |
         ((uint32_t)buf->data[buf->pos + 1] << 16) |
         ((uint32_t)buf->data[buf->pos + 2] << 8) |
         ((uint32_t)buf->data[buf->pos + 3]);
  buf->pos += 4;
  return true;
}

bool buf_read_u32_le(Buffer *buf, uint32_t *val) {
  if (buf_remaining(buf) < 4)
    return false;
  *val = ((uint32_t)buf->data[buf->pos + 3] << 24) |
         ((uint32_t)buf->data[buf->pos + 2] << 16) |
         ((uint32_t)buf->data[buf->pos + 1] << 8) |
         ((uint32_t)buf->data[buf->pos]);
  buf->pos += 4;
  return true;
}

bool buf_read_u64_be(Buffer *buf, uint64_t *val) {
  if (buf_remaining(buf) < 8)
    return false;
  *val = ((uint64_t)buf->data[buf->pos] << 56) |
         ((uint64_t)buf->data[buf->pos + 1] << 48) |
         ((uint64_t)buf->data[buf->pos + 2] << 40) |
         ((uint64_t)buf->data[buf->pos + 3] << 32) |
         ((uint64_t)buf->data[buf->pos + 4] << 24) |
         ((uint64_t)buf->data[buf->pos + 5] << 16) |
         ((uint64_t)buf->data[buf->pos + 6] << 8) |
         ((uint64_t)buf->data[buf->pos + 7]);
  buf->pos += 8;
  return true;
}

bool buf_read_u64_le(Buffer *buf, uint64_t *val) {
  if (buf_remaining(buf) < 8)
    return false;
  *val = ((uint64_t)buf->data[buf->pos + 7] << 56) |
         ((uint64_t)buf->data[buf->pos + 6] << 48) |
         ((uint64_t)buf->data[buf->pos + 5] << 40) |
         ((uint64_t)buf->data[buf->pos + 4] << 32) |
         ((uint64_t)buf->data[buf->pos + 3] << 24) |
         ((uint64_t)buf->data[buf->pos + 2] << 16) |
         ((uint64_t)buf->data[buf->pos + 1] << 8) |
         ((uint64_t)buf->data[buf->pos]);
  buf->pos += 8;
  return true;
}

bool buf_read_i8(Buffer *buf, int8_t *val) {
  return buf_read_u8(buf, (uint8_t *)val);
}

bool buf_read_i16_be(Buffer *buf, int16_t *val) {
  return buf_read_u16_be(buf, (uint16_t *)val);
}

bool buf_read_i16_le(Buffer *buf, int16_t *val) {
  return buf_read_u16_le(buf, (uint16_t *)val);
}

bool buf_read_i32_be(Buffer *buf, int32_t *val) {
  return buf_read_u32_be(buf, (uint32_t *)val);
}

bool buf_read_i32_le(Buffer *buf, int32_t *val) {
  return buf_read_u32_le(buf, (uint32_t *)val);
}

bool buf_read_i64_be(Buffer *buf, int64_t *val) {
  return buf_read_u64_be(buf, (uint64_t *)val);
}

bool buf_read_i64_le(Buffer *buf, int64_t *val) {
  return buf_read_u64_le(buf, (uint64_t *)val);
}

bool buf_read_bytes(Buffer *buf, uint8_t *out, size_t len) {
  if (buf_remaining(buf) < len)
    return false;
  memcpy(out, buf->data + buf->pos, len);
  buf->pos += len;
  return true;
}

char *buf_read_str(Buffer *buf, size_t len) {
  if (buf_remaining(buf) < len)
    return NULL;
  char *str = malloc(len + 1);
  if (!str)
    return NULL;
  memcpy(str, buf->data + buf->pos, len);
  str[len] = '\0';
  buf->pos += len;
  return str;
}

char *buf_read_cstr(Buffer *buf) {
  size_t start = buf->pos;
  while (buf->pos < buf->len && buf->data[buf->pos] != '\0') {
    buf->pos++;
  }
  if (buf->pos >= buf->len) {
    buf->pos = start;
    return NULL;
  }
  size_t len = buf->pos - start;
  buf->pos++; /* Skip null terminator */

  char *str = malloc(len + 1);
  if (!str)
    return NULL;
  memcpy(str, buf->data + start, len);
  str[len] = '\0';
  return str;
}

bool buf_skip(Buffer *buf, size_t count) {
  if (buf_remaining(buf) < count)
    return false;
  buf->pos += count;
  return true;
}

/* Peeking functions */

bool buf_peek_u8(const Buffer *buf, uint8_t *val) {
  if (buf_remaining(buf) < 1)
    return false;
  *val = buf->data[buf->pos];
  return true;
}

bool buf_peek_u16_be(const Buffer *buf, uint16_t *val) {
  if (buf_remaining(buf) < 2)
    return false;
  *val = ((uint16_t)buf->data[buf->pos] << 8) |
         ((uint16_t)buf->data[buf->pos + 1]);
  return true;
}

bool buf_peek_u32_be(const Buffer *buf, uint32_t *val) {
  if (buf_remaining(buf) < 4)
    return false;
  *val = ((uint32_t)buf->data[buf->pos] << 24) |
         ((uint32_t)buf->data[buf->pos + 1] << 16) |
         ((uint32_t)buf->data[buf->pos + 2] << 8) |
         ((uint32_t)buf->data[buf->pos + 3]);
  return true;
}

/* Position management */

size_t buf_remaining(const Buffer *buf) { return buf->len - buf->pos; }

size_t buf_tell(const Buffer *buf) { return buf->pos; }

bool buf_seek(Buffer *buf, size_t pos) {
  if (pos > buf->len)
    return false;
  buf->pos = pos;
  return true;
}

bool buf_rewind(Buffer *buf) {
  buf->pos = 0;
  return true;
}

const uint8_t *buf_ptr(const Buffer *buf) { return buf->data + buf->pos; }

const uint8_t *buf_data(const Buffer *buf) { return buf->data; }

/* Utility */

void buf_compact(Buffer *buf) {
  if (buf->pos > 0) {
    size_t remaining = buf_remaining(buf);
    memmove(buf->data, buf->data + buf->pos, remaining);
    buf->len = remaining;
    buf->pos = 0;
  }
}

Buffer *buf_slice(Buffer *buf, size_t len) {
  if (buf_remaining(buf) < len)
    return NULL;
  Buffer *slice = buf_new_from(buf->data + buf->pos, len);
  if (slice)
    buf->pos += len;
  return slice;
}

void buf_dump(Buffer *buf, const char *label) {
  fprintf(stderr, "%s: len=%zu cap=%zu pos=%zu\n", label ? label : "Buffer",
          buf->len, buf->cap, buf->pos);
  fprintf(stderr, "  ");
  for (size_t i = 0; i < buf->len && i < 64; i++) {
    fprintf(stderr, "%02x ", buf->data[i]);
    if ((i + 1) % 16 == 0)
      fprintf(stderr, "\n  ");
  }
  if (buf->len > 64)
    fprintf(stderr, "...");
  fprintf(stderr, "\n");
}
