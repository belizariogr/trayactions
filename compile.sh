#!/usr/bin/env sh
set -eu

cd "$(dirname "$0")"
mkdir -p bin src/generated

PROTOCOLS="
ext-foreign-toplevel-list-v1
ext-workspace-v1
cosmic-toplevel-info-unstable-v1
cosmic-toplevel-management-unstable-v1
cosmic-workspace-unstable-v1
"

GENERATED_SRCS=""
for name in $PROTOCOLS; do
    xml="protocols/${name}.xml"
    header="src/generated/${name}-client.h"
    source="src/generated/${name}-protocol.c"
    wayland-scanner client-header "$xml" "$header"
    wayland-scanner private-code "$xml" "$source"
    GENERATED_SRCS="$GENERATED_SRCS $source"
done

cc -std=c11 -Wall -Wextra -Wpedantic \
    src/main.c src/config.c src/menu.c src/tray.c src/utils.c \
    src/preferences.c src/workspace.c src/cosmic_workspace.c src/menu_order.c \
    $GENERATED_SRCS \
    -o bin/trayactions \
    $(pkg-config --cflags --libs gtk4 gio-2.0 json-c gdk-pixbuf-2.0 wayland-client)

echo "Compilation successful: bin/trayactions"
