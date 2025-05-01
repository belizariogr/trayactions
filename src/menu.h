#ifndef MENU_H
#define MENU_H

#include <gtk/gtk.h>
#include <json-c/json.h>

/**
 * Callback for menu items (quit or run command).
 */
void on_menu_item_clicked(GtkMenuItem *item, gpointer user_data);

/**
 * Creates a GTK menu from a JSON array of items.
 */
GtkWidget* create_menu_from_json_array(struct json_object *menu_items_array, const char* config_file_path);

#endif // MENU_H