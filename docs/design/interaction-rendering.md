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

Aliases are considered only when the name does not match, and language-neutral identifiers only when neither the name nor an alias matches. Identifier punctuation and camel case are treated as word boundaries. Web and file URI schemes are excluded because they are not application names. Results are stable-sorted by score, so equal scores retain catalog order. An empty or whitespace-only query returns no application results and shows a built-in System row below the search divider. Typing on the main page searches applications; clearing the query restores System. System opens a submenu with Shut down and Restart. Its search field filters those actions by name or aliases (`shutdown`, `power off`, `reboot`), using the same ranking rules. System entries remain available without a valid application catalog. The UI retains all ranked matches and renders at most six rows from a scrollable viewport. Query changes reset the viewport. Keyboard navigation keeps selection visible; wheel input accumulates partial deltas and follows the Windows scroll-line preference. Mouse hit testing accounts for the viewport offset.

F10 opens the launcher menu without needing a notification tray. Up and Down wrap around the result list. Enter or a left click activates the selected app or built-in menu item. Escape and the clickable back arrow in the System header return to the main page and clear the submenu filter; Escape on the main page hides the window. Every reopening starts on the main page. Changing the query resets selection to the first result.

Enter and Escape ignore auto-repeat, so one held key cannot enter System and then activate a power action, or return to main and immediately dismiss it. The edit subclass handles Enter and Escape on `WM_KEYDOWN` and consumes their queued `WM_CHAR` messages so the single-line edit control does not play an invalid-character beep.

## Window and rendering

The launcher is a topmost `WS_POPUP`/`WS_EX_TOOLWINDOW`, so it does not create a normal taskbar button. It hides when deactivated.

On show, it chooses the foreground window's monitor, falling back to the cursor monitor, and positions itself near the upper center of that monitor's work area. Width and row dimensions are constants in device-independent pixels; `ResizeAndPosition` and `LayoutEditControl` scale them for monitor DPI. Height follows the number of visible results (up to six), a System header when inside that submenu, and an optional error row. The System header has a separate back-arrow hit area and is excluded from action-row hit testing.

The search field is a native edit control. The rest is rendered with Direct2D and DirectWrite. Theme colors follow `AppsUseLightTheme` and are refreshed whenever the launcher opens.

Icon resolution is lazy and follows this order:

1. Decode the catalog's explicit `icon` path with WIC.
2. Ask the Windows shell for an icon for `target`.
3. Draw a placeholder if neither source is available.

All file reads, image decoding, and shell icon extraction run in icon workers. Custom inputs are limited to 8192 pixels per side and 16 megapixels, then scaled to the requested display size (at most 96 pixels). Workers return premultiplied BGRA pixels, never apartment-bound COM interfaces or Direct2D resources. Only the main thread creates Direct2D bitmaps.

For ICO files, decoding chooses the smallest frame at least as large as the requested size, or the largest available frame if all are smaller. Other image formats retain their first frame. Shell extraction allows a larger native bitmap with `SIIGBF_BIGGERSIZEOK`, avoiding the Shell's default GDI resize. Shell HBITMAPs are imported with `WICBitmapUseAlpha` to describe their straight-alpha pixels. WIC converts to premultiplied alpha before downscaling with its Fant filter; exact-size images bypass scaling. Rendering centers each image within the icon slot, preserves its aspect ratio, and aligns its edges to physical pixels at the current monitor DPI.

Device-dependent bitmaps are cached by catalog index and released on catalog rebuild, hide, DPI change, or render-target loss. A separate least-recently-used source cache holds at most 128 scaled images or failed lookups, keyed by target, explicit icon path, and DPI, so reopening does not repeat decoding. Explicit tray reload clears source entries and advances a generation counter; late results from older generations are ignored.

## System power actions

Built-in menu items have explicit command identities separate from application catalog indexes and launch targets. `MainMenu` owns section entries and scoped search; `SystemPower` owns the Windows effect. These actions work in normal and custom-shell modes and do not depend on Rail or Explorer.

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
