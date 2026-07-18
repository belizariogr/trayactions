#include <gtk/gtk.h>

#include "appdata.h"
#include "config.h"
#include "tray.h"
#include "utils.h"

static gboolean prepare_configuration(AppData *data) {
    char *config_dir_path = g_build_filename(g_get_user_config_dir(), "trayactions", NULL);
    data->config_file_path = g_build_filename(config_dir_path, "config.json", NULL);

    if (!g_file_test(data->config_file_path, G_FILE_TEST_EXISTS)) {
        g_info("Configuration file not found at %s. Creating default.", data->config_file_path);
        if (ensure_dir_exists(config_dir_path) != 0) {
            g_printerr("Failed to create configuration directory.\n");
            g_free(config_dir_path);
            return FALSE;
        }

        GError *error = NULL;
        if (!g_file_set_contents(data->config_file_path, default_config_json, -1, &error)) {
            g_printerr(
                "Error creating default config file %s: %s\n",
                data->config_file_path,
                error->message
            );
            g_error_free(error);
            g_free(config_dir_path);
            return FALSE;
        }
    }

    g_free(config_dir_path);
    return TRUE;
}

static void application_activate(GtkApplication *application, gpointer user_data) {
    AppData *data = user_data;
    if (data->initialized) {
        return;
    }

    data->initialized = TRUE;
    data->application = application;
    g_application_hold(G_APPLICATION(application));

    reload_configuration(data, TRUE);

    GError *error = NULL;
    if (!tray_start(data, &error)) {
        g_printerr("Could not initialize the tray icon: %s\n", error->message);
        g_error_free(error);
        g_application_quit(G_APPLICATION(application));
        return;
    }

    GFile *config_file = g_file_new_for_path(data->config_file_path);
    data->monitor = g_file_monitor_file(config_file, G_FILE_MONITOR_NONE, NULL, &error);
    g_object_unref(config_file);

    if (!data->monitor) {
        g_warning("Could not monitor %s: %s", data->config_file_path, error->message);
        g_error_free(error);
        return;
    }

    g_signal_connect(data->monitor, "changed", G_CALLBACK(on_config_changed), data);
    g_info("Monitoring %s for changes.", data->config_file_path);
}

static void application_shutdown(GApplication *application, gpointer user_data) {
    AppData *data = user_data;
    (void)application;

    if (data->monitor) {
        g_file_monitor_cancel(data->monitor);
        g_clear_object(&data->monitor);
    }
    tray_stop(data);
    g_clear_pointer(&data->menu_items, g_ptr_array_unref);
    g_clear_pointer(&data->indicator_icon, g_free);
    g_clear_pointer(&data->config_file_path, g_free);
}

int main(int argc, char **argv) {
    AppData data = {0};
    data.application = gtk_application_new(
        "io.github.belizario.TrayActions",
        G_APPLICATION_DEFAULT_FLAGS
    );

    if (!prepare_configuration(&data)) {
        g_clear_pointer(&data.config_file_path, g_free);
        g_object_unref(data.application);
        return 1;
    }

    g_signal_connect(data.application, "activate", G_CALLBACK(application_activate), &data);
    g_signal_connect(data.application, "shutdown", G_CALLBACK(application_shutdown), &data);

    int status = g_application_run(G_APPLICATION(data.application), argc, argv);
    g_object_unref(data.application);
    return status;
}
