#include <gtk/gtk.h>
#include <libappindicator/app-indicator.h>
#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *description; // Renamed from 'descricao'
    char *command;     // Renamed from 'comando'
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

GtkWidget* create_menu_from_json(const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        g_printerr("Error opening %s\n", filename); // Translated error message
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    rewind(fp);

    char *data = malloc(len + 1);
    fread(data, 1, len, fp);
    data[len] = '\0';
    fclose(fp);

    struct json_object *parsed_json;
    struct json_object *json_item; // Renamed from 'item' to be more descriptive
    size_t n_items, i;

    parsed_json = json_tokener_parse(data);
    free(data);

    if (!json_object_is_type(parsed_json, json_type_array)) {
        g_printerr("Invalid JSON\n"); // Translated error message
        json_object_put(parsed_json); // Free JSON object even on error
        return NULL;
    }

    GtkWidget *menu = gtk_menu_new();
    n_items = json_object_array_length(parsed_json);

    for (i = 0; i < n_items; i++) {
        json_item = json_object_array_get_idx(parsed_json, i);
        // Assuming JSON keys remain "descricao" and "comando"
        // If the JSON keys should also be translated, change "descricao" to "description"
        // and "comando" to "command" in the lines below.
        const char *description = json_object_get_string(json_object_object_get(json_item, "label"));
        const char *command = json_object_get_string(json_object_object_get(json_item, "command"));

        if (description && command) { // Check if keys exist and have string values
            GtkWidget *menu_item = gtk_menu_item_new_with_label(description);
            // Duplicate command to pass as user_data
            g_signal_connect(menu_item, "activate", G_CALLBACK(on_menu_item_clicked), g_strdup(command));
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_item);
        } else {
             g_printerr("Skipping invalid menu item at index %zu\n", i); // Added warning for invalid items
        }
    }

    gtk_widget_show_all(menu);
    json_object_put(parsed_json); // Free JSON

    return menu;
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);

    // Assuming the config file name remains "config.json"
    GtkWidget *menu = create_menu_from_json("config.json");
    if (!menu) return 1;

    AppIndicator *indicator = app_indicator_new(
        "json-menu-indicator", // Changed ID slightly
        "face-smile", // Icon name
        APP_INDICATOR_CATEGORY_APPLICATION_STATUS
    );

    app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_menu(indicator, GTK_MENU(menu));

    gtk_main();
    // No need to explicitly free indicator or menu widgets here, GTK handles it on exit.
    return 0;
}
