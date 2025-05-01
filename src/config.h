#ifndef CONFIG_H
#define CONFIG_H

#include <glib.h>
#include "appdata.h"

/**
 * Global default JSON config (previously declared in main.c).
 */
extern const char *default_config_json;

/**
 * Show a warning dialog about invalid JSON configuration.
 */
void show_json_error_dialog(const char *config_file_path, const char *error_message);

/**
 * Reload the configuration.
 */
void reload_configuration(AppData *data, gboolean is_initial_load);

/**
 * Timeout callback to defer reload operations slightly.
 */
gboolean reload_configuration_timeout_cb(gpointer user_data);

/**
 * Callback triggered by file monitor when config changes.
 */
void on_config_changed(GFileMonitor *monitor,
                       GFile *file,
                       GFile *other_file,
                       GFileMonitorEvent event_type,
                       gpointer user_data);

#endif // CONFIG_H