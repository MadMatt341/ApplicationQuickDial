# Interaction and rendering

Read only the sections relevant to search, keyboard input, window layout, icons, or launching. [Design map](../architecture.md) · [Verification](../testing.md#manual-verification)

## Shortcut flow

```text
keyboard event
  -> HookManager::HookProcedure (hook thread)
  -> HotkeyState::Handle
  -> kMessageToggleLauncher via PostMessageW
  -> LauncherApp::HandleMessage (main thread)
  -> ShowLauncher or HideLauncher
```

`HotkeyState` tracks left/right Windows keys and whether the Space chord is active. The first physical Space keydown while either Windows key is down triggers the launcher. Repeated Space keydowns and the matching physical keyup are suppressed, including when Windows is released first. Injected events pass through and cannot end a physical chord.

When the chord triggers, `HookManager` injects a keydown/keyup pair for unused virtual key `0xE8`. This marks the Windows-key press as a chord so releasing Windows does not open Start. The state machine ignores that injected pair.

Before each non-injected Space event, the hook checks the high bit of `GetAsyncKeyState` for both Windows keys and clears any tracked press that is no longer down. This recovers from missed Windows-key releases without arming the shortcut from injected presses or ending an active Space chord. Unavailable key state disarms the corresponding Windows key. The check runs on Space because asynchronous state for a key is not updated until after that key's own low-level callback.

The hook cannot operate on the secure desktop and may not intercept input sent to a higher-integrity process. The tray icon remains a fallback in normal mode. Relaunching the executable opens the existing launcher in either mode.

## Search and selection

Every edit-control `EN_CHANGE` notification calls `UpdateResults`. The query is trimmed and lowercased with the invariant locale. Each application name is scored first:

| Score | Match |
|---:|---|
| 0 | exact name |
| 1 | name prefix |
| 2 | name match beginning after a non-alphanumeric boundary |
| 3 | other name substring |
| 4-7 | the same four match classes on an alias |
| 8-11 | the same four match classes on an AppUserModelID, target filename, or custom URI scheme |

Aliases are considered only when the name does not match, and language-neutral identifiers only when neither the name nor an alias matches. Identifier punctuation and camel case are treated as word boundaries. Web and file URI schemes are excluded because they are not application names. Results are stable-sorted by score, so equal scores retain catalog order. An empty or whitespace-only query returns no application results and shows System, Appearance, and Startup rows below the search divider. Typing on the main page searches typed application and command results; clearing the query restores the main rows. Appearance opens the shared user appearance file in Notepad. System opens a submenu with Shut down, Restart and Sign out. Its search field filters those actions by name or aliases (`shutdown`, `power off`, `reboot`), using the same ranking rules. System entries remain available without a valid application catalog. The UI retains all ranked matches and renders at most six rows from a scrollable viewport. Query changes reset the viewport. Keyboard navigation keeps selection visible; wheel input accumulates partial deltas and follows the Windows scroll-line preference. Mouse hit testing accounts for the viewport offset.

F10 opens the launcher menu without needing a notification tray. Up and Down wrap around the result list. Enter or a left click activates the selected app or built-in menu item. Escape and the clickable back arrow in the System header return to the main page and clear the submenu filter; Escape on the main page hides the window. Every reopening starts on the main page. Changing the query resets selection to the first result.

Enter and Escape ignore auto-repeat, so one held key cannot enter System and then activate a power action, or return to main and immediately dismiss it. The edit subclass handles Enter and Escape on `WM_KEYDOWN` and consumes their queued `WM_CHAR` messages so the single-line edit control does not play an invalid-character beep.

Root search recognizes `shutdown`, `power off`, `restart`, `reboot`, and sign-out aliases. Exact command labels/aliases precede apps; partial command matches require at least three non-whitespace characters and follow app matches. Power commands have no fuzzy matching. Result dispatch, painting, scrolling, hit testing and refresh selection use one typed `LauncherResult` vector, preventing an app index from being mistaken for a command. Command rows display “System action”. Empty search retains System navigation.

## Startup page

The main menu has a Startup entry.
The exact queries `startup`, `startup apps`, and `autostart` select this entry before application results.
Selection opens the account Startup page and closes Launcher after a successful process request.
A failed request keeps Launcher open with an error.
The shell platform library owns the page, Windows adapters, and Blade startup settings.
Launcher does not change startup registrations.
The [Startup contract](https://github.com/MadMatt341/WindowManager/blob/main/docs/startup-apps.md) gives controls, preservation rules, and checks.

## Window and rendering

The launcher is a topmost `WS_POPUP`/`WS_EX_TOOLWINDOW`, so it does not create a normal taskbar button. It hides when deactivated.

The Launcher window stays in view or out of view during theme, settings, and position changes.

On show, it chooses the foreground window's monitor, falling back to the cursor monitor, and positions itself near the upper center of that monitor's work area. Width and row dimensions are constants in device-independent pixels; `ResizeAndPosition` and `LayoutEditControl` scale them for monitor DPI. Height follows the number of visible results (up to six), a System header when inside that submenu, and an optional error row. The System header has a separate back-arrow hit area and is excluded from action-row hit testing.

The search field is a native edit control. The rest is rendered with Direct2D and DirectWrite. Blade Charcoal uses the shared fixed dark appearance, independent of
`AppsUseLightTheme`. [The Blade contract](https://github.com/MadMatt341/WindowManager/blob/main/docs/shell-themes.md) owns selection,
validation, defaults, bounds and accessibility precedence. Launcher statically
links the same resolver as Rail/Rust, then adapts it through `LauncherVisualStyle`.
Colors, installed font fallback, named text sizes, selection corners and component
width/search/result sizes come from that resolver. Search placeholder ink and
native edit/menu controls also use shared roles. Windows owns outer chrome.

The Charcoal launcher is 540 logical pixels wide. The search row is 52 pixels
high; result rows are 44 pixels high. Search text is 16 pixels. Launcher uses
the shared `launcherFamilies` font role. Rail keeps its existing font role.

The native font metrics set the search control height and vertical position.
The placeholder uses the native text rectangle and search background.
The text keeps its vertical position when the user starts to type.
The background color does not change.

The search row shows an Escape hint. The bottom row shows navigation and Enter
hints. These hints do not add click targets or change keyboard commands.

`--appearance FILE` / `BLADE_APPEARANCE_FILE` select isolated development settings;
normally `%LOCALAPPDATA%\Blade\Appearance\appearance.json` is the one edit point.
The main Appearance entry and F10 menu open it. Appearance reload updates fonts,
brushes and dimensions on the UI thread, preserving HWNDs, query, selection,
catalog/provider activity and existing shortcuts. Invalid live edits retain
the last valid theme. No hook reads theme files or launches a resolver service.

Icon resolution is lazy and follows this order:

1. Decode the catalog's explicit `icon` path with WIC.
2. Ask the Windows shell for an icon for `target`.
3. Draw a placeholder if neither source is available.

All file reads, image decoding, and shell icon extraction run in icon workers. Custom inputs are limited to 8192 pixels per side and 16 megapixels, then scaled to the requested display size (at most 96 pixels). Workers return premultiplied BGRA pixels, never apartment-bound COM interfaces or Direct2D resources. Only the main thread creates Direct2D bitmaps.

For ICO files, decoding chooses the smallest frame at least as large as the requested size, or the largest available frame if all are smaller. Other image formats retain their first frame. Shell extraction allows a larger native bitmap with `SIIGBF_BIGGERSIZEOK`, avoiding the Shell's default GDI resize. Shell HBITMAPs are imported with `WICBitmapUseAlpha` to describe their straight-alpha pixels. WIC converts to premultiplied alpha before downscaling with its Fant filter; exact-size images bypass scaling. Rendering centers each image within the icon slot, preserves its aspect ratio, and aligns its edges to physical pixels at the current monitor DPI.

Device-dependent bitmaps are cached by catalog index and released on catalog rebuild, hide, DPI change, or render-target loss. A separate least-recently-used source cache holds at most 128 scaled images or failed lookups, keyed by target, explicit icon path, and DPI, so reopening does not repeat decoding. Explicit tray reload clears source entries and advances a generation counter; late results from older generations are ignored.

System icons use one vector drawing module: a gapped power ring/stem, an open restart arc with tangent arrowhead, and a sign-out door/arrow. Geometry and strokes align to physical pixels and use the current theme’s normal/selected text color. Verify Charcoal at 96/120/144/192 DPI and with both Windows app palette preferences (the result remains fixed). Inactive-desktop screen copies omit Direct2D presentation; bounded software captures reuse the production drawing path and do not establish active compositor/installed acceptance.

## System power actions

Built-in menu items have explicit command identities separate from application catalog indexes and launch targets. `MainMenu` owns section entries and unified search; `SystemPower` owns the Windows effect. These actions work in normal and custom-shell modes and do not depend on Rail or Explorer.

Selecting Shut down or Restart captures the action and posts internal `kMessageSystemPowerAction` with no payload. Repeated requests are coalesced while queued or active; query changes cannot retarget an accepted action. Back, dismissal, and window destruction cancel a queued action before dispatch. The request runs after the input handler returns, without a confirmation dialog.

The native adapter enables `SE_SHUTDOWN_NAME`, calls `ExitWindowsEx` with `EWX_POWEROFF` or `EWX_REBOOT`, and restores the previous token privilege state. It uses no force flags, preserving Windows' application shutdown negotiation. API failures remain visible. Acceptance of the request is asynchronous and does not prove the computer completed shutdown or restart. See [Microsoft's API contract](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-exitwindowsex). Automated checks substitute the power-request boundary and never power off or restart the test machine; a real end-of-session trial remains separate acceptance.

## Launching

Enter and result clicks capture the selected entry and post `kMessageLaunchSelection` with no pointer payload. The main STA activates that owned copy after returning from the input handler, so synchronously delivered input cannot invoke COM activation inside a sent message. Repeated requests are coalesced while a launch is queued or active; catalog/query changes do not retarget an accepted launch. Destruction drops a pending request.

Each argument is quoted using Windows command-line escaping rules, then joined into one parameter string.

- A `shell:AppsFolder` target with a syntactically valid packaged AppUserModelID uses `IApplicationActivationManager::ActivateApplication`, passing the quoted arguments and suppressing Windows error dialogs. This avoids the Apps-folder verb handler's dependency on Explorer's desktop broker. Activation failures stay visible and do not fall back to a Shell handler.
- Other `shell:` targets, including ordinary desktop Apps-folder shortcuts, are resolved using `SHParseDisplayName` and invoked with `ShellExecuteExW` and `SEE_MASK_INVOKEIDLIST`. Resolution or activation failures stay visible; the launcher never substitutes `explorer.exe` as a fallback.
- All other targets are passed directly to `ShellExecuteExW` with the `open` verb. This supports executables, shortcuts, documents, and URLs through normal Windows associations.
- `workingDirectory`, when present, becomes `lpDirectory` for Shell launches. Packaged app activation uses the environment provided by Windows.

A successful launch hides the launcher. A failure stays visible and adds an error row.

Sign out uses EWX_LOGOFF for the current session, without a shutdown privilege or force flags. Windows handles save-work negotiation. This action also activates a prepared Blade update at the next sign-in. Automated launcher checks substitute dispatch and verify its independent selection and visible failure; actual sign-out/sign-in remains an explicitly authorized desktop acceptance check.

### Tray controls

Search also includes separate **Tray controls** results for running applications registered with Blade's notification tray.
These results use the application tooltip, process filename, and `tray controls` or `tray menu` search aliases.
The result shows the application's icon and a **Tray controls** label.
Normal application results keep their Windows Shell launch behavior.

Selecting a tray result requests the application's own menu through the
[tray host contract](https://github.com/MadMatt341/WindowManager/blob/main/docs/notification-tray.md#general-application-recovery).
The visible result icon supplies the menu anchor.
The application controls menu contents and the effects of its commands.
Opening Launcher, reading registrations, and searching send no application callbacks.
This behavior applies to registrations without application-name rules.

One background worker reads snapshots and sends bounded menu requests.
It has two queue slots and a 250-millisecond limit for each native call.
Visible Launcher windows refresh registrations once per second.
Search preserves an existing selection when a snapshot changes.
A missing host or unsupported interface gives no tray results.
Normal application launching remains available.

Selection captures the registration and posts an internal message before it sends a request.
Launcher accepts one menu request at a time.
Dismissal cancels a request before dispatch.
The worker checks cancellation before sending the action.
It reads a new snapshot and checks the host HWND, host session, token, and process ID.
A missing registration or rejected request keeps Launcher open with an error; it does not select another application.

Applications can register a tray icon before they create a main window.
The tray menu gives access when an application ignores its normal second-instance signal.
Windows owns Shell launch behavior; applications own their startup completion and hidden windows.
