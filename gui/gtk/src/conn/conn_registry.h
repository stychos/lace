/*
 * Lace GTK frontend
 * The open database connections of the app and who holds them: one
 * LaceConnection per saved connection (or password-less connection string),
 * held by every page that shows it, in any window; the last release closes it
 *
 * (c) iloveyou, 2025. MIT License.
 * https://github.com/stychos/lace
 */

#ifndef LACE_GTK_CONN_REGISTRY_H
#define LACE_GTK_CONN_REGISTRY_H

#include "connection.h"

G_BEGIN_DECLS

typedef struct _LaceConnRegistry LaceConnRegistry;

LaceConnRegistry *lace_conn_registry_new(void);
/* Drops every reference (the connections are not closed here) */
void lace_conn_registry_free(LaceConnRegistry *r);

/* An open one: with a saved id the one opened from it whose string still
 * matches connstr (when given: an edited entry is connected anew); without,
 * one with no saved id and the same password-less string. NULL if none. */
LaceConnection *lace_conn_registry_find(LaceConnRegistry *r, const char *saved_id, const char *connstr);
/* A new connection joins, with no holder yet (the registry takes a ref) */
void lace_conn_registry_add(LaceConnRegistry *r, LaceConnection *c);
void lace_conn_registry_hold(LaceConnRegistry *r, LaceConnection *c);
/* The last release closes it (lace_connection_close) and drops it; TRUE then */
gboolean lace_conn_registry_release(LaceConnRegistry *r, LaceConnection *c);
guint lace_conn_registry_holders(LaceConnRegistry *r, LaceConnection *c);
guint lace_conn_registry_count(LaceConnRegistry *r);

G_END_DECLS

#endif /* LACE_GTK_CONN_REGISTRY_H */
