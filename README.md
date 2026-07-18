# TrayActions

TrayActions is a simple Linux tray application built with GTK 4. It displays a tray icon with a configurable menu. Configuration changes are monitored in real time, so the menu is reloaded automatically.

Tray integration uses the StatusNotifierItem and DBusMenu protocols directly through GIO. This avoids loading the GTK 3-only AppIndicator library in a GTK 4 process.

On **COSMIC Desktop**, TrayActions can also move newly opened application windows to a configured workspace using native Wayland protocols (no third-party helpers). On GNOME and other desktops the app runs normally; workspace routing stays inactive.

## Features
- Displays a tray icon with menu items defined in a JSON file.
- Each menu item can run a command or act as a separator.
- Monitors configuration changes and reloads automatically.
- GTK 4 Preferences window (Menu icon + Apps/Workspaces tabs).
- COSMIC: route new app windows to a chosen workspace by `app_id`.
- CLI launchers: `--run` always starts a command; `--run-or-focus` focuses an
  existing window by `app_id` when possible, otherwise starts the command.
  These modes never open the tray and exit as soon as the request is sent.
- Re-registers the tray icon when the panel's StatusNotifierWatcher appears
  after login (fixes missing icon on reboot), when the tray host restarts, and
  via a periodic health check if the icon is dropped while the process is still
  running (COSMIC status-area watcher).

## Building
1. Make sure all dependencies are installed:  
   - Compiler and build tools (`build-essential`)
   - GTK 4 (`libgtk-4-dev`)
   - JSON-C (`libjson-c-dev`)
   - Wayland client (`libwayland-dev`, for `wayland-scanner` and headers)
   - GIO (installed as a GTK dependency)
2. Run `./compile.sh` in the project folder.
3. The compiled binary will be placed in `bin/trayactions`.

Pushing a version tag such as `v2.0.1` triggers a GitHub Actions workflow that
builds the project for Linux `x86_64` and `aarch64` and publishes a release with
both binaries attached.

## Dependencies
On Debian and Ubuntu based systems, install the build dependencies with:

```sh
sudo apt-get update && sudo apt-get install -y build-essential libgtk-4-dev libjson-c-dev libwayland-dev
```

You can also run `./install_deps.sh`, which executes the same installation.

Runtime needs only libraries already pulled in by GTK 4 (including `libwayland-client`). No extra packages are required on GNOME. Workspace routing uses Cosmic compositor protocols already present on COSMIC Desktop.

The desktop must provide a StatusNotifierItem host. GNOME normally requires an AppIndicator/KStatusNotifierItem shell extension; KDE Plasma, COSMIC, and many other panels support it natively.

## Usage
Run `./bin/trayactions` to launch the app. A tray icon will appear, showing the configured menu. Only one instance is activated per desktop session.

Launch (or focus) apps without starting the tray:

```sh
./bin/trayactions --run google-chrome
./bin/trayactions --run-or-focus google-chrome --app-id=google-chrome
./bin/trayactions --run-or-focus "google-chrome www.uol.com.br" --app-id=chrome2
```

`--run` always starts the command and exits immediately (does not wait for the
app). `--run-or-focus` looks for an open window with the given `--app-id` on
COSMIC and focuses it; if none is found (or focus is unavailable), it runs the
command like `--run`. If the executable does not exist, the process exits with
status `127`.

Open **Preferences** from the tray menu to:
- change the tray indicator icon (24×24 theme icon picker);
- edit tray menu items (label, command, icon, separators);
- map open applications to workspace numbers (COSMIC).

The preferences window opens as a modal dialog (so tiling layouts typically leave it floating) and follows the system light/dark preference from GNOME/`org.gnome.desktop.interface`.

## Configuration
- The default config file is located at `~/.config/trayactions/config.json`.
- Modify this file to customize menu items and icons.
- `preferences_icon` and `quit_icon` control the built-in Preferences and Quit items.
  Leave them empty (`""`) for no icon. If either key is missing, TrayActions adds it
  as an empty string in the config file.
- `app_workspaces` maps application IDs to 1-based workspace indexes, for example:

```json
"app_workspaces": [
  { "app_id": "org.mozilla.firefox", "workspace": 2 }
]
```

- Changes are observed automatically, and the menu is reloaded on save.

## License
This project is distributed under open source terms. See the source code headers for details.
