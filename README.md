# TrayActions

TrayActions is a simple Linux tray application using GTK and AppIndicator. It displays a tray icon with a configurable menu. Configuration changes are monitored in real-time, so the menu can be reloaded automatically.

## Features
- Displays a tray icon with menu items defined in a JSON file.
- Each menu item can run a command or act as a separator.
- Monitors configuration changes and reloads automatically.

## Building
1. Make sure all dependencies are installed:  
   - Compiler (`gcc`)
   - GTK+ 3.0 (`libgtk-3-dev`)  
   - AppIndicator 3 (`libappindicator3-dev`)  
   - JSON-C (`libjson-c-dev`)  
   - And the runtime library `libappindicator3-1`
2. Run `./compile.sh` in the project folder.
3. The compiled binary will be placed in `bin/trayactions`.

## Dependencies
The runtime `libappindicator3-1` is required to run and build.

## Usage
Run `./bin/trayactions` to launch the app. A tray icon will appear, showing the configured menu.

## Configuration
- The default config file is located at `~/.config/trayactions/config.json`.
- Modify this file to customize menu items and icons.  
- Changes are observed automatically, and the menu is reloaded on save.

## License
This project is distributed under open source terms. See the source code headers for details.