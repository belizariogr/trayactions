#ifndef COSMIC_WORKSPACE_H
#define COSMIC_WORKSPACE_H

#include <glib.h>
#include "appdata.h"

gboolean cosmic_workspace_init(AppData *data);
void cosmic_workspace_shutdown(void);
gboolean cosmic_workspace_is_active(void);
GPtrArray *cosmic_workspace_list_open_apps(void);
gboolean cosmic_workspace_focus_app(const char *app_id);
void cosmic_workspace_refresh(AppData *data);

#endif
