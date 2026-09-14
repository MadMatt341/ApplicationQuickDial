# Architecture overview

Quick Dial is a native Win32 application with one resident GUI process per Win32 desktop. It has no service, database, or network dependency. The JSON catalog is its only persisted application data; Windows owns the separate sign-in registration.

The main thread owns windows, rendering, catalog state, selection, and launching. A dedicated keyboard-hook thread posts shortcut messages to it. Bounded background workers return owned results; app enumeration runs in a temporary child process so Shell discovery components leave memory when the scan ends.

```text
JSON catalog + Windows app discovery -> merged catalog -> ranked results -> shell launch
Keyboard hook / tray / edit control -> main-thread window and selection
Windows change notifications -> bounded discovery refresh -> main-thread completion
```

## Read at the affected boundary

| Work | Detailed contract |
|---|---|
| Process/thread lifetime, singleton, tray, sign-in | [Runtime and Windows integration](design/runtime.md) |
| Catalog parsing, merging, notifications, helper transport | [Catalog and discovery](design/catalog-discovery.md) |
| Shortcut, search, selection, rendering, icons, launching | [Interaction and rendering](design/interaction-rendering.md) |
| User-editable JSON | [Catalog reference](catalog.md) |
| Responsiveness and resource budgets | [Performance contract](performance.md) |

Use the [development map](development.md#where-to-make-a-change) for source locations and required verification. Read the relevant detail rather than the entire design set.
