/*
 * Lace
 * Recent connections: the last URLs and files connected to that are not
 * saved connections (recent.json, passwords never stored)
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_CONFIG_RECENT_H
#define LACE_CONFIG_RECENT_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#define RECENT_FILE "recent.json"
#define RECENT_MAX 10

typedef struct {
  char *url;   /* Connection string without passwords */
  time_t used; /* Last connected */
} RecentEntry;

typedef struct {
  RecentEntry *items; /* Newest first */
  size_t count;
} RecentList;

/* Load recent.json (a missing or unreadable file is an empty list) */
void recent_load(RecentList *list);

/* Write the list; false (and *error) if it could not be written */
bool recent_save(const RecentList *list, char **error);

/* The URL without its passwords (NULL if it does not parse); caller frees */
char *recent_strip_passwords(const char *url);

/* Put url (passwords removed) first, dropping an older copy and anything
 * past RECENT_MAX. False if the URL does not parse. */
bool recent_add(RecentList *list, const char *url);

/* Forget one entry */
void recent_remove(RecentList *list, size_t index);

void recent_free(RecentList *list);

#endif /* LACE_CONFIG_RECENT_H */
