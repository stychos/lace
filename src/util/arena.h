/*
 * lace - Database Viewer and Manager
 * Arena memory allocator
 */

#ifndef LACE_ARENA_H
#define LACE_ARENA_H

#include <stddef.h>
#include <stdbool.h>

/* Arena block */
typedef struct ArenaBlock {
    struct ArenaBlock *next;
    size_t             size;
    size_t             used;
    char               data[];
} ArenaBlock;

/* Arena allocator */
typedef struct {
    ArenaBlock *first;
    ArenaBlock *current;
    size_t      block_size;
    size_t      total_allocated;
    size_t      total_used;
} Arena;

/* Create a new arena with specified block size */
Arena *arena_new(size_t block_size);

/* Free all memory in the arena */
void arena_free(Arena *arena);

/* Reset arena (free all blocks except first, reset positions) */
void arena_reset(Arena *arena);

/* Allocate memory from arena */
void *arena_alloc(Arena *arena, size_t size);

/* Allocate zeroed memory from arena */
void *arena_calloc(Arena *arena, size_t count, size_t size);

/* Allocate memory with specific alignment */
void *arena_alloc_aligned(Arena *arena, size_t size, size_t alignment);

/* Duplicate a string into arena */
char *arena_strdup(Arena *arena, const char *s);

/* Duplicate a string with length into arena */
char *arena_strndup(Arena *arena, const char *s, size_t n);

/* Printf into arena-allocated string */
char *arena_printf(Arena *arena, const char *fmt, ...);

/* Get arena statistics */
size_t arena_total_allocated(Arena *arena);
size_t arena_total_used(Arena *arena);

/* Temporary arena scope (for nested allocations that can be freed together) */
typedef struct {
    Arena      *arena;
    ArenaBlock *saved_block;
    size_t      saved_used;
} ArenaScope;

ArenaScope arena_scope_begin(Arena *arena);
void arena_scope_end(ArenaScope *scope);

#endif /* LACE_ARENA_H */
