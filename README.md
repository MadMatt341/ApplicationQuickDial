# Application Quick Dial

A small Windows 11 x64 launcher for installed applications and optional manual entries. It lives in the notification tray and opens with **Win+Space**.

## Install

Download the Windows x64 ZIP from [GitHub Releases](https://github.com/MadMatt341/ApplicationQuickDial/releases), extract it to a permanent folder, and run `ApplicationQuickDial.exe`. No installer or separate C++ runtime is needed. Startup shows a tray icon rather than a taskbar window.

Right-click the tray icon and enable **Start with Windows** if wanted. Launching the executable again opens the existing instance on your Windows desktop.

To update, exit through the tray menu and replace the executable in the same folder. Your catalog is preserved. To remove, disable **Start with Windows**, exit, and delete the extracted folder. Optionally delete `%LOCALAPPDATA%\ApplicationQuickDial` to remove saved preferences.

Release binaries are unsigned and Windows may show a security warning. Each ZIP has a SHA-256 checksum.

## Use

- **Win+Space** shows or hides the search bar. Type an application name, alias, or app identifier; clearing the query collapses the results.
- **Up/Down** selects, **Enter** launches, and **Escape** hides. Scroll or use the arrow keys to reach all matches; at most six rows are visible.
- Click a result to launch it. Left-click the tray icon to open the launcher; right-click for configuration, reload, startup, and exit commands.
- Installed applications are discovered automatically and refreshed when Windows reports changes.

While running, Quick Dial replaces Windows' normal **Win+Space** keyboard-layout shortcut. The shortcut is unavailable on the secure desktop and may not intercept input aimed at an elevated application; the tray icon is the fallback.

## Configure or develop

Preferences and optional manual entries live in `%LOCALAPPDATA%\ApplicationQuickDial\apps.json`. First run creates a discovery-only catalog; updating the executable does not overwrite it.

- [Catalog reference](https://github.com/MadMatt341/ApplicationQuickDial/blob/main/docs/catalog.md): JSON examples, supported fields, aliases, hidden apps, and icons.
- [Troubleshooting](https://github.com/MadMatt341/ApplicationQuickDial/blob/main/docs/troubleshooting.md): startup, shortcut, and catalog diagnosis.
- [Development map](docs/development.md): build commands and task-specific source/reference links.
- [Architecture overview](docs/architecture.md): system boundaries and deeper design references.
- [Agent instructions](AGENTS.md): scoped working rules.

## License and support

Copyright (c) 2026 MadMatt341. Released under the [MIT license](LICENSE). This is a personal utility maintained as time allows; support and release timelines are not guaranteed.
