#!/usr/bin/env sh
set -eu

cd "$(dirname "$0")"
mkdir -p bin

cc -std=c11 -Wall -Wextra -Wpedantic \
    -Isrc \
    tests/test_menu_order.c src/menu_order.c \
    -o bin/test_menu_order \
    $(pkg-config --cflags --libs glib-2.0)

./bin/test_menu_order
