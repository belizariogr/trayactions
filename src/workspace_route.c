#include "workspace_route.h"

#include <string.h>

gint workspace_route_coord_to_index(guint32 coord0) {
    /* COSMIC advertises 1-based coordinates that match workspace names. */
    if (coord0 >= 1) {
        if (coord0 > (guint32)G_MAXINT) {
            return G_MAXINT;
        }
        return (gint)coord0;
    }
    return 1;
}

gint workspace_route_assignment_for_app(GPtrArray *assignments, const char *app_id) {
    if (!assignments || !app_id || !*app_id) {
        return 0;
    }
    for (guint i = 0; i < assignments->len; i++) {
        AppWorkspaceAssignment *assignment = g_ptr_array_index(assignments, i);
        if (assignment && assignment->app_id &&
            strcmp(assignment->app_id, app_id) == 0) {
            return assignment->workspace;
        }
    }
    return 0;
}

gint workspace_route_find_entry(
    const WorkspaceRouteEntry *entries,
    guint n_entries,
    gint workspace_number
) {
    if (!entries || n_entries == 0 || workspace_number < 1) {
        return -1;
    }

    char name_buf[16];
    g_snprintf(name_buf, sizeof(name_buf), "%d", workspace_number);

    for (guint i = 0; i < n_entries; i++) {
        if (entries[i].name && strcmp(entries[i].name, name_buf) == 0) {
            return (gint)i;
        }
    }
    for (guint i = 0; i < n_entries; i++) {
        if (entries[i].rank == workspace_number) {
            return (gint)i;
        }
    }
    for (guint i = 0; i < n_entries; i++) {
        if (entries[i].coord_index == workspace_number) {
            return (gint)i;
        }
    }
    return -1;
}

void workspace_route_assign_ranks(
    const gint *coord_indices,
    guint n_entries,
    gint *out_ranks
) {
    if (!out_ranks) {
        return;
    }
    if (!coord_indices || n_entries == 0) {
        return;
    }

    guint *order = g_new(guint, n_entries);
    for (guint i = 0; i < n_entries; i++) {
        order[i] = i;
        out_ranks[i] = 0;
    }

    for (guint i = 1; i < n_entries; i++) {
        guint key = order[i];
        gint key_coord = coord_indices[key];
        gint j = (gint)i - 1;
        while (j >= 0) {
            gint other = coord_indices[order[j]];
            gboolean key_known = key_coord > 0;
            gboolean other_known = other > 0;
            gboolean before =
                (key_known && other_known && key_coord < other) ||
                (key_known && !other_known) ||
                (!key_known && !other_known && key < order[j]);
            if (!before) {
                break;
            }
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }

    for (guint rank = 0; rank < n_entries; rank++) {
        out_ranks[order[rank]] = (gint)rank + 1;
    }
    g_free(order);
}

WorkspaceRouteAction workspace_route_decide(const WorkspaceRouteState *state) {
    if (!state) {
        return WORKSPACE_ROUTE_IGNORE;
    }
    if (!state->active || !state->initial_sync_done) {
        return WORKSPACE_ROUTE_WAIT;
    }
    if (!state->appeared_after_sync || state->handled) {
        return WORKSPACE_ROUTE_IGNORE;
    }
    if (!state->has_app_id || !state->has_cosmic_handle) {
        return WORKSPACE_ROUTE_WAIT;
    }
    if (!state->has_assignment) {
        return WORKSPACE_ROUTE_IGNORE;
    }
    if (!state->has_target_workspace || !state->has_output ||
        !state->has_mapped_workspace) {
        return WORKSPACE_ROUTE_WAIT;
    }
    return WORKSPACE_ROUTE_MOVE;
}
