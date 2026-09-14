# Performance contract

Application Quick Dial is a resident utility, so its performance contract covers both responsiveness and the cost of leaving it running all day. The benchmark is native, launches the real Release executable, and exits with a failure code when a budget is exceeded.

## Budgets

| Metric | Budget |
| --- | ---: |
| Fresh-process tray readiness, p95 of 10 launches | ≤ 100 ms |
| First show request to first painted frame | ≤ 100 ms |
| Warm show request to first painted frame, p95 of 20 opens | ≤ 50 ms |
| Hidden initial total working set | ≤ 20 MB |
| Hidden initial private working set | ≤ 5 MB |
| Hidden initial private committed bytes | ≤ 5 MB |
| Visible total working set | ≤ 64 MB |
| Visible private working set | ≤ 20 MB |
| Visible private committed bytes | ≤ 20 MB |
| Hidden-after-use total working set | ≤ 64 MB |
| Hidden-after-use private working set | ≤ 20 MB |
| Hidden-after-use private committed bytes | ≤ 16 MB |
| Hidden idle CPU over 3 seconds | ≤ 10 ms |
| Visible idle CPU over 3 seconds | ≤ 10 ms |
| Parse a synthetic 100-app catalog | ≤ 2 ms average |
| Search a synthetic 100-app catalog | ≤ 0.25 ms average |
| Release executable size | ≤ 256 KB |

Private working set counts resident pages marked private by Windows. Total working set is also capped and reported, but it includes shareable Windows, DirectWrite, WIC, and runtime pages. Private committed bytes describe process-owned virtual-memory commitment, whether or not each page is currently resident. Windows working-set measurements are point-in-time snapshots and can change under system memory pressure.

## Measurement method

- **Fresh-process readiness** starts at `CreateProcessW` and ends after the window, tray icon, manual catalog, and keyboard hook initialize and signal the benchmark event. Installed-app discovery runs asynchronously and is not included in readiness. Each trial uses a new process, but this is not a cold-boot or cold-disk-cache test.
- **First/warm show** starts when the benchmark posts the real show command and ends after the first successful paint. Shell and custom icons load asynchronously and therefore do not delay the usable search surface. The runner waits for icon completion before hiding, so warm trials reuse completed sources. These trials open the same first six results, not the entire installed catalog.
- **Hidden initial memory** is sampled before the launcher has appeared, after the discovery helper exits, its shared mapping is released, and its main-thread completion finishes, followed by 250 ms continuously without pending work. Discovery settling time is reported separately from tray readiness, including that quiet interval.
- **Visible memory/CPU** is sampled after queued icon work, main-thread completions, and pending window painting finish, followed by the same 250 ms quiet interval.
- **Hidden-after-use memory/CPU** is sampled after the visible launcher is hidden and display resources are released.
- **CPU** uses process kernel plus user time over a three-second wall-clock interval with no input. The visible search field retains its native blinking caret.
- **Parsing/search** use a 100-entry synthetic catalog, intentionally larger than the expected hand-maintained list.
- **Shutdown** posts the real close command and requires a normal process exit within three seconds. Forced termination is cleanup after a failed benchmark, never a successful result. Shutdown p95 is informational; nine of the ten children close directly after readiness, while discovery may still be running.

The runner verifies that the launcher window belongs to its child process. It queries completion flags with a bounded message timeout and treats catalog/discovery errors, unexpected process exits, visibility changes, or ten seconds without settling as an invalid run. Only launches with benchmark events enable this diagnostic. The runner checks that each three-second CPU sample ends in the intended visible/hidden and idle state.

These are settled resident-process measurements, not peak memory/CPU totals during discovery. Scanning briefly uses a second copy of the executable and a mapping capped at 4 MiB. The helper and mapping are gone before these memory samples; their Windows discovery components are therefore released instead of remaining mapped in the resident process. Icon extraction still runs in resident workers, so memory after opening the launcher is higher than startup memory.

The runner uses `GetProcessMemoryInfo` for total working set and private committed bytes, plus `QueryWorkingSet` to count non-shared resident pages. Working-set buffer sizing retries if the process grows during sampling; a failed measurement is reported as -1 and fails its budget. Microsoft notes that working-set APIs report pages physically present at the exact moment of the query: [Working Set Information](https://learn.microsoft.com/en-us/windows/win32/psapi/working-set-information).

## Run it

Exit the resident Quick Dial process first, then run from the repository root:

```powershell
cmake --build build --config Release --parallel
.\build\Release\quickdial_benchmarks.exe
```

The benchmark briefly shows and hides the real launcher while measuring presentation. Leave it in the foreground during the visible samples. Exit code `0` means every budget passed, `1` means at least one metric exceeded its budget, and `2` means setup, measurement, or graceful shutdown failed.

## Historical evidence

[Prior measurements and investigations](measurements/performance-history.md) are retained separately. They are dated evidence, not verification of the current executable.
