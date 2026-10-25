# Catalog and discovery internals

Read for parsing, merging, notifications, or helper transport changes. [User configuration contract](../catalog.md) · [Design map](../architecture.md) · [Verification](../testing.md#catalog-and-search)

## Catalog lifecycle

The preferred file is `%LOCALAPPDATA%\ApplicationQuickDial\apps.json`. If the Local AppData known folder cannot be resolved, the fallback is `apps.json` in the process working directory.

`EnsureDefaultCatalog` creates the directory and a version 1 catalog with discovery enabled and no manual entries only when the file does not exist. It never overwrites an existing file.

Version 1 catalogs may set `discoverInstalled` (default `true`) and provide `hiddenApplications` and `includedApplications` string arrays. Each hidden value is compared case-insensitively with a discovered application's display name, shell target, and AppUserModelID.

The JSON catalog is loaded:

- during application initialization;
- each time the launcher is shown; and
- when the tray menu's reload command is selected.

Discovery has three independent providers: Windows’ `shell:AppsFolder`, physical user Start-menu `Programs`, and physical shared Start-menu `Programs`. Start-menu recursion reads `.lnk`, `.url` and `.appref-ms`. The original shortcut remains the launch target, preserving its arguments, working directory, namespace/advertised behavior and associations. `IShellLinkW::GetPath(SLGP_RAWPATH)`, `GetArguments`, `GetWorkingDirectory`, and available app-ID properties supply classification/deduplication metadata. It never calls `Resolve`, avoiding repair/install/network effects during scanning. See [GetPath](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ishelllinkw-getpath) and [Resolve](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ishelllinkw-resolve).

AppsFolder remains the packaged-app provider. No raw executable/registry/package-manifest crawler is enabled: not every package executable is a launchable app, and a second package inventory does not establish activation identity. An app with no valid Windows or Start-menu registration requires [manual registration](../registration.md); diagnostics distinguishes absence from exclusions and source failures.

`InstalledAppsWatcher` subscribes independently to Apps-folder, association/protocol, and user/shared Start-menu changes. Valid subscriptions survive another subscription’s failure. Degraded subscriptions are retried on catalog reload. The kernel-directory handles remain waited on by the message loop; no watcher worker or periodic poll is added. Subscriptions precede the first scan and recover after `TaskbarCreated`.

`RefreshPolicy` owns a deterministic 750 ms debounce with a three-second maximum from the first event, and a finite two/five-second retry sequence after event scans or failures. A scan in progress coalesces events/reloads into one follow-up. A fresh healthy open reuses the cache; no healthy refresh, failure, cache age of five minutes, or watcher degradation triggers catch-up on open. Disabling discovery and teardown cancel delays/retries. Each successful source replaces only its source cache; failed providers preserve prior entries with a stale-source warning. Unreadable individual entries are skipped and counted; failed enumeration does not publish a partial source. Tray-free errors preserve the active query and do not initiate another scan.

Configured entries keep JSON order and take precedence. The launcher adds the other discovered entries in alphabetical order. The [duplicate matching rules](#duplicate-matching) define equivalent commands.

Automatic maintenance exclusions use exact known uninstall executable names or a maintenance label with command or identity data. `includedApplications` lets the launcher include an exception. `hiddenApplications` takes precedence. Manual entries bypass maintenance exclusions.

Discovery results are never written to the JSON file. Completion merges against the most recent valid configuration, preserving the query and selected target. Identical discovery results with an unchanged error state skip catalog rebuilding and icon invalidation. If a file reload failed while scanning, completion updates only the discovery cache and leaves the displayed catalog and parse error intact until a valid reload.

Parsing uses `Windows.Data.Json`. A UTF-8 BOM is accepted. Version must be numeric `1`, `applications` must be an array, and every entry must have a non-empty string `name` and `target`. Known optional fields, including `discoverInstalled`, `hiddenApplications`, `includedApplications`, and entry `id`, are type-checked. Unknown root and application fields are ignored, allowing compatible additions.

Environment variables are expanded in `target`, `workingDirectory`, and `icon`. They are not expanded in `name`, `aliases`, or `arguments`.

`LauncherApp::ReloadCatalog` replaces `catalog_` only after a complete successful parse. On failure it stores an error string and keeps the last valid catalog. This permits a user to fix a malformed file without losing the working in-memory list. On first-run failure there is no valid catalog to search. A discovery failure is non-fatal: configured entries and the last successful discovery cache remain usable while the error is surfaced in the launcher and tray notification.

## Duplicate matching

The launcher compares original launch targets, application identifiers with command data, and equivalent commands. A command includes its executable, arguments, and working folder. Windows file paths and application identifiers ignore letter case, but arguments and URI paths keep their letter case. A discovered identifier also includes the available executable path. Equal identifiers cannot hide different known executables. A configured `id` overrides discovered entries for that identifier.

A Windows indexed entry can have no working-folder data. A physical shortcut replaces this entry when its executable and equivalent arguments agree, with an equal display name or application identifier. If the indexed executable is unknown, the launcher compares the application identifier and arguments. The physical shortcut keeps its specified working folder. Only included shortcuts participate in this comparison. Different known working folders keep separate entries.

The launcher uses these rules for equivalent commands:

- Chromium browser commands can contain the launch-metrics flag `--source-shortcut-location=desktop`, `--source-shortcut-location=start-menu`, or `--source-shortcut-location=taskbar`. Comparison ignores only these flags for `brave.exe`, `chrome.exe`, `chrome_proxy.exe`, `msedge.exe`, `msedge_proxy.exe`, and `chromium.exe`. Flags after `--` stay as positional arguments. Comparison includes profiles, web-app identifiers, URLs, private-mode flags, and unknown arguments. The [Chromium switch contract](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/chrome/common/chrome_switches.h) identifies this flag as launch-metrics data.
- Squirrel commands require a `com.squirrel.` application identifier and `Update.exe --processStart <executable>`. Their working folder must be an `app-` child of the updater's folder. The requested executable must be a filename with the `.exe` extension. The optional `--process-start-args <arguments>` supplies the application's arguments. Other syntax keeps its original command identity. An equivalent stable updater shortcut takes precedence over a direct versioned executable.

Shortcut metadata uses the registered target for an advertised Windows Installer shortcut. The reader uses [MsiGetShortcutTargetW](https://learn.microsoft.com/en-us/windows/win32/api/msi/nf-msi-msigetshortcuttargetw) and [MsiGetComponentPathW](https://learn.microsoft.com/en-us/windows/win32/api/msi/nf-msi-msigetcomponentpathw). The reader accepts only an installed local file. For missing registrations, registry-key targets, or directories, the reader uses ordinary shortcut metadata. A namespace shortcut with no file path uses its stored PIDL target. URL shortcuts and indexed URLs use the same command format.

These operations read metadata only. They do not resolve, repair, install, or launch applications. The original shortcut stays as the launch target, with its arguments and working folder unchanged. Equal display names alone never remove an entry. Different browsers, profiles, web apps, application versions, and other command arguments keep separate entries.

## Discovery process boundary

Windows' discovery components remain mapped after in-process enumeration completes. `DiscoveryProcess` keeps those components in the helper so they are released when it exits. The resident process receives plain application values: display name, original target, optional AppUserModelID, source, resolved command metadata and launch key, plus each source’s success/error/skipped-entry status. Merging and ranking still run on the main thread using the existing rules; helper values never pass through catalog environment expansion.

Production scans supervise the physical user/shared menus first, then Windows apps, using one sequential temporary helper per source. Each gets one third of the 30-second total wait budget (10 seconds); at most one helper/mapping exists at once. A hung or crashing provider cannot prevent successful sources from publishing. Cancellation stops the sequence. An explicit executable override retains one whole-scan helper for deterministic process fixtures. The parent creates an unnamed, page-file-backed mapping capped at 4 MiB per helper. The child inherits only that handle through `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`. Private protocol version 2 contains a byte length, entry count, error string, and length-prefixed UTF-16 fields. `DiscoveryProtocol` rejects unsupported versions, truncated or oversized fields, embedded NULs, inconsistent responses, and more than 16,384 entries. Each string is limited to 32,768 UTF-16 code units. A failed write cannot leave a valid partial response.

The parent reads the mapping only after a normal helper exit, then unmaps and closes it before delivering the completion to the UI. A nonzero exit, malformed response, launch failure, or per-source timeout produces a stale-source error (cleanup is bounded to one additional second per timed-out helper) through the existing catalog/status path. There is no in-process fallback that could retain the discovery DLLs again. Explicit reload still coalesces into at most one follow-up scan.

Each request creates a job with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`. `PROC_THREAD_ATTRIBUTE_JOB_LIST` assigns the helper atomically during `CreateProcessW`, covering parent termination during child creation as well as later crashes. The helper cannot inherit the job handle. A stop token wakes the supervising worker when the window is destroyed; the worker closes the job and bounds its cleanup wait to one second. Main-thread teardown never joins that worker. Windows also closes the job and terminates its processes if the launcher exits abruptly. See Microsoft's [process attributes](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-updateprocthreadattribute) and [job-object lifetime](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects) documentation.
