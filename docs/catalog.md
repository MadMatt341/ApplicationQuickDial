# App catalog reference

The first run creates `%LOCALAPPDATA%\ApplicationQuickDial\apps.json` with installed-app discovery enabled and an empty manual application list. Existing files are preserved. Use **F10 → Open app list** to open it. [Quick start](../README.md) · [Implementation](design/catalog-discovery.md)

## Examples

For a discovery-only setup, use `{"version": 1, "discoverInstalled": true, "applications": []}`. Older configurations may still contain starter entries; remove unwanted entries from `applications` using the app-list edit command. Updating the executable does not erase saved entries.

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

`discoverInstalled` defaults to `true`. The launcher adds discovered applications after configured entries and sorts them alphabetically. It removes duplicate launch targets and equivalent commands. A command includes its executable, arguments, and working folder. The [duplicate matching rules](design/catalog-discovery.md#duplicate-matching) define known equivalent commands and missing Windows metadata.

Different browsers, profiles, web apps, arguments, and working folders keep separate entries. A display name alone does not identify an application. A configured `id` can override discovery for that identifier. Set `discoverInstalled` to `false` to use only the JSON list. `hiddenApplications` can contain an application's display name, shell target, or AppUserModelID. These exclusion rules ignore letter case.

Known uninstall executables and explicitly labelled maintenance scripts are excluded from automatic discovery. A label alone is insufficient: legitimate uninstall tools, documents and ordinary apps remain available. Manual entries are always retained. **F10 → Show maintenance entries** temporarily shows automatic maintenance entries without changing settings. Add a display name, original target or AppUserModelID to `includedApplications` for a permanent exception; matching is case-insensitive. Explicit `hiddenApplications` rules take precedence. Diagnostics reports excluded entries and their reasons.

## Register an application

Use **F10 → Add application…** to choose an executable or shortcut. Its filename supplies the initial display name; edit the catalog for aliases, arguments or a custom icon. Shortcut launch behavior is preserved.

Automated installers and development projects should call [Register-LauncherApp.ps1](registration.md), using a stable app `id`. It updates only that app and preserves other entries, unknown JSON fields and settings. Never assume a catalog write inside a packaged development tool updates the ordinary Windows account’s catalog.

## Search aliases and identifiers

As a lower-priority fallback, search also considers AppUserModelIDs, executable or shortcut filenames, and custom URI schemes. Identifiers are split at punctuation and camel-case boundaries, so language-neutral or English identifiers can match localized display names—for example, `calc` can find `Kalkulator` through `Microsoft.WindowsCalculator`. Explicit `aliases` remain the fallback for names an application does not expose in its identifier or target.

## Manual entry fields

For manual entries, `name` and `target` are required. `id`, `aliases`, `arguments`, `workingDirectory`, and `icon` are optional. Environment variables are expanded in `target`, `workingDirectory`, and `icon`. Targets may be executables, shortcuts, documents, URLs, or `shell:` application identifiers. An optional `id` participates in duplicate detection and can be referenced by `hiddenApplications`.

## Reload and failure behavior

The JSON file reloads whenever Launcher opens. Discovery independently reads Windows’ `shell:AppsFolder`, user Start-menu `Programs` and shared Start-menu `Programs`; `.lnk`, `.url` and `.appref-ms` shortcuts retain their original launch targets. Reading physical shortcuts does not depend on Explorer updating its app index. Portable apps with no Start-menu or Windows registration need a manual entry.

Related change notifications debounce for 750 ms, capped at three seconds so continuous installer activity cannot postpone discovery indefinitely. An event scan gets at most two readiness retries (after two and five seconds). Failed scans use the same finite budget. There is no periodic scanning. Opening reuses a fresh healthy cache; a failed/stale cache (five minutes), or degraded watcher, gets a catch-up scan. **Reload app list** is always available.

Each provider runs in its own sequential helper, with a 10-second wait deadline; a blocked provider cannot discard successful sources. Each successful source replaces only its own cache. A failed source retains its last good entries and reports itself as stale while other sources update. Per-item unreadable shortcuts are counted and skipped; incomplete provider enumeration is a source failure. Discovery and icon updates preserve the query and selected typed result. Invalid JSON retains the last valid visible catalog. Background messages without a tray preserve the query and cache rather than reloading settings.

## Icons

Icons also load in the background. Custom images keep their proportions, and multi-resolution ICO files use the best available size for the monitor's scale. Larger images are downscaled with transparency preserved; source dimensions are limited to 8192 pixels per side and 16 megapixels. Unsupported or oversized images fall back to the application's shell icon. **Reload app list** refreshes cached icons. The tray icon is restored automatically if Windows Explorer restarts.
