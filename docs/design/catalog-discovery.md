# Catalog and discovery internals

Read for parsing, merging, notifications, or helper transport changes. [User configuration contract](../catalog.md) · [Design map](../architecture.md) · [Verification](../testing.md#catalog-and-search)

## Catalog lifecycle

The preferred file is `%LOCALAPPDATA%\ApplicationQuickDial\apps.json`. If the Local AppData known folder cannot be resolved, the fallback is `apps.json` in the process working directory.

`EnsureDefaultCatalog` creates the directory and a version 1 catalog with discovery enabled and no manual entries only when the file does not exist. It never overwrites an existing file.

Version 1 catalogs may set `discoverInstalled` (default `true`) and provide a `hiddenApplications` string array. Each hidden value is compared case-insensitively with a discovered application's display name, shell target, and AppUserModelID.

The JSON catalog is loaded:

- during application initialization;
- each time the launcher is shown; and
- when the tray menu's reload command is selected.

Windows' `shell:AppsFolder` namespace is enumerated asynchronously at startup, after a relevant Windows change notification, and when the tray reload command is selected. Opening the launcher reuses cached discovery results without scanning. `InstalledAppsWatcher` subscribes to virtual Apps-folder changes and global association/protocol registration changes with `SHChangeNotifyRegister`. It watches the current-user and shared Start-menu trees with `FindFirstChangeNotificationW` for file/directory names and last-write changes. Path lookup avoids loading filesystem Shell extensions just to obtain notification PIDLs. Subscriptions are installed before the initial scan and restored after `TaskbarCreated`, with one catch-up scan after Explorer recovery.

A one-shot 750 ms window timer groups related events; each new event resets the delay. It is active only after a change, including while the launcher is hidden. If its delay expires during an existing scan, one follow-up scan is requested. Explicit reloads also coalesce into one follow-up scan. Disabling discovery or destroying the window releases subscriptions and cancels the delay. Subscription failures are surfaced through the existing status/tray paths and retried on catalog reload; there is no silent fallback to polling. A scan failure retains the previous discovered list and is retried on a later notification or explicit reload. Repeated identical errors do not repeat tray notifications.

Configured entries retain JSON order and take precedence; remaining discovered entries are de-duplicated by case-insensitive target or app identifier and appended alphabetically. Display names are not identity keys, so different applications with the same name remain searchable. Discovery results are never written to the JSON file. Completion merges against the most recent valid configuration, preserving the query and selected target. Identical discovery results with an unchanged error state skip catalog rebuilding and icon invalidation. If a file reload failed while scanning, completion updates only the discovery cache and leaves the displayed catalog and parse error intact until a valid reload.

Parsing uses `Windows.Data.Json`. A UTF-8 BOM is accepted. Version must be numeric `1`, `applications` must be an array, and every entry must have a non-empty string `name` and `target`. Known optional fields, including `discoverInstalled`, `hiddenApplications`, and entry `id`, are type-checked. Unknown root and application fields are ignored, allowing compatible additions.

Environment variables are expanded in `target`, `workingDirectory`, and `icon`. They are not expanded in `name`, `aliases`, or `arguments`.

`LauncherApp::ReloadCatalog` replaces `catalog_` only after a complete successful parse. On failure it stores an error string and keeps the last valid catalog. This permits a user to fix a malformed file without losing the working in-memory list. On first-run failure there is no valid catalog to search. A discovery failure is non-fatal: configured entries and the last successful discovery cache remain usable while the error is surfaced in the launcher and tray notification.

## Discovery process boundary

Windows' discovery components remain mapped after in-process enumeration completes. `DiscoveryProcess` keeps those components in the helper so they are released when it exits. The resident process receives only each application's display name, target, and optional AppUserModelID. Merging and ranking still run on the main thread using the existing rules; helper values never pass through catalog environment expansion.

The parent creates an unnamed, page-file-backed mapping capped at 4 MiB. The child inherits only that handle through `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`. A private versioned response contains a byte length, entry count, error string, and length-prefixed UTF-16 fields. `DiscoveryProtocol` rejects unsupported versions, truncated or oversized fields, embedded NULs, inconsistent responses, and more than 16,384 entries. Each string is limited to 32,768 UTF-16 code units. A failed write cannot leave a valid partial response.

The parent reads the mapping only after a normal helper exit, then unmaps and closes it before delivering the completion to the UI. A nonzero exit, malformed response, launch failure, or 30-second timeout produces an error through the existing catalog/status path. There is no in-process fallback that could retain the discovery DLLs again. Explicit reload still coalesces into at most one follow-up scan.

Each request creates a job with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`. `PROC_THREAD_ATTRIBUTE_JOB_LIST` assigns the helper atomically during `CreateProcessW`, covering parent termination during child creation as well as later crashes. The helper cannot inherit the job handle. A stop token wakes the supervising worker when the window is destroyed; the worker closes the job and bounds its cleanup wait to one second. Main-thread teardown never joins that worker. Windows also closes the job and terminates its processes if the launcher exits abruptly. See Microsoft's [process attributes](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-updateprocthreadattribute) and [job-object lifetime](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects) documentation.
