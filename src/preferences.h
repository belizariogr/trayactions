#ifndef PREFERENCES_H
#define PREFERENCES_H

#include "appdata.h"

/**
 * Show (or present) the preferences window.
 */
void preferences_show(AppData *data);

/**
 * Rebuild app-workspace rows if preferences is open (after config reload).
 */
void preferences_reload_app_bindings(AppData *data);

/**
 * Rebuild editable menu rows if preferences is open (after external config reload).
 */
void preferences_reload_menu_items(AppData *data);

#endif // PREFERENCES_H
