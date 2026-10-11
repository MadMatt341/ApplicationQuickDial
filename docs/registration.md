# Register an app with Blade Launcher

Use this contract for installers, portable apps and development builds with no Windows app/Start-menu registration. [Catalog](catalog.md) · [Troubleshooting](troubleshooting.md#catalog-discovery-or-icons) · [Implementation](design/catalog-discovery.md)

The UI is Launcher; the full name is Blade Launcher. `ApplicationQuickDial.exe` and `%LOCALAPPDATA%\ApplicationQuickDial\apps.json` remain compatibility identifiers. Existing settings need no migration.

Create a UTF-8 JSON file containing **one application object**, with a stable `id` and the usual catalog fields:

```json
{
  "id": "example-reader",
  "name": "Example Reader",
  "target": "C:\\Apps\\Reader\\reader.exe",
  "aliases": ["reader", "books"],
  "arguments": ["--library", "C:\\Books"],
  "workingDirectory": "C:\\Apps\\Reader",
  "icon": "C:\\Apps\\Reader\\reader.ico"
}
```

Call the self-contained script beside the launcher executable:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File 'C:\Apps\Launcher\Register-LauncherApp.ps1' `
  -ApplicationJson 'C:\Apps\Reader\launcher-entry.json' `
  -ReportDirectory 'C:\Apps\Reader\out\launcher-registration'
```

In a Blade package, the script may be under `<generation>\Launcher\` while the configured executable is `<generation>\ApplicationQuickDial.exe`. Pass `-LauncherPath '<generation>\ApplicationQuickDial.exe'` to bind to that exact build. It requires version 0.2.0 or later. `-VerifyOnly` independently checks an existing entry and does not update it.

The script snapshots the request, obtains the intended current account SID, and creates a hidden ordinary Windows PowerShell process through `Win32_Process`. The launcher’s native helper verifies the account and rejects packaged context, reads the catalog under a per-path mutex, and atomically updates the matching stable ID. It keeps an exact uniquely named pre-update backup. Different registered IDs/argument commands remain separate; matching legacy entries without an ID may be adopted only for the same target/arguments. Unknown fields, unspecified app fields, app order, hidden/inclusion rules and discovery settings are preserved. Invalid requests or existing JSON leave the catalog untouched.

A second independent ordinary Windows process rereads the entry and file hash. Success requires agreement on account, catalog path, requested values, entry uniqueness and SHA-256. Reports stay in the requested directory. If verification disagrees, inspect both reports and the backup before retrying; a write alone is not registration success. Callers should use a unique report directory per registration. All waits are bounded, and the helper never registers startup, stops the resident Launcher or sends a launch/power request.

Packaged development tools can redirect AppData access even with elevation. Do not replace this process with `Set-Content` against a nominal AppData path or overwrite the entire catalog. Reopen Launcher or use **F10 → Reload app list** after a verified update. Diagnose the actual running instance using:

```powershell
& 'C:\Apps\Launcher\ApplicationQuickDial.exe' --status --output 'C:\Reports\launcher-status.json'
& 'C:\Apps\Launcher\ApplicationQuickDial.exe' --lookup 'Example Reader' --output 'C:\Reports\reader-lookup.json'
& 'C:\Apps\Launcher\ApplicationQuickDial.exe' --search 'reboot' --output 'C:\Reports\reboot-search.json'
```

`--search` opens and searches the existing launcher; it never activates a result. The output directory must already exist. If the resident binary is older than these diagnostics, update it through the owning Blade lifecycle before claiming installed acceptance.

## Preserve an existing startup preference during a Blade update

`Migrate-LauncherStartup.ps1` is a lifecycle helper for an existing `ApplicationQuickDial` Run value. Use it from ordinary Windows PowerShell under the intended account. Preparation requires the independently reviewed old command, exact candidate executable, account SID, and a new snapshot path. Installed Blade also requires its stable installed-root dispatcher supplied by the shell owner:

```powershell
.\Migrate-LauncherStartup.ps1 -LauncherPath 'C:\Blade\candidate\ApplicationQuickDial.exe' `
  -ExpectedAccount 'S-1-5-21-...' -ExpectedCommand '"D:\Development\ApplicationQuickDial.exe" --shell-mode' `
  -StartupDispatcher 'C:\Blade\Start-BladeLauncher.ps1' -InstallDirectory 'C:\Blade' `
  -SnapshotPath 'C:\Blade\reports\launcher-startup.json'
```

Preparation does not change startup. The owning Blade updater may repeat the same candidate/account/snapshot/dispatcher arguments with `-Apply` at its approved lifecycle checkpoint, then with `-Verify` for independent readback. The Run command invokes ordinary Windows PowerShell and the stable root script, preserving the original argument suffix as UTF-8 hex data (`-LauncherArgumentsHex`, `00` means empty). The dispatcher infers its root from its own directory. It owns verified generation selection, activation/rollback and native launch quoting; migration must not be applied until that dispatcher is provisioned and tested. Direct bindings into `Releases/<UUID>` are rejected, because they can outlive rollback or an update and win the singleton with an obsolete launcher. Standalone portable installations may omit the dispatcher to keep a direct executable binding.

The helper preserves `StartupApproved` bytes, including a disabled preference. An absent Run entry stays absent. Snapshot version 2 binds candidate and dispatcher hashes, account, previous command and approval. The command must fit Windows' 260-character Run limit; a longer path/argument suffix requires a shorter stable installation path, rather than truncation. Native `StdRegProv` reads/writes the explicit account key; `Win32_StartupCommand` independently verifies the command. A failed verification restores the old command unless another writer has intervened. Unquoted or ambiguous old commands require inspection rather than automatic rewriting. The helper never edits the catalog, stops the resident, installs a build, or signs out.
