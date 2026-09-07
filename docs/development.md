# Development guide

## Prerequisites

- Windows 11 x64
- Visual Studio with Desktop development with C++
- Windows 11 SDK
- CMake 3.24 or newer

Use a Visual Studio Developer PowerShell so the MSVC environment is available.

## Configure, build, and test

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

For a clean configuration without deleting the current build, use a separate directory:

```powershell
cmake -S . -B out\dev -A x64
cmake --build out\dev --config Debug
ctest --test-dir out\dev -C Debug --output-on-failure
```

The targets are:

- `quickdial_core`: catalog parsing, installed-app discovery/merge, search, and hotkey state; linked by both the application and tests.
- `ApplicationQuickDial`: Windows GUI executable and system integration.
- `quickdial_tests`: small assertion-based test executable registered with CTest.
- `quickdial_background`: bounded background dispatch and icon decoding, shared by the app and background tests.
- `quickdial_background_tests`: CTest coverage for concurrency limits, shutdown with blocked work, late completions, handle retention, and image downscaling.
- `quickdial_discovery`: helper process supervision and the private app-list transport.
- `quickdial_discovery_tests`: CTest coverage for transport validation, child crashes/timeouts, cancellation, restricted handle inheritance, cleanup after abrupt parent exit, repeated requests, and comparison with real Shell discovery.
- `quickdial_launcher_tests`: explicit desktop integration checks using an isolated temporary catalog; briefly shows a test launcher and exercises tray restoration, catalog completion, benchmark state, and 140 distinct icon-cache entries.

All targets compile as C++20 with `/W4`, `/permissive-`, and `/EHsc`.

To inspect the Windows Apps-folder discovery output without starting the launcher:

```powershell
.\build\Release\quickdial_tests.exe --list-installed
```

With a normal interactive Windows desktop, run the launcher integration checks separately:

```powershell
.\build\Release\quickdial_launcher_tests.exe
```

These checks use their own window and temporary JSON file. They do not modify the user's catalog, launch target applications, or restart Explorer. Tray recreation is simulated by deleting only the test window's tray icon and delivering `TaskbarCreated` to that window.

The cache stress check uses a generated 32-pixel BMP and distinct application targets. It verifies eviction at 128 sources, reuse ordering, reloading an evicted source, discarding results from before a refresh, and reusing pixels after releasing the render target. It complements the performance runner's repeated opens of the same six results; neither test substitutes for long-duration use across arbitrary third-party Shell providers.

Discovery tests use private fixture modes in the test executable to exercise failed and blocked child processes. They also invoke the real launcher's helper mode and compare its complete app list with direct Windows enumeration. A restricted test token can expose fewer Windows apps; run `quickdial_discovery_tests.exe` on the normal desktop as well when validating the user's full catalog. The private helper mode bypasses the launcher singleton, so these comparisons also work with a resident instance.

## Where to make a change

| Change | Primary files | Also check |
|---|---|---|
| Catalog field or validation | `src/Catalog.h`, `src/Catalog.cpp` | `tests/CoreTests.cpp`, `README.md`, `docs/architecture.md` |
| Installed-app discovery or merge | `src/InstalledApps.*`, `src/LauncherApp.cpp` | catalog tests, tray reload, discovered app launches |
| Discovery process lifetime or transport | `src/DiscoveryProcess.*`, `src/DiscoveryProtocol.*`, `src/main.cpp` | discovery tests, error retention, real-process memory benchmark |
| Background lifetime or limits | `src/BackgroundTasks.*` | `tests/BackgroundTests.cpp`, shutdown during stalled work |
| Icon decoding and source cache | `src/IconLoader.*`, `src/LauncherApp.cpp` | background tests, custom images, repeated reload/open |
| Search scoring or result limit | `src/Search.cpp`, `src/LauncherApp.cpp` | `tests/CoreTests.cpp` |
| Global shortcut behavior | `src/HotkeyState.*`, `src/HookManager.*` | `src/AppMessages.h`, hotkey tests, manual shortcut checks |
| Window size, rows, colors, or drawing | `src/LauncherApp.cpp` | light/dark theme and mixed-DPI checks |
| Keyboard or mouse interaction | `LauncherApp::EditProcedure`, `LauncherApp::HandleMessage` | focus-loss behavior and selection bounds |
| Target launching or argument handling | `LauncherApp::LaunchApplication` | executable, document/URL, `shell:`, arguments, and working-directory checks |
| Tray commands | tray helpers in `src/LauncherApp.cpp` | command IDs and notification behavior |
| Start with Windows | `src/StartupManager.*` | current-user Run registry behavior |
| Single-instance behavior | `src/main.cpp`, `src/AppMessages.h` | second launch brings up the existing instance |
| Manifest or DPI declarations | `app.manifest`, `resources.rc`, `CMakeLists.txt` | runtime DPI behavior and executable resource embedding |

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

### Shortcut and focus

1. Press left `Win+Space` and right `Win+Space`; each should toggle the launcher once.
2. Hold Space to check that key repeat does not toggle repeatedly.
3. Release Windows after the chord and confirm Start does not open.
   Also release Windows first while continuing to hold Space: repeats must remain suppressed until Space is released.
4. Check that unrelated Windows shortcuts still work.
5. Press Escape and click another window; either action should hide the launcher.

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

### Launching and icons

1. Test an executable with arguments that include spaces and quotes.
2. Test a `shell:AppsFolder` target.
3. Test a document or URL handled through a file association.
4. Test an explicit icon path, shell-derived icon, and invalid icon fallback.
5. Confirm a failed target leaves the launcher open with an error.
6. Use a large custom image and confirm the search surface remains responsive while its scaled icon loads. Reopening should reuse cached pixels; explicit reload refreshes them.
7. Exit during icon loading and discovery. The process must exit promptly even if a Shell operation has stalled; `quickdial_background_tests` covers this using blocked work.
8. Check a multi-resolution ICO at 100%, 125%, 150%, and 200% scale: it should use a suitable frame and have sharp edges. Check wide and tall custom images for centered, undistorted proportions and transparent edges in both themes. The background suite also checks ICO frame selection and transparent-color filtering.

### Window presentation

1. Check light and dark application themes.
2. Check a secondary monitor and, if available, monitors with different scale factors.
3. Confirm the launcher stays within the monitor work area, text is sharp, rows are clickable, and the height changes with result count and errors.

### Start with Windows

1. Enable the tray option and confirm the current executable appears as the quoted `ApplicationQuickDial` value under the current-user Run key.
2. Disable the option and confirm the value is removed.

## Debugging notes

- If `Win+Space` does nothing, confirm the tray icon can still open the app. Hook installation failure is reported as a tray notification. Elevated target applications and the secure desktop are expected limitations.
- If catalog edits appear ignored, close and reopen the launcher or use Reload app list. A parse error intentionally retains the previous catalog.
- If a custom icon is missing, verify the expanded path first. Failure falls through silently to shell and generic icons.
- If a newly built executable only opens an older copy, exit the running instance before launching the new build.
- If startup initialization fails before the tray icon exists, the process displays a message box and exits with code `1`.

## Documentation maintenance

Keep `README.md` user-facing. Record system behavior and invariants in `docs/architecture.md`; record build, change, and verification guidance here. Keep `AGENTS.md` short enough to scan before implementation.
