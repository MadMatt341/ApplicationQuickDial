# Development map

Read the build section when compiling; otherwise use the table to load only the relevant source and reference. [System overview](architecture.md) · [Verification](testing.md) · [Troubleshooting](troubleshooting.md) · [Releases](releases.md)

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

The default generator must match an existing build directory. For Visual Studio 2026, use its bundled CMake and add `-G "Visual Studio 18 2026"` when configuring. Stop the running copy before replacing its executable. Generated content belongs in `build/` or `out/`.

## Where to make a change

Run `.\build\Release\ApplicationQuickDial.exe` on the user's normal desktop. It starts in the tray. For an invisible window or missing startup entry, use [startup diagnostics](troubleshooting.md#startup-and-desktop).

| Task | Source | Read next | Verify |
|---|---|---|---|
| Catalog fields or parsing | `src/Catalog.*` | [Schema](catalog.md), [lifecycle](design/catalog-discovery.md#catalog-lifecycle) | Core tests; catalog reload |
| App discovery, merge, notifications | `src/InstalledApps.*`, `src/InstalledAppsWatcher.*`, `src/LauncherApp.cpp` | [Catalog/discovery](design/catalog-discovery.md) | Core, discovery and launcher tests; real installation check for watcher changes |
| Helper lifetime or transport | `src/DiscoveryProcess.*`, `src/DiscoveryProtocol.*`, `src/main.cpp` | [Process boundary](design/catalog-discovery.md#discovery-process-boundary) | Discovery tests; relevant performance contract |
| Background queues or shutdown | `src/BackgroundTasks.*` | [Runtime ownership](design/runtime.md) | Background and launcher tests |
| Search or selection | `src/Search.*`, `src/LauncherApp.cpp` | [Ranking and selection](design/interaction-rendering.md#search-and-selection) | Core tests; catalog/search checks |
| Shortcut or edit keys | `src/HotkeyState.*`, `src/HookManager.*`, `src/LauncherApp.cpp` | [Shortcut flow](design/interaction-rendering.md#shortcut-flow) | Core tests; shortcut/focus checks |
| Layout, theme, DPI, icons | `src/LauncherApp.*`, `src/LauncherVisualStyle.*`, `src/IconLoader.*` | [Rendering](design/interaction-rendering.md#window-and-rendering) | Background/launcher tests; affected visual checks |
| Application launching | `src/LauncherApp.cpp` | [Launching](design/interaction-rendering.md#launching) | Launching/argument checks |
| Startup, singleton, tray | `src/main.cpp`, `src/SingleInstance.*`, `src/StartupManager.*`, `src/LauncherApp.cpp` | [Runtime](design/runtime.md), [diagnostics](troubleshooting.md#startup-and-desktop) | Singleton/launcher tests; lifecycle and sign-in checks |
| Message boundaries | `src/AppMessages.h` and sender/receiver | Relevant [design section](architecture.md) | Both sides of the changed message path |
| Build, manifest, packaging | `CMakeLists.txt`, `app.manifest`, `resources.rc`, `package.ps1` | [Release workflow](releases.md) | Build, CTest, affected desktop checks, package inspection |
| Performance | `benchmarks/PerformanceBenchmarks.cpp` and affected subsystem | [Contract and measurements](performance.md) | Benchmark on normal desktop |

Detailed test commands and manual scenarios live in [testing.md](testing.md); do not run unrelated manual sections.
