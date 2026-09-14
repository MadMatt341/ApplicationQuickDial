# Historical performance measurements

These dated results describe the builds tested at the time, not the current binary. Read when comparing regressions or investigating earlier measurements. [Current contract and method](../performance.md).

## Notification watcher measurements

Measured on 2026-09-07 after replacing refresh-on-open and periodic scanning with Windows notifications. The resident process watches the virtual Apps folder and association changes through the Shell, and waits on two Start-menu directory-change handles in its existing message loop. No app-list scan runs on an ordinary open or idle timer. The real shortcut integration check confirmed automatic addition while hidden and removal after the shortcut was deleted, without synthetic notifications for either operation.

| Metric | Event-driven build | Budget |
| --- | ---: | ---: |
| Fresh-process startup median / p95 | 31.31 / 33.21 ms | p95 ≤ 100 ms |
| First show to first paint | 36.47 ms | ≤ 100 ms |
| Warm show median / p95 | 4.67 / 5.62 ms | p95 ≤ 50 ms |
| Discovery settled after readiness | 647.02 ms | Informational, includes 250 ms quiet |
| Graceful shutdown p95 | 14.43 ms | Informational; normal exit required |
| Hidden initial working set / private working set / private bytes | 16.28 / 1.91 / 2.57 MB | ≤ 20 / 5 / 5 MB |
| Visible working set / private working set / private bytes | 54.18 / 11.18 / 15.70 MB | ≤ 64 / 20 / 20 MB |
| Hidden-after-use working set / private working set / private bytes | 53.27 / 10.28 / 14.79 MB | ≤ 64 / 20 / 16 MB |
| Hidden idle CPU over 3 seconds | 0.00 ms | ≤ 10 ms |
| Visible idle CPU over 3 seconds | **15.62 ms** | **≤ 10 ms: failed** |
| Parse 100 apps / search 100 apps | 0.71 / 0.02 ms | ≤ 2 / 0.25 ms |
| Release executable size | 226.50 KB | ≤ 256 KB |

All memory and responsiveness budgets passed, but this run did **not** pass the full contract because of the visible idle CPU result. No budget or sampling interval was changed. A comparison built from unchanged `HEAD` did not reach its visible resource sample because the window failed to settle; it therefore does not establish whether the visible CPU result is a regression. The benchmark now distinguishes visibility changes from background-settling timeouts in its failure output.

An initial watcher implementation used filesystem Shell PIDLs for the Start menus and retained 20.86 MB at hidden startup. Using directory paths and kernel change-notification handles instead reduced that sample to 16.28 MB. Registration does not enumerate installed apps; enumeration still runs only in the temporary helper.

## Visible idle CPU investigation

Controlled probes on 2026-09-07 identified the native search-field caret as the recurring UI-thread work. A temporary diagnostic executable built the actual launcher sources and counted dequeued messages with a thread-local hook. Its parent sampled process and thread time and cycle counters externally over three consecutive three-second intervals. Samples required the window to remain visible and foreground; a run interrupted by focus loss was discarded.

| Configuration | UI-thread cycles in first 3 seconds | Search-field system timer messages | UI-thread cycles in third 3-second sample |
| --- | ---: | ---: | ---: |
| Current notification build | 3,557,960 | 6 | 0 |
| Unchanged pre-notification source, `b9ae56c` | 3,550,053 | 6 | 0 |
| Current build with watcher stopped | 3,695,868 | 6 | 0 |
| Current build with caret destroyed for diagnosis | 0 | 0 | 0 |

Stopping the watcher left the activity intact. Destroying the caret removed all UI-thread activity in all three samples. The unchanged source exhibited the same timer messages and nearly identical first-sample cycle count. No discovery timer, discovery completion, or Shell-change message was observed during these samples. Some samples also contained a small amount of work on other threads; its exact origin was not established.

Every valid probe sample reported 0 ms through process CPU-time accounting, even when cycle counters showed work. The earlier 15.62 ms benchmark result therefore should not be interpreted as a precise measurement of watcher overhead. Microsoft explains the limited accuracy of thread-time accounting and recommends cycle counters for finer CPU-usage observation: [CPU usage measurement](https://devblogs.microsoft.com/oldnewthing/20161021-00/?p=94565). Cycles are reported directly and are not converted to milliseconds.

This establishes that the observed recurring UI work predates the watcher; it does not retroactively pass the failed benchmark or establish zero watcher overhead under every condition. Caret behavior, benchmark limits, and sample duration remain unchanged. Local diagnostic sources and raw logs are under `out/idle-probe/` and `out/idle-probe-{current,caret-off,baseline,watcher-off}.log`; these generated investigation artifacts are not tracked.

## Pre-notification baseline

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
