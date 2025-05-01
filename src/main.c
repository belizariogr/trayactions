#include <gtk/gtk.h>
#include <libappindicator/app-indicator.h>
#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> // For access()
#include <sys/stat.h> // For mkdir()
#include <errno.h> // For errno
#include <gio/gio.h> // <-- Add GIO for file monitoring
#include <limits.h> // <-- Add for PATH_MAX (might be implicitly included, but good practice)
#include <glib.h>   // <-- Add for gboolean and GTimeVal if needed later

typedef struct {
    char *label;       // Renamed from description
    char *command;
    char *icon;
} MenuItemData;

typedef struct {
    AppIndicator *indicator;
    char *config_file_path;
    GFileMonitor *monitor; // To keep track of the monitor
    GtkWidget *current_menu; // To keep track of the current menu widget
    gboolean is_reloading; // Flag to prevent rapid/recursive reloads
} AppData;

// Forward declaration
static void reload_configuration(AppData *data, gboolean is_initial_load); // <-- Add flag
static gboolean reload_configuration_timeout_cb(gpointer user_data); // Timeout callback wrapper

static void on_menu_item_clicked(GtkMenuItem *item, gpointer user_data) {
    const char *cmd = (const char *)user_data;
    if (strcmp(cmd, "quit") == 0) {
        gtk_main_quit();
    } else {
        // Execute the command in the background
        char command_str[512]; // Renamed from 'command' to avoid conflict with the struct field name if used locally
        snprintf(command_str, sizeof(command_str), "%s &", cmd);
        system(command_str);
    }
}

// Default configuration content
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

// Helper function to create directories if they don't exist
int ensure_dir_exists(const char *path) {
    char tmp[PATH_MAX]; // Use PATH_MAX from limits.h (implicitly included?) or define manually
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp),"%s",path);
    len = strlen(tmp);
    if(tmp[len - 1] == '/') // Remove trailing slash
        tmp[len - 1] = 0;
    for(p = tmp + 1; *p; p++) { // Start after the initial '/' if absolute path
        if(*p == '/') {
            *p = 0; // Temporarily truncate
            // Use stat to check if directory exists
            struct stat st = {0};
            if (stat(tmp, &st) == -1) {
                 // Directory doesn't exist, try to create it
                 if (mkdir(tmp, 0700) != 0 && errno != EEXIST) { // Create with rwx------ permissions
                     perror("mkdir error");
                     fprintf(stderr, "Failed to create directory: %s\n", tmp);
                     return -1; // Error
                 }
            } else if (!S_ISDIR(st.st_mode)) {
                 fprintf(stderr, "Error: %s exists but is not a directory\n", tmp);
                 return -1; // Path exists but is not a directory
            }
            *p = '/'; // Restore slash
        }
    }
    // Check/Create the final directory component
    struct stat st = {0};
    if (stat(tmp, &st) == -1) {
        if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
             perror("mkdir error");
             fprintf(stderr, "Failed to create directory: %s\n", tmp);
             return -1;
        }
    } else if (!S_ISDIR(st.st_mode)) {
         fprintf(stderr, "Error: %s exists but is not a directory\n", tmp);
         return -1;
    }
    return 0; // Success
}

// --- Function to show warning dialog ---
static void show_json_error_dialog(const char *config_file_path, const char *error_message) {
    GtkWidget *dialog = gtk_message_dialog_new(NULL, // No parent window
                                               GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                               GTK_MESSAGE_WARNING,
                                               GTK_BUTTONS_OK,
                                               "Invalid Configuration File");
    char *secondary_text = g_strdup_printf("Could not parse the configuration file:\n%s\n\nError: %s\n\nThe application will continue with default or last known settings.",
                                           config_file_path, error_message);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", secondary_text);
    g_free(secondary_text);

    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

GtkWidget* create_menu_from_json_array(struct json_object *menu_items_array, const char* config_file_path) {
    if (!menu_items_array || !json_object_is_type(menu_items_array, json_type_array)) {
         g_printerr("Invalid menu items array provided.\n");
         return NULL;
    }

    GtkWidget *menu = gtk_menu_new();
    size_t n_items = 0; // Initialize n_items
    size_t i;

    // Process items from JSON if the array is valid
    if (menu_items_array && json_object_is_type(menu_items_array, json_type_array)) {
        n_items = json_object_array_length(menu_items_array);
        for (i = 0; i < n_items; i++) {
            struct json_object *json_item = json_object_array_get_idx(menu_items_array, i);
            if (!json_object_is_type(json_item, json_type_object)) {
                 g_printerr("Skipping non-object item at index %zu\n", i);
                 continue;
            }

            struct json_object *separator_obj = NULL;
            gboolean is_separator = FALSE;

            // Check for separator key first
            if (json_object_object_get_ex(json_item, "separator", &separator_obj)) {
                if (json_object_is_type(separator_obj, json_type_boolean)) {
                    is_separator = json_object_get_boolean(separator_obj);
                } else {
                    g_warning("Item at index %zu has 'separator' key but it's not a boolean. Ignoring.", i);
                }
            }

            if (is_separator) {
                // It's a separator item
                GtkWidget *separator = gtk_separator_menu_item_new();
                gtk_menu_shell_append(GTK_MENU_SHELL(menu), separator);
            } else {
                // It's a regular item (or invalid), try getting label, command, icon
                struct json_object *label_obj = json_object_object_get(json_item, "label");
                struct json_object *command_obj = json_object_object_get(json_item, "command");
                struct json_object *icon_obj = json_object_object_get(json_item, "icon");

                const char *label = json_object_get_string(label_obj); // Renamed variable
                const char *command = json_object_get_string(command_obj);
                const char *icon_name = json_object_get_string(icon_obj);

                // Check for regular item condition: label and command exist
                if (label && command) { // Use renamed variable
                    GtkWidget *menu_item = NULL;
                    if (icon_name) {
                        GtkWidget *image = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_MENU);
                        if (!image) {
                            g_warning("Could not load icon named '%s'. Falling back to text-only menu item.", icon_name);
                            menu_item = gtk_menu_item_new_with_label(label); // Use renamed variable
                        } else {
                            menu_item = gtk_menu_item_new();
                            GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
                            GtkWidget *label_widget = gtk_label_new(label); // Use renamed variable
                            gtk_box_pack_start(GTK_BOX(box), image, FALSE, FALSE, 0);
                            gtk_box_pack_start(GTK_BOX(box), label_widget, FALSE, FALSE, 0); // Use renamed variable
                            gtk_container_add(GTK_CONTAINER(menu_item), box);
                        }
                    } else {
                        menu_item = gtk_menu_item_new_with_label(label); // Use renamed variable
                    }

                    if (menu_item) {
                        // *** IMPORTANT: Check if command is "quit" - skip if so, as it will be hardcoded ***
                        if (strcmp(command, "quit") != 0) {
                            g_signal_connect(menu_item, "activate", G_CALLBACK(on_menu_item_clicked), g_strdup(command));
                            gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_item);
                        } else {
                            g_info("Skipping 'quit' command from JSON, it will be added automatically.");
                        }
                    }
                } else {
                    // Handle invalid regular items (missing label or command)
                    g_printerr("Skipping invalid menu item at index %zu (not a separator and missing/invalid label or command)\n", i);
                }
            }
        } // End for loop
    } else if (menu_items_array) {
         // Handle case where menu_items_array exists but is not an array
         g_printerr("Invalid 'menu_items' format: Expected an array.\n");
    }
    // If menu_items_array is NULL, we just proceed to add the Quit item to an empty menu

    // --- Add hardcoded Preferences and Quit items ---

    // Add a separator before Preferences/Quit if there were other items
    if (gtk_container_get_children(GTK_CONTAINER(menu)) != NULL) { // Check if menu is not empty
         GtkWidget *separator = gtk_separator_menu_item_new();
         gtk_menu_shell_append(GTK_MENU_SHELL(menu), separator);
    }

    // --- Create Preferences item ---
    const char *prefs_label = "Preferences";
    const char *prefs_icon_name = "preferences-system";
    // Construct the command dynamically
    char prefs_command[PATH_MAX + 10]; // "xdg-open " + path + null
    snprintf(prefs_command, sizeof(prefs_command), "xdg-open %s", config_file_path);

    GtkWidget *prefs_menu_item = NULL;
    GtkWidget *prefs_image = gtk_image_new_from_icon_name(prefs_icon_name, GTK_ICON_SIZE_MENU);

    if (!prefs_image) {
        g_warning("Could not load preferences icon '%s'. Creating text-only Preferences item.", prefs_icon_name);
        prefs_menu_item = gtk_menu_item_new_with_label(prefs_label);
    } else {
        prefs_menu_item = gtk_menu_item_new();
        GtkWidget *prefs_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *prefs_label_widget = gtk_label_new(prefs_label);
        gtk_box_pack_start(GTK_BOX(prefs_box), prefs_image, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(prefs_box), prefs_label_widget, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(prefs_menu_item), prefs_box);
    }
    // Connect signal for Preferences item
    g_signal_connect(prefs_menu_item, "activate", G_CALLBACK(on_menu_item_clicked), g_strdup(prefs_command)); // Use dynamic command
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), prefs_menu_item);
    // --- End Preferences item ---

    // --- Create Quit item (no icon) ---
    const char *quit_label = "Quit";
    GtkWidget *quit_menu_item = gtk_menu_item_new_with_label(quit_label);

    // Connect signal for Quit item
    g_signal_connect(quit_menu_item, "activate", G_CALLBACK(on_menu_item_clicked), g_strdup("quit"));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_menu_item);
    // --- End hardcoded Quit item ---

    gtk_widget_show_all(menu);
    return menu;
}

// --- Timeout Callback Wrapper ---
static gboolean reload_configuration_timeout_cb(gpointer user_data) {
    AppData *data = (AppData *)user_data;
    reload_configuration(data, FALSE); // <-- Pass FALSE for subsequent reloads
    // Reset the flag *after* reload_configuration has finished
    data->is_reloading = FALSE;
    g_print("Reload flag reset.\n");
    return G_SOURCE_REMOVE; // Same as returning FALSE, ensures timeout doesn't repeat
}

// --- Reload Function ---
static void reload_configuration(AppData *data, gboolean is_initial_load) {
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
        // File doesn't exist or isn't readable.
        // If initial load, main() should have created it. If fopen fails now, log error.
        // If subsequent reload, log error. In both cases, proceed to use defaults.
        g_printerr("Error opening %s for reload: %s\n", data->config_file_path, strerror(errno));
        // No JSON parse error here, but we treat it like one for fallback purposes.
        parse_error = TRUE;
        json_error_str = "Could not open configuration file";
        // If it's the initial load, we might still want to show a dialog?
        // Let's show the dialog if it's an initial load and the file can't be opened *after* main tried to ensure it exists.
        if (is_initial_load) {
             show_json_error_dialog(data->config_file_path, json_error_str);
             // We won't try to write the default file here, as we couldn't even open it for reading.
        }
    } else {
        // --- Try parsing the existing file ---
        struct json_tokener *tok = json_tokener_new();
        char buffer[1024];
        int bytes_read;
        enum json_tokener_error jerr = json_tokener_continue;

        while ((bytes_read = fread(buffer, 1, sizeof(buffer), fp)) > 0 && jerr == json_tokener_continue) {
            parsed_json = json_tokener_parse_ex(tok, buffer, bytes_read);
            jerr = json_tokener_get_error(tok);
        }
        fclose(fp); // Close file after reading

        if (jerr != json_tokener_success) {
            json_error_str = json_tokener_error_desc(jerr);
            g_printerr("Failed to parse JSON from %s: %s\n", data->config_file_path, json_error_str);
            parse_error = TRUE;
            if (parsed_json) { json_object_put(parsed_json); parsed_json = NULL; }
        } else if (!parsed_json || !json_object_is_type(parsed_json, json_type_object)) {
            json_error_str = "File does not contain a valid JSON object at the top level.";
            g_printerr("Error in %s: %s\n", data->config_file_path, json_error_str);
            parse_error = TRUE;
            if (parsed_json) { json_object_put(parsed_json); parsed_json = NULL; }
        }
        json_tokener_free(tok);
        // --- End JSON parsing attempt ---

        // --- Handle parse error (if any) ---
        if (parse_error) {
            // Show dialog about the error
            show_json_error_dialog(data->config_file_path, json_error_str);

            // *** If it's the initial load, replace the invalid file AND try parsing the default string ***
            if (is_initial_load) {
                g_warning("Initial configuration file is invalid. Replacing with default content.");
                FILE *write_fp = fopen(data->config_file_path, "w");
                if (write_fp) {
                    fprintf(write_fp, "%s", default_config_json);
                    fclose(write_fp);
                    g_info("Successfully replaced %s with default configuration.", data->config_file_path);

                    // Now, try to parse the default string we just wrote
                    g_info("Attempting to parse the newly written default configuration...");
                    struct json_object *default_parsed_json = json_tokener_parse(default_config_json);

                    if (default_parsed_json && json_object_is_type(default_parsed_json, json_type_object)) {
                        g_info("Successfully parsed default configuration string.");
                        // Discard any partially parsed object from the failed attempt
                        if (parsed_json) {
                            json_object_put(parsed_json);
                        }
                        // Use the newly parsed default JSON object
                        parsed_json = default_parsed_json;
                        // Clear the error flag so we proceed with processing this default JSON
                        parse_error = FALSE;
                        json_error_str = NULL;
                    } else {
                        // This should NOT happen if default_config_json is valid!
                        g_critical("Failed to parse the internal default_config_json string! Check the string syntax.");
                        if (default_parsed_json) { // Free if parsing failed but returned something
                             json_object_put(default_parsed_json);
                        }
                        // Keep parse_error = TRUE, will fall back to NULL menu array later
                    }
                } else {
                    g_printerr("Failed to open %s for writing to replace with default: %s",
                               data->config_file_path, strerror(errno));
                    // Keep parse_error = TRUE, will fall back to NULL menu array later
                }
            }
            // If not initial load, or if writing/parsing the default failed, parse_error remains TRUE.
        }
    } // End of else block for successful fopen()

    // --- Process JSON (either originally parsed, or the default parsed after initial error) ---
    if (!parse_error && parsed_json) {
        g_info("Processing valid JSON configuration...");
        // Get indicator icon name
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

        // Get menu items array
        if (json_object_object_get_ex(parsed_json, "menu_items", &menu_items_array)) {
             if (json_object_is_type(menu_items_array, json_type_array)) {
                 new_menu = create_menu_from_json_array(menu_items_array, data->config_file_path);
             } else {
                 g_warning("'menu_items' key found in config '%s' but it's not an array. Creating default menu.", data->config_file_path);
                 new_menu = create_menu_from_json_array(NULL, data->config_file_path);
             }
        } else {
             g_warning("Missing 'menu_items' key in config '%s'. Creating default menu.", data->config_file_path);
             new_menu = create_menu_from_json_array(NULL, data->config_file_path);
        }
    } else {
        // Fallback: Parsing failed and wasn't recovered by loading defaults, or fopen failed initially.
        g_warning("Falling back to hardcoded default icon and empty menu items array.");
        allocated_icon_name = g_strdup(default_indicator_icon);
        // Pass NULL to create_menu_from_json_array which should handle it gracefully
        // (e.g., create menu with only Quit/Preferences).
        new_menu = create_menu_from_json_array(NULL, data->config_file_path);
    }

    // Free the parsed JSON object (whether original or the parsed default)
    if (parsed_json) {
        json_object_put(parsed_json);
        parsed_json = NULL;
    }
    // --- End JSON Processing ---

    // --- Update Indicator ---
    indicator_icon_name = allocated_icon_name ? allocated_icon_name : default_indicator_icon;

    if (!new_menu) {
        // This happens if create_menu_from_json_array returns NULL even when passed NULL.
        g_critical("CRITICAL: Failed to create even a default menu structure. Keeping old menu if possible.");
        g_free(allocated_icon_name);
        return;
    }

    // Update the icon
    app_indicator_set_icon_full(data->indicator, indicator_icon_name, "Indicator Icon");
    g_info("Set indicator icon to: %s", indicator_icon_name);
    g_free(allocated_icon_name); // Free the duplicated icon name

    // Destroy the old menu *before* setting the new one
    if (data->current_menu) {
        gtk_widget_destroy(data->current_menu);
        g_info("Destroyed old menu.");
    }

    // Set the new menu
    app_indicator_set_menu(data->indicator, GTK_MENU(new_menu));
    data->current_menu = new_menu; // Update the pointer to the current menu
    g_info("Set new menu.");
}
// --- End Reload Function ---

// --- File Monitor Callback ---
static void on_config_changed(GFileMonitor *monitor,
                              GFile *file,
                              GFile *other_file,
                              GFileMonitorEvent event_type,
                              gpointer user_data)
{
    AppData *data = (AppData *)user_data;

    // Check if a reload is already in progress or scheduled
    if (data->is_reloading) {
        g_print("Config change detected, but reload already in progress/scheduled. Ignoring.\n");
        return;
    }

    // We are interested in changes, creation, deletion, and moves
    if (event_type == G_FILE_MONITOR_EVENT_CHANGED ||
        event_type == G_FILE_MONITOR_EVENT_CREATED ||
        event_type == G_FILE_MONITOR_EVENT_DELETED ||
        event_type == G_FILE_MONITOR_EVENT_MOVED)
    {
        g_print("Config file change detected (event type: %d). Scheduling reload.\n", event_type);

        // Set the flag *before* scheduling the timeout
        data->is_reloading = TRUE;
        g_print("Reload flag set.\n");

        // Add a small delay to avoid rapid reloads if the editor saves in multiple steps
        // Use the wrapper function for the timeout callback
        g_timeout_add(500, reload_configuration_timeout_cb, data); // 500ms delay
    }
}
// --- End File Monitor Callback ---

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);

    GtkSettings *settings = gtk_settings_get_default();
    g_object_set(settings, "gtk-menu-images", TRUE, NULL);

    const char *home_dir = getenv("HOME");
    if (!home_dir) {
        g_printerr("Error: HOME environment variable not set.\n");
        return 1;
    }

    // Use g_build_filename for safer path construction
    char *config_dir_path = g_build_filename(home_dir, ".config", "trayactions", NULL);
    char *config_file_path = g_build_filename(config_dir_path, "config.json", NULL);

    // --- Check if config file exists, create default if not ---
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
            // Decide how to handle this - maybe exit? For now, continue.
        } else {
            fprintf(default_fp, "%s", default_config_json);
            fclose(default_fp);
            g_info("Default configuration file created at %s.", config_file_path);
        }
    }
    // --- End config check/create ---

    // --- Create AppData ---
    AppData app_data = {0}; // Initialize struct members to NULL/0/FALSE
    app_data.config_file_path = g_strdup(config_file_path); // Store a copy
    app_data.is_reloading = FALSE; // Explicitly initialize flag

    // --- Create Indicator (initially with a placeholder icon) ---
    app_data.indicator = app_indicator_new(
        "trayactions-indicator",
        "system-run", // Placeholder, will be updated by reload_configuration
        APP_INDICATOR_CATEGORY_APPLICATION_STATUS
    );
    app_indicator_set_status(app_data.indicator, APP_INDICATOR_STATUS_ACTIVE);

    // --- Initial Configuration Load ---
    reload_configuration(&app_data, TRUE); // Pass TRUE for the initial load

    // --- Setup File Monitoring ---
    GFile *config_gfile = g_file_new_for_path(app_data.config_file_path);
    GError *error = NULL;
    app_data.monitor = g_file_monitor_file(config_gfile, G_FILE_MONITOR_NONE, NULL, &error);

    if (!app_data.monitor) {
        g_printerr("Error creating file monitor for %s: %s\n", app_data.config_file_path, error->message);
        g_error_free(error);
        // Continue without monitoring? Or exit? Let's continue.
    } else {
        g_signal_connect(app_data.monitor, "changed", G_CALLBACK(on_config_changed), &app_data);
        g_print("Monitoring %s for changes...\n", app_data.config_file_path);
    }
    g_object_unref(config_gfile); // Unref the GFile, monitor holds its own reference

    // --- Run Main Loop ---
    gtk_main();

    // --- Cleanup ---
    g_print("Exiting...\n");
    if (app_data.monitor) {
        g_file_monitor_cancel(app_data.monitor); // Stop monitoring
        g_object_unref(app_data.monitor);        // Release monitor reference
    }
    if (app_data.current_menu) {
        // The menu should be owned by the indicator, but explicit destroy might be safer
        // gtk_widget_destroy(app_data.current_menu); // Or let GTK handle it via indicator cleanup
    }
    g_free(app_data.config_file_path);
    // Indicator is likely cleaned up by GTK, but g_object_unref might be needed if not parented.
    // g_object_unref(app_data.indicator);

    g_free(config_dir_path);
    g_free(config_file_path); // Free paths allocated by g_build_filename

    return 0;
}