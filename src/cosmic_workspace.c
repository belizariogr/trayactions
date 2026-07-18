#include "cosmic_workspace.h"

#include <glib-unix.h>
#include <string.h>
#include <wayland-client.h>

#include "generated/cosmic-toplevel-info-unstable-v1-client.h"
#include "generated/cosmic-toplevel-management-unstable-v1-client.h"
#include "generated/cosmic-workspace-unstable-v1-client.h"
#include "generated/ext-foreign-toplevel-list-v1-client.h"
#include "generated/ext-workspace-v1-client.h"
#include "workspace_route.h"

typedef struct {
    struct wl_output *output;
    uint32_t registry_name;
} OutputInfo;

typedef struct WorkspaceGroupInfo WorkspaceGroupInfo;

typedef struct {
    struct ext_workspace_handle_v1 *ext_handle;
    struct zcosmic_workspace_handle_v1 *cosmic_handle;
    WorkspaceGroupInfo *group;
    char *name;
    gint index;
    uint32_t coord0;
    gboolean has_coord;
} WorkspaceInfo;

struct WorkspaceGroupInfo {
    struct ext_workspace_group_handle_v1 *ext_group;
    struct zcosmic_workspace_group_handle_v1 *cosmic_group;
    GPtrArray *outputs;
    GPtrArray *workspaces;
};

typedef struct {
    struct ext_foreign_toplevel_handle_v1 *foreign;
    struct zcosmic_toplevel_handle_v1 *cosmic;
    char *app_id;
    struct wl_output *output;
    struct ext_workspace_handle_v1 *current_ext_workspace;
    struct zcosmic_workspace_handle_v1 *current_cosmic_workspace;
    gboolean appeared_after_sync;
    gboolean mapped_to_workspace;
    gboolean handled;
    guint retry_source;
    guint retry_count;
} ToplevelInfo;

typedef struct {
    uint32_t name;
    uint32_t version;
} GlobalOffer;

typedef struct {
    AppData *app_data;
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_callback *sync_callback;
    guint fd_source;

    GlobalOffer foreign_list_offer;
    GlobalOffer toplevel_info_offer;
    GlobalOffer toplevel_manager_offer;
    GlobalOffer ext_workspace_offer;
    GlobalOffer cosmic_workspace_offer;
    GArray *output_offers;

    struct ext_foreign_toplevel_list_v1 *foreign_list;
    struct zcosmic_toplevel_info_v1 *toplevel_info;
    struct zcosmic_toplevel_manager_v1 *toplevel_manager;
    struct ext_workspace_manager_v1 *ext_workspace_manager;
    struct zcosmic_workspace_manager_v1 *cosmic_workspace_manager;

    GPtrArray *outputs;
    GPtrArray *workspaces;
    GPtrArray *workspace_groups;
    GPtrArray *toplevels;

    gboolean use_ext_toplevels;
    gboolean initial_sync_done;
    gboolean active;
    gint next_workspace_index;
} CosmicState;

static CosmicState *g_cosmic = NULL;
static gboolean g_try_move_reentrant = FALSE;
static gboolean g_try_move_queued = FALSE;

static void try_move_toplevel(CosmicState *state, ToplevelInfo *toplevel);
static void try_move_pending_toplevels(CosmicState *state);
static void scrub_toplevel_workspace_refs(WorkspaceInfo *workspace);
static void scrub_output_refs(CosmicState *state, struct wl_output *output);

static struct wl_output *first_output(CosmicState *state) {
    if (!state || !state->outputs || state->outputs->len == 0) {
        return NULL;
    }
    OutputInfo *info = g_ptr_array_index(state->outputs, 0);
    return info ? info->output : NULL;
}

static struct wl_output *output_for_workspace(CosmicState *state, WorkspaceInfo *workspace) {
    if (workspace && workspace->group && workspace->group->outputs->len > 0) {
        return g_ptr_array_index(workspace->group->outputs, 0);
    }
    return first_output(state);
}

static void reindex_group(WorkspaceGroupInfo *group) {
    if (!group || !group->workspaces || group->workspaces->len == 0) {
        return;
    }

    guint n = group->workspaces->len;
    gint *coords = g_new0(gint, n);
    gint *ranks = g_new0(gint, n);
    for (guint i = 0; i < n; i++) {
        WorkspaceInfo *info = g_ptr_array_index(group->workspaces, i);
        coords[i] = (info && info->has_coord)
                        ? workspace_route_coord_to_index(info->coord0)
                        : 0;
    }
    workspace_route_assign_ranks(coords, n, ranks);
    for (guint i = 0; i < n; i++) {
        WorkspaceInfo *info = g_ptr_array_index(group->workspaces, i);
        if (info) {
            info->index = ranks[i];
        }
    }
    g_free(coords);
    g_free(ranks);
}

static void reindex_all_groups(CosmicState *state) {
    if (!state || !state->workspace_groups) {
        return;
    }
    for (guint i = 0; i < state->workspace_groups->len; i++) {
        reindex_group(g_ptr_array_index(state->workspace_groups, i));
    }

    /* Workspaces not yet linked to a group keep sequential / coord indices. */
    for (guint i = 0; i < state->workspaces->len; i++) {
        WorkspaceInfo *info = g_ptr_array_index(state->workspaces, i);
        if (!info || info->group) {
            continue;
        }
        if (info->has_coord) {
            info->index = workspace_route_coord_to_index(info->coord0);
        }
    }
}

static WorkspaceInfo *find_target_workspace(
    CosmicState *state,
    gint workspace_number,
    struct wl_output *preferred_output
) {
    if (!state || workspace_number < 1 || state->workspaces->len == 0) {
        return NULL;
    }

    WorkspaceRouteEntry *entries = g_new0(WorkspaceRouteEntry, state->workspaces->len);
    WorkspaceInfo **infos = g_new(WorkspaceInfo *, state->workspaces->len);
    guint n = 0;

    for (guint i = 0; i < state->workspaces->len; i++) {
        WorkspaceInfo *info = g_ptr_array_index(state->workspaces, i);
        if (!info) {
            continue;
        }
        if (preferred_output && info->group) {
            gboolean on_output = FALSE;
            for (guint o = 0; o < info->group->outputs->len; o++) {
                if (g_ptr_array_index(info->group->outputs, o) == preferred_output) {
                    on_output = TRUE;
                    break;
                }
            }
            if (!on_output) {
                continue;
            }
        }
        infos[n] = info;
        entries[n].name = info->name;
        entries[n].rank = info->index;
        entries[n].coord_index =
            info->has_coord ? workspace_route_coord_to_index(info->coord0) : 0;
        n++;
    }

    gint found = workspace_route_find_entry(entries, n, workspace_number);
    WorkspaceInfo *result = found >= 0 ? infos[found] : NULL;

    /* Retry without output filter if preferred output yielded nothing. */
    if (!result && preferred_output) {
        g_free(entries);
        g_free(infos);
        return find_target_workspace(state, workspace_number, NULL);
    }

    g_free(entries);
    g_free(infos);
    return result;
}

static void activate_workspace(CosmicState *state, WorkspaceInfo *workspace) {
    if (!state || !workspace) {
        return;
    }

    if (workspace->ext_handle && state->ext_workspace_manager) {
        ext_workspace_handle_v1_activate(workspace->ext_handle);
        ext_workspace_manager_v1_commit(state->ext_workspace_manager);
        return;
    }

    if (workspace->cosmic_handle && state->cosmic_workspace_manager) {
        zcosmic_workspace_handle_v1_activate(workspace->cosmic_handle);
        zcosmic_workspace_manager_v1_commit(state->cosmic_workspace_manager);
    }
}

static gboolean toplevel_on_workspace(ToplevelInfo *toplevel, WorkspaceInfo *workspace) {
    if (!toplevel || !workspace) {
        return FALSE;
    }
    if (workspace->ext_handle &&
        toplevel->current_ext_workspace == workspace->ext_handle) {
        return TRUE;
    }
    if (workspace->cosmic_handle &&
        toplevel->current_cosmic_workspace == workspace->cosmic_handle) {
        return TRUE;
    }
    return FALSE;
}

static gboolean retry_move_toplevel(gpointer user_data) {
    ToplevelInfo *toplevel = user_data;
    if (!g_cosmic || !toplevel) {
        return G_SOURCE_REMOVE;
    }
    toplevel->retry_source = 0;
    try_move_toplevel(g_cosmic, toplevel);
    return G_SOURCE_REMOVE;
}

static void schedule_move_retry(ToplevelInfo *toplevel, guint delay_ms) {
    if (!toplevel || toplevel->handled || toplevel->retry_source) {
        return;
    }
    if (toplevel->retry_count >= 8) {
        toplevel->handled = TRUE;
        g_warning(
            "Giving up moving %s after repeated attempts.",
            toplevel->app_id ? toplevel->app_id : "?"
        );
        return;
    }
    toplevel->retry_count++;
    toplevel->retry_source = g_timeout_add(delay_ms, retry_move_toplevel, toplevel);
}

static void try_move_toplevel(CosmicState *state, ToplevelInfo *toplevel) {
    if (!state || !toplevel) {
        return;
    }

    /* Nested wl_display_dispatch (e.g. from list_open_apps) can re-enter while
     * Preferences/config mutates app_workspaces — defer instead of nesting. */
    if (g_try_move_reentrant) {
        g_try_move_queued = TRUE;
        return;
    }
    g_try_move_reentrant = TRUE;

    GPtrArray *assignments =
        state->app_data ? state->app_data->app_workspaces : NULL;
    gint workspace_number =
        workspace_route_assignment_for_app(assignments, toplevel->app_id);

    WorkspaceInfo *workspace = NULL;
    struct wl_output *output = NULL;
    if (workspace_number >= 1) {
        workspace = find_target_workspace(state, workspace_number, toplevel->output);
        output = toplevel->output;
        if (!output && workspace) {
            output = output_for_workspace(state, workspace);
        }
        if (!output) {
            output = first_output(state);
        }
    }

    WorkspaceRouteState route = {
        .active = state->active,
        .initial_sync_done = state->initial_sync_done,
        .appeared_after_sync = toplevel->appeared_after_sync,
        .handled = toplevel->handled,
        .has_app_id = toplevel->app_id && *toplevel->app_id,
        .has_cosmic_handle = toplevel->cosmic != NULL,
        .has_assignment = workspace_number >= 1,
        .has_target_workspace = workspace != NULL && state->toplevel_manager != NULL,
        .has_output = output != NULL,
        .has_mapped_workspace = toplevel->mapped_to_workspace,
    };

    WorkspaceRouteAction action = workspace_route_decide(&route);
    if (action == WORKSPACE_ROUTE_WAIT) {
        goto out;
    }
    if (action == WORKSPACE_ROUTE_IGNORE) {
        toplevel->handled = TRUE;
        if (toplevel->retry_source) {
            g_source_remove(toplevel->retry_source);
            toplevel->retry_source = 0;
        }
        goto out;
    }

    if (toplevel_on_workspace(toplevel, workspace)) {
        activate_workspace(state, workspace);
        toplevel->handled = TRUE;
        if (toplevel->retry_source) {
            g_source_remove(toplevel->retry_source);
            toplevel->retry_source = 0;
        }
        wl_display_flush(state->display);
        g_message(
            "Window %s already on workspace %d; switched to it.",
            toplevel->app_id,
            workspace_number
        );
        goto out;
    }

    gboolean moved = FALSE;
    if (workspace->ext_handle &&
        zcosmic_toplevel_manager_v1_get_version(state->toplevel_manager) >= 4) {
        zcosmic_toplevel_manager_v1_move_to_ext_workspace(
            state->toplevel_manager,
            toplevel->cosmic,
            workspace->ext_handle,
            output
        );
        moved = TRUE;
    } else if (workspace->cosmic_handle &&
               zcosmic_toplevel_manager_v1_get_version(state->toplevel_manager) >= 2) {
        zcosmic_toplevel_manager_v1_move_to_workspace(
            state->toplevel_manager,
            toplevel->cosmic,
            workspace->cosmic_handle,
            output
        );
        moved = TRUE;
    }

    if (!moved) {
        schedule_move_retry(toplevel, 200);
        goto out;
    }

    activate_workspace(state, workspace);
    wl_display_flush(state->display);
    g_message(
        "Requested move of %s to workspace %d and activated it.",
        toplevel->app_id,
        workspace_number
    );
    /* cosmic-comp may ignore a too-early move; retry until we observe the
     * toplevel enter the target workspace. */
    schedule_move_retry(toplevel, 150);

out:
    g_try_move_reentrant = FALSE;
    if (g_try_move_queued) {
        g_try_move_queued = FALSE;
        try_move_pending_toplevels(state);
    }
}

static void try_move_pending_toplevels(CosmicState *state) {
    if (!state || !state->toplevels) {
        return;
    }
    for (guint i = 0; i < state->toplevels->len; i++) {
        try_move_toplevel(state, g_ptr_array_index(state->toplevels, i));
    }
}

static void output_info_free(gpointer pointer) {
    OutputInfo *info = pointer;
    if (!info) {
        return;
    }
    if (info->output) {
        wl_output_destroy(info->output);
    }
    g_free(info);
}

static void scrub_toplevel_workspace_refs(WorkspaceInfo *workspace) {
    if (!g_cosmic || !g_cosmic->toplevels || !workspace) {
        return;
    }
    for (guint i = 0; i < g_cosmic->toplevels->len; i++) {
        ToplevelInfo *toplevel = g_ptr_array_index(g_cosmic->toplevels, i);
        if (!toplevel) {
            continue;
        }
        if (workspace->ext_handle &&
            toplevel->current_ext_workspace == workspace->ext_handle) {
            toplevel->current_ext_workspace = NULL;
            toplevel->mapped_to_workspace = FALSE;
        }
        if (workspace->cosmic_handle &&
            toplevel->current_cosmic_workspace == workspace->cosmic_handle) {
            toplevel->current_cosmic_workspace = NULL;
            toplevel->mapped_to_workspace = FALSE;
        }
    }
}

static void scrub_output_refs(CosmicState *state, struct wl_output *output) {
    if (!state || !output) {
        return;
    }
    if (state->workspace_groups) {
        for (guint i = 0; i < state->workspace_groups->len; i++) {
            WorkspaceGroupInfo *group = g_ptr_array_index(state->workspace_groups, i);
            if (group && group->outputs) {
                g_ptr_array_remove(group->outputs, output);
            }
        }
    }
    if (state->toplevels) {
        for (guint i = 0; i < state->toplevels->len; i++) {
            ToplevelInfo *toplevel = g_ptr_array_index(state->toplevels, i);
            if (toplevel && toplevel->output == output) {
                toplevel->output = NULL;
            }
        }
    }
}

static void workspace_group_info_free(gpointer pointer) {
    WorkspaceGroupInfo *group = pointer;
    if (!group) {
        return;
    }
    if (group->workspaces) {
        for (guint i = 0; i < group->workspaces->len; i++) {
            WorkspaceInfo *info = g_ptr_array_index(group->workspaces, i);
            if (info && info->group == group) {
                info->group = NULL;
            }
        }
        g_ptr_array_unref(group->workspaces);
    }
    g_clear_pointer(&group->outputs, g_ptr_array_unref);
    if (group->ext_group) {
        ext_workspace_group_handle_v1_destroy(group->ext_group);
    }
    if (group->cosmic_group) {
        zcosmic_workspace_group_handle_v1_destroy(group->cosmic_group);
    }
    g_free(group);
}

static void workspace_info_free(gpointer pointer) {
    WorkspaceInfo *info = pointer;
    if (!info) {
        return;
    }
    scrub_toplevel_workspace_refs(info);
    if (info->group && info->group->workspaces) {
        g_ptr_array_remove(info->group->workspaces, info);
    }
    if (info->ext_handle) {
        ext_workspace_handle_v1_destroy(info->ext_handle);
    }
    if (info->cosmic_handle) {
        zcosmic_workspace_handle_v1_destroy(info->cosmic_handle);
    }
    g_free(info->name);
    g_free(info);
}

static void toplevel_info_free(gpointer pointer) {
    ToplevelInfo *info = pointer;
    if (!info) {
        return;
    }
    if (info->retry_source) {
        g_source_remove(info->retry_source);
        info->retry_source = 0;
    }
    if (info->cosmic) {
        zcosmic_toplevel_handle_v1_destroy(info->cosmic);
    }
    if (info->foreign) {
        ext_foreign_toplevel_handle_v1_destroy(info->foreign);
    }
    g_free(info->app_id);
    g_free(info);
}

static void assign_workspace_index(CosmicState *state, WorkspaceInfo *info) {
    if (info->has_coord) {
        info->index = workspace_route_coord_to_index(info->coord0);
        return;
    }
    info->index = state->next_workspace_index++;
}

static WorkspaceInfo *workspace_by_ext_handle(
    CosmicState *state,
    struct ext_workspace_handle_v1 *handle
) {
    if (!state || !handle) {
        return NULL;
    }
    for (guint i = 0; i < state->workspaces->len; i++) {
        WorkspaceInfo *info = g_ptr_array_index(state->workspaces, i);
        if (info && info->ext_handle == handle) {
            return info;
        }
    }
    return NULL;
}

static WorkspaceInfo *workspace_by_cosmic_handle(
    CosmicState *state,
    struct zcosmic_workspace_handle_v1 *handle
) {
    if (!state || !handle) {
        return NULL;
    }
    for (guint i = 0; i < state->workspaces->len; i++) {
        WorkspaceInfo *info = g_ptr_array_index(state->workspaces, i);
        if (info && info->cosmic_handle == handle) {
            return info;
        }
    }
    return NULL;
}

static void link_workspace_to_group(WorkspaceGroupInfo *group, WorkspaceInfo *info) {
    if (!group || !info) {
        return;
    }
    if (info->group == group) {
        return;
    }
    if (info->group && info->group->workspaces) {
        g_ptr_array_remove(info->group->workspaces, info);
    }
    info->group = group;
    g_ptr_array_add(group->workspaces, info);
    reindex_group(group);
}

/* --- ext workspace handle --- */

static void ext_workspace_handle_id(
    void *data,
    struct ext_workspace_handle_v1 *handle,
    const char *id
) {
    (void)data;
    (void)handle;
    (void)id;
}

static void ext_workspace_handle_name(
    void *data,
    struct ext_workspace_handle_v1 *handle,
    const char *name
) {
    WorkspaceInfo *info = data;
    (void)handle;
    if (!info) {
        return;
    }
    g_free(info->name);
    info->name = g_strdup(name ? name : "");
}

static void ext_workspace_handle_coordinates(
    void *data,
    struct ext_workspace_handle_v1 *handle,
    struct wl_array *coordinates
) {
    WorkspaceInfo *info = data;
    (void)handle;
    if (!info || !coordinates || coordinates->size < sizeof(uint32_t)) {
        return;
    }
    uint32_t *values = coordinates->data;
    info->coord0 = values[0];
    info->has_coord = TRUE;
    if (info->group) {
        reindex_group(info->group);
    } else if (g_cosmic) {
        assign_workspace_index(g_cosmic, info);
    }
}

static void ext_workspace_handle_state(
    void *data,
    struct ext_workspace_handle_v1 *handle,
    uint32_t state
) {
    (void)data;
    (void)handle;
    (void)state;
}

static void ext_workspace_handle_capabilities(
    void *data,
    struct ext_workspace_handle_v1 *handle,
    uint32_t capabilities
) {
    (void)data;
    (void)handle;
    (void)capabilities;
}

static void ext_workspace_handle_removed(
    void *data,
    struct ext_workspace_handle_v1 *handle
) {
    CosmicState *state = g_cosmic;
    WorkspaceInfo *info = data;
    (void)handle;
    if (!state || !info) {
        return;
    }
    g_ptr_array_remove(state->workspaces, info);
}

static const struct ext_workspace_handle_v1_listener ext_workspace_handle_listener = {
    .id = ext_workspace_handle_id,
    .name = ext_workspace_handle_name,
    .coordinates = ext_workspace_handle_coordinates,
    .state = ext_workspace_handle_state,
    .capabilities = ext_workspace_handle_capabilities,
    .removed = ext_workspace_handle_removed,
};

static void ext_workspace_group_capabilities(
    void *data,
    struct ext_workspace_group_handle_v1 *group,
    uint32_t capabilities
) {
    (void)data;
    (void)group;
    (void)capabilities;
}

static void ext_workspace_group_output_enter(
    void *data,
    struct ext_workspace_group_handle_v1 *group_handle,
    struct wl_output *output
) {
    WorkspaceGroupInfo *group = data;
    (void)group_handle;
    if (!group || !output) {
        return;
    }
    for (guint i = 0; i < group->outputs->len; i++) {
        if (g_ptr_array_index(group->outputs, i) == output) {
            return;
        }
    }
    g_ptr_array_add(group->outputs, output);
}

static void ext_workspace_group_output_leave(
    void *data,
    struct ext_workspace_group_handle_v1 *group_handle,
    struct wl_output *output
) {
    WorkspaceGroupInfo *group = data;
    (void)group_handle;
    if (!group || !output) {
        return;
    }
    g_ptr_array_remove(group->outputs, output);
}

static void ext_workspace_group_workspace_enter(
    void *data,
    struct ext_workspace_group_handle_v1 *group_handle,
    struct ext_workspace_handle_v1 *workspace
) {
    WorkspaceGroupInfo *group = data;
    (void)group_handle;
    if (!group || !g_cosmic) {
        return;
    }
    WorkspaceInfo *info = workspace_by_ext_handle(g_cosmic, workspace);
    link_workspace_to_group(group, info);
}

static void ext_workspace_group_workspace_leave(
    void *data,
    struct ext_workspace_group_handle_v1 *group_handle,
    struct ext_workspace_handle_v1 *workspace
) {
    WorkspaceGroupInfo *group = data;
    (void)group_handle;
    if (!group || !g_cosmic) {
        return;
    }
    WorkspaceInfo *info = workspace_by_ext_handle(g_cosmic, workspace);
    if (!info || info->group != group) {
        return;
    }
    g_ptr_array_remove(group->workspaces, info);
    info->group = NULL;
    reindex_group(group);
}

static void ext_workspace_group_removed(
    void *data,
    struct ext_workspace_group_handle_v1 *group_handle
) {
    CosmicState *state = g_cosmic;
    WorkspaceGroupInfo *group = data;
    (void)group_handle;
    if (!state || !group) {
        return;
    }
    g_ptr_array_remove(state->workspace_groups, group);
}

static const struct ext_workspace_group_handle_v1_listener ext_workspace_group_listener = {
    .capabilities = ext_workspace_group_capabilities,
    .output_enter = ext_workspace_group_output_enter,
    .output_leave = ext_workspace_group_output_leave,
    .workspace_enter = ext_workspace_group_workspace_enter,
    .workspace_leave = ext_workspace_group_workspace_leave,
    .removed = ext_workspace_group_removed,
};

static void ext_workspace_manager_workspace_group(
    void *data,
    struct ext_workspace_manager_v1 *manager,
    struct ext_workspace_group_handle_v1 *workspace_group
) {
    CosmicState *state = data;
    (void)manager;
    WorkspaceGroupInfo *group = g_new0(WorkspaceGroupInfo, 1);
    group->ext_group = workspace_group;
    group->outputs = g_ptr_array_new();
    group->workspaces = g_ptr_array_new();
    ext_workspace_group_handle_v1_add_listener(
        workspace_group,
        &ext_workspace_group_listener,
        group
    );
    g_ptr_array_add(state->workspace_groups, group);
}

static void ext_workspace_manager_workspace(
    void *data,
    struct ext_workspace_manager_v1 *manager,
    struct ext_workspace_handle_v1 *workspace
) {
    CosmicState *state = data;
    (void)manager;
    WorkspaceInfo *info = g_new0(WorkspaceInfo, 1);
    info->ext_handle = workspace;
    info->index = 0;
    ext_workspace_handle_v1_add_listener(workspace, &ext_workspace_handle_listener, info);
    assign_workspace_index(state, info);
    g_ptr_array_add(state->workspaces, info);
}

static void ext_workspace_manager_done(
    void *data,
    struct ext_workspace_manager_v1 *manager
) {
    CosmicState *state = data;
    (void)manager;
    reindex_all_groups(state);
    try_move_pending_toplevels(state);
}

static void ext_workspace_manager_finished(
    void *data,
    struct ext_workspace_manager_v1 *manager
) {
    CosmicState *state = data;
    if (state && state->ext_workspace_manager == manager) {
        ext_workspace_manager_v1_destroy(manager);
        state->ext_workspace_manager = NULL;
    }
}

static const struct ext_workspace_manager_v1_listener ext_workspace_manager_listener = {
    .workspace_group = ext_workspace_manager_workspace_group,
    .workspace = ext_workspace_manager_workspace,
    .done = ext_workspace_manager_done,
    .finished = ext_workspace_manager_finished,
};

/* --- legacy cosmic workspace --- */

static void cosmic_workspace_handle_name(
    void *data,
    struct zcosmic_workspace_handle_v1 *handle,
    const char *name
) {
    WorkspaceInfo *info = data;
    (void)handle;
    if (!info) {
        return;
    }
    g_free(info->name);
    info->name = g_strdup(name ? name : "");
}

static void cosmic_workspace_handle_coordinates(
    void *data,
    struct zcosmic_workspace_handle_v1 *handle,
    struct wl_array *coordinates
) {
    WorkspaceInfo *info = data;
    (void)handle;
    if (!info || !coordinates || coordinates->size < sizeof(uint32_t)) {
        return;
    }
    uint32_t *values = coordinates->data;
    info->coord0 = values[0];
    info->has_coord = TRUE;
    if (info->group) {
        reindex_group(info->group);
    } else if (g_cosmic) {
        assign_workspace_index(g_cosmic, info);
    }
}

static void cosmic_workspace_handle_state(
    void *data,
    struct zcosmic_workspace_handle_v1 *handle,
    struct wl_array *state
) {
    (void)data;
    (void)handle;
    (void)state;
}

static void cosmic_workspace_handle_capabilities(
    void *data,
    struct zcosmic_workspace_handle_v1 *handle,
    struct wl_array *capabilities
) {
    (void)data;
    (void)handle;
    (void)capabilities;
}

static void cosmic_workspace_handle_remove(
    void *data,
    struct zcosmic_workspace_handle_v1 *handle
) {
    CosmicState *state = g_cosmic;
    WorkspaceInfo *info = data;
    (void)handle;
    if (!state || !info) {
        return;
    }
    g_ptr_array_remove(state->workspaces, info);
}

static void cosmic_workspace_handle_tiling_state(
    void *data,
    struct zcosmic_workspace_handle_v1 *handle,
    uint32_t tiling_state
) {
    (void)data;
    (void)handle;
    (void)tiling_state;
}

static const struct zcosmic_workspace_handle_v1_listener cosmic_workspace_handle_listener = {
    .name = cosmic_workspace_handle_name,
    .coordinates = cosmic_workspace_handle_coordinates,
    .state = cosmic_workspace_handle_state,
    .capabilities = cosmic_workspace_handle_capabilities,
    .remove = cosmic_workspace_handle_remove,
    .tiling_state = cosmic_workspace_handle_tiling_state,
};

static void cosmic_workspace_group_capabilities(
    void *data,
    struct zcosmic_workspace_group_handle_v1 *group,
    struct wl_array *capabilities
) {
    (void)data;
    (void)group;
    (void)capabilities;
}

static void cosmic_workspace_group_output_enter(
    void *data,
    struct zcosmic_workspace_group_handle_v1 *group_handle,
    struct wl_output *output
) {
    WorkspaceGroupInfo *group = data;
    (void)group_handle;
    if (!group || !output) {
        return;
    }
    for (guint i = 0; i < group->outputs->len; i++) {
        if (g_ptr_array_index(group->outputs, i) == output) {
            return;
        }
    }
    g_ptr_array_add(group->outputs, output);
}

static void cosmic_workspace_group_output_leave(
    void *data,
    struct zcosmic_workspace_group_handle_v1 *group_handle,
    struct wl_output *output
) {
    WorkspaceGroupInfo *group = data;
    (void)group_handle;
    if (!group || !output) {
        return;
    }
    g_ptr_array_remove(group->outputs, output);
}

static void cosmic_workspace_group_workspace(
    void *data,
    struct zcosmic_workspace_group_handle_v1 *group_handle,
    struct zcosmic_workspace_handle_v1 *workspace
) {
    WorkspaceGroupInfo *group = data;
    CosmicState *state = g_cosmic;
    (void)group_handle;
    if (!group || !state) {
        return;
    }

    WorkspaceInfo *info = workspace_by_cosmic_handle(state, workspace);
    if (!info) {
        info = g_new0(WorkspaceInfo, 1);
        info->cosmic_handle = workspace;
        zcosmic_workspace_handle_v1_add_listener(
            workspace,
            &cosmic_workspace_handle_listener,
            info
        );
        assign_workspace_index(state, info);
        g_ptr_array_add(state->workspaces, info);
    }
    link_workspace_to_group(group, info);
}

static void cosmic_workspace_group_remove(
    void *data,
    struct zcosmic_workspace_group_handle_v1 *group_handle
) {
    CosmicState *state = g_cosmic;
    WorkspaceGroupInfo *group = data;
    (void)group_handle;
    if (!state || !group) {
        return;
    }
    g_ptr_array_remove(state->workspace_groups, group);
}

static const struct zcosmic_workspace_group_handle_v1_listener cosmic_workspace_group_listener = {
    .capabilities = cosmic_workspace_group_capabilities,
    .output_enter = cosmic_workspace_group_output_enter,
    .output_leave = cosmic_workspace_group_output_leave,
    .workspace = cosmic_workspace_group_workspace,
    .remove = cosmic_workspace_group_remove,
};

static void cosmic_workspace_manager_workspace_group(
    void *data,
    struct zcosmic_workspace_manager_v1 *manager,
    struct zcosmic_workspace_group_handle_v1 *workspace_group
) {
    CosmicState *state = data;
    (void)manager;
    WorkspaceGroupInfo *group = g_new0(WorkspaceGroupInfo, 1);
    group->cosmic_group = workspace_group;
    group->outputs = g_ptr_array_new();
    group->workspaces = g_ptr_array_new();
    zcosmic_workspace_group_handle_v1_add_listener(
        workspace_group,
        &cosmic_workspace_group_listener,
        group
    );
    g_ptr_array_add(state->workspace_groups, group);
}

static void cosmic_workspace_manager_done(
    void *data,
    struct zcosmic_workspace_manager_v1 *manager
) {
    CosmicState *state = data;
    (void)manager;
    reindex_all_groups(state);
    try_move_pending_toplevels(state);
}

static void cosmic_workspace_manager_finished(
    void *data,
    struct zcosmic_workspace_manager_v1 *manager
) {
    CosmicState *state = data;
    if (state && state->cosmic_workspace_manager == manager) {
        zcosmic_workspace_manager_v1_destroy(manager);
        state->cosmic_workspace_manager = NULL;
    }
}

static const struct zcosmic_workspace_manager_v1_listener cosmic_workspace_manager_listener = {
    .workspace_group = cosmic_workspace_manager_workspace_group,
    .done = cosmic_workspace_manager_done,
    .finished = cosmic_workspace_manager_finished,
};

/* --- cosmic toplevel handle --- */

static void cosmic_toplevel_closed(
    void *data,
    struct zcosmic_toplevel_handle_v1 *handle
) {
    CosmicState *state = g_cosmic;
    ToplevelInfo *info = data;
    (void)handle;
    if (!state || !info) {
        return;
    }
    g_ptr_array_remove(state->toplevels, info);
}

static void cosmic_toplevel_done(
    void *data,
    struct zcosmic_toplevel_handle_v1 *handle
) {
    (void)handle;
    if (g_cosmic) {
        try_move_toplevel(g_cosmic, data);
    }
}

static void cosmic_toplevel_title(
    void *data,
    struct zcosmic_toplevel_handle_v1 *handle,
    const char *title
) {
    (void)data;
    (void)handle;
    (void)title;
}

static void cosmic_toplevel_app_id(
    void *data,
    struct zcosmic_toplevel_handle_v1 *handle,
    const char *app_id
) {
    ToplevelInfo *info = data;
    (void)handle;
    if (!info) {
        return;
    }
    g_free(info->app_id);
    info->app_id = g_strdup(app_id ? app_id : "");
}

static void cosmic_toplevel_output_enter(
    void *data,
    struct zcosmic_toplevel_handle_v1 *handle,
    struct wl_output *output
) {
    ToplevelInfo *info = data;
    (void)handle;
    if (info) {
        info->output = output;
    }
    if (g_cosmic) {
        try_move_toplevel(g_cosmic, info);
    }
}

static void cosmic_toplevel_output_leave(
    void *data,
    struct zcosmic_toplevel_handle_v1 *handle,
    struct wl_output *output
) {
    ToplevelInfo *info = data;
    (void)handle;
    if (info && info->output == output) {
        info->output = NULL;
    }
}

static void cosmic_toplevel_workspace_enter(
    void *data,
    struct zcosmic_toplevel_handle_v1 *handle,
    struct zcosmic_workspace_handle_v1 *workspace
) {
    ToplevelInfo *info = data;
    (void)handle;
    if (info) {
        info->mapped_to_workspace = TRUE;
        info->current_cosmic_workspace = workspace;
    }
    if (g_cosmic) {
        try_move_toplevel(g_cosmic, info);
    }
}

static void cosmic_toplevel_workspace_leave(
    void *data,
    struct zcosmic_toplevel_handle_v1 *handle,
    struct zcosmic_workspace_handle_v1 *workspace
) {
    ToplevelInfo *info = data;
    (void)handle;
    if (info && info->current_cosmic_workspace == workspace) {
        info->current_cosmic_workspace = NULL;
    }
}

static void cosmic_toplevel_state(
    void *data,
    struct zcosmic_toplevel_handle_v1 *handle,
    struct wl_array *state
) {
    (void)data;
    (void)handle;
    (void)state;
}

static void cosmic_toplevel_geometry(
    void *data,
    struct zcosmic_toplevel_handle_v1 *handle,
    struct wl_output *output,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height
) {
    (void)data;
    (void)handle;
    (void)output;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}

static void cosmic_toplevel_ext_workspace_enter(
    void *data,
    struct zcosmic_toplevel_handle_v1 *handle,
    struct ext_workspace_handle_v1 *workspace
) {
    ToplevelInfo *info = data;
    (void)handle;
    if (info) {
        info->mapped_to_workspace = TRUE;
        info->current_ext_workspace = workspace;
    }
    if (g_cosmic) {
        try_move_toplevel(g_cosmic, info);
    }
}

static void cosmic_toplevel_ext_workspace_leave(
    void *data,
    struct zcosmic_toplevel_handle_v1 *handle,
    struct ext_workspace_handle_v1 *workspace
) {
    ToplevelInfo *info = data;
    (void)handle;
    if (info && info->current_ext_workspace == workspace) {
        info->current_ext_workspace = NULL;
    }
}

static const struct zcosmic_toplevel_handle_v1_listener cosmic_toplevel_listener = {
    .closed = cosmic_toplevel_closed,
    .done = cosmic_toplevel_done,
    .title = cosmic_toplevel_title,
    .app_id = cosmic_toplevel_app_id,
    .output_enter = cosmic_toplevel_output_enter,
    .output_leave = cosmic_toplevel_output_leave,
    .workspace_enter = cosmic_toplevel_workspace_enter,
    .workspace_leave = cosmic_toplevel_workspace_leave,
    .state = cosmic_toplevel_state,
    .geometry = cosmic_toplevel_geometry,
    .ext_workspace_enter = cosmic_toplevel_ext_workspace_enter,
    .ext_workspace_leave = cosmic_toplevel_ext_workspace_leave,
};

/* --- foreign toplevel (modern) --- */

static void foreign_handle_closed(
    void *data,
    struct ext_foreign_toplevel_handle_v1 *handle
) {
    CosmicState *state = g_cosmic;
    ToplevelInfo *info = data;
    (void)handle;
    if (!state || !info) {
        return;
    }
    g_ptr_array_remove(state->toplevels, info);
}

static void foreign_handle_done(
    void *data,
    struct ext_foreign_toplevel_handle_v1 *handle
) {
    (void)handle;
    if (g_cosmic) {
        try_move_toplevel(g_cosmic, data);
    }
}

static void foreign_handle_title(
    void *data,
    struct ext_foreign_toplevel_handle_v1 *handle,
    const char *title
) {
    (void)data;
    (void)handle;
    (void)title;
}

static void foreign_handle_app_id(
    void *data,
    struct ext_foreign_toplevel_handle_v1 *handle,
    const char *app_id
) {
    ToplevelInfo *info = data;
    (void)handle;
    if (!info) {
        return;
    }
    g_free(info->app_id);
    info->app_id = g_strdup(app_id ? app_id : "");
}

static void foreign_handle_identifier(
    void *data,
    struct ext_foreign_toplevel_handle_v1 *handle,
    const char *identifier
) {
    (void)data;
    (void)handle;
    (void)identifier;
}

static const struct ext_foreign_toplevel_handle_v1_listener foreign_handle_listener = {
    .closed = foreign_handle_closed,
    .done = foreign_handle_done,
    .title = foreign_handle_title,
    .app_id = foreign_handle_app_id,
    .identifier = foreign_handle_identifier,
};

static void foreign_list_toplevel(
    void *data,
    struct ext_foreign_toplevel_list_v1 *list,
    struct ext_foreign_toplevel_handle_v1 *toplevel
) {
    CosmicState *state = data;
    (void)list;

    ToplevelInfo *info = g_new0(ToplevelInfo, 1);
    info->foreign = toplevel;
    info->appeared_after_sync = state->initial_sync_done;
    ext_foreign_toplevel_handle_v1_add_listener(
        toplevel,
        &foreign_handle_listener,
        info
    );

    if (state->toplevel_info &&
        zcosmic_toplevel_info_v1_get_version(state->toplevel_info) >= 2) {
        info->cosmic = zcosmic_toplevel_info_v1_get_cosmic_toplevel(
            state->toplevel_info,
            toplevel
        );
        zcosmic_toplevel_handle_v1_add_listener(
            info->cosmic,
            &cosmic_toplevel_listener,
            info
        );
    }

    g_ptr_array_add(state->toplevels, info);
}

static void foreign_list_finished(
    void *data,
    struct ext_foreign_toplevel_list_v1 *list
) {
    CosmicState *state = data;
    if (state && state->foreign_list == list) {
        ext_foreign_toplevel_list_v1_destroy(list);
        state->foreign_list = NULL;
    }
}

static const struct ext_foreign_toplevel_list_v1_listener foreign_list_listener = {
    .toplevel = foreign_list_toplevel,
    .finished = foreign_list_finished,
};

/* --- legacy cosmic toplevel info v1 --- */

static void cosmic_info_toplevel(
    void *data,
    struct zcosmic_toplevel_info_v1 *info_obj,
    struct zcosmic_toplevel_handle_v1 *toplevel
) {
    CosmicState *state = data;
    (void)info_obj;

    ToplevelInfo *info = g_new0(ToplevelInfo, 1);
    info->cosmic = toplevel;
    info->appeared_after_sync = state->initial_sync_done;
    zcosmic_toplevel_handle_v1_add_listener(
        toplevel,
        &cosmic_toplevel_listener,
        info
    );
    g_ptr_array_add(state->toplevels, info);
}

static void cosmic_info_finished(
    void *data,
    struct zcosmic_toplevel_info_v1 *info_obj
) {
    CosmicState *state = data;
    if (state && state->toplevel_info == info_obj) {
        zcosmic_toplevel_info_v1_destroy(info_obj);
        state->toplevel_info = NULL;
    }
}

static void cosmic_info_done(
    void *data,
    struct zcosmic_toplevel_info_v1 *info_obj
) {
    CosmicState *state = data;
    (void)info_obj;
    try_move_pending_toplevels(state);
}

static const struct zcosmic_toplevel_info_v1_listener cosmic_info_listener = {
    .toplevel = cosmic_info_toplevel,
    .finished = cosmic_info_finished,
    .done = cosmic_info_done,
};

static void cosmic_manager_capabilities(
    void *data,
    struct zcosmic_toplevel_manager_v1 *manager,
    struct wl_array *capabilities
) {
    (void)data;
    (void)manager;
    (void)capabilities;
}

static const struct zcosmic_toplevel_manager_v1_listener cosmic_manager_listener = {
    .capabilities = cosmic_manager_capabilities,
};

/* --- registry --- */

static void registry_global(
    void *data,
    struct wl_registry *registry,
    uint32_t name,
    const char *interface,
    uint32_t version
) {
    CosmicState *state = data;
    (void)registry;

    if (strcmp(interface, wl_output_interface.name) == 0) {
        GlobalOffer offer = { .name = name, .version = version };
        g_array_append_val(state->output_offers, offer);
    } else if (strcmp(interface, ext_foreign_toplevel_list_v1_interface.name) == 0) {
        state->foreign_list_offer.name = name;
        state->foreign_list_offer.version = version;
    } else if (strcmp(interface, zcosmic_toplevel_info_v1_interface.name) == 0) {
        state->toplevel_info_offer.name = name;
        state->toplevel_info_offer.version = version;
    } else if (strcmp(interface, zcosmic_toplevel_manager_v1_interface.name) == 0) {
        state->toplevel_manager_offer.name = name;
        state->toplevel_manager_offer.version = version;
    } else if (strcmp(interface, ext_workspace_manager_v1_interface.name) == 0) {
        state->ext_workspace_offer.name = name;
        state->ext_workspace_offer.version = version;
    } else if (strcmp(interface, zcosmic_workspace_manager_v1_interface.name) == 0) {
        state->cosmic_workspace_offer.name = name;
        state->cosmic_workspace_offer.version = version;
    }
}

static void bind_collected_globals(CosmicState *state) {
    for (guint i = 0; i < state->output_offers->len; i++) {
        GlobalOffer offer = g_array_index(state->output_offers, GlobalOffer, i);
        OutputInfo *info = g_new0(OutputInfo, 1);
        info->registry_name = offer.name;
        info->output = wl_registry_bind(
            state->registry,
            offer.name,
            &wl_output_interface,
            1
        );
        g_ptr_array_add(state->outputs, info);
    }

    state->use_ext_toplevels = state->foreign_list_offer.name != 0 &&
                               state->toplevel_info_offer.version >= 2;

    if (state->foreign_list_offer.name != 0) {
        state->foreign_list = wl_registry_bind(
            state->registry,
            state->foreign_list_offer.name,
            &ext_foreign_toplevel_list_v1_interface,
            1
        );
    }

    if (state->toplevel_info_offer.name != 0) {
        uint32_t bind_version;
        if (state->use_ext_toplevels) {
            bind_version = state->toplevel_info_offer.version < 3
                               ? state->toplevel_info_offer.version
                               : 3;
        } else {
            bind_version = 1;
        }
        state->toplevel_info = wl_registry_bind(
            state->registry,
            state->toplevel_info_offer.name,
            &zcosmic_toplevel_info_v1_interface,
            bind_version
        );
    }

    if (state->toplevel_manager_offer.name != 0) {
        uint32_t bind_version = state->toplevel_manager_offer.version < 4
                                    ? state->toplevel_manager_offer.version
                                    : 4;
        state->toplevel_manager = wl_registry_bind(
            state->registry,
            state->toplevel_manager_offer.name,
            &zcosmic_toplevel_manager_v1_interface,
            bind_version
        );
    }

    /* Prefer ext workspaces only when toplevel-info can emit ext_workspace_enter
     * (v3+) and the manager can move_to_ext_workspace (v4+). Otherwise bind the
     * legacy cosmic workspace stack alone so workspace_enter still maps windows. */
    gboolean use_ext_workspaces =
        state->ext_workspace_offer.name != 0 &&
        state->toplevel_info_offer.version >= 3 &&
        state->toplevel_manager_offer.version >= 4;

    if (use_ext_workspaces) {
        state->ext_workspace_manager = wl_registry_bind(
            state->registry,
            state->ext_workspace_offer.name,
            &ext_workspace_manager_v1_interface,
            1
        );
    } else if (state->cosmic_workspace_offer.name != 0) {
        uint32_t bind_version = state->cosmic_workspace_offer.version < 2
                                    ? state->cosmic_workspace_offer.version
                                    : 2;
        state->cosmic_workspace_manager = wl_registry_bind(
            state->registry,
            state->cosmic_workspace_offer.name,
            &zcosmic_workspace_manager_v1_interface,
            bind_version
        );
    } else if (state->ext_workspace_offer.name != 0) {
        state->ext_workspace_manager = wl_registry_bind(
            state->registry,
            state->ext_workspace_offer.name,
            &ext_workspace_manager_v1_interface,
            1
        );
        g_warning(
            "COSMIC: ext workspaces without toplevel-info v3 / manager v4; "
            "workspace mapping may not work."
        );
    }
}

static void registry_global_remove(
    void *data,
    struct wl_registry *registry,
    uint32_t name
) {
    CosmicState *state = data;
    (void)registry;
    if (!state || !state->outputs) {
        return;
    }

    for (guint i = 0; i < state->outputs->len; i++) {
        OutputInfo *info = g_ptr_array_index(state->outputs, i);
        if (!info || info->registry_name != name) {
            continue;
        }
        scrub_output_refs(state, info->output);
        g_ptr_array_remove_index(state->outputs, i);
        return;
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void sync_callback_done(
    void *data,
    struct wl_callback *callback,
    uint32_t time
) {
    CosmicState *state = data;
    (void)time;
    wl_callback_destroy(callback);
    state->sync_callback = NULL;
    state->initial_sync_done = TRUE;
    g_info("COSMIC workspace backend initial sync complete.");
    try_move_pending_toplevels(state);
}

static const struct wl_callback_listener sync_callback_listener = {
    .done = sync_callback_done,
};

static gboolean on_wayland_fd(gint fd, GIOCondition condition, gpointer user_data) {
    CosmicState *state = user_data;
    (void)fd;

    if (!state || !state->display) {
        return G_SOURCE_REMOVE;
    }

    if (condition & (G_IO_ERR | G_IO_HUP)) {
        g_warning("COSMIC Wayland connection closed.");
        /* We are inside this GSource; clear id before shutdown tears it down. */
        state->fd_source = 0;
        cosmic_workspace_shutdown();
        return G_SOURCE_REMOVE;
    }

    if (condition & G_IO_IN) {
        if (wl_display_dispatch(state->display) == -1) {
            g_warning("COSMIC Wayland dispatch failed.");
            state->fd_source = 0;
            cosmic_workspace_shutdown();
            return G_SOURCE_REMOVE;
        }
    }

    wl_display_flush(state->display);
    return G_SOURCE_CONTINUE;
}

gboolean cosmic_workspace_init(AppData *data) {
    if (g_cosmic) {
        return g_cosmic->active;
    }

    CosmicState *state = g_new0(CosmicState, 1);
    state->app_data = data;
    state->outputs = g_ptr_array_new_with_free_func(output_info_free);
    state->workspaces = g_ptr_array_new_with_free_func(workspace_info_free);
    state->workspace_groups = g_ptr_array_new_with_free_func(workspace_group_info_free);
    state->toplevels = g_ptr_array_new_with_free_func(toplevel_info_free);
    state->output_offers = g_array_new(FALSE, FALSE, sizeof(GlobalOffer));
    state->next_workspace_index = 1;
    g_cosmic = state;

    state->display = wl_display_connect(NULL);
    if (!state->display) {
        g_info("COSMIC workspace backend inactive (no Wayland display).");
        cosmic_workspace_shutdown();
        return FALSE;
    }

    state->registry = wl_display_get_registry(state->display);
    wl_registry_add_listener(state->registry, &registry_listener, state);
    wl_display_roundtrip(state->display);
    bind_collected_globals(state);

    gboolean has_manager = state->toplevel_manager != NULL;
    gboolean has_info = state->toplevel_info != NULL;
    gboolean has_workspaces =
        state->ext_workspace_manager != NULL ||
        state->cosmic_workspace_manager != NULL;

    if (!has_manager || !has_info || !has_workspaces) {
        g_info(
            "COSMIC workspace backend inactive (missing compositor globals)."
        );
        cosmic_workspace_shutdown();
        return FALSE;
    }

    if (state->toplevel_manager) {
        zcosmic_toplevel_manager_v1_add_listener(
            state->toplevel_manager,
            &cosmic_manager_listener,
            state
        );
    }

    if (state->ext_workspace_manager) {
        ext_workspace_manager_v1_add_listener(
            state->ext_workspace_manager,
            &ext_workspace_manager_listener,
            state
        );
    } else if (state->cosmic_workspace_manager) {
        zcosmic_workspace_manager_v1_add_listener(
            state->cosmic_workspace_manager,
            &cosmic_workspace_manager_listener,
            state
        );
    }

    if (state->foreign_list &&
        state->toplevel_info &&
        zcosmic_toplevel_info_v1_get_version(state->toplevel_info) >= 2) {
        ext_foreign_toplevel_list_v1_add_listener(
            state->foreign_list,
            &foreign_list_listener,
            state
        );
        /* Still listen for info-level done so we can retry moves after
         * cosmic handle properties (output/workspace) arrive. */
        zcosmic_toplevel_info_v1_add_listener(
            state->toplevel_info,
            &cosmic_info_listener,
            state
        );
    } else if (state->toplevel_info) {
        zcosmic_toplevel_info_v1_add_listener(
            state->toplevel_info,
            &cosmic_info_listener,
            state
        );
    }

    wl_display_roundtrip(state->display);

    state->sync_callback = wl_display_sync(state->display);
    wl_callback_add_listener(state->sync_callback, &sync_callback_listener, state);
    wl_display_flush(state->display);

    int fd = wl_display_get_fd(state->display);
    state->fd_source = g_unix_fd_add(fd, G_IO_IN | G_IO_ERR | G_IO_HUP, on_wayland_fd, state);
    state->active = TRUE;
    g_info("COSMIC workspace backend active.");
    return TRUE;
}

void cosmic_workspace_shutdown(void) {
    CosmicState *state = g_cosmic;
    if (!state) {
        return;
    }
    g_cosmic = NULL;

    if (state->fd_source) {
        g_source_remove(state->fd_source);
        state->fd_source = 0;
    }

    g_clear_pointer(&state->toplevels, g_ptr_array_unref);
    g_clear_pointer(&state->workspace_groups, g_ptr_array_unref);
    g_clear_pointer(&state->workspaces, g_ptr_array_unref);
    g_clear_pointer(&state->outputs, g_ptr_array_unref);
    g_clear_pointer(&state->output_offers, g_array_unref);

    if (state->sync_callback) {
        wl_callback_destroy(state->sync_callback);
    }
    if (state->foreign_list) {
        ext_foreign_toplevel_list_v1_destroy(state->foreign_list);
    }
    if (state->toplevel_info) {
        zcosmic_toplevel_info_v1_destroy(state->toplevel_info);
    }
    if (state->toplevel_manager) {
        zcosmic_toplevel_manager_v1_destroy(state->toplevel_manager);
    }
    if (state->ext_workspace_manager) {
        ext_workspace_manager_v1_destroy(state->ext_workspace_manager);
    }
    if (state->cosmic_workspace_manager) {
        zcosmic_workspace_manager_v1_destroy(state->cosmic_workspace_manager);
    }
    if (state->registry) {
        wl_registry_destroy(state->registry);
    }
    if (state->display) {
        wl_display_disconnect(state->display);
    }

    g_free(state);
}

gboolean cosmic_workspace_is_active(void) {
    return g_cosmic && g_cosmic->active;
}

GPtrArray *cosmic_workspace_list_open_apps(void) {
    GPtrArray *apps = g_ptr_array_new_with_free_func(g_free);
    if (!g_cosmic || !g_cosmic->active) {
        return apps;
    }

    /* Flush only — avoid nested dispatch_pending from the Preferences UI path. */
    wl_display_flush(g_cosmic->display);

    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < g_cosmic->toplevels->len; i++) {
        ToplevelInfo *info = g_ptr_array_index(g_cosmic->toplevels, i);
        if (!info || !info->app_id || !*info->app_id) {
            continue;
        }
        if (g_hash_table_contains(seen, info->app_id)) {
            continue;
        }
        g_hash_table_add(seen, info->app_id);
        g_ptr_array_add(apps, g_strdup(info->app_id));
    }
    g_hash_table_destroy(seen);
    return apps;
}

void cosmic_workspace_refresh(AppData *data) {
    if (g_cosmic) {
        g_cosmic->app_data = data;
    }
}
