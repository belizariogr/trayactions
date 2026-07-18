# TrayActions

TrayActions is a simple Linux tray application built with GTK 4. It displays a tray icon with a configurable menu. Configuration changes are monitored in real time, so the menu is reloaded automatically.

Tray integration uses the StatusNotifierItem and DBusMenu protocols directly through GIO. This avoids loading the GTK 3-only AppIndicator library in a GTK 4 process.

## Features
- Displays a tray icon with menu items defined in a JSON file.
- Each menu item can run a command or act as a separator.
- Monitors configuration changes and reloads automatically.

## Building
1. Make sure all dependencies are installed:  
   - Compiler and build tools (`build-essential`)
   - GTK 4 (`libgtk-4-dev`)
   - JSON-C (`libjson-c-dev`)
   - GIO (installed as a GTK dependency)
2. Run `./compile.sh` in the project folder.
3. The compiled binary will be placed in `bin/trayactions`.

## Dependencies
On Debian and Ubuntu based systems, install the build dependencies with:

```sh
sudo apt-get update && sudo apt-get install -y build-essential libgtk-4-dev libjson-c-dev
```

You can also run `./install_deps.sh`, which executes the same installation.

The desktop must provide a StatusNotifierItem host. GNOME normally requires an AppIndicator/KStatusNotifierItem shell extension; KDE Plasma and many other panels support it natively.

## Usage
Run `./bin/trayactions` to launch the app. A tray icon will appear, showing the configured menu. Only one instance is activated per desktop session.

## Configuration
- The default config file is located at `~/.config/trayactions/config.json`.
- Modify this file to customize menu items and icons.
- `preferences_icon` and `quit_icon` control the built-in Preferences and Quit items.
  Leave them empty (`""`) for no icon. If either key is missing, TrayActions adds it
  as an empty string in the config file.
- Changes are observed automatically, and the menu is reloaded on save.

## License
This project is distributed under open source terms. See the source code headers for details.
