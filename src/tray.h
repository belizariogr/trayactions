#ifndef TRAY_H
#define TRAY_H

#include "appdata.h"

gboolean tray_start(AppData *data, GError **error);
void tray_stop(AppData *data);
void tray_notify_icon_changed(AppData *data);
void tray_notify_menu_changed(AppData *data);

#endif
