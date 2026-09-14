# Source rules

Applies to `src/`; root ownership and completion rules also apply. Select the relevant reference via the [development map](../docs/development.md#where-to-make-a-change). Read only the contract sections touched by the change.

## Boundaries to preserve

- Main thread owns windows and rendering. Hook notifications use `PostMessageW`; never do file I/O, rendering, or launching in the low-level callback. Ignore injected events and retain the Windows-key masking behavior.
- Keep installed-app enumeration in its temporary helper. Bound concurrency, transport, and waits; tie helper lifetime to the launcher. Failed refreshes retain the last valid app list.
- Failed JSON reloads retain the last valid catalog. Preserve version `1`, documented types, and acceptance of unknown fields. Equal search scores retain catalog order as the final tie-breaker.
- Use device-independent layout constants scaled for the active monitor. Release Direct2D bitmaps with their render target; keep source-icon caching bounded and catalog/device invalidation correct.
- Quote each catalog argument independently before shell launch. Keep Windows desktop scope consistent between singleton ownership and window lookup.

## Implementation

- Keep headers narrow and implementation details in `.cpp` files. Use wide strings at Win32 boundaries and UTF-8 for JSON.
- Check new Win32/HRESULT failures and report actionable errors through existing status, tray, or startup error paths.
- Add/update core tests for parsing, ranking, and hotkey-state changes. Keep system effects out of `quickdial_core` merely for test convenience; test extracted decisions and use integration fixtures for effects.
- Run CTest for code changes plus affected [integration/manual checks](../docs/testing.md). Use the [performance contract](../docs/performance.md) for performance work; dated measurements are not proof about the current binary.
