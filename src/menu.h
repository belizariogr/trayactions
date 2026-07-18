#ifndef MENU_H
#define MENU_H

#include <glib.h>
#include <json-c/json.h>
#include "appdata.h"

GPtrArray *create_menu_from_json_array(
    struct json_object *menu_items_array,
    const char *config_file_path
);

#endif // MENU_H
