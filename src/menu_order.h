#ifndef MENU_ORDER_H
#define MENU_ORDER_H

#include <glib.h>

/**
 * Move the item at @from so it ends up at index @to in @items.
 * @items must not free elements on remove (steal is used).
 * Returns TRUE if the order changed.
 */
gboolean menu_items_reorder(GPtrArray *items, guint from, guint to);

/**
 * Move the item at @from so it is inserted before @insert_before
 * (0 .. len inclusive; len means append).
 * Returns TRUE if the order changed.
 */
gboolean menu_items_move_before(GPtrArray *items, guint from, guint insert_before);

/**
 * Reorder for a drop onto @target_index with half-row @insert_before.
 * Handles adjacent no-ops by moving exactly to @target_index.
 */
gboolean menu_items_move_on_drop(
    GPtrArray *items,
    guint from,
    guint target_index,
    guint insert_before
);

#endif
