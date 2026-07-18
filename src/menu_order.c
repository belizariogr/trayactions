#include "menu_order.h"

gboolean menu_items_reorder(GPtrArray *items, guint from, guint to) {
    if (!items || items->len == 0) {
        return FALSE;
    }
    if (from >= items->len || to >= items->len || from == to) {
        return FALSE;
    }

    gpointer item = g_ptr_array_steal_index(items, from);
    g_ptr_array_insert(items, to, item);
    return TRUE;
}

gboolean menu_items_move_before(GPtrArray *items, guint from, guint insert_before) {
    if (!items || items->len == 0) {
        return FALSE;
    }
    if (from >= items->len) {
        return FALSE;
    }
    if (insert_before > items->len) {
        insert_before = items->len;
    }

    guint to = insert_before;
    if (to > from) {
        to--;
    }
    if (to == from) {
        return FALSE;
    }

    return menu_items_reorder(items, from, to);
}

/**
 * Drop helper: place @from at @target_index, using @insert_before from
 * half-row detection. If that would be a no-op on a different row
 * (typical when dropping an item onto its immediate neighbor), move
 * exactly to @target_index so adjacent swaps work.
 */
gboolean menu_items_move_on_drop(
    GPtrArray *items,
    guint from,
    guint target_index,
    guint insert_before
) {
    if (!items || from >= items->len || target_index >= items->len) {
        return FALSE;
    }

    if (menu_items_move_before(items, from, insert_before)) {
        return TRUE;
    }

    /* Adjacent no-op (e.g. drag item 1 onto lower half of item 0). */
    if (from != target_index) {
        return menu_items_reorder(items, from, target_index);
    }

    return FALSE;
}
