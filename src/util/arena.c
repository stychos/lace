/*
 * lace - Database Viewer and Manager
 * Arena memory allocator implementation
 */

#include "arena.h"
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_BLOCK_SIZE (64 * 1024) /* 64KB */
#define MIN_BLOCK_SIZE 4096

static ArenaBlock *arena_new_block(size_t size) {
  /* Check for overflow before allocation */
  if (size > SIZE_MAX - sizeof(ArenaBlock)) {
    return NULL;
  }
  ArenaBlock *block = malloc(sizeof(ArenaBlock) + size);
  if (!block)
    return NULL;

  block->next = NULL;
  block->size = size;
  block->used = 0;
  return block;
}

Arena *arena_new(size_t block_size) {
  Arena *arena = malloc(sizeof(Arena));
  if (!arena)
    return NULL;

  if (block_size < MIN_BLOCK_SIZE) {
    block_size = DEFAULT_BLOCK_SIZE;
  }

  arena->first = arena_new_block(block_size);
  if (!arena->first) {
    free(arena);
    return NULL;
  }

  arena->current = arena->first;
  arena->block_size = block_size;
  arena->total_allocated = block_size;
  arena->total_used = 0;

  return arena;
}

void arena_free(Arena *arena) {
  if (!arena)
    return;

  ArenaBlock *block = arena->first;
  while (block) {
    ArenaBlock *next = block->next;
    free(block);
    block = next;
  }

  free(arena);
}

void arena_reset(Arena *arena) {
  if (!arena || !arena->first)
    return;

  /* Free all blocks except first */
  ArenaBlock *block = arena->first->next;
  while (block) {
    ArenaBlock *next = block->next;
    arena->total_allocated -= block->size;
    free(block);
    block = next;
  }

  arena->first->next = NULL;
  arena->first->used = 0;
  arena->current = arena->first;
  arena->total_used = 0;
}

static size_t align_up(size_t size, size_t alignment) {
  /* Check for overflow: size + (alignment - 1) could wrap */
  if (alignment == 0)
    return size;
  /* Alignment must be a power of 2 for bitwise AND to work correctly */
  if ((alignment & (alignment - 1)) != 0)
    return SIZE_MAX; /* Invalid alignment - signal error */
  if (size > SIZE_MAX - (alignment - 1))
    return SIZE_MAX; /* Return max to signal overflow - caller should check */
  return (size + alignment - 1) & ~(alignment - 1);
}

void *arena_alloc_aligned(Arena *arena, size_t size, size_t alignment) {
  if (!arena || !arena->current || size == 0)
    return NULL;

  /* Find space in current block */
  ArenaBlock *block = arena->current;
  size_t aligned_offset = align_up(block->used, alignment);

  /* Check for overflow from align_up or in needed calculation */
  if (aligned_offset == SIZE_MAX || aligned_offset > SIZE_MAX - size)
    return NULL;
  size_t needed = aligned_offset + size;

  if (needed <= block->size) {
    void *ptr = block->data + aligned_offset;
    /* Calculate used bytes BEFORE updating block->used */
    arena->total_used += needed - block->used;
    block->used = needed;
    return ptr;
  }

  /* Need a new block */
  size_t new_block_size = arena->block_size;
  if (size > new_block_size) {
    /* Check for overflow in align_up */
    if (size > SIZE_MAX - MIN_BLOCK_SIZE + 1)
      return NULL;
    new_block_size = align_up(size, MIN_BLOCK_SIZE);
    /* Check if align_up returned SIZE_MAX (overflow) */
    if (new_block_size == SIZE_MAX)
      return NULL;
  }

  ArenaBlock *new_block = arena_new_block(new_block_size);
  if (!new_block)
    return NULL;

  block->next = new_block;
  arena->current = new_block;
  arena->total_allocated += new_block_size;

  aligned_offset = align_up(0, alignment);
  void *ptr = new_block->data + aligned_offset;
  new_block->used = aligned_offset + size;
  arena->total_used += new_block->used;

  return ptr;
}

void *arena_alloc(Arena *arena, size_t size) {
  return arena_alloc_aligned(arena, size, sizeof(void *));
}

void *arena_calloc(Arena *arena, size_t count, size_t size) {
  /* Check for multiplication overflow */
  if (count != 0 && size > SIZE_MAX / count) {
    return NULL;
  }
  size_t total = count * size;
  void *ptr = arena_alloc(arena, total);
  if (ptr) {
    memset(ptr, 0, total);
  }
  return ptr;
}

char *arena_strdup(Arena *arena, const char *s) {
  if (!s)
    return NULL;
  size_t len = strlen(s);
  /* Check for overflow before adding 1 for null terminator */
  if (len == SIZE_MAX)
    return NULL;
  char *dup = arena_alloc(arena, len + 1);
  if (dup) {
    memcpy(dup, s, len + 1);
  }
  return dup;
}

char *arena_strndup(Arena *arena, const char *s, size_t n) {
  if (!s)
    return NULL;
  /* Use bounded search for null terminator to avoid reading past n bytes */
  size_t len = 0;
  while (len < n && s[len] != '\0') {
    len++;
  }
  char *dup = arena_alloc(arena, len + 1);
  if (dup) {
    memcpy(dup, s, len);
    dup[len] = '\0';
  }
  return dup;
}

char *arena_printf(Arena *arena, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);

  va_list args_copy;
  va_copy(args_copy, args);
  int len = vsnprintf(NULL, 0, fmt, args_copy);
  va_end(args_copy);

  if (len < 0) {
    va_end(args);
    return NULL;
  }

  /* Check for overflow when adding 1 for null terminator */
  if (len > INT_MAX - 1) {
    va_end(args);
    return NULL;
  }

  char *buf = arena_alloc(arena, (size_t)len + 1);
  if (buf) {
    vsnprintf(buf, len + 1, fmt, args);
  }

  va_end(args);
  return buf;
}

size_t arena_total_allocated(Arena *arena) {
  return arena ? arena->total_allocated : 0;
}

size_t arena_total_used(Arena *arena) { return arena ? arena->total_used : 0; }

ArenaScope arena_scope_begin(Arena *arena) {
  if (!arena || !arena->current) {
    ArenaScope scope = {.arena = NULL, .saved_block = NULL, .saved_used = 0};
    return scope;
  }
  ArenaScope scope = {.arena = arena,
                      .saved_block = arena->current,
                      .saved_used = arena->current->used};
  return scope;
}

void arena_scope_end(ArenaScope *scope) {
  if (!scope || !scope->arena || !scope->saved_block)
    return;

  Arena *arena = scope->arena;

  /* Free blocks allocated after scope began */
  ArenaBlock *block = scope->saved_block->next;
  while (block) {
    ArenaBlock *next = block->next;
    arena->total_allocated -= block->size;
    free(block);
    block = next;
  }

  scope->saved_block->next = NULL;
  scope->saved_block->used = scope->saved_used;
  arena->current = scope->saved_block;

  /* Recalculate total_used by summing all remaining blocks */
  arena->total_used = 0;
  for (ArenaBlock *b = arena->first; b != NULL; b = b->next) {
    arena->total_used += b->used;
  }
}
