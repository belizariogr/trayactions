#include "cosmic_workspace.h"

#include <glib-unix.h>
#include <string.h>
#include <wayland-client.h>

#include "generated/cosmic-toplevel-info-unstable-v1-client.h"
#include "generated/cosmic-toplevel-management-unstable-v1-client.h"
#include "generated/cosmic-workspace-unstable-v1-client.h"
#include "generated/ext-foreign-toplevel-list-v1-client.h"
#include "generated/ext-workspace-v1-client.h"

typedef struct {
    struct wl_output *output;
    uint32_t registry_name;
} OutputInfo;

typedef struct {
    struct ext_workspace_handle_v1 *ext_handle;
    struct zcosmic_workspace_handle_v1 *cosmic_handle;
    gint index;
    uint32_t coord0;
    gboolean has_coord;
} WorkspaceInfo;

typedef struct {
    struct ext_foreign_toplevel_handle_v1 *foreign;
    struct zcosmic_toplevel_handle_v1 *cosmic;
    char *app_id;
    struct wl_output *output;
    gboolean appeared_after_sync;
    gboolean handled;
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
    GPtrArray *toplevels;

    gboolean use_ext_toplevels;
    gboolean initial_sync_done;
    gboolean active;
    gint next_workspace_index;
} CosmicState;

static CosmicState *g_cosmic = NULL;

static gint assignment_for_app(CosmicState *state, const char *app_id) {
    if (!state || !state->app_data || !state->app_data->app_workspaces || !app_id) {
        return 0;
    }
    for (guint i = 0; i < state->app_data->app_workspaces->len; i++) {
        AppWorkspaceAssignment *assignment =
            g_ptr_array_index(state->app_data->app_workspaces, i);
        if (assignment && assignment->app_id &&
            strcmp(assignment->app_id, app_id) == 0) {
            return assignment->workspace;
        }
    }
    return 0;
}

static WorkspaceInfo *workspace_by_index(CosmicState *state, gint index) {
    if (!state || index < 1) {
        return NULL;
    }
    for (guint i = 0; i < state->workspaces->len; i++) {
        WorkspaceInfo *info = g_ptr_array_index(state->workspaces, i);
        if (info && info->index == index) {
            return info;
        }
    }
    return NULL;
}

static struct wl_output *first_output(CosmicState *state) {
    if (!state || !state->outputs || state->outputs->len == 0) {
        return NULL;
    }
    OutputInfo *info = g_ptr_array_index(state->outputs, 0);
    return info ? info->output : NULL;
}

static void try_move_toplevel(CosmicState *state, ToplevelInfo *toplevel) {
    if (!state || !toplevel || !state->active || !state->initial_sync_done) {
        return;
    }
    if (!toplevel->appeared_after_sync || toplevel->handled) {
        return;
    }
    if (!toplevel->app_id || !*toplevel->app_id || !toplevel->cosmic) {
        return;
    }

    gint workspace_index = assignment_for_app(state, toplevel->app_id);
    if (workspace_index < 1) {
        toplevel->handled = TRUE;
        return;
    }

    WorkspaceInfo *workspace = workspace_by_index(state, workspace_index);
    if (!workspace || !state->toplevel_manager) {
        return;
    }

    struct wl_output *output = toplevel->output ? toplevel->output : first_output(state);
    if (!output) {
        g_warning("COSMIC workspace move skipped: no wl_output available.");
        return;
    }

    if (workspace->ext_handle &&
        zcosmic_toplevel_manager_v1_get_version(state->toplevel_manager) >= 4) {
        zcosmic_toplevel_manager_v1_move_to_ext_workspace(
            state->toplevel_manager,
            toplevel->cosmic,
            workspace->ext_handle,
            output
        );
        g_info("Moved %s to workspace %d (ext).", toplevel->app_id, workspace_index);
    } else if (workspace->cosmic_handle &&
               zcosmic_toplevel_manager_v1_get_version(state->toplevel_manager) >= 2) {
        zcosmic_toplevel_manager_v1_move_to_workspace(
            state->toplevel_manager,
            toplevel->cosmic,
            workspace->cosmic_handle,
            output
        );
        g_info("Moved %s to workspace %d (cosmic).", toplevel->app_id, workspace_index);
    } else {
        return;
    }

    toplevel->handled = TRUE;
    wl_display_flush(state->display);
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

static void workspace_info_free(gpointer pointer) {
    WorkspaceInfo *info = pointer;
    if (!info) {
        return;
    }
    if (info->ext_handle) {
        ext_workspace_handle_v1_destroy(info->ext_handle);
    }
    if (info->cosmic_handle) {
        zcosmic_workspace_handle_v1_destroy(info->cosmic_handle);
    }
    g_free(info);
}

static void toplevel_info_free(gpointer pointer) {
    ToplevelInfo *info = pointer;
    if (!info) {
        return;
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
        info->index = (gint)info->coord0 + 1;
        return;
    }
    info->index = state->next_workspace_index++;
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
    (void)data;
    (void)handle;
    (void)name;
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
    if (g_cosmic) {
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

static void ext_workspace_manager_workspace_group(
    void *data,
    struct ext_workspace_manager_v1 *manager,
    struct ext_workspace_group_handle_v1 *workspace_group
) {
    (void)data;
    (void)manager;
    /* Group events are optional for our index-based moves. */
    ext_workspace_group_handle_v1_destroy(workspace_group);
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
    (void)data;
    (void)manager;
}

static void ext_workspace_manager_finished(
    void *data,
    struct ext_workspace_manager_v1 *manager
) {
    CosmicState *state = data;
    (void)manager;
    if (state) {
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
    (void)data;
    (void)handle;
    (void)name;
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
    if (g_cosmic) {
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
    struct zcosmic_workspace_group_handle_v1 *group,
    struct wl_output *output
) {
    (void)data;
    (void)group;
    (void)output;
}

static void cosmic_workspace_group_output_leave(
    void *data,
    struct zcosmic_workspace_group_handle_v1 *group,
    struct wl_output *output
) {
    (void)data;
    (void)group;
    (void)output;
}

static void cosmic_workspace_group_workspace(
    void *data,
    struct zcosmic_workspace_group_handle_v1 *group,
    struct zcosmic_workspace_handle_v1 *workspace
) {
    CosmicState *state = data;
    (void)group;
    WorkspaceInfo *info = g_new0(WorkspaceInfo, 1);
    info->cosmic_handle = workspace;
    zcosmic_workspace_handle_v1_add_listener(
        workspace,
        &cosmic_workspace_handle_listener,
        info
    );
    assign_workspace_index(state, info);
    g_ptr_array_add(state->workspaces, info);
}

static void cosmic_workspace_group_remove(
    void *data,
    struct zcosmic_workspace_group_handle_v1 *group
) {
    (void)data;
    zcosmic_workspace_group_handle_v1_destroy(group);
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
    zcosmic_workspace_group_handle_v1_add_listener(
        workspace_group,
        &cosmic_workspace_group_listener,
        state
    );
}

static void cosmic_workspace_manager_done(
    void *data,
    struct zcosmic_workspace_manager_v1 *manager
) {
    (void)data;
    (void)manager;
}

static void cosmic_workspace_manager_finished(
    void *data,
    struct zcosmic_workspace_manager_v1 *manager
) {
    CosmicState *state = data;
    (void)manager;
    if (state) {
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
    (void)data;
    (void)handle;
    (void)workspace;
}

static void cosmic_toplevel_workspace_leave(
    void *data,
    struct zcosmic_toplevel_handle_v1 *handle,
    struct zcosmic_workspace_handle_v1 *workspace
) {
    (void)data;
    (void)handle;
    (void)workspace;
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
    (void)data;
    (void)handle;
    (void)workspace;
}

static void cosmic_toplevel_ext_workspace_leave(
    void *data,
    struct zcosmic_toplevel_handle_v1 *handle,
    struct ext_workspace_handle_v1 *workspace
) {
    (void)data;
    (void)handle;
    (void)workspace;
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
    (void)list;
    if (state) {
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
    (void)info_obj;
    if (state) {
        state->toplevel_info = NULL;
    }
}

static void cosmic_info_done(
    void *data,
    struct zcosmic_toplevel_info_v1 *info_obj
) {
    (void)data;
    (void)info_obj;
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

    if (state->ext_workspace_offer.name != 0) {
        state->ext_workspace_manager = wl_registry_bind(
            state->registry,
            state->ext_workspace_offer.name,
            &ext_workspace_manager_v1_interface,
            1
        );
    }

    if (state->cosmic_workspace_offer.name != 0) {
        uint32_t bind_version = state->cosmic_workspace_offer.version < 2
                                    ? state->cosmic_workspace_offer.version
                                    : 2;
        state->cosmic_workspace_manager = wl_registry_bind(
            state->registry,
            state->cosmic_workspace_offer.name,
            &zcosmic_workspace_manager_v1_interface,
            bind_version
        );
    }
}

static void registry_global_remove(
    void *data,
    struct wl_registry *registry,
    uint32_t name
) {
    (void)data;
    (void)registry;
    (void)name;
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
        state->active = FALSE;
        return G_SOURCE_REMOVE;
    }

    if (condition & G_IO_IN) {
        if (wl_display_dispatch(state->display) == -1) {
            g_warning("COSMIC Wayland dispatch failed.");
            state->active = FALSE;
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

    /* Dispatch pending events so the list is fresh. */
    wl_display_dispatch_pending(g_cosmic->display);
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
