#!/usr/bin/env bash
set -euo pipefail

./compile.sh

sudo cp -f bin/trayactions /usr/local/bin/

DESKTOP_DIR="${HOME}/.local/share/applications"
mkdir -p "$DESKTOP_DIR"
cp -f trayactions.desktop "$DESKTOP_DIR/trayactions.desktop"
chmod 644 "$DESKTOP_DIR/trayactions.desktop"

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$DESKTOP_DIR" >/dev/null 2>&1 || true
fi

echo "Installed trayactions to /usr/local/bin/"
echo "Installed desktop entry to $DESKTOP_DIR/trayactions.desktop"

nohup trayactions > /dev/null 2>&1 &
