#!/usr/bin/env sh
set -eu

cd "$(dirname "$0")"
mkdir -p bin

cc -std=c11 -Wall -Wextra -Wpedantic \
    src/main.c src/config.c src/menu.c src/tray.c src/utils.c \
    -o bin/trayactions \
    $(pkg-config --cflags --libs gtk4 gio-2.0 json-c gdk-pixbuf-2.0)

echo "Compilation successful: bin/trayactions"
