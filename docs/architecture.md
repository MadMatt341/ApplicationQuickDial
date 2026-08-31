# Architecture

## Process model

Application Quick Dial is one GUI process with two threads:

1. The main STA thread owns COM initialization, the hidden launcher window, tray icon, catalog state, Direct2D resources, and the Win32 message loop.
2. A hook thread installs `WH_KEYBOARD_LL` and runs the message loop required by the keyboard hook.

There is no worker queue, service, database, or network dependency. The JSON catalog is the only persisted application data; installed-app discovery is rebuilt from the Windows Shell namespace.

## Startup sequence

`wWinMain` performs the following work:

1. Enables per-monitor v2 DPI awareness.
2. Initializes C++/WinRT as a single-threaded apartment.
3. Creates `Local\ApplicationQuickDial.SingleInstance`.
4. If another instance owns the mutex, finds its launcher window, posts `kMessageShowLauncher`, and exits.
5. Creates `LauncherApp`, initializes it, and enters its message loop.

`LauncherApp::Initialize` creates common controls, Direct2D, DirectWrite, and WIC factories; registers and creates the popup window; loads the catalog; adds the tray icon; and starts the keyboard hook. The window is created hidden, so normal startup shows only the tray icon.

## Shortcut flow

```text
keyboard event
  -> HookManager::HookProcedure (hook thread)
  -> HotkeyState::Handle
  -> kMessageToggleLauncher via PostMessageW
  -> LauncherApp::HandleMessage (main thread)
  -> ShowLauncher or HideLauncher
```

`HotkeyState` tracks left/right Windows keys and whether the Space chord is active. The first physical Space keydown while either Windows key is down triggers the launcher. Repeated Space keydowns and the matching keyup are suppressed. Injected events pass through.

When the chord triggers, `HookManager` injects a keydown/keyup pair for unused virtual key `0xE8`. This marks the Windows-key press as a chord so releasing Windows does not open Start. The state machine ignores that injected pair.

The hook cannot operate on the secure desktop and may not intercept input sent to a higher-integrity process. The tray icon remains the fallback entry point.

## Catalog lifecycle

The preferred file is `%LOCALAPPDATA%\ApplicationQuickDial\apps.json`. If the Local AppData known folder cannot be resolved, the fallback is `apps.json` in the process working directory.

`EnsureDefaultCatalog` creates the directory and a version 1 catalog only when the file does not exist. It never overwrites an existing file.

Version 1 catalogs may set `discoverInstalled` (default `true`) and provide a `hiddenApplications` string array. Each hidden value is compared case-insensitively with a discovered application's display name, shell target, and AppUserModelID.

The JSON catalog is loaded:

- during application initialization;
- each time the launcher is shown; and
- when the tray menu's reload command is selected.

Windows' `shell:AppsFolder` namespace is enumerated during initialization and when the tray reload command is selected. The discovered entries are cached between explicit refreshes, so opening the launcher only rereads the small JSON file. Configured entries retain JSON order and take precedence; remaining discovered entries are de-duplicated and appended alphabetically. Discovery results are never written to the JSON file.

Parsing uses `Windows.Data.Json`. A UTF-8 BOM is accepted. Version must be numeric `1`, `applications` must be an array, and every entry must have a non-empty string `name` and `target`. Known optional fields, including `discoverInstalled`, `hiddenApplications`, and entry `id`, are type-checked. Unknown root and application fields are ignored, allowing compatible additions.

Environment variables are expanded in `target`, `workingDirectory`, and `icon`. They are not expanded in `name`, `aliases`, or `arguments`.

`LauncherApp::ReloadCatalog` replaces `catalog_` only after a complete successful parse. On failure it stores an error string and keeps the last valid catalog. This permits a user to fix a malformed file without losing the working in-memory list. On first-run failure there is no valid catalog to search. A discovery failure is non-fatal: configured entries and the last successful discovery cache remain usable while the error is surfaced in the launcher and tray notification.

## Search and selection

Every edit-control `EN_CHANGE` notification calls `UpdateResults`. The query is trimmed and lowercased with the invariant locale. Each application name is scored first:

| Score | Match |
|---:|---|
| 0 | exact name |
| 1 | name prefix |
| 2 | name match beginning after a non-alphanumeric boundary |
| 3 | other name substring |
| 4-7 | the same four match classes on an alias |

Aliases are considered only when the name does not match. Results are stable-sorted by score, so equal scores retain catalog order. An empty query returns catalog order. The UI requests at most six results.

Up and Down wrap around the result list. Enter launches the selected result, Escape hides the window, and a left click launches the clicked row. Changing the query resets selection to the first result.

## Window and rendering

The launcher is a topmost `WS_POPUP`/`WS_EX_TOOLWINDOW`, so it does not create a normal taskbar button. It hides when deactivated.

On show, it chooses the foreground window's monitor, falling back to the cursor monitor, and positions itself near the upper center of that monitor's work area. Width and row dimensions are constants in device-independent pixels; `ResizeAndPosition` and `LayoutEditControl` scale them for monitor DPI. Height follows the number of visible results plus an optional error row.

The search field is a native edit control. The rest is rendered with Direct2D and DirectWrite. Theme colors follow `AppsUseLightTheme` and are refreshed whenever the launcher opens.

Icon resolution is lazy and follows this order:

1. Decode the catalog's explicit `icon` path with WIC.
2. Ask the Windows shell for an icon for `target`.
3. Use the generic application icon.

Attempts and successful bitmaps are cached by catalog index. The cache is cleared after a successful catalog reload, when the launcher hides, or when the Direct2D target must be recreated.

## Launching

Each argument is quoted using Windows command-line escaping rules, then joined into one parameter string.

- A `shell:` target is opened by running `explorer.exe` with the shell identifier as its first parameter.
- All other targets are passed directly to `ShellExecuteExW` with the `open` verb. This supports executables, shortcuts, documents, and URLs through normal Windows associations.
- `workingDirectory`, when present, becomes `lpDirectory`.

A successful launch hides the launcher. A failure stays visible and adds an error row.

## Tray and sign-in integration

The shell delivers tray events to the main window as `kMessageTray`. The menu can open the launcher, open the catalog with Windows' registered JSON handler, reload it, toggle start-with-Windows, or destroy the window to exit.

Start-with-Windows is stored for the current user at:

```text
HKCU\Software\Microsoft\Windows\CurrentVersion\Run
  ApplicationQuickDial = "<absolute executable path>"
```

The menu is checked only when the stored value exactly matches the quoted path of the running executable.

## Ownership and cleanup

`LauncherApp` owns HWND-related state, GDI objects, COM factories, the render target, cached bitmaps, and `HookManager`. `HookManager::Stop` posts `WM_QUIT` to its thread and joins it. Window destruction stops the hook, removes the tray icon, and posts the main-thread quit message. Destructors repeat safe cleanup for partial initialization and failure paths.
