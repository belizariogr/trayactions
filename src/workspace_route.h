#ifndef WORKSPACE_ROUTE_H
#define WORKSPACE_ROUTE_H

#include <glib.h>
#include "appdata.h"

/**
 * Return the configured 1-based workspace for @app_id, or 0 if none.
 */
gint workspace_route_assignment_for_app(GPtrArray *assignments, const char *app_id);

typedef struct {
    const char *name; /* may be NULL */
    gint rank;        /* 1-based position within a group; 0 if unknown */
    gint coord_index; /* user-facing index derived from coordinates; 0 if unknown */
} WorkspaceRouteEntry;

/**
 * Convert a compositor coordinate to a 1-based user workspace number.
 * COSMIC sends 1-based coordinates; other compositors may send 0-based.
 */
gint workspace_route_coord_to_index(guint32 coord0);

/**
 * Choose the best entry for a 1-based @workspace_number.
 * Preference: exact name ("2"), then rank, then coord_index.
 * Returns index into @entries, or -1.
 */
gint workspace_route_find_entry(
    const WorkspaceRouteEntry *entries,
    guint n_entries,
    gint workspace_number
);

/**
 * Assign 1-based ranks in-place for entries that share a group.
 * Sorts by coord_index when > 0, otherwise stable by original order.
 * Writes rank into a parallel array of gint (same length).
 */
void workspace_route_assign_ranks(
    const gint *coord_indices,
    guint n_entries,
    gint *out_ranks
);

typedef enum {
    WORKSPACE_ROUTE_WAIT = 0, /* try again when more events arrive */
    WORKSPACE_ROUTE_IGNORE,   /* permanently skip this toplevel */
    WORKSPACE_ROUTE_MOVE      /* send the move request now */
} WorkspaceRouteAction;

typedef struct {
    gboolean active;
    gboolean initial_sync_done;
    gboolean appeared_after_sync;
    gboolean handled;
    gboolean has_app_id;
    gboolean has_cosmic_handle;
    gboolean has_assignment;
    gboolean has_target_workspace;
    gboolean has_output;
    /* cosmic-comp ignores move_to_* until the window is on a workspace. */
    gboolean has_mapped_workspace;
} WorkspaceRouteState;

WorkspaceRouteAction workspace_route_decide(const WorkspaceRouteState *state);

#endif
