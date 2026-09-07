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
- **Hidden initial memory** is sampled before the launcher has appeared, after background discovery and its main-thread completion finish, followed by 250 ms continuously without pending work. Discovery settling time is reported separately from tray readiness, including that quiet interval.
- **Visible memory/CPU** is sampled after queued icon work, main-thread completions, and pending window painting finish, followed by the same 250 ms quiet interval.
- **Hidden-after-use memory/CPU** is sampled after the visible launcher is hidden and display resources are released.
- **CPU** uses process kernel plus user time over a three-second wall-clock interval with no input or animation.
- **Parsing/search** use a 100-entry synthetic catalog, intentionally larger than the expected hand-maintained list.
- **Shutdown** posts the real close command and requires a normal process exit within three seconds. Forced termination is cleanup after a failed benchmark, never a successful result. Shutdown p95 is informational; nine of the ten children close directly after readiness, while discovery may still be running.

The runner verifies that the launcher window belongs to its child process. It queries completion flags with a bounded message timeout and treats catalog/discovery errors, unexpected process exits, visibility changes, or ten seconds without settling as an invalid run. Only launches with benchmark events enable this diagnostic. The runner checks that each three-second CPU sample ends in the intended visible/hidden and idle state.

The runner uses `GetProcessMemoryInfo` for total working set and private committed bytes, plus `QueryWorkingSet` to count non-shared resident pages. Working-set buffer sizing retries if the process grows during sampling; a failed measurement is reported as -1 and fails its budget. Microsoft notes that working-set APIs report pages physically present at the exact moment of the query: [Working Set Information](https://learn.microsoft.com/en-us/windows/win32/psapi/working-set-information).

## Run it

Exit the resident Quick Dial process first, then run from the repository root:

```powershell
cmake --build build --config Release --parallel
.\build\Release\quickdial_benchmarks.exe
```

The benchmark briefly shows and hides the real launcher while measuring presentation. Leave it in the foreground during the visible samples. Exit code `0` means every budget passed, `1` means at least one metric exceeded its budget, and `2` means setup, measurement, or graceful shutdown failed.

## Current baseline

Measured on the development Windows 11 x64 workstation on 2026-09-07 with installed-app discovery enabled (162 apps), using the corrected completion-aware runner. The overall benchmark **fails** the unchanged hidden-initial total working-set and private-commit budgets. All responsiveness, idle CPU, visible/after-use memory, and executable-size budgets passed in this run. Discovery settled 568.10 ms after readiness, including the 250 ms quiet interval. All ten children exited normally; shutdown p95 was 11.48 ms.

The earlier 2026-08-31 baseline predates the current discovery behavior and is no longer representative. The previous runner's 29.83 MB hidden sample, taken only 250 ms after readiness, could precede discovery completion and understated the settled footprint. During investigation, synchronous discovery alone took 205–409 ms; readiness now completes without waiting for that scan. Total working set includes shared Windows DLL pages as well as private pages, so meeting the initial total-memory cap remains separate work; it is not hidden by trimming the working set or relaxing the budgets. Earlier runs also slightly exceeded the visible and hidden-after-use total caps (65.84 and 64.95 MB), illustrating the variability of total working set.

| Metric | Baseline | Budget |
| --- | ---: | ---: |
| Fresh-process startup median | 21.77 ms | Informational |
| Fresh-process startup p95 | 44.43 ms | 100 ms |
| First show to first paint | 40.60 ms | 100 ms |
| Warm show median | 10.63 ms | Informational |
| Warm show p95 | 12.70 ms | 50 ms |
| Graceful shutdown p95 | 11.48 ms | Informational; normal exit required within 3 seconds |
| Hidden initial working set | **35.99 MB (FAIL)** | 20 MB |
| Hidden initial private working set | 3.95 MB | 5 MB |
| Hidden initial private bytes | **5.17 MB (FAIL)** | 5 MB |
| Visible working set | 62.60 MB | 64 MB |
| Visible private working set | 11.25 MB | 20 MB |
| Visible private bytes | 15.62 MB | 20 MB |
| Hidden-after-use working set | 61.68 MB | 64 MB |
| Hidden-after-use private working set | 10.33 MB | 20 MB |
| Hidden-after-use private bytes | 14.68 MB | 16 MB |
| Hidden idle CPU over 3 seconds | 0.00 ms | 10 ms |
| Visible idle CPU over 3 seconds | 0.00 ms | 10 ms |
| Parse 100-app catalog | 0.60 ms | 2 ms |
| Search 100 apps | 0.02 ms | 0.25 ms |
| Release executable size | 198.00 KB | 256 KB |

`quickdial_background_tests` separately checks queue limits, 200 sequential jobs without handle growth, prompt destruction with a blocked worker, and bounded custom-icon output. `quickdial_launcher_tests` checks tray restoration, asynchronous catalog/error/selection behavior, benchmark completion flags, and eviction/reuse across 140 distinct icon-cache entries with an isolated catalog and generated image. Physical shortcut input, mixed-monitor presentation, and long-duration interactive use still need manual validation.
