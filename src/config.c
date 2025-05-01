#include "config.h"
#include <gtk/gtk.h>
#include <json-c/json.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include "menu.h"
#include "utils.h"

// Global default configuration content
const char *default_config_json =
"{\n"
"    \"indicator_icon\": \"media-playback-start\",\n"
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

void show_json_error_dialog(const char *config_file_path, const char *error_message) {
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

    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
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
    GtkWidget *new_menu = NULL;
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
            show_json_error_dialog(data->config_file_path, json_error_str);
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
            show_json_error_dialog(data->config_file_path, json_error_str);
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
    if (!parse_error && parsed_json) {
        g_info("Processing valid JSON configuration...");
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
                new_menu = create_menu_from_json_array(menu_items_array, data->config_file_path);
            } else {
                g_warning("'menu_items' key found but not an array. Creating default menu.");
                new_menu = create_menu_from_json_array(NULL, data->config_file_path);
            }
        } else {
            g_warning("Missing 'menu_items' key. Creating default menu.");
            new_menu = create_menu_from_json_array(NULL, data->config_file_path);
        }
    } else {
        g_warning("Falling back to default icon and minimal menu.");
        allocated_icon_name = g_strdup(default_indicator_icon);
        new_menu = create_menu_from_json_array(NULL, data->config_file_path);
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
    app_indicator_set_icon_full(data->indicator, indicator_icon_name, "Indicator Icon");
    g_info("Set indicator icon to: %s", indicator_icon_name);
    g_free(allocated_icon_name);

    if (data->current_menu) {
        gtk_widget_destroy(data->current_menu);
        g_info("Destroyed old menu.");
    }

    app_indicator_set_menu(data->indicator, GTK_MENU(new_menu));
    data->current_menu = new_menu;
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