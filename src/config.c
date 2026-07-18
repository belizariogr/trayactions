#include "config.h"
#include <gtk/gtk.h>
#include <json-c/json.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include "menu.h"
#include "tray.h"
#include "utils.h"
#include "workspace.h"
#include "preferences.h"

// Global default configuration content
const char *default_config_json =
"{\n"
"    \"indicator_icon\": \"media-playback-start\",\n"
"    \"preferences_icon\": \"\",\n"
"    \"quit_icon\": \"\",\n"
"    \"app_workspaces\": [],\n"
"    \"menu_items\": [\n"
"      {\n"
"        \"label\": \"Open Terminal\",\n"
"        \"command\": \"gnome-terminal\",\n"
"        \"icon\": \"utilities-terminal\"\n"
"      },\n"
"      {\n"
"        \"label\": \"Edit Config File\",\n"
"        \"command\": \"gedit ~/.config/trayactions/config.json\",\n"
"        \"icon\": \"accessories-text-editor\"\n"
"      },\n"
"      {\n"
"        \"separator\": true\n"
"      },\n"
"      {\n"
"        \"label\": \"Show System Monitor\",\n"
"        \"command\": \"gnome-system-monitor\",\n"
"        \"icon\": \"utilities-system-monitor\"\n"
"      }\n"
"    ]\n"
"}\n";

void app_workspace_assignment_free(gpointer pointer) {
    AppWorkspaceAssignment *assignment = pointer;
    if (!assignment) {
        return;
    }
    g_free(assignment->app_id);
    g_free(assignment);
}

void menu_item_data_free(gpointer pointer) {
    MenuItemData *item = pointer;
    if (!item) {
        return;
    }
    g_free(item->label);
    g_free(item->command);
    g_free(item->icon);
    g_clear_pointer(&item->icon_png, g_bytes_unref);
    g_free(item);
}

static gboolean ensure_empty_string_key(struct json_object *root, const char *key) {
    struct json_object *value = NULL;
    if (json_object_object_get_ex(root, key, &value)) {
        return FALSE;
    }
    json_object_object_add(root, key, json_object_new_string(""));
    return TRUE;
}

static gboolean ensure_empty_array_key(struct json_object *root, const char *key) {
    struct json_object *value = NULL;
    if (json_object_object_get_ex(root, key, &value)) {
        return FALSE;
    }
    json_object_object_add(root, key, json_object_new_array());
    return TRUE;
}

static struct json_object *load_config_root(const char *config_file_path) {
    gchar *contents = NULL;
    gsize length = 0;
    GError *error = NULL;
    if (!g_file_get_contents(config_file_path, &contents, &length, &error)) {
        g_warning("Could not read %s: %s", config_file_path, error->message);
        g_error_free(error);
        return NULL;
    }

    struct json_object *root = json_tokener_parse(contents);
    g_free(contents);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) {
            json_object_put(root);
        }
        g_warning("Could not parse %s as a JSON object.", config_file_path);
        return NULL;
    }
    return root;
}

static GPtrArray *parse_app_workspaces(struct json_object *array) {
    GPtrArray *assignments = g_ptr_array_new_with_free_func(app_workspace_assignment_free);
    if (!array || !json_object_is_type(array, json_type_array)) {
        return assignments;
    }

    const size_t length = json_object_array_length(array);
    for (size_t i = 0; i < length; i++) {
        struct json_object *item = json_object_array_get_idx(array, i);
        if (!json_object_is_type(item, json_type_object)) {
            continue;
        }

        struct json_object *app_id_obj = NULL;
        struct json_object *workspace_obj = NULL;
        if (!json_object_object_get_ex(item, "app_id", &app_id_obj) ||
            !json_object_is_type(app_id_obj, json_type_string)) {
            continue;
        }
        if (!json_object_object_get_ex(item, "workspace", &workspace_obj) ||
            !json_object_is_type(workspace_obj, json_type_int)) {
            continue;
        }

        const char *app_id = json_object_get_string(app_id_obj);
        gint workspace = json_object_get_int(workspace_obj);
        if (!app_id || !*app_id || workspace < 1) {
            continue;
        }

        AppWorkspaceAssignment *assignment = g_new0(AppWorkspaceAssignment, 1);
        assignment->app_id = g_strdup(app_id);
        assignment->workspace = workspace;
        g_ptr_array_add(assignments, assignment);
    }

    return assignments;
}

static void write_config_json(const char *config_file_path, struct json_object *root) {
    const char *json_text = json_object_to_json_string_ext(
        root,
        JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_SPACED
    );
    GError *error = NULL;
    if (!g_file_set_contents(config_file_path, json_text, -1, &error)) {
        g_warning("Could not update %s with missing keys: %s", config_file_path, error->message);
        g_error_free(error);
        return;
    }
    g_info("Updated %s with missing configuration keys.", config_file_path);
}

static const char *read_optional_icon(
    struct json_object *root,
    const char *key
) {
    struct json_object *value = NULL;
    if (!json_object_object_get_ex(root, key, &value) ||
        !json_object_is_type(value, json_type_string)) {
        return "";
    }
    return json_object_get_string(value);
}

void show_json_error_dialog(
    GtkApplication *application,
    const char *config_file_path,
    const char *error_message
) {
    GtkWidget *dialog = gtk_message_dialog_new(NULL,
                                               GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                               GTK_MESSAGE_WARNING,
                                               GTK_BUTTONS_OK,
                                               "Invalid Configuration File");
    char *secondary_text = g_strdup_printf(
        "Could not parse the configuration file:\n%s\n\nError: %s\n\n"
        "The application will continue with default or last known settings.",
        config_file_path, error_message
    );
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", secondary_text);
    g_free(secondary_text);

    gtk_window_set_application(GTK_WINDOW(dialog), application);
    g_signal_connect_swapped(dialog, "response", G_CALLBACK(gtk_window_destroy), dialog);
    gtk_window_present(GTK_WINDOW(dialog));
}

gboolean reload_configuration_timeout_cb(gpointer user_data) {
    AppData *data = (AppData *)user_data;
    data->reload_source = 0;
    reload_configuration(data, FALSE);
    data->is_reloading = FALSE;
    g_print("Reload flag reset.\n");
    return G_SOURCE_REMOVE;
}

static gboolean clear_self_write_guard(gpointer user_data) {
    AppData *data = user_data;
    if (data) {
        data->self_write_guard_source = 0;
        data->is_reloading = FALSE;
    }
    return G_SOURCE_REMOVE;
}

/* Prevent the file monitor from reloading over in-memory pointers still
 * owned by an open Preferences window after we write the config ourselves. */
static void guard_self_write(AppData *data) {
    if (!data) {
        return;
    }
    if (data->self_write_guard_source) {
        g_source_remove(data->self_write_guard_source);
        data->self_write_guard_source = 0;
    }
    data->is_reloading = TRUE;
    data->self_write_guard_source = g_timeout_add(750, clear_self_write_guard, data);
}

void reload_configuration(AppData *data, gboolean is_initial_load) {
    g_print("Reloading configuration from %s (Initial Load: %s)\n",
            data->config_file_path, is_initial_load ? "Yes" : "No");

    const char *default_indicator_icon = "system-run";
    const char *indicator_icon_name = NULL;
    gchar *allocated_icon_name = NULL;
    GPtrArray *new_menu = NULL;
    struct json_object *parsed_json = NULL;
    struct json_object *menu_items_array = NULL;
    struct json_object *indicator_icon_obj = NULL;
    gboolean parse_error = FALSE;
    const char *json_error_str = "Unknown parsing error";

    // --- Read and parse config file ---
    FILE *fp = fopen(data->config_file_path, "r");
    if (!fp) {
        g_printerr("Error opening %s for reload: %s\n", data->config_file_path, strerror(errno));
        parse_error = TRUE;
        json_error_str = "Could not open configuration file";
        if (is_initial_load) {
            show_json_error_dialog(data->application, data->config_file_path, json_error_str);
        }
    } else {
        struct json_tokener *tok = json_tokener_new();
        char buffer[1024];
        int bytes_read;
        enum json_tokener_error jerr = json_tokener_continue;

        while ((bytes_read = fread(buffer, 1, sizeof(buffer), fp)) > 0 && jerr == json_tokener_continue) {
            parsed_json = json_tokener_parse_ex(tok, buffer, bytes_read);
            jerr = json_tokener_get_error(tok);
        }
        fclose(fp);

        if (jerr != json_tokener_success) {
            json_error_str = json_tokener_error_desc(jerr);
            g_printerr("Failed to parse JSON from %s: %s\n", data->config_file_path, json_error_str);
            parse_error = TRUE;
            if (parsed_json) {
                json_object_put(parsed_json);
                parsed_json = NULL;
            }
        } else if (!parsed_json || !json_object_is_type(parsed_json, json_type_object)) {
            json_error_str = "File does not contain a valid JSON object at the top level.";
            g_printerr("Error in %s: %s\n", data->config_file_path, json_error_str);
            parse_error = TRUE;
            if (parsed_json) {
                json_object_put(parsed_json);
                parsed_json = NULL;
            }
        }
        json_tokener_free(tok);

        if (parse_error) {
            show_json_error_dialog(data->application, data->config_file_path, json_error_str);
            if (is_initial_load) {
                g_warning("Initial configuration file is invalid. Replacing with default content.");
                FILE *write_fp = fopen(data->config_file_path, "w");
                if (write_fp) {
                    fprintf(write_fp, "%s", default_config_json);
                    fclose(write_fp);
                    g_info("Successfully replaced %s with default configuration.", data->config_file_path);

                    struct json_object *default_parsed_json = json_tokener_parse(default_config_json);
                    if (default_parsed_json && json_object_is_type(default_parsed_json, json_type_object)) {
                        if (parsed_json) {
                            json_object_put(parsed_json);
                        }
                        parsed_json = default_parsed_json;
                        parse_error = FALSE;
                        json_error_str = NULL;
                    } else {
                        g_critical("Failed to parse the internal default_config_json string!");
                        if (default_parsed_json) {
                            json_object_put(default_parsed_json);
                        }
                    }
                } else {
                    g_printerr("Failed to open %s to replace with default: %s",
                               data->config_file_path, strerror(errno));
                }
            }
        }
    }

    // --- Process final JSON (parsed or fallback) ---
    const char *preferences_icon = "";
    const char *quit_icon = "";
    GPtrArray *new_assignments = NULL;
    gboolean built_assignments = FALSE;

    if (!parse_error && parsed_json) {
        g_info("Processing valid JSON configuration...");

        gboolean config_updated = FALSE;
        config_updated |= ensure_empty_string_key(parsed_json, "preferences_icon");
        config_updated |= ensure_empty_string_key(parsed_json, "quit_icon");
        config_updated |= ensure_empty_array_key(parsed_json, "app_workspaces");
        if (config_updated) {
            write_config_json(data->config_file_path, parsed_json);
        }

        preferences_icon = read_optional_icon(parsed_json, "preferences_icon");
        quit_icon = read_optional_icon(parsed_json, "quit_icon");

        struct json_object *app_workspaces_obj = NULL;
        if (json_object_object_get_ex(parsed_json, "app_workspaces", &app_workspaces_obj)) {
            new_assignments = parse_app_workspaces(app_workspaces_obj);
        } else {
            new_assignments = g_ptr_array_new_with_free_func(app_workspace_assignment_free);
        }
        built_assignments = TRUE;

        if (json_object_object_get_ex(parsed_json, "indicator_icon", &indicator_icon_obj)) {
            const char *temp_icon_name = json_object_get_string(indicator_icon_obj);
            if (temp_icon_name && strlen(temp_icon_name) > 0) {
                allocated_icon_name = g_strdup(temp_icon_name);
            } else {
                g_warning("Invalid or empty 'indicator_icon' in config '%s'. Using default.", data->config_file_path);
                allocated_icon_name = g_strdup(default_indicator_icon);
            }
        } else {
            g_warning("Missing 'indicator_icon' key in config '%s'. Using default.", data->config_file_path);
            allocated_icon_name = g_strdup(default_indicator_icon);
        }

        if (json_object_object_get_ex(parsed_json, "menu_items", &menu_items_array)) {
            if (json_object_is_type(menu_items_array, json_type_array)) {
                new_menu = create_menu_from_json_array(
                    menu_items_array,
                    data->config_file_path,
                    preferences_icon,
                    quit_icon
                );
            } else {
                g_warning("'menu_items' key found but not an array. Creating default menu.");
                new_menu = create_menu_from_json_array(
                    NULL,
                    data->config_file_path,
                    preferences_icon,
                    quit_icon
                );
            }
        } else {
            g_warning("Missing 'menu_items' key. Creating default menu.");
            new_menu = create_menu_from_json_array(
                NULL,
                data->config_file_path,
                preferences_icon,
                quit_icon
            );
        }
    } else {
        g_warning("Falling back to default icon and minimal menu.");
        allocated_icon_name = g_strdup(default_indicator_icon);
        new_menu = create_menu_from_json_array(
            NULL,
            data->config_file_path,
            preferences_icon,
            quit_icon
        );
        if (!data->app_workspaces) {
            new_assignments = g_ptr_array_new_with_free_func(app_workspace_assignment_free);
            built_assignments = TRUE;
        }
    }
    if (parsed_json) {
        json_object_put(parsed_json);
    }

    if (!new_menu) {
        g_critical("Failed to create a menu. Keeping old menu if present.");
        g_free(allocated_icon_name);
        if (built_assignments) {
            g_ptr_array_unref(new_assignments);
        }
        return;
    }

    /* Commit icon, menu, and assignments together after all builds succeed. */
    if (built_assignments) {
        g_clear_pointer(&data->app_workspaces, g_ptr_array_unref);
        data->app_workspaces = new_assignments;
        workspace_watcher_refresh(data);
        preferences_reload_app_bindings(data);
    }

    indicator_icon_name = allocated_icon_name ? allocated_icon_name : default_indicator_icon;
    g_free(data->indicator_icon);
    data->indicator_icon = g_strdup(indicator_icon_name);
    tray_notify_icon_changed(data);
    g_info("Set indicator icon to: %s", indicator_icon_name);
    g_free(allocated_icon_name);

    if (data->menu_items) {
        g_ptr_array_unref(data->menu_items);
    }

    data->menu_items = new_menu;
    tray_notify_menu_changed(data);
    preferences_reload_menu_items(data);
    g_info("Set new menu.");
}

void on_config_changed(GFileMonitor *monitor,
                       GFile *file,
                       GFile *other_file,
                       GFileMonitorEvent event_type,
                       gpointer user_data)
{
    AppData *data = (AppData *)user_data;
    (void)monitor;
    (void)file;
    (void)other_file;
    if (data->is_reloading) {
        g_print("Config change detected, but reload already in progress. Ignoring.\n");
        return;
    }

    if (event_type == G_FILE_MONITOR_EVENT_CHANGED ||
        event_type == G_FILE_MONITOR_EVENT_CREATED ||
        event_type == G_FILE_MONITOR_EVENT_DELETED ||
        event_type == G_FILE_MONITOR_EVENT_MOVED)
    {
        g_print("Config file change detected (event type: %d). Scheduling reload.\n", event_type);
        if (data->reload_source) {
            g_source_remove(data->reload_source);
            data->reload_source = 0;
        }
        data->is_reloading = TRUE;
        g_print("Reload flag set.\n");
        data->reload_source = g_timeout_add(500, reload_configuration_timeout_cb, data);
    }
}

static void sync_desktop_icon(const char *icon_name) {
    if (!icon_name || !*icon_name) {
        return;
    }

    char *desktop_path = g_build_filename(
        g_get_user_data_dir(),
        "applications",
        "trayactions.desktop",
        NULL
    );
    if (!g_file_test(desktop_path, G_FILE_TEST_IS_REGULAR)) {
        g_free(desktop_path);
        return;
    }

    GError *error = NULL;
    GKeyFile *key_file = g_key_file_new();
    if (!g_key_file_load_from_file(
            key_file,
            desktop_path,
            G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS,
            &error
        )) {
        g_warning(
            "Could not load desktop file '%s': %s",
            desktop_path,
            error->message
        );
        g_clear_error(&error);
        g_key_file_unref(key_file);
        g_free(desktop_path);
        return;
    }

    g_key_file_set_string(
        key_file,
        G_KEY_FILE_DESKTOP_GROUP,
        G_KEY_FILE_DESKTOP_KEY_ICON,
        icon_name
    );

    if (!g_key_file_save_to_file(key_file, desktop_path, &error)) {
        g_warning(
            "Could not update desktop icon in '%s': %s",
            desktop_path,
            error->message
        );
        g_clear_error(&error);
    } else {
        g_info("Updated desktop icon to '%s' in %s", icon_name, desktop_path);
    }

    g_key_file_unref(key_file);
    g_free(desktop_path);
}

gboolean config_save_indicator_icon(AppData *data, const char *icon_name) {
    if (!data || !data->config_file_path || !icon_name) {
        return FALSE;
    }

    struct json_object *root = load_config_root(data->config_file_path);
    if (!root) {
        return FALSE;
    }

    guard_self_write(data);
    json_object_object_add(root, "indicator_icon", json_object_new_string(icon_name));
    write_config_json(data->config_file_path, root);
    json_object_put(root);
    sync_desktop_icon(icon_name);
    tray_notify_icon_changed(data);
    return TRUE;
}

gboolean config_save_app_workspaces(AppData *data) {
    if (!data || !data->config_file_path) {
        return FALSE;
    }

    struct json_object *root = load_config_root(data->config_file_path);
    if (!root) {
        return FALSE;
    }

    struct json_object *array = json_object_new_array();
    if (data->app_workspaces) {
        for (guint i = 0; i < data->app_workspaces->len; i++) {
            AppWorkspaceAssignment *assignment = g_ptr_array_index(data->app_workspaces, i);
            if (!assignment || !assignment->app_id) {
                continue;
            }
            struct json_object *item = json_object_new_object();
            json_object_object_add(item, "app_id", json_object_new_string(assignment->app_id));
            json_object_object_add(item, "workspace", json_object_new_int(assignment->workspace));
            json_object_array_add(array, item);
        }
    }
    guard_self_write(data);
    json_object_object_add(root, "app_workspaces", array);
    write_config_json(data->config_file_path, root);
    json_object_put(root);
    workspace_watcher_refresh(data);
    return TRUE;
}

GPtrArray *config_load_editable_menu_items(AppData *data) {
    GPtrArray *items = g_ptr_array_new_with_free_func(menu_item_data_free);
    if (!data || !data->config_file_path) {
        return items;
    }

    struct json_object *root = load_config_root(data->config_file_path);
    if (!root) {
        return items;
    }

    struct json_object *array = NULL;
    if (!json_object_object_get_ex(root, "menu_items", &array) ||
        !json_object_is_type(array, json_type_array)) {
        json_object_put(root);
        return items;
    }

    const size_t length = json_object_array_length(array);
    for (size_t i = 0; i < length; i++) {
        struct json_object *json_item = json_object_array_get_idx(array, i);
        if (!json_object_is_type(json_item, json_type_object)) {
            continue;
        }

        MenuItemData *item = g_new0(MenuItemData, 1);
        struct json_object *value = NULL;
        if (json_object_object_get_ex(json_item, "separator", &value) &&
            json_object_is_type(value, json_type_boolean) &&
            json_object_get_boolean(value)) {
            item->separator = TRUE;
            g_ptr_array_add(items, item);
            continue;
        }

        struct json_object *label_object = NULL;
        struct json_object *command_object = NULL;
        struct json_object *icon_object = NULL;
        json_object_object_get_ex(json_item, "label", &label_object);
        json_object_object_get_ex(json_item, "command", &command_object);
        json_object_object_get_ex(json_item, "icon", &icon_object);

        if (!label_object || !json_object_is_type(label_object, json_type_string) ||
            !command_object || !json_object_is_type(command_object, json_type_string)) {
            g_free(item);
            continue;
        }

        const char *command = json_object_get_string(command_object);
        if (strcmp(command, "quit") == 0 || strcmp(command, "preferences") == 0) {
            g_free(item);
            continue;
        }

        item->label = g_strdup(json_object_get_string(label_object));
        item->command = g_strdup(command);
        if (icon_object && json_object_is_type(icon_object, json_type_string)) {
            const char *icon = json_object_get_string(icon_object);
            if (icon && *icon) {
                item->icon = g_strdup(icon);
            }
        }
        g_ptr_array_add(items, item);
    }

    json_object_put(root);
    return items;
}

gboolean config_save_menu_items(AppData *data, GPtrArray *items) {
    if (!data || !data->config_file_path) {
        return FALSE;
    }

    struct json_object *root = load_config_root(data->config_file_path);
    if (!root) {
        return FALSE;
    }

    struct json_object *array = json_object_new_array();
    if (items) {
        for (guint i = 0; i < items->len; i++) {
            MenuItemData *item = g_ptr_array_index(items, i);
            if (!item) {
                continue;
            }
            struct json_object *json_item = json_object_new_object();
            if (item->separator) {
                json_object_object_add(json_item, "separator", json_object_new_boolean(TRUE));
            } else {
                json_object_object_add(
                    json_item,
                    "label",
                    json_object_new_string(item->label ? item->label : "")
                );
                json_object_object_add(
                    json_item,
                    "command",
                    json_object_new_string(item->command ? item->command : "")
                );
                json_object_object_add(
                    json_item,
                    "icon",
                    json_object_new_string(item->icon ? item->icon : "")
                );
            }
            json_object_array_add(array, json_item);
        }
    }
    guard_self_write(data);
    json_object_object_add(root, "menu_items", array);
    write_config_json(data->config_file_path, root);
    json_object_put(root);
    /* Refresh live tray immediately — file-monitor reload is suppressed by the guard. */
    reload_configuration(data, FALSE);
    return TRUE;
}
