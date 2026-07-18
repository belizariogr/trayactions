#ifndef APPDATA_H
#define APPDATA_H

#include <gio/gio.h>
#include <glib.h>

typedef struct _GtkApplication GtkApplication;
typedef struct _GtkWidget GtkWidget;

typedef struct {
    gint id;
    char *label;
    char *command;
    char *icon;
    GBytes *icon_png;
    gboolean icon_png_resolved;
    gboolean separator;
} MenuItemData;

typedef struct {
    char *app_id;
    gint workspace;
} AppWorkspaceAssignment;

typedef struct {
    GtkApplication *application;
    char *config_file_path;
    char *indicator_icon;
    GFileMonitor *monitor;
    GDBusConnection *bus;
    GPtrArray *menu_items;
    GPtrArray *app_workspaces;
    GtkWidget *preferences_window;
    guint status_notifier_registration_id;
    guint dbus_menu_registration_id;
    guint menu_revision;
    gboolean is_reloading;
    gboolean initialized;
} AppData;

#endif // APPDATA_H
