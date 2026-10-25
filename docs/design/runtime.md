# Runtime and Windows integration

Read for process lifetime, threading, startup, singleton, tray, or sign-in work. [Design map](../architecture.md) · [Verification](../testing.md#basic-lifecycle) · [Startup troubleshooting](../troubleshooting.md#startup-and-desktop)

## Process model

Blade Launcher has one resident GUI process with two resident threads, bounded background work, and a temporary discovery helper:

1. The main STA thread owns COM initialization, the hidden launcher window, tray icon, catalog state, Direct2D resources, and the Win32 message loop.
2. A hook thread installs `WH_KEYBOARD_LL` and runs the message loop required by the keyboard hook.
3. At most one discovery worker supervises a helper process without initializing COM in the resident process. Up to two icon workers perform Shell and WIC operations in their own COM apartments. Workers exit when their queues empty.
4. For each scan, the launcher starts another copy of its own executable in private `--discover-apps` mode. That process initializes a COM STA, reads one Windows-app or physical Start-menu provider, returns plain values, and exits. It creates no window, tray icon, hook, or single-instance mutex. Discovery still uses the same executable; automated account-verified registration is shipped as the optional self-contained `Register-LauncherApp.ps1`.

`BackgroundTasks` limits both concurrency and outstanding work (one discovery request and 32 icon jobs, including completed results awaiting dispatch). Workers own shared queue state and value inputs. They post `kMessageBackgroundComplete` with no payload; the main thread drains owned completion objects. A 100 ms timer drains results while jobs are outstanding as a fallback if posting fails, and stops when the queues empty. There is no service, database, or network dependency. The JSON catalog is the only persisted application data; installed-app discovery is rebuilt from Windows apps and physical Start-menu sources.

`InstalledAppsWatcher` owns up to two Shell notification registrations and two kernel directory-change handles; partial subscriptions remain usable and visible diagnostics identify degraded operation. The main loop blocks in `MsgWaitForMultipleObjectsEx` for window messages or directory changes; watching adds no resident worker or polling loop. Shell notifications arrive as `kMessageInstalledApplicationsChanged`, and their shared-memory payload is locked and released on the main thread. No enumeration runs in these handlers.

An armed [sign-in input trace](#sign-in-input-trace) adds one temporary worker thread.
This worker records input observations and Windows focus state.
The worker stops after two minutes from the shared Blade startup clock, or when the log reaches its size limit.

## Startup sequence

`wWinMain` performs the following work:

1. Handles the private discovery-helper mode and exits if requested, before any GUI or singleton setup.
2. Enables per-monitor v2 DPI awareness.
3. Initializes C++/WinRT as a single-threaded apartment.
4. Creates and owns a session-local mutex scoped to the current Win32 window station and desktop. A launcher on an isolated desktop cannot block the interactive desktop's launcher.
5. If an existing launcher window is on this desktop (including an older version), sends `kMessageShowLauncher` with a two-second timeout and exits after successful delivery. If another instance is still creating its window, waits up to two seconds for it; abandoned startup ownership can be recovered. Unreachable or unresponsive instances produce an actionable startup error instead of silent success.
6. Creates `LauncherApp`, initializes it, and enters its message loop.

`LauncherApp::Initialize` creates common controls, Direct2D and DirectWrite factories; registers and creates the popup window; adds the tray icon; starts the keyboard hook; loads the manual catalog; and queues discovery. Readiness does not wait for Shell enumeration. The window is created hidden, so normal startup shows only the tray icon. WIC factories are created in icon workers as needed.

When launched with benchmark events, the window also answers the read-only `kMessageBenchmarkState` diagnostic. Its flags distinguish protocol availability, unfinished discovery/icon work or visible painting, and current catalog/background errors. Ordinary launches return zero. The benchmark waits for completion before sampling memory; this diagnostic does not drain work or change scheduling.

## Custom-shell mode

`ApplicationQuickDial.exe --blade-contract-v1` exits immediately with code `73`.
The Blade installer uses this versioned probe to reject older launchers before
changing shell registration. It does not create a window, register a hotkey, or
forward to an existing instance. The probe certifies the launcher contract in
the selected binary; it does not replace an Explorer-free application launch
check on the target desktop.

`--shell-mode` starts the launcher hidden without a tray icon or tray-registration retries.
Win+Space opens it; F10 opens the existing menu while the launcher is visible. Start-with-Windows is omitted in this mode;
the host shell is responsible for lifecycle. Status messages appear in the launcher.
Launching the executable without `--shell-mode` again remains a fallback entry point.

A second shell-mode launch forwards `kMessageEnterShellMode` to the existing instance.
The message returns `1` after switching modes and hiding the launcher. An older instance
that does not acknowledge it produces an explicit restart/update error. No pointer data
crosses that message boundary. Explorer replacement and emergency recovery are owned by
the host shell, not Launcher.

## Tray and sign-in integration

The shell delivers tray events to the main window as `kMessageTray`. The same menu is available through F10 in the search field. It can open the launcher, open the catalog with Windows' registered JSON handler, reload it, toggle start-with-Windows, or destroy the window to exit.

The window registers `TaskbarCreated` and restores its icon after Explorer recreates the notification area. Failed registration retries every two seconds until successful; successful registration stops the retry timer. Repeated broadcasts can also update an icon that still exists.

Start-with-Windows is stored for the current user at:

```text
HKCU\Software\Microsoft\Windows\CurrentVersion\Run
  ApplicationQuickDial = "<absolute executable path>"
```

The menu is checked only when the stored value exactly matches the quoted path of the running executable.

The repository's `startup.ps1` manages the same value, without launching or stopping the application. From a normal interactive 64-bit PowerShell:

```powershell
.\startup.ps1                                      # Enable build\Release\ApplicationQuickDial.exe
.\startup.ps1 -ExecutablePath 'C:\Apps\ApplicationQuickDial.exe'
.\startup.ps1 -Status                              # Read-only registration and approval status
.\startup.ps1 -Disable                             # Remove the Run value
```

`-Disable` and `-Status` are mutually exclusive. The default executable is relative to the script directory; an explicit executable must exist. The script quotes its absolute path, adds no arguments, and rejects commands longer than Windows' 260-character Run limit. Normal application launch does not enable startup.

The script requires Explorer in the current session with the same SID as the calling account. It uses `StdRegProv` under that explicit `HKEY_USERS` SID (the intended user's HKCU) and independently checks `Win32_StartupCommand` before and after changes. A disagreement is an error, and failed changes attempt to restore and verify the previous Run value. Status does not require the default build to exist.

Windows can independently disable startup through `Explorer\StartupApproved\Run`. The script reports this state and refuses to enable a disabled or unrecognized approval record; enable it through Windows Settings after review. It never writes approval records. Registration verification does not prove a subsequent sign-in launch or override startup policies. The existing tray check reflects the Run command, not Windows' approval state.

Use a permanent executable location. A linked Git worktree (a `.git` file) refuses default enabling: pass the stable executable explicitly with `-ExecutablePath`. Status and disable remain available there. Verification must preserve live registrations and running applications; use isolated provider fixtures for mutation tests. No scheduled task or service is created.

## Ownership and cleanup

`LauncherApp` owns HWND-related state, GDI objects, COM factories, the render target, cached images, background queues, the pending launch entry, and `HookManager`. Internal `kMessageLaunchSelection` carries no payload; the main STA activates the captured entry after the input handler returns. Window destruction requests discovery cancellation and stops the queues: it revokes their notification HWND under the same lock used for posting and discards queued jobs and completions. Detached workers hold only shared state and value inputs; late completions are destroyed without accessing the window or its owner. Shutdown never waits for a stalled Shell operation, and no completed thread handles accumulate. A stalled icon operation occupies only its bounded worker slot until it returns or the process exits; discovery runs in the separately supervised helper described above.

`HookManager::Stop` posts `WM_QUIT` to its thread and joins it. Window destruction removes the tray icon, stops timers, and posts the main-thread quit message. Destructors repeat safe cleanup for partial initialization and failure paths.

## Diagnostic boundary

`--status --output <absolute JSON file>` reads the running desktop-local instance. `--lookup <text> --output <file>` includes matching configured/discovered entries and inclusion reasons. `--search <text> --output <file>` opens the existing instance and sets its query; it never activates a result. Reports identify executable/version/PID, account/catalog path, provider health/skipped counts, watcher/parse/discovery errors, refresh state, entry commands/icons and the actual typed visible results. Old instances reject unsupported diagnostics with an explicit update error.

Bounded `WM_COPYDATA` copies each request into a main-thread queue (maximum eight), then a posted message generates the response outside synchronous input. The caller pumps replies with a bounded wait. This avoids Windows Runtime/COM activation inside an incoming sent message. No shared application pointers or arbitrary receiver file writes cross the boundary. The caller writes its own report. Request text is capped at 4095 UTF-16 code units; responses at four MiB; NUL and framing validation precede dispatch. Calls have bounded timeouts and do not bypass the singleton. The protocol is read-only except the explicit `--search` UI request; it has no launch or power-action verb.

`--register-app <single-app JSON> --account <SID> --output <report>` and read-only `--catalog-status` are native helpers before singleton setup. The public [registration script](../registration.md) creates ordinary account-checked Windows processes and requires independent readback; inherited packaged context is rejected. They do not require replacing or stopping the running instance.

## Sign-in input trace

Use this trace when Win+Space fails before the first mouse click after Windows sign-in.
The trace requires a new shell-mode Launcher process and a valid shared Blade startup clock.
It does not change shortcut decisions or window activation.

To arm one capture, run this command in an ordinary PowerShell window:

```powershell
New-Item -ItemType File -Force -Path "$env:USERPROFILE\AppData\Local\WindowManager\Logs\Startup\trace-launcher-input.once"
```

The startup log directory must exist.
For a custom `RUST_SHELL_LOG_DIR`, use its `Startup` directory.
Launcher removes the marker after it creates the trace file and starts the worker.
An invalid clock or a file creation failure keeps the marker for a later start.
To cancel an armed capture, remove only this marker file.

At the next planned sign-in, use this sequence within the first two minutes:

1. Before a mouse click, press Win+Space.
2. If Launcher does not open, click an application window.
3. Press Win+Space again.

The capture writes `launcher-input-<pid>.jsonl` in that sign-in's `Logs/Startup/<run>` directory.
The log stops at 120 seconds from shared startup, 256 KiB, or Launcher exit.
The worker then stops and requests removal of the temporary mouse hook.
The `capture_closed` row gives the stop condition and the number of queue drops.
The next sign-in does not capture input unless you create another marker.

The trace records keyboard-hook installation, Win/Space events, tracked modifier bits, asynchronous key-state results, shortcut decisions, mask delivery, and toggle-message delivery.
It also records Launcher show/hide events, foreground activation results, and keyboard focus after activation.
Native focus samples include HWNDs, process/thread identities, window class, executable path, integrity level when available, and desktop names.
The mouse hook records physical button-down events without coordinates.
The trace does not record other keys, typed text, window titles, or application content.

Hook callbacks put fixed-size observations in a 256-entry queue without waiting for a lock.
The temporary worker owns file writing and JSON conversion.
The existing keyboard-hook thread owns the temporary mouse hook.
It checks native focus state at intervals of 250 ms and after queued observations.
It writes a focus sample only when the observed state changes.
Write failures stop recording without stopping Launcher.

Rows use Blade startup schema version 1 and its shared QPC clock.
The row timestamp identifies writer time.
For queued observations, `observedQpc` and `observedElapsedUs` identify callback or UI observation time.
Use observation time to compare a shortcut with a mouse click.
Queue delay, queue drops, and upstream hooks can limit the evidence.
A missing key event alone does not establish a Launcher decision.
A missing final row can indicate a write failure or an abrupt process exit.
