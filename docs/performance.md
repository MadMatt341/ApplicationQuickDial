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
- **CPU** uses process kernel plus user time over a three-second wall-clock interval with no input or animation.
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

## Current baseline

Measured on the development Windows 11 x64 workstation on 2026-09-07 with installed-app discovery enabled (162 apps), after moving discovery to the temporary helper. The overall benchmark **passes every unchanged budget**. Discovery settled 690.84 ms after readiness, including the 250 ms quiet interval. All ten children exited normally; shutdown p95 was 13.20 ms. Full-desktop discovery tests separately confirmed that the helper returns the same 162 names, targets, identifiers, and ordering as direct Shell enumeration.

With the same completion-aware measurement method, in-process discovery previously retained 35.99 MB total working set, 3.95 MB private working set, and 5.17 MB private committed bytes before first show. The helper reduces those to 15.45 MB, 1.86 MB, and 2.50 MB respectively: about 57% less total resident memory and 52% less private commitment at hidden startup. Hidden-after-use total working set fell from 61.68 MB to 52.84 MB. This change releases discovery resources through process exit; it does not trim the working set or relax the budgets.

The earlier 2026-08-31 baseline predates discovery, and the old 250-ms startup sample could precede scan completion; neither is directly comparable to settled measurements. Total working set also varies with shared Windows pages and system pressure. Treat these results as this workstation's baseline rather than a guarantee for every installed Shell provider or machine.

| Metric | Baseline | Budget |
| --- | ---: | ---: |
| Fresh-process startup median | 23.01 ms | Informational |
| Fresh-process startup p95 | 27.19 ms | 100 ms |
| First show to first paint | 35.08 ms | 100 ms |
| Warm show median | 3.80 ms | Informational |
| Warm show p95 | 4.16 ms | 50 ms |
| Graceful shutdown p95 | 13.20 ms | Informational; normal exit required within 3 seconds |
| Hidden initial working set | 15.45 MB | 20 MB |
| Hidden initial private working set | 1.86 MB | 5 MB |
| Hidden initial private bytes | 2.50 MB | 5 MB |
| Visible working set | 53.75 MB | 64 MB |
| Visible private working set | 10.92 MB | 20 MB |
| Visible private bytes | 15.35 MB | 20 MB |
| Hidden-after-use working set | 52.84 MB | 64 MB |
| Hidden-after-use private working set | 10.01 MB | 20 MB |
| Hidden-after-use private bytes | 14.42 MB | 16 MB |
| Hidden idle CPU over 3 seconds | 0.00 ms | 10 ms |
| Visible idle CPU over 3 seconds | 0.00 ms | 10 ms |
| Parse 100-app catalog | 0.58 ms | 2 ms |
| Search 100 apps | 0.02 ms | 0.25 ms |
| Release executable size | 217.00 KB | 256 KB |

`quickdial_background_tests` separately checks queue limits, 200 sequential jobs without handle growth, prompt destruction with a blocked worker, and bounded custom-icon output. `quickdial_launcher_tests` checks tray restoration, asynchronous catalog/error/selection behavior, benchmark completion flags, and eviction/reuse across 140 distinct icon-cache entries with an isolated catalog and generated image. Physical shortcut input, mixed-monitor presentation, and long-duration interactive use still need manual validation.

`quickdial_discovery_tests` adds malformed/truncated/oversized response coverage, actual child-process failures and timeouts, cancellation, restricted inheritance, cleanup after abrupt parent exit, and ten repeated requests without handle growth. It compares real helper output with direct enumeration. The restricted-token CTest run passed against its four visible apps; an additional normal-desktop run passed against all 162 apps.
