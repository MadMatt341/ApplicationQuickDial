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

All targets compile as C++20 with `/W4`, `/permissive-`, and `/EHsc`.

To inspect the Windows Apps-folder discovery output without starting the launcher:

```powershell
.\build\Release\quickdial_tests.exe --list-installed
```

## Where to make a change

| Change | Primary files | Also check |
|---|---|---|
| Catalog field or validation | `src/Catalog.h`, `src/Catalog.cpp` | `tests/CoreTests.cpp`, `README.md`, `docs/architecture.md` |
| Installed-app discovery or merge | `src/InstalledApps.*`, `src/LauncherApp.cpp` | catalog tests, tray reload, discovered app launches |
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

### Shortcut and focus

1. Press left `Win+Space` and right `Win+Space`; each should toggle the launcher once.
2. Hold Space to check that key repeat does not toggle repeatedly.
3. Release Windows after the chord and confirm Start does not open.
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

### Launching and icons

1. Test an executable with arguments that include spaces and quotes.
2. Test a `shell:AppsFolder` target.
3. Test a document or URL handled through a file association.
4. Test an explicit icon path, shell-derived icon, and invalid icon fallback.
5. Confirm a failed target leaves the launcher open with an error.

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
