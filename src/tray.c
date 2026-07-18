#include "tray.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gtk/gtk.h>
#include <string.h>

/* Panel menus typically use 16–24px icons. */
static const int MENU_ICON_SIZE = 22;

/*
 * Hosts that render DBusMenu often treat icon-name as symbolic and recolor it
 * (e.g. an all-white utilities-terminal). Load the regular theme icon ourselves
 * and ship PNG bytes via icon-data so the host shows the same full-color art as
 * Nautilus and other GTK apps.
 */
static GBytes *load_theme_icon_png(const char *icon_name) {
    if (!icon_name || !*icon_name) {
        return NULL;
    }

    GdkDisplay *display = gdk_display_get_default();
    if (!display) {
        return NULL;
    }

    GtkIconTheme *theme = gtk_icon_theme_get_for_display(display);
    GtkIconPaintable *paintable = gtk_icon_theme_lookup_icon(
        theme,
        icon_name,
        NULL,
        MENU_ICON_SIZE,
        1,
        GTK_TEXT_DIR_NONE,
        GTK_ICON_LOOKUP_FORCE_REGULAR
    );
    if (!paintable) {
        return NULL;
    }

    GdkPixbuf *pixbuf = NULL;
    GFile *file = gtk_icon_paintable_get_file(paintable);
    if (file) {
        GFileInputStream *stream = g_file_read(file, NULL, NULL);
        if (stream) {
            pixbuf = gdk_pixbuf_new_from_stream_at_scale(
                G_INPUT_STREAM(stream),
                MENU_ICON_SIZE,
                MENU_ICON_SIZE,
                TRUE,
                NULL,
                NULL
            );
            g_object_unref(stream);
        }
        g_object_unref(file);
    }
    g_object_unref(paintable);

    if (!pixbuf) {
        return NULL;
    }

    gchar *png_buffer = NULL;
    gsize png_size = 0;
    GError *error = NULL;
    if (!gdk_pixbuf_save_to_buffer(pixbuf, &png_buffer, &png_size, "png", &error, NULL)) {
        g_warning("Could not encode icon '%s' as PNG: %s", icon_name, error->message);
        g_error_free(error);
        g_object_unref(pixbuf);
        return NULL;
    }
    g_object_unref(pixbuf);
    return g_bytes_new_take(png_buffer, png_size);
}

static GBytes *menu_item_icon_png(MenuItemData *item) {
    if (item->icon_png_resolved) {
        return item->icon_png;
    }
    item->icon_png_resolved = TRUE;
    item->icon_png = load_theme_icon_png(item->icon);
    return item->icon_png;
}

static GVariant *icon_data_variant(GBytes *png) {
    gsize size = 0;
    gconstpointer data = g_bytes_get_data(png, &size);
    return g_variant_new_from_data(
        G_VARIANT_TYPE("ay"),
        data,
        size,
        TRUE,
        (GDestroyNotify)g_bytes_unref,
        g_bytes_ref(png)
    );
}

static const char *status_notifier_xml =
    "<node>"
    " <interface name='org.kde.StatusNotifierItem'>"
    "  <property name='Category' type='s' access='read'/>"
    "  <property name='Id' type='s' access='read'/>"
    "  <property name='Title' type='s' access='read'/>"
    "  <property name='Status' type='s' access='read'/>"
    "  <property name='WindowId' type='u' access='read'/>"
    "  <property name='IconThemePath' type='s' access='read'/>"
    "  <property name='IconName' type='s' access='read'/>"
    "  <property name='IconPixmap' type='a(iiay)' access='read'/>"
    "  <property name='AttentionIconName' type='s' access='read'/>"
    "  <property name='AttentionIconPixmap' type='a(iiay)' access='read'/>"
    "  <property name='AttentionMovieName' type='s' access='read'/>"
    "  <property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
    "  <property name='ItemIsMenu' type='b' access='read'/>"
    "  <property name='Menu' type='o' access='read'/>"
    "  <method name='ContextMenu'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "  <method name='Activate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "  <method name='SecondaryActivate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
    "  <method name='Scroll'><arg type='i' direction='in'/><arg type='s' direction='in'/></method>"
    "  <signal name='NewTitle'/>"
    "  <signal name='NewIcon'/>"
    "  <signal name='NewAttentionIcon'/>"
    "  <signal name='NewOverlayIcon'/>"
    "  <signal name='NewToolTip'/>"
    "  <signal name='NewStatus'><arg type='s'/></signal>"
    " </interface>"
    "</node>";

static const char *dbus_menu_xml =
    "<node>"
    " <interface name='com.canonical.dbusmenu'>"
    "  <property name='Version' type='u' access='read'/>"
    "  <property name='TextDirection' type='s' access='read'/>"
    "  <property name='Status' type='s' access='read'/>"
    "  <property name='IconThemePath' type='as' access='read'/>"
    "  <method name='GetLayout'>"
    "   <arg name='parentId' type='i' direction='in'/>"
    "   <arg name='recursionDepth' type='i' direction='in'/>"
    "   <arg name='propertyNames' type='as' direction='in'/>"
    "   <arg name='revision' type='u' direction='out'/>"
    "   <arg name='layout' type='(ia{sv}av)' direction='out'/>"
    "  </method>"
    "  <method name='GetGroupProperties'>"
    "   <arg name='ids' type='ai' direction='in'/>"
    "   <arg name='propertyNames' type='as' direction='in'/>"
    "   <arg name='properties' type='a(ia{sv})' direction='out'/>"
    "  </method>"
    "  <method name='GetProperty'>"
    "   <arg name='id' type='i' direction='in'/>"
    "   <arg name='name' type='s' direction='in'/>"
    "   <arg name='value' type='v' direction='out'/>"
    "  </method>"
    "  <method name='Event'>"
    "   <arg name='id' type='i' direction='in'/>"
    "   <arg name='eventId' type='s' direction='in'/>"
    "   <arg name='data' type='v' direction='in'/>"
    "   <arg name='timestamp' type='u' direction='in'/>"
    "  </method>"
    "  <method name='EventGroup'>"
    "   <arg name='events' type='a(isvu)' direction='in'/>"
    "   <arg name='idErrors' type='ai' direction='out'/>"
    "  </method>"
    "  <method name='AboutToShow'>"
    "   <arg name='id' type='i' direction='in'/>"
    "   <arg name='needUpdate' type='b' direction='out'/>"
    "  </method>"
    "  <method name='AboutToShowGroup'>"
    "   <arg name='ids' type='ai' direction='in'/>"
    "   <arg name='updatesNeeded' type='ai' direction='out'/>"
    "   <arg name='idErrors' type='ai' direction='out'/>"
    "  </method>"
    "  <signal name='ItemsPropertiesUpdated'>"
    "   <arg name='updatedProps' type='a(ia{sv})'/>"
    "   <arg name='removedProps' type='a(ias)'/>"
    "  </signal>"
    "  <signal name='LayoutUpdated'><arg name='revision' type='u'/><arg name='parent' type='i'/></signal>"
    "  <signal name='ItemActivationRequested'><arg name='id' type='i'/><arg name='timestamp' type='u'/></signal>"
    " </interface>"
    "</node>";

static GVariant *empty_pixmaps(void) {
    return g_variant_new_array(G_VARIANT_TYPE("(iiay)"), NULL, 0);
}

static MenuItemData *find_menu_item(AppData *data, gint id) {
    for (guint i = 0; data->menu_items && i < data->menu_items->len; i++) {
        MenuItemData *item = g_ptr_array_index(data->menu_items, i);
        if (item->id == id) {
            return item;
        }
    }
    return NULL;
}

static GVariant *menu_property(MenuItemData *item, const char *name) {
    if (strcmp(name, "label") == 0) {
        return g_variant_new_string(item->label ? item->label : "");
    }
    if (strcmp(name, "enabled") == 0 || strcmp(name, "visible") == 0) {
        return g_variant_new_boolean(TRUE);
    }
    if (strcmp(name, "type") == 0) {
        return g_variant_new_string(item->separator ? "separator" : "standard");
    }
    if (strcmp(name, "icon-name") == 0) {
        return g_variant_new_string(item->icon ? item->icon : "");
    }
    if (strcmp(name, "icon-data") == 0) {
        GBytes *png = menu_item_icon_png(item);
        if (png) {
            return icon_data_variant(png);
        }
        return g_variant_new_array(G_VARIANT_TYPE_BYTE, NULL, 0);
    }
    return g_variant_new_string("");
}

static GVariant *menu_item_properties(MenuItemData *item) {
    GVariantBuilder properties;
    g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&properties, "{sv}", "visible", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&properties, "{sv}", "enabled", g_variant_new_boolean(TRUE));

    if (item->separator) {
        g_variant_builder_add(&properties, "{sv}", "type", g_variant_new_string("separator"));
    } else {
        g_variant_builder_add(&properties, "{sv}", "label", g_variant_new_string(item->label));
        GBytes *png = menu_item_icon_png(item);
        if (png) {
            /* Prefer icon-data so hosts do not recolor a symbolic icon-name. */
            g_variant_builder_add(&properties, "{sv}", "icon-data", icon_data_variant(png));
        } else if (item->icon && *item->icon) {
            g_variant_builder_add(&properties, "{sv}", "icon-name", g_variant_new_string(item->icon));
        }
    }
    return g_variant_builder_end(&properties);
}

static GVariant *menu_layout(AppData *data) {
    GVariantBuilder properties;
    GVariantBuilder children;
    g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
    g_variant_builder_add(&properties, "{sv}", "children-display", g_variant_new_string("submenu"));

    for (guint i = 0; data->menu_items && i < data->menu_items->len; i++) {
        MenuItemData *item = g_ptr_array_index(data->menu_items, i);
        GVariantBuilder no_children;
        g_variant_builder_init(&no_children, G_VARIANT_TYPE("av"));
        GVariant *child = g_variant_new(
            "(i@a{sv}@av)",
            item->id,
            menu_item_properties(item),
            g_variant_builder_end(&no_children)
        );
        g_variant_builder_add(&children, "v", child);
    }

    return g_variant_new(
        "(i@a{sv}@av)",
        0,
        g_variant_builder_end(&properties),
        g_variant_builder_end(&children)
    );
}

static void activate_menu_item(AppData *data, gint id) {
    MenuItemData *item = find_menu_item(data, id);
    if (!item || item->separator || !item->command) {
        return;
    }

    if (strcmp(item->command, "quit") == 0) {
        g_application_quit(G_APPLICATION(data->application));
        return;
    }

    const char *argv[] = {"/bin/sh", "-c", item->command, NULL};
    GError *error = NULL;
    if (!g_spawn_async(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error)) {
        g_warning("Could not run '%s': %s", item->command, error->message);
        g_error_free(error);
    }
}

static GVariant *status_notifier_get_property(
    GDBusConnection *connection,
    const char *sender,
    const char *object_path,
    const char *interface_name,
    const char *property_name,
    GError **error,
    gpointer user_data
) {
    AppData *data = user_data;
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)error;

    if (strcmp(property_name, "Category") == 0) return g_variant_new_string("ApplicationStatus");
    if (strcmp(property_name, "Id") == 0) return g_variant_new_string("trayactions");
    if (strcmp(property_name, "Title") == 0) return g_variant_new_string("TrayActions");
    if (strcmp(property_name, "Status") == 0) return g_variant_new_string("Active");
    if (strcmp(property_name, "WindowId") == 0) return g_variant_new_uint32(0);
    if (strcmp(property_name, "IconThemePath") == 0) return g_variant_new_string("");
    if (strcmp(property_name, "IconName") == 0) {
        return g_variant_new_string(data->indicator_icon ? data->indicator_icon : "system-run");
    }
    if (strcmp(property_name, "IconPixmap") == 0 ||
        strcmp(property_name, "AttentionIconPixmap") == 0) {
        return empty_pixmaps();
    }
    if (strcmp(property_name, "AttentionIconName") == 0 ||
        strcmp(property_name, "AttentionMovieName") == 0) {
        return g_variant_new_string("");
    }
    if (strcmp(property_name, "ToolTip") == 0) {
        return g_variant_new(
            "(s@a(iiay)ss)",
            data->indicator_icon ? data->indicator_icon : "system-run",
            empty_pixmaps(),
            "TrayActions",
            "Configurable actions"
        );
    }
    if (strcmp(property_name, "ItemIsMenu") == 0) return g_variant_new_boolean(TRUE);
    if (strcmp(property_name, "Menu") == 0) return g_variant_new_object_path("/MenuBar");
    return NULL;
}

static void status_notifier_method_call(
    GDBusConnection *connection,
    const char *sender,
    const char *object_path,
    const char *interface_name,
    const char *method_name,
    GVariant *parameters,
    GDBusMethodInvocation *invocation,
    gpointer user_data
) {
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)parameters;
    (void)user_data;

    if (strcmp(method_name, "ContextMenu") == 0 ||
        strcmp(method_name, "Activate") == 0 ||
        strcmp(method_name, "SecondaryActivate") == 0 ||
        strcmp(method_name, "Scroll") == 0) {
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    g_dbus_method_invocation_return_dbus_error(
        invocation, "org.freedesktop.DBus.Error.UnknownMethod", "Unknown method"
    );
}

static GVariant *dbus_menu_get_property(
    GDBusConnection *connection,
    const char *sender,
    const char *object_path,
    const char *interface_name,
    const char *property_name,
    GError **error,
    gpointer user_data
) {
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)error;
    (void)user_data;

    if (strcmp(property_name, "Version") == 0) return g_variant_new_uint32(4);
    if (strcmp(property_name, "TextDirection") == 0) return g_variant_new_string("ltr");
    if (strcmp(property_name, "Status") == 0) return g_variant_new_string("normal");
    if (strcmp(property_name, "IconThemePath") == 0) {
        return g_variant_new_strv(NULL, 0);
    }
    return NULL;
}

static void dbus_menu_method_call(
    GDBusConnection *connection,
    const char *sender,
    const char *object_path,
    const char *interface_name,
    const char *method_name,
    GVariant *parameters,
    GDBusMethodInvocation *invocation,
    gpointer user_data
) {
    AppData *data = user_data;
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;

    if (strcmp(method_name, "GetLayout") == 0) {
        g_dbus_method_invocation_return_value(
            invocation,
            g_variant_new("(u@(ia{sv}av))", data->menu_revision, menu_layout(data))
        );
        return;
    }

    if (strcmp(method_name, "GetGroupProperties") == 0) {
        GVariant *ids = g_variant_get_child_value(parameters, 0);
        GVariantIter iterator;
        gint id;
        GVariantBuilder result;
        g_variant_builder_init(&result, G_VARIANT_TYPE("a(ia{sv})"));
        g_variant_iter_init(&iterator, ids);
        while (g_variant_iter_next(&iterator, "i", &id)) {
            MenuItemData *item = find_menu_item(data, id);
            if (item) {
                g_variant_builder_add(&result, "(i@a{sv})", id, menu_item_properties(item));
            }
        }
        g_variant_unref(ids);
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(@a(ia{sv}))", g_variant_builder_end(&result))
        );
        return;
    }

    if (strcmp(method_name, "GetProperty") == 0) {
        gint id;
        const char *name;
        g_variant_get(parameters, "(i&s)", &id, &name);
        MenuItemData *item = find_menu_item(data, id);
        GVariant *value = item ? menu_property(item, name) : g_variant_new_string("");
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", value));
        return;
    }

    if (strcmp(method_name, "Event") == 0) {
        gint id;
        const char *event_id;
        GVariant *event_data;
        guint timestamp;
        g_variant_get(parameters, "(i&s@vu)", &id, &event_id, &event_data, &timestamp);
        if (strcmp(event_id, "clicked") == 0) {
            activate_menu_item(data, id);
        }
        g_variant_unref(event_data);
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    if (strcmp(method_name, "EventGroup") == 0) {
        GVariant *events = g_variant_get_child_value(parameters, 0);
        GVariantIter iterator;
        gint id;
        const char *event_id;
        GVariant *event_data;
        guint timestamp;
        g_variant_iter_init(&iterator, events);
        while (g_variant_iter_next(&iterator, "(i&s@vu)", &id, &event_id, &event_data, &timestamp)) {
            if (strcmp(event_id, "clicked") == 0) {
                activate_menu_item(data, id);
            }
            g_variant_unref(event_data);
        }
        g_variant_unref(events);
        GVariantBuilder errors;
        g_variant_builder_init(&errors, G_VARIANT_TYPE("ai"));
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(@ai)", g_variant_builder_end(&errors))
        );
        return;
    }

    if (strcmp(method_name, "AboutToShow") == 0) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
        return;
    }

    if (strcmp(method_name, "AboutToShowGroup") == 0) {
        GVariantBuilder updates;
        GVariantBuilder errors;
        g_variant_builder_init(&updates, G_VARIANT_TYPE("ai"));
        g_variant_builder_init(&errors, G_VARIANT_TYPE("ai"));
        g_dbus_method_invocation_return_value(
            invocation,
            g_variant_new(
                "(@ai@ai)",
                g_variant_builder_end(&updates),
                g_variant_builder_end(&errors)
            )
        );
        return;
    }

    g_dbus_method_invocation_return_dbus_error(
        invocation, "org.freedesktop.DBus.Error.UnknownMethod", "Unknown method"
    );
}

static const GDBusInterfaceVTable status_notifier_vtable = {
    status_notifier_method_call,
    status_notifier_get_property,
    NULL,
    {0}
};

static const GDBusInterfaceVTable dbus_menu_vtable = {
    dbus_menu_method_call,
    dbus_menu_get_property,
    NULL,
    {0}
};

gboolean tray_start(AppData *data, GError **error) {
    GDBusNodeInfo *status_info = NULL;
    GDBusNodeInfo *menu_info = NULL;

    data->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, error);
    if (!data->bus) {
        return FALSE;
    }

    status_info = g_dbus_node_info_new_for_xml(status_notifier_xml, error);
    if (!status_info) {
        goto fail;
    }
    menu_info = g_dbus_node_info_new_for_xml(dbus_menu_xml, error);
    if (!menu_info) {
        goto fail;
    }

    data->status_notifier_registration_id = g_dbus_connection_register_object(
        data->bus,
        "/StatusNotifierItem",
        status_info->interfaces[0],
        &status_notifier_vtable,
        data,
        NULL,
        error
    );
    if (!data->status_notifier_registration_id) {
        goto fail;
    }

    data->dbus_menu_registration_id = g_dbus_connection_register_object(
        data->bus,
        "/MenuBar",
        menu_info->interfaces[0],
        &dbus_menu_vtable,
        data,
        NULL,
        error
    );
    if (!data->dbus_menu_registration_id) {
        goto fail;
    }

    GError *registration_error = NULL;
    g_dbus_connection_call_sync(
        data->bus,
        "org.kde.StatusNotifierWatcher",
        "/StatusNotifierWatcher",
        "org.kde.StatusNotifierWatcher",
        "RegisterStatusNotifierItem",
        g_variant_new("(s)", g_dbus_connection_get_unique_name(data->bus)),
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        -1,
        NULL,
        &registration_error
    );
    if (registration_error) {
        g_warning("Could not register the tray icon: %s", registration_error->message);
        g_error_free(registration_error);
    }

    g_dbus_node_info_unref(status_info);
    g_dbus_node_info_unref(menu_info);
    return TRUE;

fail:
    if (status_info) g_dbus_node_info_unref(status_info);
    if (menu_info) g_dbus_node_info_unref(menu_info);
    tray_stop(data);
    return FALSE;
}

void tray_stop(AppData *data) {
    if (!data->bus) {
        return;
    }
    if (data->status_notifier_registration_id) {
        g_dbus_connection_unregister_object(data->bus, data->status_notifier_registration_id);
        data->status_notifier_registration_id = 0;
    }
    if (data->dbus_menu_registration_id) {
        g_dbus_connection_unregister_object(data->bus, data->dbus_menu_registration_id);
        data->dbus_menu_registration_id = 0;
    }
    g_clear_object(&data->bus);
}

void tray_notify_icon_changed(AppData *data) {
    if (data->bus && data->status_notifier_registration_id) {
        g_dbus_connection_emit_signal(
            data->bus, NULL, "/StatusNotifierItem",
            "org.kde.StatusNotifierItem", "NewIcon", NULL, NULL
        );
        g_dbus_connection_emit_signal(
            data->bus, NULL, "/StatusNotifierItem",
            "org.kde.StatusNotifierItem", "NewToolTip", NULL, NULL
        );
    }
}

void tray_notify_menu_changed(AppData *data) {
    data->menu_revision++;
    if (data->bus && data->dbus_menu_registration_id) {
        g_dbus_connection_emit_signal(
            data->bus,
            NULL,
            "/MenuBar",
            "com.canonical.dbusmenu",
            "LayoutUpdated",
            g_variant_new("(ui)", data->menu_revision, 0),
            NULL
        );
    }
}
