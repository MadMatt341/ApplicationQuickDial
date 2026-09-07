# Architecture

## Process model

Application Quick Dial has one resident GUI process with two resident threads, bounded background work, and a temporary discovery helper:

1. The main STA thread owns COM initialization, the hidden launcher window, tray icon, catalog state, Direct2D resources, and the Win32 message loop.
2. A hook thread installs `WH_KEYBOARD_LL` and runs the message loop required by the keyboard hook.
3. At most one discovery worker supervises a helper process without initializing COM in the resident process. Up to two icon workers perform Shell and WIC operations in their own COM apartments. Workers exit when their queues empty.
4. For each scan, the launcher starts another copy of its own executable in private `--discover-apps` mode. That process initializes a COM STA, enumerates Windows' Apps folder, returns plain values, and exits. It creates no window, tray icon, hook, or single-instance mutex. Shipping the app still requires only one executable.

`BackgroundTasks` limits both concurrency and outstanding work (one discovery request and 32 icon jobs, including completed results awaiting dispatch). Workers own shared queue state and value inputs. They post `kMessageBackgroundComplete` with no payload; the main thread drains owned completion objects. A 100 ms timer drains results while jobs are outstanding as a fallback if posting fails, and stops when the queues empty. There is no service, database, or network dependency. The JSON catalog is the only persisted application data; installed-app discovery is rebuilt from the Windows Shell namespace.

## Startup sequence

`wWinMain` performs the following work:

1. Handles the private discovery-helper mode and exits if requested, before any GUI or singleton setup.
2. Enables per-monitor v2 DPI awareness.
3. Initializes C++/WinRT as a single-threaded apartment.
4. Creates `Local\ApplicationQuickDial.SingleInstance`.
5. If another instance owns the mutex, finds its launcher window, posts `kMessageShowLauncher`, and exits.
6. Creates `LauncherApp`, initializes it, and enters its message loop.

`LauncherApp::Initialize` creates common controls, Direct2D and DirectWrite factories; registers and creates the popup window; adds the tray icon; starts the keyboard hook; loads the manual catalog; and queues discovery. Readiness does not wait for Shell enumeration. The window is created hidden, so normal startup shows only the tray icon. WIC factories are created in icon workers as needed.

When launched with benchmark events, the window also answers the read-only `kMessageBenchmarkState` diagnostic. Its flags distinguish protocol availability, unfinished discovery/icon work or visible painting, and current catalog/background errors. Ordinary launches return zero. The benchmark waits for completion before sampling memory; this diagnostic does not drain work or change scheduling.

## Shortcut flow

```text
keyboard event
  -> HookManager::HookProcedure (hook thread)
  -> HotkeyState::Handle
  -> kMessageToggleLauncher via PostMessageW
  -> LauncherApp::HandleMessage (main thread)
  -> ShowLauncher or HideLauncher
```

`HotkeyState` tracks left/right Windows keys and whether the Space chord is active. The first physical Space keydown while either Windows key is down triggers the launcher. Repeated Space keydowns and the matching physical keyup are suppressed, including when Windows is released first. Injected events pass through and cannot end a physical chord.

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

Windows' `shell:AppsFolder` namespace is enumerated asynchronously at startup and when the tray reload command is selected. Repeated explicit refresh requests coalesce into one follow-up scan. The discovered entries are cached between explicit refreshes; a failed initial scan is not retried on every open. Configured entries retain JSON order and take precedence; remaining discovered entries are de-duplicated and appended alphabetically. Discovery results are never written to the JSON file. Completion merges against the most recent valid configuration, preserving the query and selected target. If a file reload failed while scanning, completion updates only the discovery cache and leaves the displayed catalog and parse error intact until a valid reload.

Parsing uses `Windows.Data.Json`. A UTF-8 BOM is accepted. Version must be numeric `1`, `applications` must be an array, and every entry must have a non-empty string `name` and `target`. Known optional fields, including `discoverInstalled`, `hiddenApplications`, and entry `id`, are type-checked. Unknown root and application fields are ignored, allowing compatible additions.

Environment variables are expanded in `target`, `workingDirectory`, and `icon`. They are not expanded in `name`, `aliases`, or `arguments`.

`LauncherApp::ReloadCatalog` replaces `catalog_` only after a complete successful parse. On failure it stores an error string and keeps the last valid catalog. This permits a user to fix a malformed file without losing the working in-memory list. On first-run failure there is no valid catalog to search. A discovery failure is non-fatal: configured entries and the last successful discovery cache remain usable while the error is surfaced in the launcher and tray notification.

## Discovery process boundary

Windows' discovery components remain mapped after in-process enumeration completes. `DiscoveryProcess` keeps those components in the helper so they are released when it exits. The resident process receives only each application's display name, target, and optional AppUserModelID. Merging and ranking still run on the main thread using the existing rules; helper values never pass through catalog environment expansion.

The parent creates an unnamed, page-file-backed mapping capped at 4 MiB. The child inherits only that handle through `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`. A private versioned response contains a byte length, entry count, error string, and length-prefixed UTF-16 fields. `DiscoveryProtocol` rejects unsupported versions, truncated or oversized fields, embedded NULs, inconsistent responses, and more than 16,384 entries. Each string is limited to 32,768 UTF-16 code units. A failed write cannot leave a valid partial response.

The parent reads the mapping only after a normal helper exit, then unmaps and closes it before delivering the completion to the UI. A nonzero exit, malformed response, launch failure, or 30-second timeout produces an error through the existing catalog/status path. There is no in-process fallback that could retain the discovery DLLs again. Explicit reload still coalesces into at most one follow-up scan.

Each request creates a job with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`. `PROC_THREAD_ATTRIBUTE_JOB_LIST` assigns the helper atomically during `CreateProcessW`, covering parent termination during child creation as well as later crashes. The helper cannot inherit the job handle. A stop token wakes the supervising worker when the window is destroyed; the worker closes the job and bounds its cleanup wait to one second. Main-thread teardown never joins that worker. Windows also closes the job and terminates its processes if the launcher exits abruptly. See Microsoft's [process attributes](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-updateprocthreadattribute) and [job-object lifetime](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects) documentation.

## Search and selection

Every edit-control `EN_CHANGE` notification calls `UpdateResults`. The query is trimmed and lowercased with the invariant locale. Each application name is scored first:

| Score | Match |
|---:|---|
| 0 | exact name |
| 1 | name prefix |
| 2 | name match beginning after a non-alphanumeric boundary |
| 3 | other name substring |
| 4-7 | the same four match classes on an alias |
| 8-11 | the same four match classes on an AppUserModelID, target filename, or custom URI scheme |

Aliases are considered only when the name does not match, and language-neutral identifiers only when neither the name nor an alias matches. Identifier punctuation and camel case are treated as word boundaries. Web and file URI schemes are excluded because they are not application names. Results are stable-sorted by score, so equal scores retain catalog order. An empty query returns catalog order. The UI requests at most six results.

Up and Down wrap around the result list. Enter launches the selected result, Escape hides the window, and a left click launches the clicked row. Changing the query resets selection to the first result.

## Window and rendering

The launcher is a topmost `WS_POPUP`/`WS_EX_TOOLWINDOW`, so it does not create a normal taskbar button. It hides when deactivated.

On show, it chooses the foreground window's monitor, falling back to the cursor monitor, and positions itself near the upper center of that monitor's work area. Width and row dimensions are constants in device-independent pixels; `ResizeAndPosition` and `LayoutEditControl` scale them for monitor DPI. Height follows the number of visible results plus an optional error row.

The search field is a native edit control. The rest is rendered with Direct2D and DirectWrite. Theme colors follow `AppsUseLightTheme` and are refreshed whenever the launcher opens.

Icon resolution is lazy and follows this order:

1. Decode the catalog's explicit `icon` path with WIC.
2. Ask the Windows shell for an icon for `target`.
3. Draw a placeholder if neither source is available.

All file reads, image decoding, and shell icon extraction run in icon workers. Custom inputs are limited to 8192 pixels per side and 16 megapixels, then scaled to the requested display size (at most 96 pixels). Workers return premultiplied BGRA pixels, never apartment-bound COM interfaces or Direct2D resources. Only the main thread creates Direct2D bitmaps.

For ICO files, decoding chooses the smallest frame at least as large as the requested size, or the largest available frame if all are smaller. Other image formats retain their first frame. Shell extraction allows a larger native bitmap with `SIIGBF_BIGGERSIZEOK`, avoiding the Shell's default GDI resize. Shell HBITMAPs are imported with `WICBitmapUseAlpha` to describe their straight-alpha pixels. WIC converts to premultiplied alpha before downscaling with its Fant filter; exact-size images bypass scaling. Rendering centers each image within the icon slot, preserves its aspect ratio, and aligns its edges to physical pixels at the current monitor DPI.

Device-dependent bitmaps are cached by catalog index and released on catalog rebuild, hide, DPI change, or render-target loss. A separate least-recently-used source cache holds at most 128 scaled images or failed lookups, keyed by target, explicit icon path, and DPI, so reopening does not repeat decoding. Explicit tray reload clears source entries and advances a generation counter; late results from older generations are ignored.

## Launching

Each argument is quoted using Windows command-line escaping rules, then joined into one parameter string.

- A `shell:` target is opened by running `explorer.exe` with the shell identifier as its first parameter.
- All other targets are passed directly to `ShellExecuteExW` with the `open` verb. This supports executables, shortcuts, documents, and URLs through normal Windows associations.
- `workingDirectory`, when present, becomes `lpDirectory`.

A successful launch hides the launcher. A failure stays visible and adds an error row.

## Tray and sign-in integration

The shell delivers tray events to the main window as `kMessageTray`. The menu can open the launcher, open the catalog with Windows' registered JSON handler, reload it, toggle start-with-Windows, or destroy the window to exit.

The window registers `TaskbarCreated` and restores its icon after Explorer recreates the notification area. Failed registration retries every two seconds until successful; successful registration stops the retry timer. Repeated broadcasts can also update an icon that still exists.

Start-with-Windows is stored for the current user at:

```text
HKCU\Software\Microsoft\Windows\CurrentVersion\Run
  ApplicationQuickDial = "<absolute executable path>"
```

The menu is checked only when the stored value exactly matches the quoted path of the running executable.

## Ownership and cleanup

`LauncherApp` owns HWND-related state, GDI objects, COM factories, the render target, cached images, background queues, and `HookManager`. Window destruction requests discovery cancellation and stops the queues: it revokes their notification HWND under the same lock used for posting and discards queued jobs and completions. Detached workers hold only shared state and value inputs; late completions are destroyed without accessing the window or its owner. Shutdown never waits for a stalled Shell operation, and no completed thread handles accumulate. A stalled icon operation occupies only its bounded worker slot until it returns or the process exits; discovery runs in the separately supervised helper described above.

`HookManager::Stop` posts `WM_QUIT` to its thread and joins it. Window destruction removes the tray icon, stops timers, and posts the main-thread quit message. Destructors repeat safe cleanup for partial initialization and failure paths.
