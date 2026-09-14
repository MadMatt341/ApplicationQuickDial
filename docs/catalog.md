# App catalog reference

The first run creates `%LOCALAPPDATA%\ApplicationQuickDial\apps.json` with installed-app discovery enabled and an empty manual application list. Existing files are preserved. Use **Open app list** from the tray menu to open it. [Quick start](../README.md) · [Implementation](design/catalog-discovery.md)

## Examples

For a discovery-only setup, use `{"version": 1, "discoverInstalled": true, "applications": []}`. Older configurations may still contain starter entries; remove unwanted entries from `applications` using the tray menu's edit command. Updating the executable does not erase saved entries.

An example with optional manual entries is:

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

## Discovery and hidden apps

`discoverInstalled` defaults to `true`. Discovered applications are merged in memory after the configured entries and sorted alphabetically. Duplicate detection uses the launch target or app identifier, compared case-insensitively; matching display names alone do not hide distinct apps. Set it to `false` to use only the JSON list. `hiddenApplications` can contain a discovered application's display name, shell target, or AppUserModelID; matching is case-insensitive.

## Search aliases and identifiers

As a lower-priority fallback, search also considers AppUserModelIDs, executable or shortcut filenames, and custom URI schemes. Identifiers are split at punctuation and camel-case boundaries, so language-neutral or English identifiers can match localized display names—for example, `calc` can find `Kalkulator` through `Microsoft.WindowsCalculator`. Explicit `aliases` remain the fallback for names an application does not expose in its identifier or target.

## Manual entry fields

For manual entries, `name` and `target` are required. `id`, `aliases`, `arguments`, `workingDirectory`, and `icon` are optional. Environment variables are expanded in `target`, `workingDirectory`, and `icon`. Targets may be executables, shortcuts, documents, URLs, or `shell:` application identifiers. An optional `id` participates in duplicate detection and can be referenced by `hiddenApplications`.

## Reload and failure behavior

The JSON file is reloaded whenever the launcher opens. Windows app discovery runs in the background at startup and in response to Windows application-list, registration, or Start-menu changes. Related notifications are grouped into one refresh after a short pause, including while Quick Dial is hidden. Opening the launcher uses the cached app list; there is no periodic scanning. New apps appear after Windows registers them in its Apps folder and the refresh finishes; removed discovered apps disappear on refresh. Manual entries and the previous discovered list remain available while scanning, preserving your query and selection. A failed scan, including one that exceeds the 30-second timeout, retains the previous discovered list; another change notification or **Reload app list** retries discovery. Invalid edits never replace the last catalog that was parsed successfully.

## Icons

Icons also load in the background. Custom images keep their proportions, and multi-resolution ICO files use the best available size for the monitor's scale. Larger images are downscaled with transparency preserved; source dimensions are limited to 8192 pixels per side and 16 megapixels. Unsupported or oversized images fall back to the application's shell icon. **Reload app list** refreshes cached icons. The tray icon is restored automatically if Windows Explorer restarts.
