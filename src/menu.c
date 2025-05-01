#include "menu.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <glib.h>
#include <gtk/gtk.h>
#include "appdata.h"

// Menu item click callback
void on_menu_item_clicked(GtkMenuItem *item, gpointer user_data) {
    const char *cmd = (const char *)user_data;
    if (strcmp(cmd, "quit") == 0) {
        gtk_main_quit();
    } else {
        char command_str[512];
        snprintf(command_str, sizeof(command_str), "%s &", cmd);
        system(command_str);
    }
}

// Creates a GTK menu from a JSON array of menu items
GtkWidget* create_menu_from_json_array(struct json_object *menu_items_array, const char* config_file_path) {
    if (menu_items_array && !json_object_is_type(menu_items_array, json_type_array)) {
        g_printerr("Invalid 'menu_items' array.\n");
        return NULL;
    }

    GtkWidget *menu = gtk_menu_new();
    size_t n_items = 0;
    size_t i;

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

            if (json_object_object_get_ex(json_item, "separator", &separator_obj)) {
                if (json_object_is_type(separator_obj, json_type_boolean)) {
                    is_separator = json_object_get_boolean(separator_obj);
                } else {
                    g_warning("Item at index %zu has 'separator' key but not boolean. Ignoring.", i);
                }
            }

            if (is_separator) {
                GtkWidget *separator = gtk_separator_menu_item_new();
                gtk_menu_shell_append(GTK_MENU_SHELL(menu), separator);
            } else {
                struct json_object *label_obj = json_object_object_get(json_item, "label");
                struct json_object *command_obj = json_object_object_get(json_item, "command");
                struct json_object *icon_obj = json_object_object_get(json_item, "icon");

                const char *label = label_obj ? json_object_get_string(label_obj) : NULL;
                const char *command = command_obj ? json_object_get_string(command_obj) : NULL;
                const char *icon_name = icon_obj ? json_object_get_string(icon_obj) : NULL;

                if (label && command) {
                    GtkWidget *menu_item = NULL;
                    if (icon_name) {
                        GtkWidget *image = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_MENU);
                        if (!image) {
                            g_warning("Could not load icon '%s'. Using text-only item.", icon_name);
                            menu_item = gtk_menu_item_new_with_label(label);
                        } else {
                            menu_item = gtk_menu_item_new();
                            GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
                            GtkWidget *label_widget = gtk_label_new(label);
                            gtk_box_pack_start(GTK_BOX(box), image, FALSE, FALSE, 0);
                            gtk_box_pack_start(GTK_BOX(box), label_widget, FALSE, FALSE, 0);
                            gtk_container_add(GTK_CONTAINER(menu_item), box);
                        }
                    } else {
                        menu_item = gtk_menu_item_new_with_label(label);
                    }

                    if (menu_item) {
                        // Skip "quit" from JSON (hardcoded later)
                        if (strcmp(command, "quit") != 0) {
                            g_signal_connect(menu_item, "activate",
                                             G_CALLBACK(on_menu_item_clicked),
                                             g_strdup(command));
                            gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_item);
                        } else {
                            g_info("Skipping 'quit' command in JSON, it will be added automatically.");
                        }
                    }
                } else {
                    g_printerr("Skipping invalid item at index %zu (missing label/command)\n", i);
                }
            }
        }
    }

    // Add a separator if we have already appended items
    if (gtk_container_get_children(GTK_CONTAINER(menu)) != NULL) {
        GtkWidget *separator = gtk_separator_menu_item_new();
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), separator);
    }

    // Preferences item
    const char *prefs_label = "Preferences";
    const char *prefs_icon_name = "preferences-system";
    char prefs_command[PATH_MAX + 10];
    snprintf(prefs_command, sizeof(prefs_command), "xdg-open %s", config_file_path);

    GtkWidget *prefs_menu_item = NULL;
    GtkWidget *prefs_image = gtk_image_new_from_icon_name(prefs_icon_name, GTK_ICON_SIZE_MENU);

    if (!prefs_image) {
        g_warning("Could not load preferences icon '%s'. Text-only item.", prefs_icon_name);
        prefs_menu_item = gtk_menu_item_new_with_label(prefs_label);
    } else {
        prefs_menu_item = gtk_menu_item_new();
        GtkWidget *prefs_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *prefs_label_widget = gtk_label_new(prefs_label);
        gtk_box_pack_start(GTK_BOX(prefs_box), prefs_image, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(prefs_box), prefs_label_widget, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(prefs_menu_item), prefs_box);
    }
    g_signal_connect(prefs_menu_item, "activate", G_CALLBACK(on_menu_item_clicked), g_strdup(prefs_command));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), prefs_menu_item);

    // Quit item
    const char *quit_label = "Quit";
    GtkWidget *quit_menu_item = gtk_menu_item_new_with_label(quit_label);
    g_signal_connect(quit_menu_item, "activate", G_CALLBACK(on_menu_item_clicked), g_strdup("quit"));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_menu_item);

    gtk_widget_show_all(menu);
    return menu;
}