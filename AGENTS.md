# Application Quick Dial agent guide

## Scope

This repository builds a Windows 11 x64 launcher in native C++20. Keep changes small and consistent with the existing Win32 design. Do not introduce a UI framework, background service, installer, or cross-platform abstraction unless the task requires it.

## Ownership

The user owns product decisions: what the launcher does, its interactions, appearance, and feel. The agent owns implementation, reliability, performance, and test coverage, and should improve those autonomously within the agreed product behavior. Bring changes to behavior or feel, and engineering tradeoffs that affect them, to the user before implementing them. Routine internal fixes and verification do not require renewed approval.

## Before changing code

Read these before changing code:

- `README.md` for user-visible behavior and the catalog format.
- `docs/architecture.md` for runtime, threading, and data flow.
- `docs/development.md` for build commands and change locations.

## Build and test

Use a Visual Studio Developer PowerShell with the Desktop development with C++ workload and a Windows 11 SDK.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Run the application with:

```powershell
.\build\Release\ApplicationQuickDial.exe
```

Generated content belongs in `build/` or `out/`; do not add generated Visual Studio or CMake files to source control.

## Code map

- `src/main.cpp`: process setup, single-instance handling, and entry point.
- `src/LauncherApp.*`: main-thread Win32 window, tray menu, rendering, catalog reload, result selection, and launching.
- `src/Catalog.*`: catalog model, JSON parsing, default file creation, and configuration path.
- `src/Search.*`: case-insensitive match scoring and stable ranking.
- `src/HotkeyState.*`: testable `Win+Space` key-state machine.
- `src/HookManager.*`: low-level keyboard hook thread and UI notification.
- `src/StartupManager.*`: current-user `Run` registry entry.
- `src/AppMessages.h`: private messages used to cross thread/process boundaries.
- `tests/CoreTests.cpp`: tests for catalog parsing, search, and hotkey state.
- `app.manifest` and `resources.rc`: Windows compatibility, DPI, common controls, and embedded manifest.

## Required invariants

- Keep window and rendering operations on the main thread. The hook thread communicates with the window using `PostMessageW`.
- Keep the low-level keyboard callback short. Do not perform file I/O, rendering, or application launching in it.
- Ignore injected keyboard events. The hook injects an unassigned key to prevent the Windows key release from opening Start.
- A failed catalog reload must not replace the last valid in-memory catalog.
- Preserve catalog version `1` compatibility. Unknown JSON fields are accepted; known fields must keep their documented types.
- Preserve stable search ordering for equal scores. JSON order is the final tie-breaker.
- Treat layout constants in `LauncherApp.cpp` as device-independent pixels and scale them using the active monitor DPI.
- Release device-dependent Direct2D bitmaps with the render target. Icon lookup is intentionally cached per catalog while the launcher is visible.
- Quote each catalog argument independently before passing it to `ShellExecuteExW`.

## Change and verification rules

- Add or update core tests when changing catalog parsing, ranking, or hotkey state.
- Keep headers narrow and implementation details in `.cpp` files.
- Use wide strings at Win32 boundaries. Catalog files remain UTF-8 JSON.
- Check Win32 and HRESULT failures on new system calls and surface actionable errors through the existing status or tray-notification paths.
- Run the core test suite for every code change. For UI or shell integration changes, also run the relevant manual checks in `docs/development.md`.
- Update the documentation when changing the catalog schema, shortcut, startup behavior, message flow, or build requirements.
