#include "menu_order.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void expect_true(gboolean cond, const char *name) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", name);
        failures++;
    } else {
        printf("PASS: %s\n", name);
    }
}

static GPtrArray *make_labels(const char **labels, guint n) {
    GPtrArray *items = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < n; i++) {
        g_ptr_array_add(items, g_strdup(labels[i]));
    }
    return items;
}

static gboolean labels_equal(GPtrArray *items, const char **expected, guint n) {
    if (items->len != n) {
        return FALSE;
    }
    for (guint i = 0; i < n; i++) {
        if (strcmp(g_ptr_array_index(items, i), expected[i]) != 0) {
            return FALSE;
        }
    }
    return TRUE;
}

static void test_move_before_append(void) {
    const char *start[] = {"A", "B", "C", "D"};
    const char *want[] = {"B", "C", "D", "A"};
    GPtrArray *items = make_labels(start, 4);
    expect_true(menu_items_move_before(items, 0, 4), "move A before end returns TRUE");
    expect_true(labels_equal(items, want, 4), "A moved to end");
    g_ptr_array_unref(items);
}

static void test_move_before_front(void) {
    const char *start[] = {"A", "B", "C", "D"};
    const char *want[] = {"D", "A", "B", "C"};
    GPtrArray *items = make_labels(start, 4);
    expect_true(menu_items_move_before(items, 3, 0), "move D before A returns TRUE");
    expect_true(labels_equal(items, want, 4), "D moved to front");
    g_ptr_array_unref(items);
}

static void test_move_before_middle(void) {
    const char *start[] = {"A", "B", "C", "D"};
    const char *want[] = {"B", "A", "C", "D"};
    GPtrArray *items = make_labels(start, 4);
    expect_true(menu_items_move_before(items, 0, 2), "move A before C returns TRUE");
    expect_true(labels_equal(items, want, 4), "A inserted before C");
    g_ptr_array_unref(items);
}

static void test_noop_same_position(void) {
    const char *start[] = {"A", "B", "C"};
    GPtrArray *items = make_labels(start, 3);
    expect_true(!menu_items_move_before(items, 1, 1), "insert before self is no-op");
    expect_true(!menu_items_move_before(items, 1, 2), "insert before next is no-op after adjust");
    expect_true(labels_equal(items, start, 3), "order unchanged");
    g_ptr_array_unref(items);
}

static void test_invalid_indices(void) {
    const char *start[] = {"A", "B"};
    GPtrArray *items = make_labels(start, 2);
    expect_true(!menu_items_move_before(items, 5, 0), "from out of range");
    expect_true(!menu_items_reorder(NULL, 0, 1), "NULL array");
    expect_true(labels_equal(items, start, 2), "order unchanged after invalid");
    g_ptr_array_unref(items);
}

static void test_reorder_direct(void) {
    const char *start[] = {"A", "B", "C"};
    const char *want[] = {"B", "A", "C"};
    GPtrArray *items = make_labels(start, 3);
    expect_true(menu_items_reorder(items, 0, 1), "reorder 0->1");
    expect_true(labels_equal(items, want, 3), "A and B swapped via reorder");
    g_ptr_array_unref(items);
}

static void test_drop_second_on_first_lower_half(void) {
    /* Drag B onto lower half of A: insert_before == 1 == from → used to no-op. */
    const char *start[] = {"A", "B", "C"};
    const char *want[] = {"B", "A", "C"};
    GPtrArray *items = make_labels(start, 3);
    expect_true(
        menu_items_move_on_drop(items, 1, 0, 1),
        "drop B on lower half of A returns TRUE"
    );
    expect_true(labels_equal(items, want, 3), "B and A swapped");
    g_ptr_array_unref(items);
}

static void test_drop_first_on_second_upper_half(void) {
    /* Drag A onto upper half of B: insert_before == 1 → used to no-op. */
    const char *start[] = {"A", "B", "C"};
    const char *want[] = {"B", "A", "C"};
    GPtrArray *items = make_labels(start, 3);
    expect_true(
        menu_items_move_on_drop(items, 0, 1, 1),
        "drop A on upper half of B returns TRUE"
    );
    expect_true(labels_equal(items, want, 3), "A moved below B");
    g_ptr_array_unref(items);
}

int main(void) {
    test_move_before_append();
    test_move_before_front();
    test_move_before_middle();
    test_noop_same_position();
    test_invalid_indices();
    test_reorder_direct();
    test_drop_second_on_first_lower_half();
    test_drop_first_on_second_upper_half();

    if (failures > 0) {
        fprintf(stderr, "\n%d test(s) failed\n", failures);
        return 1;
    }
    printf("\nAll menu_order tests passed\n");
    return 0;
}
