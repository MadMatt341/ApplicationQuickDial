# Troubleshooting

Read the section matching the symptom. [Development map](development.md) · [Runtime contract](design/runtime.md) · [Verification](testing.md)

## Startup and desktop

1. Establish the actual user account, session, and Win32 desktop. A development tool may use another account or desktop even when it can inspect the user's processes. Start a user-facing launcher on the user's interactive desktop.
2. Check the process path and desktop-local launcher window (`ApplicationQuickDial.LauncherWindow`). Confirm the window answers a bounded message and that Windows reports a tray icon rectangle. Process existence or `Get-Process.Responding` alone is insufficient.
3. For sign-in failures, verify the quoted executable exists and is registered under the intended user's `Software\Microsoft\Windows\CurrentVersion\Run` value `ApplicationQuickDial`. Check for a disabled entry in `Explorer\StartupApproved\Run` and for relevant startup policies.
4. Independently confirm the registration is visible to Windows, for example through `Win32_StartupCommand`. If tool-local registry reads disagree with that inventory, compare `StdRegProv` reads under `HKEY_USERS\<actual-user-SID>` before claiming success. Do not infer visibility from account name or a successful write alone.
5. Look for startup attempts in `Microsoft-Windows-Shell-Core/Operational` and crashes/hangs in the Application event log. Absence of a crash entry does not establish that the program was launched or prove why it exited.
6. Test the registered command from the normal desktop, using a normal startup working directory. Confirm its tray/window behavior. Actual automatic startup is verified only after a subsequent sign-in; do not restart or sign out the user just to complete this check without authorization.

When using PowerShell P/Invoke diagnostics, pass a real C# `null` for an unrestricted window title. PowerShell `$null` can become an empty string at a string parameter, producing a false negative for a named window.

An instance on another Win32 desktop no longer blocks the user's launcher. An older copy on the same desktop is intentionally reused; exit it before testing a newly built executable. Initialization failures display a startup message box and exit with code `1`.

## Shortcut or focus

If **Win+Space** does nothing, try the tray icon. Hook installation failure is reported by tray notification. The secure desktop and elevated target applications are expected shortcut limitations. See [shortcut checks](testing.md#shortcut-and-focus) before changing the key-state machine.

## Catalog, discovery, or icons

- Catalog edits reload on open or **Reload app list**. A parse error retains the previous valid catalog; inspect the error before assuming the edit was ignored.
- New apps appear after Windows registration and a completed background refresh. A failed scan retains the previous list; explicit reload retries. See [discovery behavior](design/catalog-discovery.md).
- For a missing custom icon, verify its expanded path and the [supported limits](catalog.md). Unsupported images fall back to the Shell icon or placeholder.

## Build environment

Use the Visual Studio Developer PowerShell and a CMake version supporting the selected generator. An existing build directory must use its original generator; use a separate `out/` directory when changing it.

If MSBuild reports duplicate `Path`/`PATH` entries, normalize names in the child build process's environment. If its file tracker fails with access denied in a restricted tool session, use the authorized normal build context. These are environment failures, not evidence of a C++ compilation defect.
