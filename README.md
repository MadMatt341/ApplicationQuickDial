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
- Type part of an application name, one of its aliases, or its language-neutral app identifier.
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

As a lower-priority fallback, search also considers AppUserModelIDs, executable or shortcut filenames, and custom URI schemes. Identifiers are split at punctuation and camel-case boundaries, so language-neutral or English identifiers can match localized display names—for example, `calc` can find `Kalkulator` through `Microsoft.WindowsCalculator`. Explicit `aliases` remain the fallback for names an application does not expose in its identifier or target.

For manual entries, `name` and `target` are required. `id`, `aliases`, `arguments`, `workingDirectory`, and `icon` are optional. Environment variables are expanded in `target`, `workingDirectory`, and `icon`. Targets may be executables, shortcuts, documents, URLs, or `shell:` application identifiers. An optional `id` participates in duplicate detection and can be referenced by `hiddenApplications`.

The JSON file is reloaded whenever the launcher opens. Windows app discovery runs in the background at startup and when **Reload app list** is selected from the tray menu. Manual entries are available immediately; discovered entries appear when scanning finishes, preserving your query and selection. A failed scan, including one that exceeds the 30-second timeout, retains the previous discovered list and can be retried with **Reload app list**. Invalid edits never replace the last catalog that was parsed successfully.

Icons also load in the background. Custom images keep their proportions, and multi-resolution ICO files use the best available size for the monitor's scale. Larger images are downscaled with transparency preserved; source dimensions are limited to 8192 pixels per side and 16 megapixels. Unsupported or oversized images fall back to the application's shell icon. **Reload app list** refreshes cached icons. The tray icon is restored automatically if Windows Explorer restarts.

## Developer documentation

- [Architecture](docs/architecture.md): process, threading, catalog, search, rendering, and launch flow.
- [Development guide](docs/development.md): build targets, change locations, tests, and manual verification.
- [Agent guide](AGENTS.md): repository constraints and implementation rules.
