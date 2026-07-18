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
void show_json_error_dialog(
    GtkApplication *application,
    const char *config_file_path,
    const char *error_message
);

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

/**
 * Persist indicator_icon while preserving the rest of the config file.
 */
gboolean config_save_indicator_icon(AppData *data, const char *icon_name);

/**
 * Persist app_workspaces while preserving the rest of the config file.
 */
gboolean config_save_app_workspaces(AppData *data);

/**
 * Load user-editable menu_items from the config file (excludes Preferences/Quit).
 * Caller owns the GPtrArray (elements are MenuItemData*).
 */
GPtrArray *config_load_editable_menu_items(AppData *data);

/**
 * Persist editable menu_items while preserving the rest of the config file.
 */
gboolean config_save_menu_items(AppData *data, GPtrArray *items);

/**
 * Free an AppWorkspaceAssignment.
 */
void app_workspace_assignment_free(gpointer pointer);

/**
 * Free a MenuItemData (for editable menu arrays).
 */
void menu_item_data_free(gpointer pointer);

#endif // CONFIG_H
