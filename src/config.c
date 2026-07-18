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

// Global default configuration content
const char *default_config_json =
"{\n"
"    \"indicator_icon\": \"media-playback-start\",\n"
"    \"preferences_icon\": \"\",\n"
"    \"quit_icon\": \"\",\n"
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

static gboolean ensure_empty_string_key(struct json_object *root, const char *key) {
    struct json_object *value = NULL;
    if (json_object_object_get_ex(root, key, &value)) {
        return FALSE;
    }
    json_object_object_add(root, key, json_object_new_string(""));
    return TRUE;
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
    reload_configuration(data, FALSE); // Pass FALSE for subsequent reloads
    data->is_reloading = FALSE;
    g_print("Reload flag reset.\n");
    return G_SOURCE_REMOVE; 
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

    if (!parse_error && parsed_json) {
        g_info("Processing valid JSON configuration...");

        gboolean config_updated = FALSE;
        config_updated |= ensure_empty_string_key(parsed_json, "preferences_icon");
        config_updated |= ensure_empty_string_key(parsed_json, "quit_icon");
        if (config_updated) {
            write_config_json(data->config_file_path, parsed_json);
        }

        preferences_icon = read_optional_icon(parsed_json, "preferences_icon");
        quit_icon = read_optional_icon(parsed_json, "quit_icon");

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
    }
    if (parsed_json) {
        json_object_put(parsed_json);
    }

    if (!new_menu) {
        g_critical("Failed to create a menu. Keeping old menu if present.");
        g_free(allocated_icon_name);
        return;
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
    g_info("Set new menu.");
}

void on_config_changed(GFileMonitor *monitor,
                       GFile *file,
                       GFile *other_file,
                       GFileMonitorEvent event_type,
                       gpointer user_data)
{
    AppData *data = (AppData *)user_data;
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
        data->is_reloading = TRUE;
        g_print("Reload flag set.\n");
        g_timeout_add(500, reload_configuration_timeout_cb, data);
    }
}
