#include <gtk/gtk.h>
#include <libappindicator/app-indicator.h>
#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> // For access()
#include <sys/stat.h> // For mkdir()
#include <errno.h> // For errno

typedef struct {
    char *description; // Renamed from 'descricao'
    char *command;     // Renamed from 'comando'
    char *icon;        // Added icon field
} MenuItemData;

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

                const char *description = json_object_get_string(label_obj);
                const char *command = json_object_get_string(command_obj);
                const char *icon_name = json_object_get_string(icon_obj);

                // Check for regular item condition: label and command exist
                if (description && command) {
                    GtkWidget *menu_item = NULL;
                    if (icon_name) {
                        GtkWidget *image = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_MENU);
                        if (!image) {
                            g_warning("Could not load icon named '%s'. Falling back to text-only menu item.", icon_name);
                            menu_item = gtk_menu_item_new_with_label(description);
                        } else {
                            menu_item = gtk_menu_item_new();
                            GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
                            GtkWidget *label = gtk_label_new(description);
                            gtk_box_pack_start(GTK_BOX(box), image, FALSE, FALSE, 0);
                            gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
                            gtk_container_add(GTK_CONTAINER(menu_item), box);
                        }
                    } else {
                        menu_item = gtk_menu_item_new_with_label(description);
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

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);

    GtkSettings *settings = gtk_settings_get_default();
    g_object_set(settings, "gtk-menu-images", TRUE, NULL);

    const char *home_dir = getenv("HOME");
    if (!home_dir) {
        g_printerr("Error: HOME environment variable not set.\n");
        return 1;
    }

    char config_dir_path[PATH_MAX];
    // Check snprintf return value for config_dir_path as well
    int needed_dir = snprintf(config_dir_path, sizeof(config_dir_path), "%s/.config/trayactions", home_dir);
    if (needed_dir < 0 || (size_t)needed_dir >= sizeof(config_dir_path)) {
        g_printerr("Error: Configuration directory path is too long.\n");
        return 1;
    }

    char config_file_path[PATH_MAX];
    // Check snprintf return value for config_file_path
    int needed_file = snprintf(config_file_path, sizeof(config_file_path), "%s/config.json", config_dir_path);
    if (needed_file < 0 || (size_t)needed_file >= sizeof(config_file_path)) {
        g_printerr("Error: Configuration file path is too long.\n");
        return 1;
    }

    // --- Check if config file exists, create default if not ---
    if (access(config_file_path, F_OK) != 0) {
        g_info("Configuration file not found at %s. Creating default.", config_file_path);

        // Ensure the directory exists
        if (ensure_dir_exists(config_dir_path) != 0) {
             g_printerr("Failed to create configuration directory. Exiting.\n");
             return 1;
        }

        // Create the default file
        FILE *default_fp = fopen(config_file_path, "w");
        if (!default_fp) {
            g_printerr("Error creating default config file %s: %s\n", config_file_path, strerror(errno));
            // Proceed without config? Or exit? Let's proceed.
        } else {
            fprintf(default_fp, "%s", default_config_json);
            fclose(default_fp);
            g_info("Default configuration file created at %s.", config_file_path);
        }
    }
    // --- End config check/create ---

    // --- Read and parse config file (using the full path) ---
    const char *default_indicator_icon = "system-run"; // Fallback icon
    const char *indicator_icon_name = default_indicator_icon;
    GtkWidget *menu = NULL;
    struct json_object *parsed_json = NULL;
    struct json_object *menu_items_array = NULL;
    struct json_object *indicator_icon_obj = NULL;

    FILE *fp = fopen(config_file_path, "r"); // Use config_file_path
    if (!fp) {
        g_printerr("Error opening %s\n", config_file_path);
        // Proceed with default icon and empty menu? Or exit? Let's proceed.
    } else {
        fseek(fp, 0, SEEK_END);
        long len = ftell(fp);
        rewind(fp);
        char *data = malloc(len + 1);
        if (data) { // Check malloc result
            fread(data, 1, len, fp);
            data[len] = '\0';
            parsed_json = json_tokener_parse(data);
            free(data);
        }
        fclose(fp);
    }
    // --- End Read and parse ---

    // --- Process parsed JSON ---
    if (parsed_json && json_object_is_type(parsed_json, json_type_object)) {
        // Get indicator icon name
        if (json_object_object_get_ex(parsed_json, "indicator_icon", &indicator_icon_obj)) {
             const char *temp_icon_name = json_object_get_string(indicator_icon_obj);
             if (temp_icon_name && strlen(temp_icon_name) > 0) { // Basic check
                 indicator_icon_name = temp_icon_name; // Use icon from JSON
             } else {
                 g_warning("Invalid or empty 'indicator_icon' in %s. Using default.", config_file_path);
             }
        } else {
             g_warning("Missing 'indicator_icon' key in %s. Using default.", config_file_path);
        }

        // Get menu items array
        if (json_object_object_get_ex(parsed_json, "menu_items", &menu_items_array)) {
             menu = create_menu_from_json_array(menu_items_array, config_file_path); // Pass the array part
        } else {
             g_warning("Missing 'menu_items' key in %s. No menu items will be loaded.", config_file_path);
             menu = create_menu_from_json_array(NULL, config_file_path); // Create empty menu + hardcoded items
        }
    } else {
        g_printerr("Failed to parse %s or it's not a JSON object. Using defaults.\n", config_file_path);
        if (parsed_json) json_object_put(parsed_json); // Free if parsing failed partially
        parsed_json = NULL;
        menu = create_menu_from_json_array(NULL, config_file_path); // Create empty menu + hardcoded items
    }
    // --- End Process parsed JSON ---

    if (!menu) {
        g_printerr("Failed to create menu. Exiting.\n");
         if (parsed_json) json_object_put(parsed_json);
         return 1; // Exit if menu creation fails completely
    }

    AppIndicator *indicator = app_indicator_new(
        "trayactions-indicator",
        indicator_icon_name, // Use the icon name from JSON or default
        APP_INDICATOR_CATEGORY_APPLICATION_STATUS
    );

    app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_menu(indicator, GTK_MENU(menu));

    gtk_main();

    // Free the top-level JSON object when done
    if (parsed_json) {
        json_object_put(parsed_json);
    }

    return 0;
}