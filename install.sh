#!/usr/bin/env bash
set -euo pipefail

./compile.sh

sudo cp -f bin/trayactions /usr/local/bin/

DESKTOP_DIR="${HOME}/.local/share/applications"
AUTOSTART_DIR="${HOME}/.config/autostart"
CONFIG_FILE="${HOME}/.config/trayactions/config.json"

# Keep the user's chosen app icon across reinstalls (do not reset to the
# stock Icon= from the repo .desktop template).
resolve_desktop_icon() {
    local icon=""
    if [ -f "$CONFIG_FILE" ]; then
        icon="$(sed -n 's/.*"indicator_icon"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$CONFIG_FILE" | head -n1)"
    fi
    if [ -z "$icon" ] && [ -f "$DESKTOP_DIR/trayactions.desktop" ]; then
        icon="$(sed -n 's/^Icon=//p' "$DESKTOP_DIR/trayactions.desktop" | head -n1)"
    fi
    if [ -z "$icon" ]; then
        icon="$(sed -n 's/^Icon=//p' trayactions.desktop | head -n1)"
    fi
    printf '%s\n' "$icon"
}

DESKTOP_ICON="$(resolve_desktop_icon)"

mkdir -p "$DESKTOP_DIR"
cp -f trayactions.desktop "$DESKTOP_DIR/trayactions.desktop"
sed -i "s|^Icon=.*|Icon=${DESKTOP_ICON}|" "$DESKTOP_DIR/trayactions.desktop"
chmod 644 "$DESKTOP_DIR/trayactions.desktop"

# Prefer the newly installed binary on login (replace older /usr/bin autostart entries).
mkdir -p "$AUTOSTART_DIR"
cp -f trayactions.desktop "$AUTOSTART_DIR/trayactions.desktop"
sed -i 's|^Exec=.*|Exec=/usr/local/bin/trayactions|' "$AUTOSTART_DIR/trayactions.desktop"
sed -i "s|^Icon=.*|Icon=${DESKTOP_ICON}|" "$AUTOSTART_DIR/trayactions.desktop"
chmod 644 "$AUTOSTART_DIR/trayactions.desktop"

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$DESKTOP_DIR" >/dev/null 2>&1 || true
fi

echo "Installed trayactions to /usr/local/bin/"
echo "Installed desktop entry to $DESKTOP_DIR/trayactions.desktop (Icon=${DESKTOP_ICON})"
echo "Installed autostart entry to $AUTOSTART_DIR/trayactions.desktop"

# Replace any running instance so this binary becomes the primary
# (GApplication single-instance would otherwise activate the old process and exit).
if pgrep -x trayactions >/dev/null 2>&1; then
    pkill -x trayactions 2>/dev/null || true
    # Wait for the process and D-Bus name to go away (up to ~5s).
    for _ in $(seq 1 50); do
        if ! pgrep -x trayactions >/dev/null 2>&1; then
            break
        fi
        sleep 0.1
    done
    for _ in $(seq 1 30); do
        if ! gdbus call --session \
            --dest org.freedesktop.DBus \
            --object-path /org/freedesktop/DBus \
            --method org.freedesktop.DBus.NameHasOwner \
            io.github.belizario.TrayActions 2>/dev/null | grep -q true; then
            break
        fi
        sleep 0.1
    done
fi

# Pin the newly installed binary — do not rely on PATH (/usr/bin may be stale).
nohup /usr/local/bin/trayactions > /dev/null 2>&1 &
