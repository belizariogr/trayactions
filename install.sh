#!/usr/bin/env bash
set -euo pipefail

./compile.sh

sudo cp -f bin/trayactions /usr/local/bin/

DESKTOP_DIR="${HOME}/.local/share/applications"
mkdir -p "$DESKTOP_DIR"
cp -f trayactions.desktop "$DESKTOP_DIR/trayactions.desktop"
chmod 644 "$DESKTOP_DIR/trayactions.desktop"

# Prefer the newly installed binary on login (replace older /usr/bin autostart entries).
AUTOSTART_DIR="${HOME}/.config/autostart"
mkdir -p "$AUTOSTART_DIR"
cp -f trayactions.desktop "$AUTOSTART_DIR/trayactions.desktop"
# Resolve via PATH so /usr/local/bin wins over a stale /usr/bin copy.
sed -i 's|^Exec=.*|Exec=trayactions|' "$AUTOSTART_DIR/trayactions.desktop"
chmod 644 "$AUTOSTART_DIR/trayactions.desktop"

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$DESKTOP_DIR" >/dev/null 2>&1 || true
fi

echo "Installed trayactions to /usr/local/bin/"
echo "Installed desktop entry to $DESKTOP_DIR/trayactions.desktop"
echo "Installed autostart entry to $AUTOSTART_DIR/trayactions.desktop"

# Replace any running instance so this binary becomes the primary
# (GApplication single-instance would otherwise activate the old process and exit).
if pgrep -x trayactions >/dev/null 2>&1; then
    pkill -x trayactions 2>/dev/null || true
    # Wait briefly for the D-Bus name to be released.
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        if ! pgrep -x trayactions >/dev/null 2>&1; then
            break
        fi
        sleep 0.1
    done
fi

nohup trayactions > /dev/null 2>&1 &
