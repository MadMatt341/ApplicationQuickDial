# Runtime and Windows integration

Read for process lifetime, threading, startup, singleton, tray, or sign-in work. [Design map](../architecture.md) · [Verification](../testing.md#basic-lifecycle) · [Startup troubleshooting](../troubleshooting.md#startup-and-desktop)

## Process model

Application Quick Dial has one resident GUI process with two resident threads, bounded background work, and a temporary discovery helper:

1. The main STA thread owns COM initialization, the hidden launcher window, tray icon, catalog state, Direct2D resources, and the Win32 message loop.
2. A hook thread installs `WH_KEYBOARD_LL` and runs the message loop required by the keyboard hook.
3. At most one discovery worker supervises a helper process without initializing COM in the resident process. Up to two icon workers perform Shell and WIC operations in their own COM apartments. Workers exit when their queues empty.
4. For each scan, the launcher starts another copy of its own executable in private `--discover-apps` mode. That process initializes a COM STA, enumerates Windows' Apps folder, returns plain values, and exits. It creates no window, tray icon, hook, or single-instance mutex. Shipping the app still requires only one executable.

`BackgroundTasks` limits both concurrency and outstanding work (one discovery request and 32 icon jobs, including completed results awaiting dispatch). Workers own shared queue state and value inputs. They post `kMessageBackgroundComplete` with no payload; the main thread drains owned completion objects. A 100 ms timer drains results while jobs are outstanding as a fallback if posting fails, and stops when the queues empty. There is no service, database, or network dependency. The JSON catalog is the only persisted application data; installed-app discovery is rebuilt from the Windows Shell namespace.

`InstalledAppsWatcher` owns two Shell notification registrations and two kernel directory-change handles. The main loop blocks in `MsgWaitForMultipleObjectsEx` for window messages or directory changes; watching adds no resident worker or polling loop. Shell notifications arrive as `kMessageInstalledApplicationsChanged`, and their shared-memory payload is locked and released on the main thread. No enumeration runs in these handlers.

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

## Tray and sign-in integration

The shell delivers tray events to the main window as `kMessageTray`. The menu can open the launcher, open the catalog with Windows' registered JSON handler, reload it, toggle start-with-Windows, or destroy the window to exit.

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

`LauncherApp` owns HWND-related state, GDI objects, COM factories, the render target, cached images, background queues, and `HookManager`. Window destruction requests discovery cancellation and stops the queues: it revokes their notification HWND under the same lock used for posting and discards queued jobs and completions. Detached workers hold only shared state and value inputs; late completions are destroyed without accessing the window or its owner. Shutdown never waits for a stalled Shell operation, and no completed thread handles accumulate. A stalled icon operation occupies only its bounded worker slot until it returns or the process exits; discovery runs in the separately supervised helper described above.

`HookManager::Stop` posts `WM_QUIT` to its thread and joins it. Window destruction removes the tray icon, stops timers, and posts the main-thread quit message. Destructors repeat safe cleanup for partial initialization and failure paths.
