#include "preferences.h"

#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "menu_order.h"
#include "workspace.h"

typedef struct {
    AppData *data;
    GtkWidget *window;
    GtkWidget *icon_image;
    GtkWidget *icon_button;
    GtkWidget *menu_list;
    GtkWidget *apps_list;
    GPtrArray *menu_items;
    gint drag_from_index;
    gboolean reload_menu_after_drag;
} PreferencesState;

static void preferences_ensure_drop_style(void) {
    static gboolean loaded = FALSE;
    if (loaded) {
        return;
    }
    loaded = TRUE;

    const char *css =
        /* Suppress GTK/Adwaita green drop highlight on menu rows. */
        "list.boxed-list > row:drop(active),"
        "list.boxed-list > row:drop(active) > *,"
        ".menu-drop-row:drop(active) {"
        "  background-color: transparent;"
        "  box-shadow: none;"
        "  outline: none;"
        "  border-color: transparent;"
        "}"
        ".menu-drop-row.drop-before {"
        "  border-top: 2px solid #3584e4;"
        "  border-top-left-radius: 0;"
        "  border-top-right-radius: 0;"
        "  margin-top: -1px;"
        "  background-color: transparent;"
        "  box-shadow: none;"
        "}"
        ".menu-drop-row.drop-after {"
        "  border-bottom: 2px solid #3584e4;"
        "  border-bottom-left-radius: 0;"
        "  border-bottom-right-radius: 0;"
        "  margin-bottom: -1px;"
        "  background-color: transparent;"
        "  box-shadow: none;"
        "}";

    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );
    g_object_unref(provider);
}

static void preferences_apply_system_theme(void) {
    GtkSettings *settings = gtk_settings_get_default();
    if (!settings) {
        return;
    }

    gboolean prefer_dark = FALSE;
    GSettingsSchemaSource *source = g_settings_schema_source_get_default();
    if (source) {
        GSettingsSchema *schema = g_settings_schema_source_lookup(
            source,
            "org.gnome.desktop.interface",
            TRUE
        );
        if (schema) {
            GSettings *gsettings = g_settings_new("org.gnome.desktop.interface");
            char *scheme = g_settings_get_string(gsettings, "color-scheme");
            prefer_dark = g_strcmp0(scheme, "prefer-dark") == 0;
            g_free(scheme);

            char *theme = g_settings_get_string(gsettings, "gtk-theme");
            if (theme && *theme) {
                g_object_set(settings, "gtk-theme-name", theme, NULL);
            }
            g_free(theme);
            g_object_unref(gsettings);
            g_settings_schema_unref(schema);
        }
    }

    g_object_set(settings, "gtk-application-prefer-dark-theme", prefer_dark, NULL);
}

static void preferences_state_free(gpointer pointer) {
    PreferencesState *state = pointer;
    if (!state) {
        return;
    }
    g_clear_pointer(&state->menu_items, g_ptr_array_unref);
    g_free(state);
}

static void mark_window_as_dialog(GtkWidget *window) {
    gtk_window_set_modal(GTK_WINDOW(window), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(window), TRUE);
    gtk_widget_add_css_class(window, "preferences-dialog");

    GtkNative *native = gtk_widget_get_native(window);
    if (!native) {
        return;
    }
    GdkSurface *surface = gtk_native_get_surface(native);
    if (surface && GDK_IS_TOPLEVEL(surface)) {
        gdk_toplevel_set_modal(GDK_TOPLEVEL(surface), TRUE);
    }
}

static void on_window_realize(GtkWidget *window, gpointer user_data) {
    (void)user_data;
    mark_window_as_dialog(window);
}

static void update_icon_button(PreferencesState *state, const char *icon_name) {
    if (!state || !state->icon_image) {
        return;
    }
    const char *name = (icon_name && *icon_name) ? icon_name : "image-missing";
    gtk_image_set_from_icon_name(GTK_IMAGE(state->icon_image), name);
    gtk_image_set_pixel_size(GTK_IMAGE(state->icon_image), 24);
}

static void on_icon_selected(GtkButton *button, gpointer user_data) {
    PreferencesState *state = user_data;
    const char *icon_name = g_object_get_data(G_OBJECT(button), "icon-name");
    GtkWidget *dialog = GTK_WIDGET(g_object_get_data(G_OBJECT(button), "icon-dialog"));
    GtkWidget *target_image = g_object_get_data(G_OBJECT(button), "target-image");
    char **target_name = g_object_get_data(G_OBJECT(button), "target-name");

    if (!icon_name) {
        return;
    }

    if (target_name) {
        g_free(*target_name);
        *target_name = g_strdup(icon_name);
    }
    if (target_image) {
        gtk_image_set_from_icon_name(GTK_IMAGE(target_image), icon_name);
        gtk_image_set_pixel_size(GTK_IMAGE(target_image), 24);
    }

    if (state && !target_name) {
        g_free(state->data->indicator_icon);
        state->data->indicator_icon = g_strdup(icon_name);
        update_icon_button(state, icon_name);
        config_save_indicator_icon(state->data, icon_name);
    }

    if (dialog) {
        gtk_window_destroy(GTK_WINDOW(dialog));
    }
}

static gboolean icon_name_visible(GtkFlowBoxChild *child, gpointer user_data) {
    const char *filter = user_data;
    if (!filter || !*filter) {
        return TRUE;
    }
    GtkWidget *button = gtk_flow_box_child_get_child(child);
    const char *icon_name = g_object_get_data(G_OBJECT(button), "icon-name");
    if (!icon_name) {
        return FALSE;
    }
    char *haystack = g_utf8_strdown(icon_name, -1);
    char *needle = g_utf8_strdown(filter, -1);
    gboolean match = strstr(haystack, needle) != NULL;
    g_free(haystack);
    g_free(needle);
    return match;
}

static void on_icon_filter_changed(GtkEditable *editable, gpointer user_data) {
    GtkFlowBox *flow = user_data;
    const char *text = gtk_editable_get_text(editable);
    gtk_flow_box_set_filter_func(flow, icon_name_visible, (gpointer)text, NULL);
}

static void show_icon_picker(
    PreferencesState *state,
    GtkWindow *parent,
    GtkWidget *target_image,
    char **target_name
) {
    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Select Icon");
    gtk_window_set_transient_for(GTK_WINDOW(dialog), parent);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 480, 400);
    gtk_widget_add_css_class(dialog, "preferences-dialog");
    g_signal_connect(dialog, "realize", G_CALLBACK(on_window_realize), NULL);

    GtkWidget *header = gtk_header_bar_new();
    gtk_window_set_titlebar(GTK_WINDOW(dialog), header);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_window_set_child(GTK_WINDOW(dialog), box);

    GtkWidget *search = gtk_search_entry_new();
    gtk_box_append(GTK_BOX(box), search);

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scrolled),
        GTK_POLICY_NEVER,
        GTK_POLICY_AUTOMATIC
    );
    gtk_box_append(GTK_BOX(box), scrolled);

    GtkWidget *flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 10);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flow), 4);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), flow);

    GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(parent));
    GtkIconTheme *theme = gtk_icon_theme_get_for_display(display);
    char **names = gtk_icon_theme_get_icon_names(theme);
    if (names) {
        for (char **name = names; *name; name++) {
            GtkWidget *image = gtk_image_new_from_icon_name(*name);
            gtk_image_set_pixel_size(GTK_IMAGE(image), 24);
            GtkWidget *button = gtk_button_new();
            gtk_button_set_child(GTK_BUTTON(button), image);
            gtk_widget_set_tooltip_text(button, *name);
            gtk_widget_add_css_class(button, "flat");
            g_object_set_data_full(G_OBJECT(button), "icon-name", g_strdup(*name), g_free);
            g_object_set_data(G_OBJECT(button), "icon-dialog", dialog);
            g_object_set_data(G_OBJECT(button), "target-image", target_image);
            g_object_set_data(G_OBJECT(button), "target-name", target_name);
            g_signal_connect(button, "clicked", G_CALLBACK(on_icon_selected), state);
            gtk_flow_box_append(GTK_FLOW_BOX(flow), button);
        }
        g_strfreev(names);
    }

    g_signal_connect(search, "search-changed", G_CALLBACK(on_icon_filter_changed), flow);
    gtk_window_present(GTK_WINDOW(dialog));
}

static void on_indicator_icon_clicked(GtkButton *button, gpointer user_data) {
    PreferencesState *state = user_data;
    (void)button;
    show_icon_picker(state, GTK_WINDOW(state->window), NULL, NULL);
}

static void persist_menu_items(PreferencesState *state) {
    config_save_menu_items(state->data, state->menu_items);
}

static void menu_list_reload(PreferencesState *state);

static void on_menu_item_remove(GtkButton *button, gpointer user_data) {
    PreferencesState *state = user_data;
    MenuItemData *item = g_object_get_data(G_OBJECT(button), "menu-item");
    if (!state || !item || !state->menu_items) {
        return;
    }
    g_ptr_array_remove(state->menu_items, item);
    persist_menu_items(state);
    menu_list_reload(state);
}

static void free_icon_storage(gpointer pointer) {
    char **storage = pointer;
    if (!storage) {
        return;
    }
    g_free(*storage);
    g_free(storage);
}

static void on_menu_editor_destroy(GtkWidget *dialog, gpointer user_data) {
    (void)user_data;
    MenuItemData *pending = g_object_get_data(G_OBJECT(dialog), "pending-item");
    if (pending) {
        menu_item_data_free(pending);
        g_object_set_data(G_OBJECT(dialog), "pending-item", NULL);
    }
}

static void on_menu_edit_save(GtkButton *button, gpointer user_data) {
    PreferencesState *state = g_object_get_data(G_OBJECT(button), "prefs-state");
    MenuItemData *item = g_object_get_data(G_OBJECT(button), "menu-item");
    GtkWidget *dialog = user_data;
    GtkEntry *label_entry = g_object_get_data(G_OBJECT(button), "label-entry");
    GtkEntry *command_entry = g_object_get_data(G_OBJECT(button), "command-entry");
    char **icon_name_ptr = g_object_get_data(G_OBJECT(button), "icon-name-ptr");
    gboolean is_new = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "is-new"));

    if (!state || !item || !label_entry || !command_entry) {
        return;
    }

    const char *label = gtk_editable_get_text(GTK_EDITABLE(label_entry));
    const char *command = gtk_editable_get_text(GTK_EDITABLE(command_entry));
    if (!label || !*label || !command || !*command) {
        return;
    }

    g_free(item->label);
    g_free(item->command);
    g_free(item->icon);
    item->label = g_strdup(label);
    item->command = g_strdup(command);
    item->icon = (icon_name_ptr && *icon_name_ptr && **icon_name_ptr)
                     ? g_strdup(*icon_name_ptr)
                     : NULL;
    item->separator = FALSE;

    if (is_new) {
        g_object_set_data(G_OBJECT(dialog), "pending-item", NULL);
        g_ptr_array_add(state->menu_items, item);
    }

    persist_menu_items(state);
    menu_list_reload(state);
    gtk_window_destroy(GTK_WINDOW(dialog));
}

static void on_menu_edit_cancel(GtkButton *button, gpointer user_data) {
    (void)button;
    gtk_window_destroy(GTK_WINDOW(user_data));
}

static void on_edit_icon_clicked(GtkButton *button, gpointer user_data) {
    PreferencesState *state = g_object_get_data(G_OBJECT(button), "prefs-state");
    GtkWidget *image = g_object_get_data(G_OBJECT(button), "icon-image");
    char **icon_name = g_object_get_data(G_OBJECT(button), "icon-name-ptr");
    GtkWindow *parent = user_data;
    show_icon_picker(state, parent, image, icon_name);
}

static void show_menu_item_editor(
    PreferencesState *state,
    MenuItemData *item,
    gboolean is_new
) {
    char **icon_storage = g_new0(char *, 1);
    *icon_storage = g_strdup(item->icon ? item->icon : "");

    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), is_new ? "Add Menu Item" : "Edit Menu Item");
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(state->window));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 420, 280);
    g_signal_connect(dialog, "realize", G_CALLBACK(on_window_realize), NULL);
    g_object_set_data_full(G_OBJECT(dialog), "icon-storage", icon_storage, free_icon_storage);
    if (is_new) {
        g_object_set_data(G_OBJECT(dialog), "pending-item", item);
        g_signal_connect(dialog, "destroy", G_CALLBACK(on_menu_editor_destroy), NULL);
    }

    GtkWidget *header = gtk_header_bar_new();
    gtk_window_set_titlebar(GTK_WINDOW(dialog), header);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    gtk_window_set_child(GTK_WINDOW(dialog), box);

    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "boxed-list");
    gtk_box_append(GTK_BOX(box), list);

    GtkWidget *label_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(label_row, 8);
    gtk_widget_set_margin_bottom(label_row, 8);
    gtk_widget_set_margin_start(label_row, 12);
    gtk_widget_set_margin_end(label_row, 12);
    GtkWidget *label_title = gtk_label_new("Label");
    gtk_widget_set_halign(label_title, GTK_ALIGN_START);
    gtk_widget_set_hexpand(label_title, TRUE);
    GtkWidget *label_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(label_entry), item->label ? item->label : "");
    gtk_widget_set_size_request(label_entry, 220, -1);
    gtk_box_append(GTK_BOX(label_row), label_title);
    gtk_box_append(GTK_BOX(label_row), label_entry);
    gtk_list_box_append(GTK_LIST_BOX(list), label_row);

    GtkWidget *command_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(command_row, 8);
    gtk_widget_set_margin_bottom(command_row, 8);
    gtk_widget_set_margin_start(command_row, 12);
    gtk_widget_set_margin_end(command_row, 12);
    GtkWidget *command_title = gtk_label_new("Command");
    gtk_widget_set_halign(command_title, GTK_ALIGN_START);
    gtk_widget_set_hexpand(command_title, TRUE);
    GtkWidget *command_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(command_entry), item->command ? item->command : "");
    gtk_widget_set_size_request(command_entry, 220, -1);
    gtk_box_append(GTK_BOX(command_row), command_title);
    gtk_box_append(GTK_BOX(command_row), command_entry);
    gtk_list_box_append(GTK_LIST_BOX(list), command_row);

    GtkWidget *icon_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(icon_row, 8);
    gtk_widget_set_margin_bottom(icon_row, 8);
    gtk_widget_set_margin_start(icon_row, 12);
    gtk_widget_set_margin_end(icon_row, 12);
    GtkWidget *icon_title = gtk_label_new("Icon");
    gtk_widget_set_halign(icon_title, GTK_ALIGN_START);
    gtk_widget_set_hexpand(icon_title, TRUE);
    GtkWidget *icon_image = gtk_image_new_from_icon_name(
        (item->icon && *item->icon) ? item->icon : "image-missing"
    );
    gtk_image_set_pixel_size(GTK_IMAGE(icon_image), 24);
    GtkWidget *icon_button = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(icon_button), icon_image);
    g_object_set_data(G_OBJECT(icon_button), "prefs-state", state);
    g_object_set_data(G_OBJECT(icon_button), "icon-image", icon_image);
    g_object_set_data(G_OBJECT(icon_button), "icon-name-ptr", icon_storage);
    g_signal_connect(
        icon_button,
        "clicked",
        G_CALLBACK(on_edit_icon_clicked),
        dialog
    );
    gtk_box_append(GTK_BOX(icon_row), icon_title);
    gtk_box_append(GTK_BOX(icon_row), icon_button);
    gtk_list_box_append(GTK_LIST_BOX(list), icon_row);

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(actions, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    GtkWidget *save = gtk_button_new_with_label(is_new ? "Add" : "Save");
    gtk_widget_add_css_class(save, "suggested-action");
    g_signal_connect(cancel, "clicked", G_CALLBACK(on_menu_edit_cancel), dialog);
    g_object_set_data(G_OBJECT(save), "prefs-state", state);
    g_object_set_data(G_OBJECT(save), "menu-item", item);
    g_object_set_data(G_OBJECT(save), "label-entry", label_entry);
    g_object_set_data(G_OBJECT(save), "command-entry", command_entry);
    g_object_set_data(G_OBJECT(save), "icon-name-ptr", icon_storage);
    g_object_set_data(G_OBJECT(save), "is-new", GINT_TO_POINTER(is_new));
    g_signal_connect(save, "clicked", G_CALLBACK(on_menu_edit_save), dialog);
    gtk_box_append(GTK_BOX(actions), cancel);
    gtk_box_append(GTK_BOX(actions), save);
    gtk_box_append(GTK_BOX(box), actions);

    gtk_window_present(GTK_WINDOW(dialog));
}

static void on_menu_item_edit(GtkButton *button, gpointer user_data) {
    PreferencesState *state = user_data;
    MenuItemData *item = g_object_get_data(G_OBJECT(button), "menu-item");
    if (!item || item->separator) {
        return;
    }
    show_menu_item_editor(state, item, FALSE);
}

static void on_menu_add_item(GtkButton *button, gpointer user_data) {
    PreferencesState *state = user_data;
    (void)button;
    MenuItemData *item = g_new0(MenuItemData, 1);
    item->label = g_strdup("New Item");
    item->command = g_strdup("");
    show_menu_item_editor(state, item, TRUE);
}

static void on_menu_add_separator(GtkButton *button, gpointer user_data) {
    PreferencesState *state = user_data;
    (void)button;
    MenuItemData *item = g_new0(MenuItemData, 1);
    item->separator = TRUE;
    g_ptr_array_add(state->menu_items, item);
    persist_menu_items(state);
    menu_list_reload(state);
}

static gint menu_item_index(PreferencesState *state, MenuItemData *item) {
    if (!state || !state->menu_items || !item) {
        return -1;
    }
    for (guint i = 0; i < state->menu_items->len; i++) {
        if (g_ptr_array_index(state->menu_items, i) == item) {
            return (gint)i;
        }
    }
    return -1;
}

static void menu_drop_clear_indicators(PreferencesState *state) {
    if (!state || !state->menu_list) {
        return;
    }

    gtk_list_box_drag_unhighlight_row(GTK_LIST_BOX(state->menu_list));

    for (GtkWidget *child = gtk_widget_get_first_child(state->menu_list);
         child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *row_box = child;
        if (GTK_IS_LIST_BOX_ROW(child)) {
            gtk_widget_remove_css_class(child, "accent");
            row_box = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(child));
        }
        if (!GTK_IS_WIDGET(row_box)) {
            continue;
        }
        gtk_widget_remove_css_class(row_box, "drop-before");
        gtk_widget_remove_css_class(row_box, "drop-after");
        gtk_widget_remove_css_class(row_box, "accent");
    }
}

static void menu_drop_update_indicator(
    PreferencesState *state,
    GtkWidget *row,
    gdouble y
) {
    if (!state || !GTK_IS_WIDGET(row)) {
        return;
    }

    menu_drop_clear_indicators(state);

    gdouble height = gtk_widget_get_height(row);
    gboolean insert_after = height > 0 && y > height / 2.0;

    if (insert_after) {
        GtkWidget *list_row = gtk_widget_get_parent(row);
        GtkWidget *next_list_row =
            list_row ? gtk_widget_get_next_sibling(list_row) : NULL;
        if (next_list_row && GTK_IS_LIST_BOX_ROW(next_list_row)) {
            GtkWidget *next_box =
                gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(next_list_row));
            if (GTK_IS_WIDGET(next_box)) {
                gtk_widget_add_css_class(next_box, "drop-before");
                return;
            }
        }
        /* Last row: show line at the bottom (insertion at end). */
        gtk_widget_add_css_class(row, "drop-after");
        return;
    }

    gtk_widget_add_css_class(row, "drop-before");
}

static GdkContentProvider *on_menu_drag_prepare(
    GtkDragSource *source,
    gdouble x,
    gdouble y,
    gpointer user_data
) {
    (void)x;
    (void)y;
    MenuItemData *item = user_data;
    PreferencesState *state = g_object_get_data(G_OBJECT(source), "prefs-state");
    gint index = menu_item_index(state, item);
    if (!state || index < 0) {
        return NULL;
    }
    state->drag_from_index = index;
    state->reload_menu_after_drag = FALSE;
    return gdk_content_provider_new_typed(G_TYPE_UINT, (guint)index);
}

static void on_menu_drag_begin(
    GtkDragSource *source,
    GdkDrag *drag,
    gpointer user_data
) {
    (void)drag;
    (void)user_data;
    PreferencesState *state = g_object_get_data(G_OBJECT(source), "prefs-state");
    if (state) {
        menu_drop_clear_indicators(state);
    }

    GtkIconTheme *theme = gtk_icon_theme_get_for_display(gdk_display_get_default());
    GtkIconPaintable *icon = gtk_icon_theme_lookup_icon(
        theme,
        "list-drag-handle-symbolic",
        NULL,
        32,
        1,
        GTK_TEXT_DIR_NONE,
        0
    );
    if (icon) {
        gtk_drag_source_set_icon(source, GDK_PAINTABLE(icon), 16, 16);
        g_object_unref(icon);
    }
}

static void on_menu_drag_end(
    GtkDragSource *source,
    GdkDrag *drag,
    gboolean delete_data,
    gpointer user_data
) {
    (void)drag;
    (void)delete_data;
    (void)user_data;
    PreferencesState *state = g_object_get_data(G_OBJECT(source), "prefs-state");
    if (!state) {
        return;
    }

    menu_drop_clear_indicators(state);
    state->drag_from_index = -1;

    if (state->reload_menu_after_drag) {
        state->reload_menu_after_drag = FALSE;
        menu_list_reload(state);
    }
}

static gboolean on_menu_drop(
    GtkDropTarget *target,
    const GValue *value,
    gdouble x,
    gdouble y,
    gpointer user_data
) {
    (void)x;
    PreferencesState *state = g_object_get_data(G_OBJECT(target), "prefs-state");
    MenuItemData *target_item = user_data;
    if (!state || !target_item || !state->menu_items) {
        return FALSE;
    }

    guint from = (guint)-1;
    if (state->drag_from_index >= 0) {
        from = (guint)state->drag_from_index;
    } else if (value && G_VALUE_HOLDS(value, G_TYPE_UINT)) {
        from = g_value_get_uint(value);
    }
    if (from == (guint)-1 || from >= state->menu_items->len) {
        return FALSE;
    }

    gint target_index = menu_item_index(state, target_item);
    if (target_index < 0) {
        return FALSE;
    }

    GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
    menu_drop_clear_indicators(state);

    guint insert_before = (guint)target_index;
    gdouble height = GTK_IS_WIDGET(row) ? gtk_widget_get_height(row) : 0;
    if (height > 0 && y > height / 2.0) {
        insert_before = (guint)target_index + 1;
    }

    if (!menu_items_move_on_drop(
            state->menu_items,
            from,
            (guint)target_index,
            insert_before
        )) {
        return TRUE;
    }

    persist_menu_items(state);
    state->reload_menu_after_drag = TRUE;
    return TRUE;
}

static GdkDragAction on_menu_drop_enter(
    GtkDropTarget *target,
    gdouble x,
    gdouble y,
    gpointer user_data
) {
    (void)user_data;
    PreferencesState *state = g_object_get_data(G_OBJECT(target), "prefs-state");
    GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
    menu_drop_update_indicator(state, row, y);
    (void)x;
    return GDK_ACTION_MOVE;
}

static GdkDragAction on_menu_drop_motion(
    GtkDropTarget *target,
    gdouble x,
    gdouble y,
    gpointer user_data
) {
    (void)x;
    (void)user_data;
    PreferencesState *state = g_object_get_data(G_OBJECT(target), "prefs-state");
    GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
    menu_drop_update_indicator(state, row, y);
    return GDK_ACTION_MOVE;
}

static void on_menu_drop_leave(GtkDropTarget *target, gpointer user_data) {
    (void)user_data;
    PreferencesState *state = g_object_get_data(G_OBJECT(target), "prefs-state");
    menu_drop_clear_indicators(state);
}

static void attach_menu_row_dnd(
    PreferencesState *state,
    GtkWidget *row,
    GtkWidget *handle,
    MenuItemData *item
) {
    GtkDragSource *drag_source = gtk_drag_source_new();
    gtk_drag_source_set_actions(drag_source, GDK_ACTION_MOVE);
    g_object_set_data(G_OBJECT(drag_source), "prefs-state", state);
    g_signal_connect(drag_source, "prepare", G_CALLBACK(on_menu_drag_prepare), item);
    g_signal_connect(drag_source, "drag-begin", G_CALLBACK(on_menu_drag_begin), row);
    g_signal_connect(drag_source, "drag-end", G_CALLBACK(on_menu_drag_end), row);
    gtk_widget_add_controller(handle, GTK_EVENT_CONTROLLER(drag_source));

    GtkDropTarget *drop_target = gtk_drop_target_new(G_TYPE_UINT, GDK_ACTION_MOVE);
    gtk_drop_target_set_preload(drop_target, TRUE);
    g_object_set_data(G_OBJECT(drop_target), "prefs-state", state);
    g_signal_connect(drop_target, "drop", G_CALLBACK(on_menu_drop), item);
    g_signal_connect(drop_target, "enter", G_CALLBACK(on_menu_drop_enter), item);
    g_signal_connect(drop_target, "motion", G_CALLBACK(on_menu_drop_motion), item);
    g_signal_connect(drop_target, "leave", G_CALLBACK(on_menu_drop_leave), item);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(drop_target));
}

static GtkWidget *create_menu_row(PreferencesState *state, MenuItemData *item) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(row, "menu-drop-row");
    gtk_widget_set_margin_top(row, 8);
    gtk_widget_set_margin_bottom(row, 8);
    gtk_widget_set_margin_start(row, 12);
    gtk_widget_set_margin_end(row, 8);

    GtkWidget *handle = gtk_image_new_from_icon_name("list-drag-handle-symbolic");
    if (!gtk_icon_theme_has_icon(
            gtk_icon_theme_get_for_display(gdk_display_get_default()),
            "list-drag-handle-symbolic"
        )) {
        gtk_image_set_from_icon_name(GTK_IMAGE(handle), "open-menu-symbolic");
    }
    gtk_widget_set_tooltip_text(handle, "Drag to reorder");
    gtk_widget_add_css_class(handle, "dim-label");
    gtk_box_append(GTK_BOX(row), handle);

    if (item->separator) {
        GtkWidget *label = gtk_label_new("Separator");
        gtk_widget_add_css_class(label, "dim-label");
        gtk_widget_set_hexpand(label, TRUE);
        gtk_widget_set_halign(label, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(row), label);
    } else {
        GtkWidget *image = gtk_image_new_from_icon_name(
            (item->icon && *item->icon) ? item->icon : "application-x-executable-symbolic"
        );
        gtk_image_set_pixel_size(GTK_IMAGE(image), 24);
        gtk_box_append(GTK_BOX(row), image);

        GtkWidget *text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_hexpand(text_box, TRUE);
        GtkWidget *title = gtk_label_new(item->label ? item->label : "");
        gtk_widget_set_halign(title, GTK_ALIGN_START);
        gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
        GtkWidget *subtitle = gtk_label_new(item->command ? item->command : "");
        gtk_widget_set_halign(subtitle, GTK_ALIGN_START);
        gtk_widget_add_css_class(subtitle, "dim-label");
        gtk_widget_add_css_class(subtitle, "caption");
        gtk_label_set_ellipsize(GTK_LABEL(subtitle), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(text_box), title);
        gtk_box_append(GTK_BOX(text_box), subtitle);
        gtk_box_append(GTK_BOX(row), text_box);

        GtkWidget *edit = gtk_button_new_from_icon_name("document-edit-symbolic");
        gtk_widget_add_css_class(edit, "flat");
        g_object_set_data(G_OBJECT(edit), "menu-item", item);
        g_signal_connect(edit, "clicked", G_CALLBACK(on_menu_item_edit), state);
        gtk_box_append(GTK_BOX(row), edit);
    }

    GtkWidget *remove = gtk_button_new_from_icon_name("user-trash-symbolic");
    gtk_widget_add_css_class(remove, "flat");
    g_object_set_data(G_OBJECT(remove), "menu-item", item);
    g_signal_connect(remove, "clicked", G_CALLBACK(on_menu_item_remove), state);
    gtk_box_append(GTK_BOX(row), remove);

    attach_menu_row_dnd(state, row, handle, item);
    return row;
}

static void menu_list_reload(PreferencesState *state) {
    GtkWidget *child = gtk_widget_get_first_child(state->menu_list);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(GTK_LIST_BOX(state->menu_list), child);
        child = next;
    }

    if (!state->menu_items || state->menu_items->len == 0) {
        GtkWidget *empty = gtk_label_new("No menu items yet");
        gtk_widget_add_css_class(empty, "dim-label");
        gtk_widget_set_margin_top(empty, 16);
        gtk_widget_set_margin_bottom(empty, 16);
        gtk_list_box_append(GTK_LIST_BOX(state->menu_list), empty);
        return;
    }

    for (guint i = 0; i < state->menu_items->len; i++) {
        MenuItemData *item = g_ptr_array_index(state->menu_items, i);
        gtk_list_box_append(GTK_LIST_BOX(state->menu_list), create_menu_row(state, item));
    }
}

static void apps_list_reload(PreferencesState *state);

static void persist_apps(PreferencesState *state) {
    config_save_app_workspaces(state->data);
}

static void on_app_workspace_changed(GtkSpinButton *spin, gpointer user_data) {
    PreferencesState *state = g_object_get_data(G_OBJECT(spin), "prefs-state");
    AppWorkspaceAssignment *assignment = user_data;
    if (!state || !assignment) {
        return;
    }
    assignment->workspace = gtk_spin_button_get_value_as_int(spin);
    persist_apps(state);
}

static void on_app_remove(GtkButton *button, gpointer user_data) {
    PreferencesState *state = user_data;
    AppWorkspaceAssignment *assignment = g_object_get_data(G_OBJECT(button), "assignment");
    if (!state || !assignment || !state->data->app_workspaces) {
        return;
    }
    g_ptr_array_remove(state->data->app_workspaces, assignment);
    persist_apps(state);
    apps_list_reload(state);
}

static GtkWidget *create_app_row(PreferencesState *state, AppWorkspaceAssignment *assignment) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(row, 8);
    gtk_widget_set_margin_bottom(row, 8);
    gtk_widget_set_margin_start(row, 12);
    gtk_widget_set_margin_end(row, 8);

    GtkWidget *text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(text_box, TRUE);
    GtkWidget *title = gtk_label_new(assignment->app_id);
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    GtkWidget *subtitle = gtk_label_new("Workspace");
    gtk_widget_set_halign(subtitle, GTK_ALIGN_START);
    gtk_widget_add_css_class(subtitle, "dim-label");
    gtk_widget_add_css_class(subtitle, "caption");
    gtk_box_append(GTK_BOX(text_box), title);
    gtk_box_append(GTK_BOX(text_box), subtitle);
    gtk_box_append(GTK_BOX(row), text_box);

    GtkWidget *spin = gtk_spin_button_new_with_range(1, 64, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), assignment->workspace);
    g_object_set_data(G_OBJECT(spin), "prefs-state", state);
    g_signal_connect(spin, "value-changed", G_CALLBACK(on_app_workspace_changed), assignment);
    gtk_box_append(GTK_BOX(row), spin);

    GtkWidget *remove = gtk_button_new_from_icon_name("user-trash-symbolic");
    gtk_widget_add_css_class(remove, "flat");
    g_object_set_data(G_OBJECT(remove), "assignment", assignment);
    g_signal_connect(remove, "clicked", G_CALLBACK(on_app_remove), state);
    gtk_box_append(GTK_BOX(row), remove);

    return row;
}

static void apps_list_reload(PreferencesState *state) {
    GtkWidget *child = gtk_widget_get_first_child(state->apps_list);
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(GTK_LIST_BOX(state->apps_list), child);
        child = next;
    }

    if (!state->data->app_workspaces || state->data->app_workspaces->len == 0) {
        GtkWidget *empty = gtk_label_new("No application bindings yet");
        gtk_widget_add_css_class(empty, "dim-label");
        gtk_widget_set_margin_top(empty, 16);
        gtk_widget_set_margin_bottom(empty, 16);
        gtk_list_box_append(GTK_LIST_BOX(state->apps_list), empty);
        return;
    }

    for (guint i = 0; i < state->data->app_workspaces->len; i++) {
        AppWorkspaceAssignment *assignment =
            g_ptr_array_index(state->data->app_workspaces, i);
        gtk_list_box_append(
            GTK_LIST_BOX(state->apps_list),
            create_app_row(state, assignment)
        );
    }
}

static void on_open_app_chosen(GtkButton *button, gpointer user_data) {
    PreferencesState *state = g_object_get_data(G_OBJECT(button), "prefs-state");
    const char *app_id = g_object_get_data(G_OBJECT(button), "app-id");
    GtkWidget *dialog = user_data;
    if (!state || !app_id) {
        return;
    }

    if (!state->data->app_workspaces) {
        state->data->app_workspaces =
            g_ptr_array_new_with_free_func(app_workspace_assignment_free);
    }

    for (guint i = 0; i < state->data->app_workspaces->len; i++) {
        AppWorkspaceAssignment *existing =
            g_ptr_array_index(state->data->app_workspaces, i);
        if (existing && existing->app_id && strcmp(existing->app_id, app_id) == 0) {
            gtk_window_destroy(GTK_WINDOW(dialog));
            return;
        }
    }

    AppWorkspaceAssignment *assignment = g_new0(AppWorkspaceAssignment, 1);
    assignment->app_id = g_strdup(app_id);
    assignment->workspace = 1;
    g_ptr_array_add(state->data->app_workspaces, assignment);
    persist_apps(state);
    apps_list_reload(state);
    gtk_window_destroy(GTK_WINDOW(dialog));
}

static void show_add_app_dialog(PreferencesState *state) {
    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Add Application");
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(state->window));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 400, 360);
    g_signal_connect(dialog, "realize", G_CALLBACK(on_window_realize), NULL);

    GtkWidget *header = gtk_header_bar_new();
    gtk_window_set_titlebar(GTK_WINDOW(dialog), header);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_window_set_child(GTK_WINDOW(dialog), box);

    if (!workspace_backend_is_active()) {
        GtkWidget *label = gtk_label_new(
            "Listing open applications requires COSMIC Desktop."
        );
        gtk_label_set_wrap(GTK_LABEL(label), TRUE);
        gtk_widget_add_css_class(label, "dim-label");
        gtk_box_append(GTK_BOX(box), label);
        gtk_window_present(GTK_WINDOW(dialog));
        return;
    }

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_box_append(GTK_BOX(box), scrolled);

    GtkWidget *list = gtk_list_box_new();
    gtk_widget_add_css_class(list, "boxed-list");
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), list);

    GPtrArray *apps = workspace_list_open_apps();
    if (apps->len == 0) {
        GtkWidget *label = gtk_label_new("No open applications found.");
        gtk_widget_add_css_class(label, "dim-label");
        gtk_widget_set_margin_top(label, 16);
        gtk_widget_set_margin_bottom(label, 16);
        gtk_list_box_append(GTK_LIST_BOX(list), label);
    } else {
        for (guint i = 0; i < apps->len; i++) {
            const char *app_id = g_ptr_array_index(apps, i);
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            gtk_widget_set_margin_top(row, 10);
            gtk_widget_set_margin_bottom(row, 10);
            gtk_widget_set_margin_start(row, 12);
            gtk_widget_set_margin_end(row, 12);
            GtkWidget *label = gtk_label_new(app_id);
            gtk_widget_set_halign(label, GTK_ALIGN_START);
            gtk_widget_set_hexpand(label, TRUE);
            gtk_box_append(GTK_BOX(row), label);

            GtkWidget *choose = gtk_button_new_with_label("Add");
            gtk_widget_add_css_class(choose, "suggested-action");
            g_object_set_data(G_OBJECT(choose), "prefs-state", state);
            g_object_set_data_full(G_OBJECT(choose), "app-id", g_strdup(app_id), g_free);
            g_signal_connect(choose, "clicked", G_CALLBACK(on_open_app_chosen), dialog);
            gtk_box_append(GTK_BOX(row), choose);
            gtk_list_box_append(GTK_LIST_BOX(list), row);
        }
    }
    g_ptr_array_unref(apps);
    gtk_window_present(GTK_WINDOW(dialog));
}

static void on_add_app_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    show_add_app_dialog(user_data);
}

static GtkWidget *build_section_label(const char *text) {
    GtkWidget *label = gtk_label_new(text);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_add_css_class(label, "heading");
    gtk_widget_set_margin_bottom(label, 6);
    return label;
}

static GtkWidget *build_menu_page(PreferencesState *state) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_top(page, 24);
    gtk_widget_set_margin_bottom(page, 24);
    gtk_widget_set_margin_start(page, 24);
    gtk_widget_set_margin_end(page, 24);
    gtk_widget_set_hexpand(page, TRUE);

    gtk_box_append(GTK_BOX(page), build_section_label("Appearance"));

    GtkWidget *appearance = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(appearance), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(appearance, "boxed-list");
    gtk_box_append(GTK_BOX(page), appearance);

    GtkWidget *icon_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(icon_row, 10);
    gtk_widget_set_margin_bottom(icon_row, 10);
    gtk_widget_set_margin_start(icon_row, 12);
    gtk_widget_set_margin_end(icon_row, 12);

    GtkWidget *icon_text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(icon_text, TRUE);
    GtkWidget *icon_title = gtk_label_new("Tray icon");
    gtk_widget_set_halign(icon_title, GTK_ALIGN_START);
    GtkWidget *icon_subtitle = gtk_label_new("Shown in the system tray");
    gtk_widget_set_halign(icon_subtitle, GTK_ALIGN_START);
    gtk_widget_add_css_class(icon_subtitle, "dim-label");
    gtk_widget_add_css_class(icon_subtitle, "caption");
    gtk_box_append(GTK_BOX(icon_text), icon_title);
    gtk_box_append(GTK_BOX(icon_text), icon_subtitle);
    gtk_box_append(GTK_BOX(icon_row), icon_text);

    state->icon_image = gtk_image_new();
    gtk_image_set_pixel_size(GTK_IMAGE(state->icon_image), 24);
    state->icon_button = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(state->icon_button), state->icon_image);
    update_icon_button(state, state->data->indicator_icon);
    g_signal_connect(
        state->icon_button,
        "clicked",
        G_CALLBACK(on_indicator_icon_clicked),
        state
    );
    gtk_box_append(GTK_BOX(icon_row), state->icon_button);
    gtk_list_box_append(GTK_LIST_BOX(appearance), icon_row);

    gtk_box_append(GTK_BOX(page), build_section_label("Menu items"));

    state->menu_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(state->menu_list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(state->menu_list, "boxed-list");
    gtk_box_append(GTK_BOX(page), state->menu_list);
    menu_list_reload(state);

    GtkWidget *menu_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *add_item = gtk_button_new_with_label("Add Item");
    GtkWidget *add_sep = gtk_button_new_with_label("Add Separator");
    g_signal_connect(add_item, "clicked", G_CALLBACK(on_menu_add_item), state);
    g_signal_connect(add_sep, "clicked", G_CALLBACK(on_menu_add_separator), state);
    gtk_box_append(GTK_BOX(menu_actions), add_item);
    gtk_box_append(GTK_BOX(menu_actions), add_sep);
    gtk_box_append(GTK_BOX(page), menu_actions);

    return page;
}

static GtkWidget *build_apps_page(PreferencesState *state) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_top(page, 24);
    gtk_widget_set_margin_bottom(page, 24);
    gtk_widget_set_margin_start(page, 24);
    gtk_widget_set_margin_end(page, 24);

    gtk_box_append(GTK_BOX(page), build_section_label("Workspace bindings"));

    GtkWidget *hint = gtk_label_new(
        "When a matching application opens on COSMIC, its window is moved to the chosen workspace."
    );
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_widget_set_halign(hint, GTK_ALIGN_START);
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_box_append(GTK_BOX(page), hint);

    state->apps_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(state->apps_list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(state->apps_list, "boxed-list");
    gtk_box_append(GTK_BOX(page), state->apps_list);
    apps_list_reload(state);

    GtkWidget *add = gtk_button_new_with_label("Add");
    gtk_widget_add_css_class(add, "suggested-action");
    gtk_widget_set_halign(add, GTK_ALIGN_START);
    g_signal_connect(add, "clicked", G_CALLBACK(on_add_app_clicked), state);
    gtk_box_append(GTK_BOX(page), add);

    return page;
}

static void on_preferences_destroy(GtkWidget *window, gpointer user_data) {
    AppData *data = user_data;
    (void)window;
    data->preferences_window = NULL;
}

void preferences_show(AppData *data) {
    if (!data) {
        return;
    }

    preferences_apply_system_theme();
    preferences_ensure_drop_style();

    if (data->preferences_window) {
        gtk_window_present(GTK_WINDOW(data->preferences_window));
        return;
    }

    PreferencesState *state = g_new0(PreferencesState, 1);
    state->data = data;
    state->menu_items = config_load_editable_menu_items(data);
    state->drag_from_index = -1;

    GtkWidget *window = gtk_window_new();
    state->window = window;
    data->preferences_window = window;
    gtk_window_set_application(GTK_WINDOW(window), data->application);
    gtk_window_set_title(GTK_WINDOW(window), "TrayActions");
    gtk_window_set_default_size(GTK_WINDOW(window), 520, 580);
    gtk_window_set_modal(GTK_WINDOW(window), TRUE);
    g_signal_connect(window, "realize", G_CALLBACK(on_window_realize), NULL);
    g_object_set_data_full(
        G_OBJECT(window),
        "preferences-state",
        state,
        preferences_state_free
    );
    g_signal_connect(window, "destroy", G_CALLBACK(on_preferences_destroy), data);

    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), TRUE);
    gtk_window_set_titlebar(GTK_WINDOW(window), header);

    GtkWidget *stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    gtk_stack_add_titled(GTK_STACK(stack), build_menu_page(state), "menu", "Menu");
    gtk_stack_add_titled(
        GTK_STACK(stack),
        build_apps_page(state),
        "apps",
        "Workspaces"
    );

    GtkWidget *switcher = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(stack));
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), switcher);

    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(
        GTK_SCROLLED_WINDOW(scrolled),
        GTK_POLICY_NEVER,
        GTK_POLICY_AUTOMATIC
    );
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), stack);
    gtk_window_set_child(GTK_WINDOW(window), scrolled);

    gtk_window_present(GTK_WINDOW(window));
}
