#ifndef WORKSPACE_H
#define WORKSPACE_H

#include <glib.h>
#include "appdata.h"

/**
 * Try to initialize the Cosmic Wayland workspace backend.
 * Safe on GNOME/X11: returns FALSE without error dialogs.
 */
gboolean workspace_backend_init(AppData *data);

/**
 * Tear down the backend and Wayland connection.
 */
void workspace_backend_shutdown(void);

/**
 * TRUE when Cosmic globals were bound successfully.
 */
gboolean workspace_backend_is_active(void);

/**
 * Unique app_id strings of currently open toplevels.
 * Caller owns the GPtrArray (free_func g_free). Empty if inactive.
 */
GPtrArray *workspace_list_open_apps(void);

/**
 * Re-read assignments from AppData after config reload.
 */
void workspace_watcher_refresh(AppData *data);

#endif // WORKSPACE_H
