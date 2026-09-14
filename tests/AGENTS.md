# Test rules

Applies to `tests/`. Use [testing.md](../docs/testing.md#targets-and-integration-commands) for target coverage and commands; read only affected manual sections. Build setup lives in [development.md](../docs/development.md#prerequisites).

- Test observable contracts and failure modes, not copies of implementation logic. Extend the existing assertion-based style and make each failure identify the violated behavior.
- Core tests cover parsing, ranking, and hotkey decisions. Use integration fixtures for Windows/process effects; do not move those effects into the core library just to expose them to tests.
- Isolate catalogs, windows, desktop names, handles, and child processes. Bound waits and clean up after failures. Never overwrite the user's catalog or use their real applications as launch fixtures.
- Launcher and singleton integration executables run explicitly on the normal desktop, outside CTest. Private desktops must not switch the user's active desktop. The installation check owns and removes only its uniquely named fixture shortcut and executable.
- A running process or exit code alone does not prove usable UI or successful sign-in registration. Verify the effect through the relevant Windows interface; see [startup diagnostics](../docs/troubleshooting.md#startup-and-desktop).
