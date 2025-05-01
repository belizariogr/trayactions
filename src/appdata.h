#ifndef APPDATA_H
#define APPDATA_H

#include <gtk/gtk.h>
#include <libappindicator/app-indicator.h>
#include <gio/gio.h>
#include <glib.h>

// Menu item structure
typedef struct {
    char *label;
    char *command;
    char *icon;
} MenuItemData;

// Application data structure
typedef struct {
    AppIndicator *indicator;
    char *config_file_path;
    GFileMonitor *monitor; 
    GtkWidget *current_menu;
    gboolean is_reloading;
} AppData;

#endif // APPDATA_H
