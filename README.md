# Application Quick Dial

Application Quick Dial is a tiny Windows 11 launcher that searches applications registered with Windows plus any entries you add yourself. It stays in the notification tray, opens with `Win+Space`, and reads its preferences and manual entries from a plain JSON file.

The first run creates `%LOCALAPPDATA%\ApplicationQuickDial\apps.json` with a small curated set: ChatGPT/Codex, Obsidian, Slack, Brave, P4V, and UnrealGameSync. The launcher also discovers the launchable applications in Windows' Apps folder without writing that generated list into the JSON file.

## Build

Requirements:

- Windows 11 x64
- Visual Studio with the Desktop development with C++ workload
- Windows 11 SDK
- CMake 3.24 or newer

From a Visual Studio Developer PowerShell:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

To run the real-process performance contract, exit any resident Quick Dial instance and run:

```powershell
.\build\Release\quickdial_benchmarks.exe
```

The exact budgets, measurement definitions, and current baseline are documented in [docs/performance.md](docs/performance.md).

Run `build\Release\ApplicationQuickDial.exe`. The app starts in the notification tray rather than showing a normal taskbar window.

## Use

- Press `Win+Space` to show or hide the launcher.
- Type part of an application name or one of its aliases.
- Use Up/Down to select, Enter to launch, or Escape to hide.
- Left-click the tray icon to open the launcher.
- Right-click the tray icon to edit/reload the app list, opt into starting with Windows, or exit.

While Application Quick Dial is running, it intentionally replaces Windows' normal `Win+Space` keyboard-layout shortcut. The low-level shortcut is unavailable on the secure desktop and may not intercept input aimed at a higher-integrity application.

## App catalog

The catalog format is:

```json
{
  "version": 1,
  "discoverInstalled": true,
  "hiddenApplications": ["Microsoft.WindowsNotepad_8wekyb3d8bbwe!App"],
  "applications": [
    {
      "name": "ChatGPT",
      "target": "shell:AppsFolder\\OpenAI.Codex_2p2nqsd0c76g0!App",
      "aliases": ["codex", "openai"]
    },
    {
      "name": "Notepad",
      "target": "notepad.exe",
      "aliases": ["text"],
      "arguments": [],
      "workingDirectory": "%USERPROFILE%",
      "icon": "%SystemRoot%\\System32\\notepad.exe"
    }
  ]
}
```

`discoverInstalled` defaults to `true`. Discovered applications are merged in memory after the configured entries and sorted alphabetically. Set it to `false` to use only the JSON list. `hiddenApplications` can contain a discovered application's display name, shell target, or AppUserModelID; matching is case-insensitive.

For manual entries, `name` and `target` are required. `id`, `aliases`, `arguments`, `workingDirectory`, and `icon` are optional. Environment variables are expanded in `target`, `workingDirectory`, and `icon`. Targets may be executables, shortcuts, documents, URLs, or `shell:` application identifiers. An optional `id` participates in duplicate detection and can be referenced by `hiddenApplications`.

The JSON file is reloaded whenever the launcher opens. Windows app discovery runs at startup and when **Reload app list** is selected from the tray menu. Invalid edits never replace the last catalog that was parsed successfully.

## Developer documentation

- [Architecture](docs/architecture.md): process, threading, catalog, search, rendering, and launch flow.
- [Development guide](docs/development.md): build targets, change locations, tests, and manual verification.
- [Agent guide](AGENTS.md): repository constraints and implementation rules.
