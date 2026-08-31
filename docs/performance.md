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

Private working set is the most useful resident-memory figure for this utility because it counts physical pages that are not shared with other processes. Total working set is also capped and reported, but it includes shared Windows, DirectWrite, WIC, and runtime pages. Private committed bytes describe process-owned virtual-memory commitment, whether or not each page is currently resident. Windows working-set measurements are point-in-time snapshots and can change under system memory pressure.

## Measurement method

- **Fresh-process readiness** starts at `CreateProcessW` and ends after the window, tray icon, catalog, and keyboard hook initialize and signal the benchmark event. Each trial uses a new process, but this is not a cold-boot or cold-disk-cache test.
- **First/warm show** starts when the benchmark posts the real show command and ends after the first successful paint. Shell icons load asynchronously and therefore do not delay the usable search surface.
- **Hidden initial memory** is sampled 250 ms after startup, before the launcher has appeared.
- **Visible memory/CPU** is sampled after the launcher has painted and remained static for 250 ms.
- **Hidden-after-use memory/CPU** is sampled after the visible launcher is hidden and display resources are released.
- **CPU** uses process kernel plus user time over a three-second wall-clock interval with no input or animation.
- **Parsing/search** use a 100-entry synthetic catalog, intentionally larger than the expected hand-maintained list.

The runner uses `GetProcessMemoryInfo` for total working set and private committed bytes, plus `QueryWorkingSet` to count non-shared resident pages. Microsoft notes that working-set APIs report pages physically present at the exact moment of the query: [Working Set Information](https://learn.microsoft.com/en-us/windows/win32/psapi/working-set-information).

## Run it

Exit the resident Quick Dial process first, then run from the repository root:

```powershell
cmake --build build --config Release --parallel
.\build\Release\quickdial_benchmarks.exe
```

The benchmark briefly shows and hides the real launcher while measuring presentation. Exit code `0` means every budget passed, `1` means at least one metric exceeded its budget, and `2` means the benchmark could not be set up.

## Current baseline

Measured on the development Windows 11 x64 workstation on 2026-08-31. These values are the worse of two consecutive passing runs after a warm build and include normal run-to-run variance.

| Metric | Baseline | Budget |
| --- | ---: | ---: |
| Fresh-process startup median | 25.10 ms | Informational |
| Fresh-process startup p95 | 33.37 ms | 100 ms |
| First show to first paint | 48.18 ms | 100 ms |
| Warm show median | 5.02 ms | Informational |
| Warm show p95 | 11.75 ms | 50 ms |
| Hidden initial working set | 16.05 MB | 20 MB |
| Hidden initial private working set | 1.65 MB | 5 MB |
| Hidden initial private bytes | 2.25 MB | 5 MB |
| Visible working set | 54.57 MB | 64 MB |
| Visible private working set | 11.66 MB | 20 MB |
| Visible private bytes | 15.94 MB | 20 MB |
| Hidden-after-use working set | 53.52 MB | 64 MB |
| Hidden-after-use private working set | 10.61 MB | 20 MB |
| Hidden-after-use private bytes | 14.89 MB | 16 MB |
| Hidden idle CPU over 3 seconds | 0.00 ms | 10 ms |
| Visible idle CPU over 3 seconds | 0.00 ms | 10 ms |
| Parse 100-app catalog | 0.69 ms | 2 ms |
| Search 100 apps | 0.01 ms | 0.25 ms |
| Release executable size | 146.50 KB | 256 KB |
