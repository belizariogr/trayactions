#include "workspace_route.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void expect_true(gboolean cond, const char *name) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    } else {
        printf("PASS: %s\n", name);
    }
}

static AppWorkspaceAssignment *make_assignment(const char *app_id, gint workspace) {
    AppWorkspaceAssignment *assignment = g_new0(AppWorkspaceAssignment, 1);
    assignment->app_id = g_strdup(app_id);
    assignment->workspace = workspace;
    return assignment;
}

static void free_assignment(gpointer pointer) {
    AppWorkspaceAssignment *assignment = pointer;
    if (!assignment) {
        return;
    }
    g_free(assignment->app_id);
    g_free(assignment);
}

static void test_assignment_lookup(void) {
    GPtrArray *assignments = g_ptr_array_new_with_free_func(free_assignment);
    g_ptr_array_add(assignments, make_assignment("firefox", 2));
    g_ptr_array_add(assignments, make_assignment("org.gnome.Nautilus", 1));

    expect_true(
        workspace_route_assignment_for_app(assignments, "firefox") == 2,
        "finds firefox workspace"
    );
    expect_true(
        workspace_route_assignment_for_app(assignments, "org.gnome.Nautilus") == 1,
        "finds nautilus workspace"
    );
    expect_true(
        workspace_route_assignment_for_app(assignments, "missing") == 0,
        "missing app returns 0"
    );
    expect_true(
        workspace_route_assignment_for_app(assignments, "") == 0,
        "empty app_id returns 0"
    );
    expect_true(
        workspace_route_assignment_for_app(NULL, "firefox") == 0,
        "NULL assignments returns 0"
    );
    g_ptr_array_unref(assignments);
}

static void test_find_entry_prefers_name(void) {
    WorkspaceRouteEntry entries[] = {
        { .name = "3", .rank = 1, .coord_index = 1 },
        { .name = "1", .rank = 2, .coord_index = 2 },
        { .name = "2", .rank = 3, .coord_index = 3 },
    };
    expect_true(
        workspace_route_find_entry(entries, 3, 2) == 2,
        "name match beats rank/coord"
    );
}

static void test_find_entry_falls_back_to_rank(void) {
    WorkspaceRouteEntry entries[] = {
        { .name = NULL, .rank = 1, .coord_index = 10 },
        { .name = NULL, .rank = 2, .coord_index = 20 },
        { .name = NULL, .rank = 3, .coord_index = 30 },
    };
    expect_true(
        workspace_route_find_entry(entries, 3, 2) == 1,
        "rank match when names absent"
    );
}

static void test_find_entry_falls_back_to_coord(void) {
    WorkspaceRouteEntry entries[] = {
        { .name = NULL, .rank = 0, .coord_index = 1 },
        { .name = NULL, .rank = 0, .coord_index = 2 },
    };
    expect_true(
        workspace_route_find_entry(entries, 2, 2) == 1,
        "coord_index match when rank unknown"
    );
    expect_true(
        workspace_route_find_entry(entries, 2, 5) == -1,
        "missing workspace returns -1"
    );
}

static void test_assign_ranks_by_coord(void) {
    gint coords[] = { 30, 10, 20 };
    gint ranks[3] = { 0 };
    workspace_route_assign_ranks(coords, 3, ranks);
    expect_true(ranks[1] == 1, "lowest coord gets rank 1");
    expect_true(ranks[2] == 2, "middle coord gets rank 2");
    expect_true(ranks[0] == 3, "highest coord gets rank 3");
}

static void test_assign_ranks_stable_without_coords(void) {
    gint coords[] = { 0, 0, 0 };
    gint ranks[3] = { 0 };
    workspace_route_assign_ranks(coords, 3, ranks);
    expect_true(ranks[0] == 1 && ranks[1] == 2 && ranks[2] == 3,
                "stable order when coords unknown");
}

static void test_decide_waits_until_ready(void) {
    WorkspaceRouteState state = {
        .active = TRUE,
        .initial_sync_done = TRUE,
        .appeared_after_sync = TRUE,
        .handled = FALSE,
        .has_app_id = TRUE,
        .has_cosmic_handle = TRUE,
        .has_assignment = TRUE,
        .has_target_workspace = TRUE,
        .has_output = FALSE,
        .has_mapped_workspace = TRUE,
    };
    expect_true(
        workspace_route_decide(&state) == WORKSPACE_ROUTE_WAIT,
        "waits when output missing"
    );

    state.has_output = TRUE;
    state.has_target_workspace = FALSE;
    expect_true(
        workspace_route_decide(&state) == WORKSPACE_ROUTE_WAIT,
        "waits when workspace missing"
    );

    state.has_target_workspace = TRUE;
    state.has_cosmic_handle = FALSE;
    expect_true(
        workspace_route_decide(&state) == WORKSPACE_ROUTE_WAIT,
        "waits when cosmic handle missing"
    );

    state.has_cosmic_handle = TRUE;
    state.has_mapped_workspace = FALSE;
    expect_true(
        workspace_route_decide(&state) == WORKSPACE_ROUTE_WAIT,
        "waits until toplevel is mapped to a workspace"
    );
}

static void test_decide_move_when_ready(void) {
    WorkspaceRouteState state = {
        .active = TRUE,
        .initial_sync_done = TRUE,
        .appeared_after_sync = TRUE,
        .handled = FALSE,
        .has_app_id = TRUE,
        .has_cosmic_handle = TRUE,
        .has_assignment = TRUE,
        .has_target_workspace = TRUE,
        .has_output = TRUE,
        .has_mapped_workspace = TRUE,
    };
    expect_true(
        workspace_route_decide(&state) == WORKSPACE_ROUTE_MOVE,
        "moves when all inputs ready"
    );
}

static void test_decide_ignore_cases(void) {
    WorkspaceRouteState state = {
        .active = TRUE,
        .initial_sync_done = TRUE,
        .appeared_after_sync = TRUE,
        .handled = FALSE,
        .has_app_id = TRUE,
        .has_cosmic_handle = TRUE,
        .has_assignment = FALSE,
        .has_target_workspace = TRUE,
        .has_output = TRUE,
        .has_mapped_workspace = TRUE,
    };
    expect_true(
        workspace_route_decide(&state) == WORKSPACE_ROUTE_IGNORE,
        "ignores toplevels without assignment"
    );

    state.has_assignment = TRUE;
    state.appeared_after_sync = FALSE;
    expect_true(
        workspace_route_decide(&state) == WORKSPACE_ROUTE_IGNORE,
        "ignores pre-sync toplevels"
    );

    state.appeared_after_sync = TRUE;
    state.handled = TRUE;
    expect_true(
        workspace_route_decide(&state) == WORKSPACE_ROUTE_IGNORE,
        "ignores already handled toplevels"
    );
}

static void test_decide_waits_before_sync(void) {
    WorkspaceRouteState state = {
        .active = TRUE,
        .initial_sync_done = FALSE,
        .appeared_after_sync = TRUE,
        .handled = FALSE,
        .has_app_id = TRUE,
        .has_cosmic_handle = TRUE,
        .has_assignment = TRUE,
        .has_target_workspace = TRUE,
        .has_output = TRUE,
        .has_mapped_workspace = TRUE,
    };
    expect_true(
        workspace_route_decide(&state) == WORKSPACE_ROUTE_WAIT,
        "waits until initial sync completes"
    );
}

static void test_coord_to_index_cosmic_one_based(void) {
    expect_true(workspace_route_coord_to_index(1) == 1, "coord 1 → workspace 1");
    expect_true(workspace_route_coord_to_index(2) == 2, "coord 2 → workspace 2");
    expect_true(workspace_route_coord_to_index(0) == 1, "coord 0 → workspace 1");
    expect_true(
        workspace_route_coord_to_index(G_MAXUINT) == G_MAXINT,
        "huge coord clamps to G_MAXINT"
    );
}

static void test_find_entry_with_cosmic_coords(void) {
    /* Live COSMIC: name and coord both equal the user-facing number. */
    WorkspaceRouteEntry entries[] = {
        { .name = "1", .rank = 1, .coord_index = 1 },
        { .name = "2", .rank = 2, .coord_index = 2 },
        { .name = "3", .rank = 3, .coord_index = 3 },
    };
    expect_true(workspace_route_find_entry(entries, 3, 2) == 1,
                "finds COSMIC workspace 2");
}

int main(void) {
    test_assignment_lookup();
    test_find_entry_prefers_name();
    test_find_entry_falls_back_to_rank();
    test_find_entry_falls_back_to_coord();
    test_find_entry_with_cosmic_coords();
    test_coord_to_index_cosmic_one_based();
    test_assign_ranks_by_coord();
    test_assign_ranks_stable_without_coords();
    test_decide_waits_until_ready();
    test_decide_move_when_ready();
    test_decide_ignore_cases();
    test_decide_waits_before_sync();

    if (failures > 0) {
        fprintf(stderr, "\n%d test(s) failed\n", failures);
        return 1;
    }
    printf("\nAll workspace_route tests passed\n");
    return 0;
}
