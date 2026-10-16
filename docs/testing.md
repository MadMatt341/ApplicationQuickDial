# Verification reference

Run the CTest suite for every code change, then only the integration/manual sections affected by the change. Documentation-only edits need link and content checks, not a native rebuild. See [build commands](development.md#configure-build-and-test).

## Targets and integration commands

The targets are:

- `quickdial_core`: catalog parsing, installed-app discovery/merge, search, and hotkey state; linked by both the application and tests.
- `ApplicationQuickDial`: Windows GUI executable and system integration.
- `quickdial_tests`: small assertion-based test executable registered with CTest.
- `quickdial_background`: bounded background dispatch and icon decoding, shared by the app and background tests.
- `quickdial_background_tests`: CTest coverage for concurrency limits, shutdown with blocked work, late completions, handle retention, and image downscaling.
- `quickdial_discovery`: helper process supervision and the private app-list transport.
- `quickdial_discovery_tests`: CTest coverage for transport validation, child crashes/timeouts, cancellation, restricted handle inheritance, cleanup after abrupt parent exit, repeated requests, and comparison with real Shell discovery.
- `quickdial_launcher_tests`: explicit desktop integration checks using an isolated temporary catalog; briefly shows a test launcher and exercises tray restoration, catalog completion, benchmark state, and 140 distinct icon-cache entries.
- `quickdial_single_instance_tests`: explicit integration checks using private Win32 desktops and fixture processes. Covers independent desktop instances, repeated launches, concurrent startup, abandoned ownership, older-version forwarding, and bounded failures for missing or unresponsive windows. It does not switch the user's desktop or use their catalog.
- `quickdial_ugs_activation_tests`: CTest coverage for the UGS activation adapter with an application fixture and tray host on a private desktop.

All targets compile as C++20 with `/W4`, `/permissive-`, and `/EHsc`.

To inspect the Windows Apps-folder discovery output without starting the launcher:

```powershell
.\build\Release\quickdial_tests.exe --list-installed
```

With a normal interactive Windows desktop, run the launcher integration checks separately:

```powershell
.\build\Release\quickdial_launcher_tests.exe
.\build\Release\quickdial_single_instance_tests.exe
```

These checks use their own window and temporary JSON file. They do not modify the user's catalog, launch the user's applications, or restart Explorer. Shell-mode checks verify hidden tray-free startup, the actual F10 menu, visible tray-free messages, mode handoff, and absence of a tray icon. Launch checks run only a private test-executable mode, directly and via a uniquely named temporary Start-menu shortcut with an Apps-folder identity, and verify arguments through an output file. The shortcut and output are removed afterward. A nonexistent packaged-app identity also verifies direct activation failure, case-insensitive Apps-folder routing, and the absence of Shell fallback. Repeated synchronously delivered Enter messages verify that activation waits until the input handler returns, preserves the selected entry across catalog changes, and retains launch failure status without queuing duplicates. These checks do not establish compatibility with every packaged app or prove operation after Explorer is stopped; verify the affected packaged app from the real launcher in the custom-shell session. Tray recreation is simulated by deleting only the test window's tray icon and delivering `TaskbarCreated` to that window. Automatic discovery checks exercise actual Apps-folder Shell notification delivery, event coalescing, refresh while hidden, retry after failure, cancellation when disabled, and fresh cached reopening, failure/stale catch-up and finite readiness retries. Deterministic completion checks also verify that a newly installed app appears in the current query, disappears after removal, and leaves icon state intact when a scan finds no changes.

Run `quickdial_launcher_tests.exe --check-launching` for only the launch fixture and missing-target checks, using a tray-free launcher. This mode can run in a custom-shell session without Explorer: it verifies the temporary shortcut by its `.lnk` path and explicitly skips its new desktop Apps-folder registration, which requires Explorer's app index. It also omits the full suite's tray restoration checks.

CTest includes `quickdial_ugs_activation_tests` for the
[UGS compatibility path](design/interaction-rendering.md#ugs-hidden-startup-compatibility).
The test creates a private desktop and an owned application named `UnrealGameSync.exe`.
It checks native tray messages, visible activation, process images, changed shortcut arguments, incorrect replies, and call timeouts.
It also checks that an unrelated tray application keeps the normal launch path.
It does not switch the input desktop or use the installed UGS.
Installed acceptance must separately check the original UGS through Blade Launcher.

The test requires native Windows desktop permissions and fails if these permissions are missing.

Run `quickdial_launcher_tests.exe --private-desktop --check-installation` for the real Start-menu notification check. It creates a uniquely named temporary executable and user Start-menu shortcut while the launcher is hidden, verifies discovery without opening or manually reloading, then removes the shortcut and verifies the result disappears. It sends no synthetic change notification for the shortcut. Both fixture files are cleaned up; the target is never launched.

The cache stress check uses a generated 32-pixel BMP and distinct application targets. It verifies eviction at 128 sources, reuse ordering, reloading an evicted source, discarding results from before a refresh, and reusing pixels after releasing the render target. It complements the performance runner's repeated opens of the same six results; neither test substitutes for long-duration use across arbitrary third-party Shell providers.

Discovery tests use private fixture modes in the test executable to exercise failed and blocked child processes. They also invoke the real launcher's helper mode and compare its complete app list with direct Windows enumeration. A restricted test token can expose fewer Windows apps; run `quickdial_discovery_tests.exe` on the normal desktop as well when validating the user's full catalog. The private helper mode bypasses the launcher singleton, so these comparisons also work with a resident instance.

## Adding testable logic

Put logic that does not require an HWND, render target, shell launch, or registry write into `quickdial_core`. Add its source to that library in `CMakeLists.txt`, expose only the required API in `src/`, and add coverage to `tests/CoreTests.cpp`.

The current test executable uses a local `Check` helper rather than a test framework. Add a focused test function, call it from `main`, and make every failure description identify the expected behavior.

UI, shell, registry, and low-level hook behavior currently require manual verification. Avoid moving system side effects into `quickdial_core` only to make them reachable from tests; extract and test the decision logic instead.

## Manual verification

Run only the sections affected by a change. Before testing, exit any installed or previously built copy so the single-instance mutex does not redirect to the wrong executable.

### Basic lifecycle

1. Start the executable and confirm it appears in the notification area without a taskbar window.
2. Start it a second time and confirm the existing instance opens and the second process exits.
3. Exit from the tray menu and confirm the tray icon disappears.
4. Confirm tray restoration with `quickdial_launcher_tests.exe`; no Explorer restart is required.
5. Run `quickdial_single_instance_tests.exe` to verify that an instance on another Win32 desktop cannot block startup and that repeated launches on each desktop reach only their own instance.

### Shortcut and focus

1. Press left `Win+Space` and right `Win+Space`; each should toggle the launcher once.
2. Hold Space to check that key repeat does not toggle repeatedly.
3. Release Windows after the chord and confirm Start does not open.
   Also release Windows first while continuing to hold Space: repeats must remain suppressed until Space is released.
4. Check that unrelated Windows shortcuts still work.
   After releasing Windows, type several words with spaces and confirm the launcher stays hidden.
   Also use Win+L, release Windows while locked, unlock, and repeat the typing check to cover a missed release across desktops.
5. Press Escape and click another window; either action should hide the launcher.
6. Confirm Enter launches a selected result and Escape hides the launcher without a system beep.
   Also check Enter with no matching results, normal typing, and Backspace.

### Catalog and search

1. Back up `%LOCALAPPDATA%\ApplicationQuickDial\apps.json` before destructive test edits.
2. Confirm opening the launcher reloads a valid edit.
3. Introduce invalid JSON and confirm an error is shown while results from the last valid load remain available.
4. Restore valid JSON and confirm the error clears on the next open or reload.
5. Check exact, prefix, word-boundary, substring, and alias searches, including mixed case.
6. With `discoverInstalled` enabled, confirm an app not present in JSON is searchable after startup or tray reload.
7. Add its name or AppUserModelID to `hiddenApplications`, reload, and confirm it is excluded.
8. On a localized Windows installation, confirm an English component of an AppUserModelID or executable name (for example, `calc`) finds the localized application.
9. Type and select a result during discovery/reload; completion must preserve both. A failed discovery must retain the last successful installed-app list. These completion paths are covered by `quickdial_launcher_tests.exe` with deterministic results.
10. Run `quickdial_launcher_tests.exe --check-launching` for only the launch fixture and missing-target checks, using a tray-free launcher. This mode can run in a custom-shell session without Explorer: it verifies the temporary shortcut by its `.lnk` path and explicitly skips its new desktop Apps-folder registration, which requires Explorer's app index. It also omits the full suite's tray restoration checks.

Run `quickdial_launcher_tests.exe --private-desktop --check-installation` to verify actual shortcut registration/removal through Windows events while Launcher is running, including while hidden. The ordinary checks also verify that `discoverInstalled: false` stops subscriptions and that repeated opening and idle timer messages do not start scans.

### Launching and icons

1. Test an executable with arguments that include spaces and quotes.
2. Test both an ordinary desktop shortcut and a packaged-app `shell:AppsFolder` target. For a custom-shell launch fix, verify the affected packaged app from Launcher while Explorer is absent, and confirm the app has a usable window.
3. Test a document or URL handled through a file association.
4. Test an explicit icon path, shell-derived icon, and invalid icon fallback.
5. Confirm a failed target leaves the launcher open with an error.
6. Use a large custom image and confirm the search surface remains responsive while its scaled icon loads. Reopening should reuse cached pixels; explicit reload refreshes them.
7. Exit during icon loading and discovery. The process must exit promptly even if a Shell operation has stalled; `quickdial_background_tests` covers this using blocked work.
8. Check a multi-resolution ICO at 100%, 125%, 150%, and 200% scale: it should use a suitable frame and have sharp edges. Check wide and tall custom images for centered, undistorted proportions and transparent edges in both themes. The background suite also checks ICO frame selection and transparent-color filtering.

### Main menu and system actions

Run `quickdial_launcher_tests.exe --check-main-menu` for only the tray-free menu checks, including in a custom-shell session without Explorer. The core suite verifies built-in section entries and root and scoped action search. The launcher integration suite verifies hidden shell-mode startup, System navigation, Escape/back-arrow behavior, catalog independence, captured/coalesced requests without confirmation, canceled queued requests, fake request failure, and accepted fake dispatch. For visual captures, run on the active ordinary desktop; inactive-desktop screen copies can contain unrelated/black pixels and are not visual evidence. Pass an optional output directory after `--check-main-menu` to save fixture-window BMP captures in light/dark themes at 96, 120, 144 and 192 DPI; the fixture uses the production common-controls manifest. It substitutes the power-request boundary and does not shut down, restart, or sign out of Windows.

1. Open with Win+Space: System appears below the divider. Down and Enter open it; Up/Down reach Shut down, Restart and Sign out.
2. Escape or click the back arrow: return to main without hiding. Escape again hides. Reopening starts on main.
3. Type an application name on main; app ranking, launch, and scrolling remain available. Clearing restores System. On main or in System, search `shutdown`, `power off`, `restart`, `reboot`, or `logoff`; an unmatched query has no action.
4. Verify the action labels and selection behavior without activating a real power action. Activating Shut down, Restart or Sign out requests that operation immediately.
5. Check the header, icons, selection, and click targets in both themes and affected DPI settings.

A real Shut down/Restart/Sign out test ends the desktop session and requires explicit user authorization. Build, core tests, and fake dispatch are not evidence of actual Windows shutdown or restart, saved-work negotiation, or subsequent custom-shell sign-in.

### Window presentation

1. Check light and dark application themes.
2. Check a secondary monitor and, if available, monitors with different scale factors.
3. Confirm the launcher stays within the monitor work area, text is sharp, rows are clickable, and the height changes with result count and errors.

### Start with Windows

Run `powershell -NoProfile -ExecutionPolicy Bypass -File tests\StartupScriptTests.ps1` for isolated startup-script provider fixtures. These exercise registration, removal, status, identity rejection, Windows approval, command validation, and independent verification/rollback without touching the registry. Run `.\startup.ps1 -Status` on the normal desktop to check the real provider and inventory without altering registration. Preserve any existing live registration and resident app during script-only verification; tray/lifecycle checks below apply when changing native integration.

1. Enable the tray option and confirm the current executable appears as the quoted `ApplicationQuickDial` value under the current-user Run key.
2. Disable the option and confirm the value is removed.

Verify both changes independently through Windows' startup inventory, not only the tool's direct registry view. Follow [startup diagnostics](troubleshooting.md#startup-and-desktop) if the views disagree. A real sign-in check is distinct from registration verification and requires the user's authorization to sign out or restart.

## Reliability and account verification

The reliability suite verifies indexed-entry replacement when a shortcut supplies a working folder. It also verifies different commands, names, and working folders.

CTest includes `quickdial_reliability_tests`: fixed command/app collision expectations, short/typo power-query rejection, deterministic bounded refresh clocks, per-source stale-cache recovery, conservative maintenance classification/overrides, launch-command identity, real recursive shortcut/URL fixtures with malformed items, and atomic catalog updates preserving unknown fields, app order and settings. Temporary catalogs/shortcuts stay in the test’s owned temp directory; nothing launches.

`quickdial_launcher_tests.exe --private-desktop --check-installation` is the focused tray-free discovery suite. It verifies notification errors preserve the query/cache, invalid-file completion, healthy cache reuse and failed-open catch-up, then creates/removes its own uniquely named real Start-menu shortcut while hidden. It does not send a synthetic change event or launch the fixture target. Explicit missing prerequisites fail. `--private-desktop --check-main-menu` verifies typed search, fake power dispatch and the real diagnostic request/response boundary using a separate native client process, without switching the input desktop. The client targets the owned fixture HWND explicitly. `--private-desktop --check-launching` verifies only owned launch fixtures; the fresh Apps-folder registration requires Explorer and is excluded in that selected mode. The full default suite requires Explorer/tray and is unsuitable for a private or Explorer-free desktop.

Run `powershell -NoProfile -ExecutionPolicy Bypass -File tests/LauncherStartupMigrationTests.ps1 <built-executable>` for startup migration fixtures. They replace native registry/inventory providers with owned in-memory state and verify preserved arguments and disabled approval bytes, rollback on independent verification failure, intervening edits, absent preferences, dispatcher hash changes, and rejection of direct immutable-generation bindings. They never edit the registry or stop a process. Installed stable-dispatch selection across activation/rollback belongs to Blade's shell tests; these fixtures do not establish sign-in acceptance.

For installed acceptance, bind to the configured Blade executable with `--status`, verify its build/path and sources, search a preserved manual entry and power aliases through `--search`, and inspect the visible UI. Capture/query checks never activate real system actions. Verify the preserved manual-entry JSON using `Register-LauncherApp.ps1 -VerifyOnly` and compare independently read native catalog hashes. A source build or private fixture pass does not establish the installed generation; sign-in verification belongs to Blade’s coordinated update acceptance.

The discovery suite also runs a genuinely blocked Apps-folder fixture child alongside successful shortcut helpers. It requires the timeout to kill that child and publish both healthy source results. Separate real-provider helpers are compared with the current Windows inventory.

## Shared appearance fixture

Set `BLADE_APPEARANCE_FILE` to `<capture-directory>/appearance.json` containing
the Blade selection example, then run `quickdial_launcher_tests --private-desktop
--check-main-menu <capture-directory>`. The prerequisite is required; this check
must not write the real user appearance file. It owns a private desktop without
switching the input desktop, replaces its isolated settings atomically, observes
real query/selection/HWND/font/metric retention, verifies malformed rejection and
recovery, and captures the same production Direct2D drawing path into a software
DC at four scales. System actions remain faked. Windows title/compositor behavior,
global hotkeys, real high contrast and installed acceptance need a separately
authorized interactive check.

The main-menu fixture also compares placeholder and typed glyph pixels from the
real native edit control. It checks equal backgrounds and vertical text bounds
at 96, 120, 144 and 192 DPI, with 16, 22 and 48 pixel fonts. It checks that the
search control stays in the search row. The capture output includes typed
application results and the keyboard hints.
