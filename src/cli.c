#include "cli.h"

#include <string.h>

#include <glib.h>

#include "appdata.h"
#include "workspace.h"

static void print_usage(const char *argv0) {
    g_printerr(
        "Usage:\n"
        "  %s --run <command>\n"
        "  %s --run-or-focus <command> --app_id=<id>\n"
        "\n"
        "Examples:\n"
        "  %s --run google-chrome\n"
        "  %s --run-or-focus google-chrome --app_id=google-chrome\n"
        "  %s --run-or-focus \"google-chrome www.uol.com.br\" --app_id=chrome2\n",
        argv0,
        argv0,
        argv0,
        argv0,
        argv0
    );
}

static char *command_executable(const char *command, GError **error) {
    gint argc = 0;
    gchar **argv = NULL;
    if (!g_shell_parse_argv(command, &argc, &argv, error) || argc < 1 || !argv[0]) {
        if (error && !*error) {
            g_set_error(error, G_SPAWN_ERROR, G_SPAWN_ERROR_NOENT, "empty command");
        }
        g_strfreev(argv);
        return NULL;
    }

    char *resolved = g_find_program_in_path(argv[0]);
    if (!resolved) {
        g_set_error(
            error,
            G_SPAWN_ERROR,
            G_SPAWN_ERROR_NOENT,
            "command not found: %s",
            argv[0]
        );
    }
    g_strfreev(argv);
    return resolved;
}

static int run_command(const char *command) {
    GError *error = NULL;
    char *resolved = command_executable(command, &error);
    if (!resolved) {
        g_printerr("%s\n", error ? error->message : "invalid command");
        g_clear_error(&error);
        return 127;
    }
    g_free(resolved);

    /* Match tray launchers: shell form supports args/URLs; do not wait. */
    char *argv[] = {"/bin/sh", "-c", (char *)command, NULL};
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error)) {
        g_printerr("Could not run '%s': %s\n", command, error->message);
        g_error_free(error);
        return 1;
    }
    return 0;
}

static gboolean try_focus_app(const char *app_id) {
    AppData data = {0};
    if (!workspace_backend_init(&data)) {
        return FALSE;
    }

    gboolean focused = workspace_focus_app(app_id);
    workspace_backend_shutdown();
    return focused;
}

int cli_try_handle(int argc, char **argv) {
    enum { MODE_NONE, MODE_RUN, MODE_RUN_OR_FOCUS } mode = MODE_NONE;
    const char *command = NULL;
    const char *app_id = NULL;
    gboolean want_help = FALSE;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            want_help = TRUE;
            continue;
        }
        if (strcmp(argv[i], "--run") == 0) {
            if (mode != MODE_NONE) {
                g_printerr("Only one of --run / --run-or-focus is allowed.\n");
                print_usage(argv[0]);
                return 1;
            }
            mode = MODE_RUN;
            if (i + 1 >= argc) {
                g_printerr("--run requires a command.\n");
                print_usage(argv[0]);
                return 1;
            }
            command = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--run-or-focus") == 0) {
            if (mode != MODE_NONE) {
                g_printerr("Only one of --run / --run-or-focus is allowed.\n");
                print_usage(argv[0]);
                return 1;
            }
            mode = MODE_RUN_OR_FOCUS;
            if (i + 1 >= argc) {
                g_printerr("--run-or-focus requires a command.\n");
                print_usage(argv[0]);
                return 1;
            }
            command = argv[++i];
            continue;
        }
        if (g_str_has_prefix(argv[i], "--app_id=")) {
            app_id = argv[i] + strlen("--app_id=");
            continue;
        }

        if (mode != MODE_NONE) {
            g_printerr("Unexpected argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (mode == MODE_NONE) {
        return -1;
    }

    if (want_help || !command || !*command) {
        print_usage(argv[0]);
        return want_help ? 0 : 1;
    }

    if (mode == MODE_RUN_OR_FOCUS) {
        if (!app_id || !*app_id) {
            g_printerr("--app_id=<id> is required with --run-or-focus.\n");
            print_usage(argv[0]);
            return 1;
        }
        if (try_focus_app(app_id)) {
            return 0;
        }
    }

    return run_command(command);
}
