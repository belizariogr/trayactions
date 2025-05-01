#include <gtk/gtk.h>
#include <libappindicator/app-indicator.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <glib.h>
#include "appdata.h"
#include "utils.h"
#include "config.h"
#include "menu.h"

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);

    GtkSettings *settings = gtk_settings_get_default();
    g_object_set(settings, "gtk-menu-images", TRUE, NULL);

    const char *home_dir = getenv("HOME");
    if (!home_dir) {
        g_printerr("Error: HOME environment variable not set.\n");
        return 1;
    }

    char *config_dir_path = g_build_filename(home_dir, ".config", "trayactions", NULL);
    char *config_file_path = g_build_filename(config_dir_path, "config.json", NULL);

    if (access(config_file_path, F_OK) != 0) {
        g_info("Configuration file not found at %s. Creating default.", config_file_path);
        if (ensure_dir_exists(config_dir_path) != 0) {
            g_printerr("Failed to create configuration directory. Exiting.\n");
            g_free(config_dir_path);
            g_free(config_file_path);
            return 1;
        }
        FILE *default_fp = fopen(config_file_path, "w");
        if (!default_fp) {
            g_printerr("Error creating default config file %s: %s\n", config_file_path, strerror(errno));
        } else {
            fprintf(default_fp, "%s", default_config_json);
            fclose(default_fp);
            g_info("Default configuration file created at %s.", config_file_path);
        }
    }

    AppData app_data = {0};
    app_data.config_file_path = g_strdup(config_file_path);
    app_data.is_reloading = FALSE;

    app_data.indicator = app_indicator_new(
        "trayactions-indicator",
        "system-run",
        APP_INDICATOR_CATEGORY_APPLICATION_STATUS
    );
    app_indicator_set_status(app_data.indicator, APP_INDICATOR_STATUS_ACTIVE);

    reload_configuration(&app_data, TRUE);

    GFile *config_gfile = g_file_new_for_path(app_data.config_file_path);
    GError *error = NULL;
    app_data.monitor = g_file_monitor_file(config_gfile, G_FILE_MONITOR_NONE, NULL, &error);
    if (!app_data.monitor) {
        g_printerr("Error creating file monitor for %s: %s\n", app_data.config_file_path, error->message);
        g_error_free(error);
    } else {
        g_signal_connect(app_data.monitor, "changed", G_CALLBACK(on_config_changed), &app_data);
        g_print("Monitoring %s for changes...\n", app_data.config_file_path);
    }
    g_object_unref(config_gfile);

    gtk_main();

    g_print("Exiting...\n");
    if (app_data.monitor) {
        g_file_monitor_cancel(app_data.monitor);
        g_object_unref(app_data.monitor);
    }
    if (app_data.current_menu) {
        // Optional: gtk_widget_destroy(app_data.current_menu);
    }
    g_free(app_data.config_file_path);
    // Optional: g_object_unref(app_data.indicator);

    g_free(config_dir_path);
    g_free(config_file_path);
    return 0;
}