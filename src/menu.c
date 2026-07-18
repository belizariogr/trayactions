#include "menu.h"

#include <string.h>

static void menu_item_data_free(gpointer pointer) {
    MenuItemData *item = pointer;
    if (!item) {
        return;
    }
    g_free(item->label);
    g_free(item->command);
    g_free(item->icon);
    g_free(item);
}

static void append_menu_item(
    GPtrArray *items,
    gint *next_id,
    const char *label,
    const char *command,
    const char *icon,
    gboolean separator
) {
    MenuItemData *item = g_new0(MenuItemData, 1);
    item->id = (*next_id)++;
    item->label = g_strdup(label);
    item->command = g_strdup(command);
    item->icon = g_strdup(icon);
    item->separator = separator;
    g_ptr_array_add(items, item);
}

GPtrArray *create_menu_from_json_array(
    struct json_object *menu_items_array,
    const char *config_file_path
) {
    GPtrArray *items = g_ptr_array_new_with_free_func(menu_item_data_free);
    gint next_id = 1;

    if (menu_items_array && !json_object_is_type(menu_items_array, json_type_array)) {
        g_warning("Invalid 'menu_items' value; using the built-in menu.");
        menu_items_array = NULL;
    }

    if (menu_items_array) {
        const size_t length = json_object_array_length(menu_items_array);
        for (size_t i = 0; i < length; i++) {
            struct json_object *json_item = json_object_array_get_idx(menu_items_array, i);
            if (!json_object_is_type(json_item, json_type_object)) {
                g_warning("Skipping non-object menu item at index %zu.", i);
                continue;
            }

            struct json_object *value = NULL;
            if (json_object_object_get_ex(json_item, "separator", &value) &&
                json_object_is_type(value, json_type_boolean) &&
                json_object_get_boolean(value)) {
                append_menu_item(items, &next_id, NULL, NULL, NULL, TRUE);
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
                g_warning("Skipping invalid menu item at index %zu (missing label/command).", i);
                continue;
            }

            const char *command = json_object_get_string(command_object);
            if (strcmp(command, "quit") == 0) {
                g_info("Skipping JSON 'quit' item; it is added automatically.");
                continue;
            }

            const char *icon = NULL;
            if (icon_object && json_object_is_type(icon_object, json_type_string)) {
                icon = json_object_get_string(icon_object);
            }
            append_menu_item(
                items,
                &next_id,
                json_object_get_string(label_object),
                command,
                icon,
                FALSE
            );
        }
    }

    if (items->len > 0) {
        append_menu_item(items, &next_id, NULL, NULL, NULL, TRUE);
    }

    char *quoted_path = g_shell_quote(config_file_path);
    char *preferences_command = g_strdup_printf("xdg-open %s", quoted_path);
    append_menu_item(
        items,
        &next_id,
        "Preferences",
        preferences_command,
        "preferences-system",
        FALSE
    );
    append_menu_item(items, &next_id, "Quit", "quit", "application-exit", FALSE);
    g_free(preferences_command);
    g_free(quoted_path);

    return items;
}
