#include "workspace.h"
#include "cosmic_workspace.h"

gboolean workspace_backend_init(AppData *data) {
    return cosmic_workspace_init(data);
}

void workspace_backend_shutdown(void) {
    cosmic_workspace_shutdown();
}

gboolean workspace_backend_is_active(void) {
    return cosmic_workspace_is_active();
}

GPtrArray *workspace_list_open_apps(void) {
    return cosmic_workspace_list_open_apps();
}

void workspace_watcher_refresh(AppData *data) {
    cosmic_workspace_refresh(data);
}
