#include "blade_appearance.h"
#include "LauncherApp.h"
#include "UgsActivation.h"

#include "AppMessages.h"
#include "BackgroundTasks.h"
#include "DiscoveryProcess.h"
#include "HookManager.h"
#include "InstalledApps.h"
#include "InstalledAppsWatcher.h"
#include "LauncherVisualStyle.h"
#include "Search.h"
#include "SystemIcons.h"
#include "StartupManager.h"
#include "CatalogRegistration.h"
#include "LauncherDiagnostics.h"
#include "TextUtilities.h"
#include <fstream>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <appmodel.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shobjidl.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <string_view>
#include <utility>

#include <winrt/base.h>

namespace quickdial {
namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kTrayIconId = 1;
constexpr UINT kCommandOpen = 1001;
constexpr UINT kCommandOpenCatalog = 1002;
constexpr UINT kCommandReloadCatalog = 1003;
constexpr UINT kCommandStartWithWindows = 1004;
constexpr UINT kCommandExit = 1005;
constexpr UINT kCommandAddApplication = 1006;
constexpr UINT kCommandDiagnostics = 1007;
constexpr UINT kCommandMaintenance = 1008;
constexpr UINT kCommandAppearance = 1009;

constexpr std::size_t kMaximumVisibleResults = 6;
constexpr UINT_PTR kTrayRetryTimer = 1;
constexpr UINT_PTR kBackgroundTimer = 2;
constexpr UINT_PTR kInstalledApplicationsChangeTimer = 3;
constexpr UINT_PTR kInstalledApplicationsRetryTimer = 4;
constexpr std::size_t kMaximumIconSources = 128;

float ScaleForDpi(float value, UINT dpi) {
  return value * static_cast<float>(dpi) / 96.0f;
}

int ScaleForDpiInt(float value, UINT dpi) {
  return static_cast<int>(std::lround(ScaleForDpi(value, dpi)));
}

bool StartsWithCaseInsensitive(std::wstring_view value, std::wstring_view prefix) {
  if (value.size() < prefix.size()) {
    return false;
  }
  return _wcsnicmp(value.data(), prefix.data(), prefix.size()) == 0;
}

std::wstring QuoteArgument(std::wstring_view argument) {
  if (argument.empty()) {
    return L"\"\"";
  }
  if (argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
    return std::wstring(argument);
  }

  std::wstring result(1, L'\"');
  std::size_t backslashes = 0;
  for (const wchar_t character : argument) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'\"') {
      result.append(backslashes * 2 + 1, L'\\');
      result.push_back(L'\"');
      backslashes = 0;
      continue;
    }
    result.append(backslashes, L'\\');
    backslashes = 0;
    result.push_back(character);
  }
  result.append(backslashes * 2, L'\\');
  result.push_back(L'\"');
  return result;
}

std::wstring JoinArguments(const std::vector<std::wstring>& arguments) {
  std::wstring result;
  for (const auto& argument : arguments) {
    if (!result.empty()) {
      result.push_back(L' ');
    }
    result += QuoteArgument(argument);
  }
  return result;
}

std::wstring WindowsErrorMessage(DWORD error) {
  wchar_t* buffer = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
  std::wstring result = length > 0 ? std::wstring(buffer, length) : L"Unknown Windows error";
  if (buffer != nullptr) {
    LocalFree(buffer);
  }
  while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) {
    result.pop_back();
  }
  return result;
}

bool ReadLightThemePreference() {
  DWORD value = 0;
  DWORD size = sizeof(value);
  const LONG result = RegGetValueW(
      HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
      L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
  return result == ERROR_SUCCESS && value != 0;
}

}  // namespace

LauncherApp::LauncherApp(HANDLE benchmarkPresentedEvent, std::filesystem::path catalogPath, bool shellMode)
    : shellMode_(shellMode), benchmarkPresentedEvent_(benchmarkPresentedEvent), catalogPath_(std::move(catalogPath)) {}

LauncherApp::~LauncherApp() {
  StopBackgroundTasks();
  if (hookManager_) {
    hookManager_->Stop();
  }
  RemoveTrayIcon();
  ReleaseDeviceResources();
  if (editFont_ != nullptr) {
    DeleteObject(editFont_);
  }
  if (editBackgroundBrush_ != nullptr) {
    DeleteObject(editBackgroundBrush_);
  }
}

bool LauncherApp::Initialize(HINSTANCE instance) {
  instance_ = instance;
  if (catalogPath_.empty()) catalogPath_ = GetCatalogPath();

  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&controls);

  if (!CreateFactories() || !CreateMainWindow()) {
    return false;
  }

  iconTasks_ = std::make_unique<BackgroundTasks>(window_, kMessageBackgroundComplete, 2, 32);
  discoveryTasks_ = std::make_unique<BackgroundTasks>(window_, kMessageBackgroundComplete, 1, 1);
  installedAppsWatcher_ = std::make_unique<InstalledAppsWatcher>();
  taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
  if (taskbarCreatedMessage_ == 0) {
    return false;
  }
  AddTrayIcon();

  hookManager_ = std::make_unique<HookManager>(window_);
  const bool hookReady = hookManager_->Start();
  ReloadCatalog();
  if (shellMode_) EnterShellMode();
  if (!hookReady) {
    ShowNotification(L"Blade Launcher", L"Win+Space could not be captured. Run Launcher again to open it.");
  }
  return true;
}

int LauncherApp::Run() {
  for (;;) {
    const auto directories = installedAppsWatcher_->DirectoryHandles();
    const DWORD count = static_cast<DWORD>(directories.size());
    const DWORD ready = MsgWaitForMultipleObjectsEx(count, directories.data(), INFINITE,
        QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (ready == WAIT_FAILED) {
      ShowNotification(L"Blade Launcher", L"Could not wait for Windows events. Restart Launcher.");
      DestroyWindow(window_);
      return 1;
    }
    if (ready < WAIT_OBJECT_0 + count) {
      HandleInstalledApplicationsDirectoryChange(ready - WAIT_OBJECT_0);
    }
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (message.message == WM_QUIT) return static_cast<int>(message.wParam);
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
}

bool LauncherApp::CreateFactories() {
  visuals::Refresh();
  HRESULT result = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory_.ReleaseAndGetAddressOf());
  if (FAILED(result)) {
    return false;
  }
  result = DWriteCreateFactory(
      DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
      reinterpret_cast<IUnknown**>(dwriteFactory_.ReleaseAndGetAddressOf()));
  if (FAILED(result)) {
    return false;
  }
  fontFamily_ = visuals::ResolveFontFamily(dwriteFactory_.Get());
  return CreateTextFormats();
}

bool LauncherApp::CreateTextFormats() {
  std::array<ComPtr<IDWriteTextFormat>, 3> formats;
  const std::array sizes{visuals::kResultFontSize, visuals::kMessageFontSize, visuals::kHintFontSize};
  for (std::size_t index = 0; index < formats.size(); ++index) {
    const HRESULT result = dwriteFactory_->CreateTextFormat(
        fontFamily_.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, sizes[index], L"", &formats[index]);
    if (FAILED(result) ||
        FAILED(formats[index]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP)) ||
        FAILED(formats[index]->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING)) ||
        FAILED(formats[index]->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER))) return false;
  }
  resultTextFormat_ = std::move(formats[0]);
  messageTextFormat_ = std::move(formats[1]);
  hintTextFormat_ = std::move(formats[2]);
  return true;
}

bool LauncherApp::CreateMainWindow() {
  WNDCLASSEXW windowClass{};
  windowClass.cbSize = sizeof(windowClass);
  windowClass.style = CS_HREDRAW | CS_VREDRAW;
  windowClass.lpfnWndProc = WindowProcedure;
  windowClass.hInstance = instance_;
  windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  windowClass.lpszClassName = kWindowClassName;
  if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }

  window_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kWindowClassName, L"Blade Launcher", WS_POPUP,
      CW_USEDEFAULT, CW_USEDEFAULT, static_cast<int>(visuals::kWindowWidth), 160, nullptr, nullptr, instance_, this);
  return window_ != nullptr;
}

void LauncherApp::ConfigureWindowAppearance() {
  const DWM_WINDOW_CORNER_PREFERENCE cornerPreference = DWMWCP_ROUND;
  DwmSetWindowAttribute(window_, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPreference, sizeof(cornerPreference));
  ApplyTheme();
  blade_appearance_style_window(window_);
  // Keep Launcher's native edit on its renderer-owned background. The shared
  // native adapter still owns menus and ordinary controls in other windows.
  if (!SetWindowSubclass(window_, AppearanceProcedure, 2, reinterpret_cast<DWORD_PTR>(this))) {
    launchError_ = L"Could not apply the launcher search background.";
  }
  LayoutEditControl();
}

void LauncherApp::ApplyTheme() {
  lightTheme_ = false;
  visuals::Refresh();
  fontFamily_ = visuals::ResolveFontFamily(dwriteFactory_.Get());
  if (dwriteFactory_ && !CreateTextFormats()) {
    launchError_ = L"Could not update launcher text. The previous text style is still available.";
  }
  LayoutEditControl();
  const BOOL darkMode = lightTheme_ ? FALSE : TRUE;
  DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

  if (editBackgroundBrush_ != nullptr) {
    DeleteObject(editBackgroundBrush_);
  }
  editBackgroundBrush_ = CreateSolidBrush(visuals::GetPalette(lightTheme_).editBackground);
  InvalidateRect(edit_, nullptr, TRUE);
}

void LauncherApp::LayoutEditControl() {
  if (edit_ == nullptr) {
    return;
  }
  const int x = ScaleForDpiInt(visuals::kSearchEditLeft, dpi_);
  const int width = ScaleForDpiInt(
      visuals::kWindowWidth - visuals::kSearchEditLeft - visuals::kSearchEditRight, dpi_);
  const HFONT replacement = CreateFontW(
      -ScaleForDpiInt(visuals::kSearchFontSize, dpi_), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, fontFamily_.c_str());
  if (!replacement) {
    launchError_ = L"Could not update the search font. The previous font is still available.";
    return;
  }
  TEXTMETRICW metrics{};
  const HDC dc = GetDC(edit_);
  const HGDIOBJ previous = dc ? SelectObject(dc, replacement) : nullptr;
  const bool measured = previous && GetTextMetricsW(dc, &metrics);
  if (previous) SelectObject(dc, previous);
  if (dc) ReleaseDC(edit_, dc);
  if (!measured) {
    DeleteObject(replacement);
    launchError_ = L"Could not measure the search font. The previous font is still available.";
    return;
  }
  const int height = metrics.tmHeight + 2;
  const int y = (ScaleForDpiInt(visuals::kSearchHeight, dpi_) - height) / 2;
  SetWindowPos(edit_, nullptr, x, y, width, height, SWP_NOACTIVATE | SWP_NOZORDER);
  const HFONT oldFont = editFont_;
  editFont_ = replacement;
  SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(replacement), TRUE);
  if (oldFont) DeleteObject(oldFont);
  SendMessageW(edit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(0, 0));
}

void LauncherApp::ResizeAndPosition(bool chooseMonitor) {
  const bool hasMessage = !catalogError_.empty() || !launchError_.empty();
  const std::size_t visibleRows = VisibleResultRows();
  const float heightDip = ResultsTop() + static_cast<float>(visibleRows) * visuals::kResultHeight +
                          visuals::kBottomPadding + visuals::kFooterHeight +
                          (hasMessage ? visuals::kStatusHeight : 0.0f);

  if (chooseMonitor || anchorMonitor_ == nullptr) {
    HWND foreground = GetForegroundWindow();
    if (foreground == nullptr || foreground == window_) {
      POINT cursor{};
      GetCursorPos(&cursor);
      anchorMonitor_ = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    } else {
      anchorMonitor_ = MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
    }
  }

  UINT dpiX = 96;
  UINT dpiY = 96;
  if (anchorMonitor_ != nullptr) {
    GetDpiForMonitor(anchorMonitor_, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
  }
  dpi_ = dpiX;

  const int width = ScaleForDpiInt(visuals::kWindowWidth, dpi_);
  const int height = ScaleForDpiInt(heightDip, dpi_);
  int x = 0;
  int y = 0;

  MONITORINFO monitorInfo{sizeof(monitorInfo)};
  if (anchorMonitor_ != nullptr && GetMonitorInfoW(anchorMonitor_, &monitorInfo)) {
    const RECT& work = monitorInfo.rcWork;
    x = work.left + ((work.right - work.left) - width) / 2;
    y = work.top + std::max(ScaleForDpiInt(24.0f, dpi_), static_cast<int>((work.bottom - work.top) * 0.18));
  }

  SetWindowPos(window_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
  LayoutEditControl();
}

void LauncherApp::AddTrayIcon() {
  if (shellMode_) return;
  if (trayIconAdded_) {
    return;
  }
  NOTIFYICONDATAW icon{};
  icon.cbSize = sizeof(icon);
  icon.hWnd = window_;
  icon.uID = kTrayIconId;
  icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  icon.uCallbackMessage = kMessageTray;
  icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  wcscpy_s(icon.szTip, L"Blade Launcher");
  trayIconAdded_ = Shell_NotifyIconW(NIM_ADD, &icon) != FALSE ||
                   Shell_NotifyIconW(NIM_MODIFY, &icon) != FALSE;
  if (trayIconAdded_) {
    KillTimer(window_, kTrayRetryTimer);
  } else if (SetTimer(window_, kTrayRetryTimer, 2000, nullptr) == 0) {
    launchError_ = L"Could not restore the tray icon. Restart Launcher when Explorer is available.";
  }
}

void LauncherApp::RemoveTrayIcon() {
  if (!trayIconAdded_ || window_ == nullptr) {
    return;
  }
  NOTIFYICONDATAW icon{};
  icon.cbSize = sizeof(icon);
  icon.hWnd = window_;
  icon.uID = kTrayIconId;
  Shell_NotifyIconW(NIM_DELETE, &icon);
  trayIconAdded_ = false;
}

void LauncherApp::EnterShellMode() {
  shellMode_ = true;
  RemoveTrayIcon();
  KillTimer(window_, kTrayRetryTimer);
  UpdateSearchHint();
  HideLauncher();
}

void LauncherApp::ShowTrayMenu(bool atLauncher) {
  HMENU menu = CreatePopupMenu();
  if (menu == nullptr) {
    return;
  }
  AppendMenuW(menu, MF_STRING, kCommandOpen, L"Open Launcher");
  AppendMenuW(menu, MF_STRING, kCommandOpenCatalog, L"Open app list");
  AppendMenuW(menu, MF_STRING, kCommandReloadCatalog, L"Reload app list");
  AppendMenuW(menu, MF_STRING, kCommandAddApplication, L"Add application…");
  AppendMenuW(menu, MF_STRING, kCommandDiagnostics, L"Diagnostics");
  AppendMenuW(menu, MF_STRING, kCommandAppearance, L"Appearance…");
  AppendMenuW(menu, MF_STRING | (showMaintenance_ ? MF_CHECKED : MF_UNCHECKED),
              kCommandMaintenance, L"Show maintenance entries");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  if (!shellMode_) {
    AppendMenuW(menu, MF_STRING | (IsStartWithWindowsEnabled() ? MF_CHECKED : MF_UNCHECKED),
                kCommandStartWithWindows, L"Start with Windows");
  }
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kCommandExit, L"Exit");

  POINT point{};
  GetCursorPos(&point);
  if (atLauncher) {
    RECT bounds{};
    if (GetWindowRect(window_, &bounds)) point = {bounds.left, bounds.bottom};
  }
  SetForegroundWindow(window_);
  blade_appearance_style_menu(menu);
  const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                      point.x, point.y, 0, window_, nullptr);
  DestroyMenu(menu);
  PostMessageW(window_, WM_NULL, 0, 0);
  if (command != 0) {
    HandleTrayCommand(command);
  }
}

void LauncherApp::HandleTrayCommand(UINT command) {
  switch (command) {
    case kCommandOpen:
      ShowLauncher();
      break;
    case kCommandOpenCatalog:
      OpenCatalog();
      break;
    case kCommandAddApplication:
      AddApplication();
      break;
    case kCommandAppearance:
      blade_appearance_edit();
      break;
    case kCommandDiagnostics:
      OpenDiagnostics();
      break;
    case kCommandMaintenance:
      showMaintenance_ = !showMaintenance_;
      RebuildCatalog();
      break;
    case kCommandReloadCatalog:
      ++iconGeneration_;
      iconSources_.clear();
      pendingIconTargets_.clear();
      notifyAfterDiscovery_ = true;
      ReloadCatalog(true);
      UpdateResults();
      if (IsWindowVisible(window_)) {
        ResizeAndPosition(false);
        InvalidateRect(window_, nullptr, FALSE);
      }
      if (!catalogFileError_.empty()) {
        notifyAfterDiscovery_ = false;
        ShowNotification(L"Could not reload app list", catalogError_);
      } else if (!discoveryPending_) {
        notifyAfterDiscovery_ = false;
        if (catalogError_.empty()) {
          ShowNotification(L"Blade Launcher", L"The app list was reloaded.");
        } else {
          ShowNotification(L"Could not reload app list", catalogError_);
        }
      }
      break;
    case kCommandStartWithWindows: {
      std::wstring error;
      const bool enable = !IsStartWithWindowsEnabled();
      if (!SetStartWithWindowsEnabled(enable, error)) {
        ShowNotification(L"Blade Launcher", error);
      }
      break;
    }
    case kCommandExit:
      DestroyWindow(window_);
      break;
    default:
      break;
  }
}

void LauncherApp::ShowNotification(std::wstring_view title, std::wstring_view message) {
  if (!trayIconAdded_) {
    launchError_ = std::wstring(title) + L": " + std::wstring(message);
    // A background error must not reload settings, clear the query, or start
    // another discovery merely because Explorer has no tray to display it.
    if (!IsWindowVisible(window_)) {
      ApplyTheme();
      ResizeAndPosition(true);
      ShowWindow(window_, SW_SHOWNORMAL);
    }
    ResizeAndPosition(false);
    InvalidateRect(window_, nullptr, FALSE);
    return;
  }
  NOTIFYICONDATAW icon{};
  icon.cbSize = sizeof(icon);
  icon.hWnd = window_;
  icon.uID = kTrayIconId;
  icon.uFlags = NIF_INFO;
  wcsncpy_s(icon.szInfoTitle, title.data(), _TRUNCATE);
  wcsncpy_s(icon.szInfo, message.data(), _TRUNCATE);
  icon.dwInfoFlags = NIIF_INFO;
  Shell_NotifyIconW(NIM_MODIFY, &icon);
}

void LauncherApp::ShowLauncher() {
  pendingPowerAction_.reset();
  menuPage_ = MenuPage::Main;
  UpdateSearchHint();
  launchError_.clear();
  anchorMonitor_ = nullptr;
  ReloadCatalog();
  if (configuredCatalog_.discoverInstalled && !discoveryPending_ && !discoveryChangePending_ &&
      refreshPolicy_.NeedsCatchUp(GetTickCount64(), !installedApplicationsError_.empty() ||
          !installedApplicationsWatchError_.empty())) RequestInstalledApplications();
  ApplyTheme();
  SetWindowTextW(edit_, L"");
  UpdateResults();
  ResizeAndPosition(true);
  ShowWindow(window_, SW_SHOWNORMAL);
  SetForegroundWindow(window_);
  SetActiveWindow(window_);
  SetFocus(edit_);
  SendMessageW(edit_, EM_SETSEL, 0, -1);
  InvalidateRect(window_, nullptr, FALSE);
}

void LauncherApp::HideLauncher() {
  pendingPowerAction_.reset();
  if (IsWindowVisible(window_)) {
    ShowWindow(window_, SW_HIDE);
  }
  launchError_.clear();
  ReleaseDeviceResources();
}

void LauncherApp::ToggleLauncher() {
  if (IsWindowVisible(window_)) {
    HideLauncher();
  } else {
    ShowLauncher();
  }
}

void LauncherApp::ReloadCatalog(bool refreshInstalledApplications) {
  std::wstring createError;
  if (!EnsureDefaultCatalog(catalogPath_, createError)) {
    catalogFileError_ = catalogError_ = createError;
    return;
  }

  CatalogResult result = LoadCatalogFile(catalogPath_);
  if (!result) {
    catalogFileError_ = catalogError_ = result.error;
    return;
  }

  const bool enableDiscovery = !configuredCatalog_.discoverInstalled && result.catalog->discoverInstalled;
  configuredCatalog_ = std::move(*result.catalog);
  catalogFileError_.clear();
  hasValidCatalog_ = true;
  UpdateInstalledApplicationsWatcher();
  RebuildCatalog();
  if (configuredCatalog_.discoverInstalled &&
      (refreshInstalledApplications || enableDiscovery || !discoveryRequested_)) {
    if (discoveryPending_) {
      discoveryAgain_ = true;
    } else {
      RequestInstalledApplications();
    }
  }
}

void LauncherApp::RebuildCatalog() {
  // Preserve selection and query if discovery completes while the user is typing.
  std::wstring selectedTarget;
  std::wstring selectedLaunchKey;
  std::optional<std::wstring> selectedIdentity;
  const auto selectedMenuItem = selectedResult_ < searchResults_.size() &&
      searchResults_[selectedResult_].kind == LauncherResult::Kind::Command
      ? std::optional(searchResults_[selectedResult_].command) : std::nullopt;
  if (selectedResult_ < searchResults_.size() && searchResults_[selectedResult_].kind == LauncherResult::Kind::Application &&
      searchResults_[selectedResult_].application < catalog_.applications.size()) {
    const auto& selected = catalog_.applications[searchResults_[selectedResult_].application];
    selectedTarget = selected.target;
    selectedLaunchKey = selected.launchKey;
    selectedIdentity = selected.identity;
  }
  catalog_ = MergeInstalledApplications(configuredCatalog_, installedApplications_, showMaintenance_);
  catalogError_ = catalog_.discoverInstalled ?
      (installedApplicationsError_.empty() ? installedApplicationsWatchError_ : installedApplicationsError_) :
      std::wstring{};
  iconCache_.clear();
  iconAttempted_.clear();
  UpdateResults();
  if (selectedMenuItem) {
    const auto item = std::find_if(searchResults_.begin(), searchResults_.end(), [&selectedMenuItem](const auto& result) {
      return result.kind == LauncherResult::Kind::Command && result.command == *selectedMenuItem;
    });
    if (item != searchResults_.end()) selectedResult_ = static_cast<std::size_t>(item - searchResults_.begin());
  }
  for (std::size_t index = 0; index < searchResults_.size(); ++index) {
    if (searchResults_[index].kind != LauncherResult::Kind::Application) continue;
    const auto& candidate = catalog_.applications[searchResults_[index].application];
    if (candidate.target == selectedTarget || (!selectedLaunchKey.empty() && candidate.launchKey == selectedLaunchKey) ||
        (selectedIdentity && candidate.identity && FoldCase(*selectedIdentity) == FoldCase(*candidate.identity))) {
      selectedResult_ = index;
      EnsureSelectionVisible();
      break;
    }
  }
  EnsureSelectionVisible();
}

void LauncherApp::UpdateInstalledApplicationsWatcher() {
  const std::wstring previousError = installedApplicationsWatchError_;
  if (!hasValidCatalog_ || !configuredCatalog_.discoverInstalled) {
    KillTimer(window_, kInstalledApplicationsChangeTimer);
    discoveryChangePending_ = discoveryAgain_ = false;
    KillTimer(window_, kInstalledApplicationsRetryTimer);
    discoveryRetryPending_ = false;
    refreshPolicy_.Reset();
    installedApplicationsWatchError_.clear();
    if (!installedAppsWatcher_->Stop()) {
      installedApplicationsWatchError_ = L"Could not stop app-change notifications. Restart Launcher.";
    }
  } else {
    installedAppsWatcher_->Start(window_, kMessageInstalledApplicationsChanged, installedApplicationsWatchError_);
  }
  if (!installedApplicationsWatchError_.empty() && installedApplicationsWatchError_ != previousError) {
    ShowNotification(L"Blade Launcher", installedApplicationsWatchError_);
  }
}

void LauncherApp::ScheduleInstalledApplicationsRefresh() {
  if (!hasValidCatalog_ || !configuredCatalog_.discoverInstalled) return;
  discoveryChangePending_ = true;
  // A one-shot debounce after an event, never a periodic app-list poll.
  KillTimer(window_, kInstalledApplicationsRetryTimer);
  discoveryRetryPending_ = false;
  const auto delay = refreshPolicy_.Changed(GetTickCount64());
  if (SetTimer(window_, kInstalledApplicationsChangeTimer, static_cast<UINT>(delay), nullptr) == 0) {
    installedApplicationsWatchError_ = L"Could not delay an app-list refresh. Restart Launcher.";
    ShowNotification(L"Blade Launcher", installedApplicationsWatchError_);
    RefreshChangedInstalledApplications();
  }
}

void LauncherApp::HandleInstalledApplicationsDirectoryChange(std::size_t index) {
  std::wstring error;
  const bool changed = installedAppsWatcher_->ConsumeDirectoryChange(index, error);
  if (!error.empty()) {
    installedApplicationsWatchError_ = error;
    if (catalogFileError_.empty()) RebuildCatalog();
    ShowNotification(L"Blade Launcher", error);
  }
  if (changed || !error.empty()) ScheduleInstalledApplicationsRefresh();
}

void LauncherApp::RefreshChangedInstalledApplications() {
  KillTimer(window_, kInstalledApplicationsChangeTimer);
  if (!std::exchange(discoveryChangePending_, false) || !configuredCatalog_.discoverInstalled) return;
  if (discoveryPending_) discoveryAgain_ = true;
  else RequestInstalledApplications();
}

void LauncherApp::RequestInstalledApplications() {
  KillTimer(window_, kInstalledApplicationsRetryTimer);
  discoveryRetryPending_ = false;
  refreshPolicy_.Begin();
  discoveryRequested_ = true;
  discoveryPending_ = discoveryTasks_->Submit(
      [this, cancellation = discoveryCancellation_.get_token()]() -> BackgroundTasks::Completion {
    InstalledAppsResult installed = DiscoverInstalledApplicationsIsolated(cancellation);
    // Only the completion dereferences this. Stop discards it before HWND teardown.
    return [this, installed = std::move(installed)]() mutable {
      ApplyInstalledApplications(std::move(installed));
    };
  });
  if (!discoveryPending_) {
    installedApplicationsError_ = L"Could not start Windows app discovery. Try Reload app list.";
    if (catalogFileError_.empty()) RebuildCatalog();
  } else {
    StartBackgroundTimer();
  }
}

void LauncherApp::ApplyInstalledApplications(InstalledAppsResult installed) {
  discoveryPending_ = false;
  const std::wstring previousError = installedApplicationsError_;
  bool changed = false;
  sourceStatus_ = installed.sources;
  if (installed) {
    auto reconciled = ReconcileDiscoverySources(installedApplications_, installed);
    changed = reconciled.size() != installedApplications_.size() ||
        !std::equal(reconciled.begin(), reconciled.end(), installedApplications_.begin(),
            [](const ApplicationEntry& left, const ApplicationEntry& right) {
              return left.name == right.name && left.target == right.target && left.identity == right.identity &&
                     left.source == right.source && left.resolvedTarget == right.resolvedTarget &&
                     left.shortcutArguments == right.shortcutArguments &&
                     left.shortcutWorkingDirectory == right.shortcutWorkingDirectory && left.launchKey == right.launchKey;
            });
    installedApplications_ = std::move(reconciled);
    installedApplicationsError_.clear();
    for (const auto& source : sourceStatus_) if (!source.succeeded) {
      if (!installedApplicationsError_.empty()) installedApplicationsError_ += L" ";
      installedApplicationsError_ += std::wstring(ApplicationSourceName(source.source)) +
          L" is stale: " + source.error;
    }
    if (installedApplicationsError_.empty()) lastRefreshTick_ = GetTickCount64();
  } else {
    installedApplicationsError_ = std::move(installed.error);
  }
  const auto retry = refreshPolicy_.Completed(GetTickCount64(), installedApplicationsError_.empty());
  if (catalogFileError_.empty() && (changed || previousError != installedApplicationsError_)) {
    RebuildCatalog();
  }
  const bool refreshAgain = std::exchange(discoveryAgain_, false);
  if (refreshAgain && configuredCatalog_.discoverInstalled) {
    RequestInstalledApplications();
  } else if (notifyAfterDiscovery_) {
    notifyAfterDiscovery_ = false;
    ShowNotification(L"Blade Launcher",
                     catalogError_.empty() ? L"The app list was reloaded." : catalogError_);
  } else if (!installedApplicationsError_.empty() && installedApplicationsError_ != previousError &&
             configuredCatalog_.discoverInstalled) {
    ShowNotification(L"Could not discover installed apps", installedApplicationsError_);
  }
  if (!refreshAgain && retry && configuredCatalog_.discoverInstalled) {
    discoveryRetryPending_ = SetTimer(window_, kInstalledApplicationsRetryTimer,
        static_cast<UINT>(*retry), nullptr) != 0;
    if (!discoveryRetryPending_) {
      installedApplicationsWatchError_ = L"Could not schedule a refresh retry. Use Reload app list.";
      if (catalogFileError_.empty()) RebuildCatalog();
    }
  }
}

void LauncherApp::StartBackgroundTimer() {
  if (SetTimer(window_, kBackgroundTimer, 100, nullptr) == 0) {
    launchError_ = L"Could not monitor background work. Try reopening Launcher.";
  }
}

void LauncherApp::ProcessBackgroundResults() {
  discoveryTasks_->Dispatch();
  const std::size_t completedIcons = iconTasks_->Dispatch();
  if (completedIcons != 0 && IsWindowVisible(window_)) {
    // Even discarded old-generation results free queue slots. Retry any icons
    // that could not be queued during a reload because the queue was full.
    InvalidateRect(window_, nullptr, FALSE);
  }
  if (discoveryTasks_->TakeFailure()) {
    ApplyInstalledApplications({{}, L"Windows app discovery failed. Try Reload app list."});
  }
  if (iconTasks_->TakeFailure()) {
    pendingIconTargets_.clear();
    iconCache_.clear();
    iconAttempted_.clear();
    launchError_ = L"Some icons could not be loaded. Try Reload app list.";
    ShowNotification(L"Blade Launcher", launchError_);
    if (IsWindowVisible(window_)) {
      ResizeAndPosition(false);
      InvalidateRect(window_, nullptr, FALSE);
    }
  }
  if (!discoveryTasks_->HasPending() && !iconTasks_->HasPending()) {
    KillTimer(window_, kBackgroundTimer);
  }
}

void LauncherApp::StopBackgroundTasks() {
  discoveryChangePending_ = false;
  if (installedAppsWatcher_ && !installedAppsWatcher_->Stop()) {
    OutputDebugStringW(L"Launcher: could not stop app-change notifications during shutdown.\n");
  }
  discoveryCancellation_.request_stop();
  if (iconTasks_) iconTasks_->Stop();
  if (discoveryTasks_) discoveryTasks_->Stop();
  if (window_) {
    KillTimer(window_, kBackgroundTimer);
    KillTimer(window_, kTrayRetryTimer);
    KillTimer(window_, kInstalledApplicationsChangeTimer);
    KillTimer(window_, kInstalledApplicationsRetryTimer);
    discoveryRetryPending_ = false;
  }
}

void LauncherApp::UpdateResults() {
  std::wstring query;
  const int length = GetWindowTextLengthW(edit_);
  if (length > 0) {
    query.resize(static_cast<std::size_t>(length) + 1);
    GetWindowTextW(edit_, query.data(), length + 1);
    query.resize(static_cast<std::size_t>(length));
  }
  queryHasText_ = std::any_of(query.begin(), query.end(), [](wchar_t c) { return !std::iswspace(c); });
  results_ = menuPage_ == MenuPage::Main && hasValidCatalog_
      ? RankApplications(catalog_, query) : std::vector<std::size_t>{};
  menuItems_ = MenuItems(menuPage_, query);
  searchResults_ = RankLauncherResults(hasValidCatalog_ ? catalog_ : Catalog{}, menuPage_, query);
  selectedResult_ = 0;
  firstVisibleResult_ = 0;
  wheelRemainder_ = 0;
  if (IsWindowVisible(window_)) {
    ResizeAndPosition(false);
  }
  InvalidateRect(window_, nullptr, FALSE);
}

void LauncherApp::MoveSelection(int delta) {
  if (ResultCount() == 0) return;
  const int count = static_cast<int>(ResultCount());
  int selected = static_cast<int>(selectedResult_);
  selected = (selected + delta + count) % count;
  selectedResult_ = static_cast<std::size_t>(selected);
  EnsureSelectionVisible();
  InvalidateRect(window_, nullptr, FALSE);
}

void LauncherApp::UpdateSearchHint() {
  const wchar_t* hint = menuPage_ == MenuPage::System ? L"Search system actions" :
      L"Search applications";
  SendMessageW(edit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(hint));
}

bool LauncherApp::IsMenuView() const {
  return menuPage_ != MenuPage::Main || !queryHasText_;
}

std::size_t LauncherApp::ResultCount() const {
  return searchResults_.size();
}

float LauncherApp::ResultsTop() const {
  return visuals::kSearchHeight + (menuPage_ == MenuPage::Main ? 0.0f : visuals::kMenuHeaderHeight);
}

void LauncherApp::GoBack() {
  pendingPowerAction_.reset();
  if (menuPage_ == MenuPage::Main) {
    HideLauncher();
    return;
  }
  menuPage_ = MenuPage::Main;
  launchError_.clear();
  UpdateSearchHint();
  SetWindowTextW(edit_, L"");
  UpdateResults();
  SetFocus(edit_);
}

std::size_t LauncherApp::VisibleResultRows() const {
  return std::clamp<std::size_t>(ResultCount(), 1, kMaximumVisibleResults);
}

void LauncherApp::EnsureSelectionVisible() {
  if (ResultCount() == 0) {
    firstVisibleResult_ = 0;
    return;
  }
  if (selectedResult_ < firstVisibleResult_) firstVisibleResult_ = selectedResult_;
  else if (selectedResult_ >= firstVisibleResult_ + kMaximumVisibleResults)
    firstVisibleResult_ = selectedResult_ - kMaximumVisibleResults + 1;
  const auto maximum = ResultCount() > kMaximumVisibleResults ? ResultCount() - kMaximumVisibleResults : 0;
  firstVisibleResult_ = std::min(firstVisibleResult_, maximum);
}

void LauncherApp::ScrollResults(int wheelDelta) {
  wheelRemainder_ += wheelDelta;
  const int notches = wheelRemainder_ / WHEEL_DELTA;
  wheelRemainder_ %= WHEEL_DELTA;
  if (!notches || ResultCount() <= kMaximumVisibleResults) return;
  UINT lines = 3;
  SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
  if (lines == WHEEL_PAGESCROLL) lines = static_cast<UINT>(kMaximumVisibleResults);
  const auto maximum = static_cast<long long>(ResultCount() - kMaximumVisibleResults);
  const auto next = static_cast<long long>(firstVisibleResult_) - static_cast<long long>(notches) * lines;
  firstVisibleResult_ = static_cast<std::size_t>(std::clamp(next, 0LL, maximum));
  selectedResult_ = std::clamp(selectedResult_, firstVisibleResult_, firstVisibleResult_ + kMaximumVisibleResults - 1);
  InvalidateRect(window_, nullptr, FALSE);
}

std::optional<std::size_t> LauncherApp::ResultAtY(float y) const {
  if (y < ResultsTop()) return std::nullopt;
  const auto row = static_cast<std::size_t>((y - ResultsTop()) / visuals::kResultHeight);
  const auto result = firstVisibleResult_ + row;
  if (row >= kMaximumVisibleResults || result >= ResultCount()) return std::nullopt;
  return result;
}

void LauncherApp::LaunchSelection() {
  if (pendingLaunch_ || pendingPowerAction_ || powerActionActive_) return;
  if (selectedResult_ >= searchResults_.size()) return;
  const auto selected = searchResults_[selectedResult_];
  if (selected.kind == LauncherResult::Kind::Command) {
    const MenuItemId item = selected.command;
    if (item == MenuItemId::System) {
      menuPage_ = MenuPage::System;
      launchError_.clear();
      UpdateSearchHint();
      UpdateResults();
      SetFocus(edit_);
    } else if(item==MenuItemId::Appearance) {
      blade_appearance_edit();
    } else {
      pendingPowerAction_ = item;
      if (PostMessageW(window_, kMessageSystemPowerAction, 0, 0)) return;
      pendingPowerAction_.reset();
      launchError_ = L"Could not queue power action: " + WindowsErrorMessage(GetLastError());
      ResizeAndPosition(false);
      InvalidateRect(window_, nullptr, FALSE);
    }
    return;
  }
  const std::size_t applicationIndex = selected.application;
  if (applicationIndex >= catalog_.applications.size()) {
    return;
  }

  if (pendingLaunch_) return;
  pendingLaunch_ = catalog_.applications[applicationIndex];
  // A sent input message cannot make an outgoing COM activation call. Return
  // from that message first, then activate the captured entry on the main STA.
  if (PostMessageW(window_, kMessageLaunchSelection, 0, 0)) return;
  launchError_ = L"Could not queue launch of " + pendingLaunch_->name + L": " + WindowsErrorMessage(GetLastError());
  pendingLaunch_.reset();
  ResizeAndPosition(false);
  InvalidateRect(window_, nullptr, FALSE);
}

void LauncherApp::ExecutePendingPowerAction() {
  if (!pendingPowerAction_ || powerActionActive_) return;
  const MenuItemId action = *pendingPowerAction_;
  powerActionActive_ = true;
  std::wstring error;
  bool requested = false;
  // Dismissal, navigation, or destruction before dispatch cancels the request.
  if (pendingPowerAction_ == action && IsWindow(window_) && IsWindowVisible(window_))
    requested = requestPowerAction_(action, error);
  pendingPowerAction_.reset();
  powerActionActive_ = false;
  if (!IsWindow(window_)) return;
  if (requested && error.empty()) {
    HideLauncher();
  } else {
    launchError_ = std::move(error);
    if (IsWindowVisible(window_)) ResizeAndPosition(false);
    SetFocus(edit_);
    InvalidateRect(window_, nullptr, FALSE);
  }
}

void LauncherApp::ExecutePendingLaunch() {
  if (!pendingLaunch_) return;
  const ApplicationEntry application = std::move(*pendingLaunch_);
  std::wstring error;
  const bool launched = LaunchApplication(application, error);
  pendingLaunch_.reset();
  if (launched) {
    HideLauncher();
    return;
  }
  launchError_ = std::move(error);
  ResizeAndPosition(false);
  InvalidateRect(window_, nullptr, FALSE);
}

bool LauncherApp::LaunchApplication(const ApplicationEntry& application, std::wstring& error) {
  if (TryActivateRunningUgs(application)) return true;
  std::wstring file = application.target;
  std::wstring parameters = JoinArguments(application.arguments);
  constexpr std::wstring_view appsPrefix = L"shell:AppsFolder\\";
  if (StartsWithCaseInsensitive(file, appsPrefix)) {
    const std::wstring appId = file.substr(appsPrefix.size());
    // Packaged apps must bypass the Apps-folder verb handler, which can depend
    // on Explorer's desktop broker even though ShellExecute does not start it.
    if (VerifyApplicationUserModelId(appId.c_str()) == ERROR_SUCCESS) {
      ComPtr<IApplicationActivationManager> activation;
      HRESULT result = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(activation.ReleaseAndGetAddressOf()));
      if (SUCCEEDED(result)) {
        DWORD processId = 0;
        result = activation->ActivateApplication(appId.c_str(), parameters.empty() ? nullptr : parameters.c_str(),
                                                 AO_NOERRORUI, &processId);
      }
      if (FAILED(result)) {
        error = L"Could not activate " + application.name + L": " + winrt::hresult_error(result).message().c_str();
        return false;
      }
      return true;
    }
  }
  PIDLIST_ABSOLUTE item = nullptr;
  const auto freeItem = [](ITEMIDLIST* value) { CoTaskMemFree(value); };
  std::unique_ptr<ITEMIDLIST, decltype(freeItem)> itemOwner(nullptr, freeItem);
  if (StartsWithCaseInsensitive(application.target, L"shell:")) {
    const HRESULT parsed = SHParseDisplayName(file.c_str(), nullptr, &item, 0, nullptr);
    itemOwner.reset(reinterpret_cast<ITEMIDLIST*>(item));
    if (FAILED(parsed)) {
      error = L"Could not resolve " + application.name + L": " + winrt::hresult_error(parsed).message().c_str();
      return false;
    }
  }

  SHELLEXECUTEINFOW execute{};
  execute.cbSize = sizeof(execute);
  execute.fMask = SEE_MASK_FLAG_NO_UI | (item ? SEE_MASK_INVOKEIDLIST : 0);
  execute.lpIDList = item;
  execute.hwnd = window_;
  execute.lpVerb = L"open";
  execute.lpFile = item ? nullptr : file.c_str();
  execute.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
  execute.lpDirectory = application.workingDirectory ? application.workingDirectory->c_str() : nullptr;
  execute.nShow = SW_SHOWNORMAL;
  if (!ShellExecuteExW(&execute)) {
    error = L"Could not launch " + application.name + L": " + WindowsErrorMessage(GetLastError());
    return false;
  }
  if (reinterpret_cast<INT_PTR>(execute.hInstApp) <= 32) {
    error = L"Could not launch " + application.name;
    return false;
  }
  return true;
}

void LauncherApp::OpenCatalog() {
  std::wstring error;
  if (!EnsureDefaultCatalog(catalogPath_, error)) {
    ShowNotification(L"Blade Launcher", error);
    return;
  }
  if (reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", catalogPath_.c_str(), nullptr,
                                              catalogPath_.parent_path().c_str(), SW_SHOWNORMAL)) <= 32) {
    ShowNotification(L"Blade Launcher", L"Could not open the app list.");
  }
}

void LauncherApp::AddApplication() {
  if (IsPackagedProcess()) {
    ShowNotification(L"Add application", L"Run Register-LauncherApp.ps1 from your Windows account to verify the catalog.");
    return;
  }
  ComPtr<IFileOpenDialog> dialog;
  const auto fail = [this](HRESULT result) {
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return;
    ShowNotification(L"Could not add application", winrt::hresult_error(result).message().c_str());
  };
  HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
      IID_PPV_ARGS(dialog.ReleaseAndGetAddressOf()));
  if (FAILED(result)) { fail(result); return; }
  result = dialog->SetTitle(L"Add application to Launcher");
  if (FAILED(result)) { fail(result); return; }
  const COMDLG_FILTERSPEC filters[] = {{L"Applications and shortcuts", L"*.exe;*.lnk;*.url;*.appref-ms"},
                                      {L"All files", L"*.*"}};
  result = dialog->SetFileTypes(2, filters);
  if (SUCCEEDED(result)) result = dialog->SetOptions(FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_NODEREFERENCELINKS);
  if (SUCCEEDED(result)) result = dialog->Show(window_);
  if (FAILED(result)) { fail(result); return; }
  ComPtr<IShellItem> item;
  PWSTR selected = nullptr;
  result = dialog->GetResult(item.ReleaseAndGetAddressOf());
  if (SUCCEEDED(result)) result = item->GetDisplayName(SIGDN_FILESYSPATH, &selected);
  if (FAILED(result)) { fail(result); return; }
  const std::filesystem::path target(selected);
  CoTaskMemFree(selected);
  using namespace winrt::Windows::Data::Json;
  JsonObject entry;
  entry.SetNamedValue(L"id", JsonValue::CreateStringValue(L"manual:" + FoldCase(target.wstring())));
  entry.SetNamedValue(L"name", JsonValue::CreateStringValue(target.stem().wstring()));
  entry.SetNamedValue(L"target", JsonValue::CreateStringValue(target.wstring()));
  const auto registration = RegisterApplicationJson(catalogPath_, entry.Stringify().c_str());
  if (!registration.success) { ShowNotification(L"Could not add application", registration.error); return; }
  ShowLauncher();
  SetWindowTextW(edit_, target.stem().c_str());
  UpdateResults();
}

std::wstring LauncherApp::DiagnosticReport(std::wstring_view lookup) const {
  using namespace winrt::Windows::Data::Json;
  JsonObject root;
  const auto string = [](std::wstring_view value) { return JsonValue::CreateStringValue(value); };
  root.SetNamedValue(L"success", JsonValue::CreateBooleanValue(true));
  root.SetNamedValue(L"product", string(L"Blade Launcher"));
  for(int attempt=0;attempt<5;++attempt) {
    std::string bytes(blade_appearance_json(nullptr,0),'\0');
    if(bytes.size()>512*1024) break;
    const auto actual=blade_appearance_json(bytes.data(),bytes.size());
    if(!actual || actual>bytes.size()) continue;
    bytes.resize(actual-1);JsonObject appearance;
    if(JsonObject::TryParse(winrt::to_hstring(bytes),appearance)) {
      appearance.Remove(L"themeData");root.SetNamedValue(L"appearance",appearance);
    }
    break;
  }
  root.SetNamedValue(L"version", JsonValue::CreateStringValue(winrt::to_hstring(BLADE_LAUNCHER_VERSION)));
  wchar_t executable[32768]{};
  GetModuleFileNameW(nullptr, executable, 32768);
  root.SetNamedValue(L"executable", string(executable));
  root.SetNamedValue(L"processId", JsonValue::CreateNumberValue(GetCurrentProcessId()));
  root.SetNamedValue(L"visible", JsonValue::CreateBooleanValue(IsWindowVisible(window_) != FALSE));
  const auto queryLength = GetWindowTextLengthW(edit_);
  std::wstring query(static_cast<std::size_t>(queryLength) + 1, L'\0');
  GetWindowTextW(edit_, query.data(), queryLength + 1);
  query.resize(static_cast<std::size_t>(queryLength));
  root.SetNamedValue(L"query", string(query));
  root.SetNamedValue(L"selectedResult", JsonValue::CreateNumberValue(static_cast<double>(selectedResult_)));
  root.SetNamedValue(L"catalogPath", string(catalogPath_.wstring()));
  root.SetNamedValue(L"account", string(CurrentAccountSid()));
  root.SetNamedValue(L"discoverInstalled", JsonValue::CreateBooleanValue(configuredCatalog_.discoverInstalled));
  root.SetNamedValue(L"catalogError", string(catalogFileError_));
  root.SetNamedValue(L"discoveryError", string(installedApplicationsError_));
  root.SetNamedValue(L"watchError", string(installedApplicationsWatchError_));
  root.SetNamedValue(L"discoveryPending", JsonValue::CreateBooleanValue(discoveryPending_));
  root.SetNamedValue(L"refreshScheduled", JsonValue::CreateBooleanValue(discoveryChangePending_ || discoveryRetryPending_ || discoveryAgain_));
  root.SetNamedValue(L"lastHealthyRefreshAgeMs", lastRefreshTick_ ?
      JsonValue::CreateNumberValue(static_cast<double>(GetTickCount64() - lastRefreshTick_)) : JsonValue::CreateNullValue());
  JsonArray sources;
  for (const auto& source : sourceStatus_) {
    JsonObject item;
    item.SetNamedValue(L"name", string(ApplicationSourceName(source.source)));
    item.SetNamedValue(L"healthy", JsonValue::CreateBooleanValue(source.succeeded));
    item.SetNamedValue(L"skippedEntries", JsonValue::CreateNumberValue(source.skipped));
    item.SetNamedValue(L"error", string(source.error));
    sources.Append(item);
  }
  root.SetNamedValue(L"sources", sources);
  const auto folded = FoldCase(lookup);
  JsonArray entries;
  const auto add = [&](const ApplicationEntry& app) {
    if (!folded.empty() && FoldCase(app.name + L" " + app.target + L" " + app.identity.value_or(L"")).find(folded) == std::wstring::npos &&
        std::none_of(app.aliases.begin(), app.aliases.end(), [&](const auto& alias) { return FoldCase(alias).find(folded) != std::wstring::npos; })) return;
    JsonObject item;
    item.SetNamedValue(L"name", string(app.name));
    item.SetNamedValue(L"target", string(app.target));
    item.SetNamedValue(L"id", string(app.identity.value_or(L"")));
    item.SetNamedValue(L"source", string(ApplicationSourceName(app.source)));
    item.SetNamedValue(L"resolvedTarget", string(app.resolvedTarget));
    item.SetNamedValue(L"shortcutArguments", string(app.shortcutArguments));
    item.SetNamedValue(L"shortcutWorkingDirectory", string(app.shortcutWorkingDirectory));
    JsonArray args;
    for (const auto& argument : app.arguments) args.Append(string(argument));
    item.SetNamedValue(L"arguments", args);
    item.SetNamedValue(L"workingDirectory", string(app.workingDirectory.value_or(L"")));
    item.SetNamedValue(L"icon", string(app.icon.value_or(L"")));
    std::wstring reason;
    if (app.source == ApplicationSource::Manual) reason = L"Included: manual registration";
    else if (!configuredCatalog_.discoverInstalled) reason = L"Excluded: discovery disabled";
    else if (MatchesApplication(app, configuredCatalog_.hiddenApplications)) reason = L"Excluded: hiddenApplications";
    else if (!showMaintenance_ && IsMaintenanceApplication(app) && !MatchesApplication(app, configuredCatalog_.includedApplications)) reason = L"Excluded: maintenance entry";
    else if (std::none_of(catalog_.applications.begin(), catalog_.applications.end(), [&](const auto& visible) { return visible.target == app.target; })) reason = L"Excluded: duplicate launch identity";
    else reason = L"Included";
    item.SetNamedValue(L"reason", string(reason));
    entries.Append(item);
  };
  for (const auto& app : configuredCatalog_.applications) add(app);
  for (const auto& app : installedApplications_) add(app);
  root.SetNamedValue(L"entries", entries);
  root.SetNamedValue(L"lookup", string(lookup));
  JsonArray results;
  for (const auto& result : searchResults_) {
    JsonObject item;
    const bool command = result.kind == LauncherResult::Kind::Command;
    item.SetNamedValue(L"kind", string(command ? L"command" : L"application"));
    item.SetNamedValue(L"label", string(command ? MenuItemLabel(result.command) : catalog_.applications[result.application].name));
    results.Append(item);
  }
  root.SetNamedValue(L"results", results);
  return root.Stringify().c_str();
}

void LauncherApp::OpenDiagnostics() {
  wchar_t temporary[MAX_PATH]{};
  if (!GetTempPathW(MAX_PATH, temporary)) return;
  const auto path = std::filesystem::path(temporary) / (L"Blade-Launcher-diagnostics-" + std::to_wstring(GetCurrentProcessId()) + L".json");
  const auto report = winrt::to_string(winrt::hstring(DiagnosticReport()));
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(report.data(), static_cast<std::streamsize>(report.size()));
  file.close();
  ShellExecuteW(window_, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

HRESULT LauncherApp::EnsureRenderTarget() {
  if (renderTarget_) {
    return S_OK;
  }
  RECT client{};
  GetClientRect(window_, &client);
  const D2D1_SIZE_U size = D2D1::SizeU(
      static_cast<UINT32>(std::max(1L, client.right - client.left)),
      static_cast<UINT32>(std::max(1L, client.bottom - client.top)));
  const HRESULT result = d2dFactory_->CreateHwndRenderTarget(
      D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE),
      D2D1::HwndRenderTargetProperties(window_, size),
      renderTarget_.ReleaseAndGetAddressOf());
  if (SUCCEEDED(result)) {
    renderTarget_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
  }
  return result;
}

void LauncherApp::ReleaseDeviceResources() {
  iconCache_.clear();
  iconAttempted_.clear();
  renderTarget_.Reset();
}

void LauncherApp::Render() {
  PAINTSTRUCT paint{};
  BeginPaint(window_, &paint);
  if (FAILED(EnsureRenderTarget())) {
    EndPaint(window_, &paint);
    return;
  }

  renderTarget_->BeginDraw();
  DrawContent(renderTarget_.Get(),true);

  const HRESULT result = renderTarget_->EndDraw();
  if (result == D2DERR_RECREATE_TARGET) {
    ReleaseDeviceResources();
  } else if (SUCCEEDED(result) && benchmarkPresentedEvent_ != nullptr) {
    SetEvent(benchmarkPresentedEvent_);
  }
  EndPaint(window_, &paint);
}

// The HWND and bounded software capture use the same drawing path.
void LauncherApp::DrawContent(ID2D1RenderTarget* target, bool applicationIcons) {
  const visuals::Palette palette = visuals::GetPalette(lightTheme_);

  ComPtr<ID2D1SolidColorBrush> backgroundBrush;
  ComPtr<ID2D1SolidColorBrush> textBrush;
  ComPtr<ID2D1SolidColorBrush> mutedBrush;
  ComPtr<ID2D1SolidColorBrush> selectedBrush;
  ComPtr<ID2D1SolidColorBrush> separatorBrush;
  ComPtr<ID2D1SolidColorBrush> errorBrush;
  target->CreateSolidColorBrush(palette.background, &backgroundBrush);
  target->CreateSolidColorBrush(palette.text, &textBrush);
  target->CreateSolidColorBrush(palette.muted, &mutedBrush);
  target->CreateSolidColorBrush(palette.selected, &selectedBrush);
  target->CreateSolidColorBrush(palette.separator, &separatorBrush);
  target->CreateSolidColorBrush(palette.error, &errorBrush);

  target->Clear(palette.background);

  const float searchCenter = visuals::kSearchHeight / 2.0f;
  target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(25.0f, searchCenter - 2.0f), 6.0f, 6.0f), mutedBrush.Get(), 1.5f);
  target->DrawLine(D2D1::Point2F(29.0f, searchCenter + 2.0f), D2D1::Point2F(34.0f, searchCenter + 7.0f), mutedBrush.Get(), 1.5f);
  const float escapeLeft = visuals::kWindowWidth - 46.0f;
  target->DrawRoundedRectangle(D2D1::RoundedRect(
      D2D1::RectF(escapeLeft, searchCenter - 10.0f, visuals::kWindowWidth - 18.0f, searchCenter + 10.0f),
      4.0f, 4.0f), separatorBrush.Get(), 1.0f);
  target->DrawTextW(L"esc", 3, hintTextFormat_.Get(),
      D2D1::RectF(escapeLeft + 6.0f, searchCenter - 10.0f, visuals::kWindowWidth - 20.0f, searchCenter + 10.0f),
      mutedBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
  target->DrawLine(D2D1::Point2F(16.0f, visuals::kSearchHeight),
                          D2D1::Point2F(visuals::kWindowWidth - 16.0f, visuals::kSearchHeight),
                          separatorBrush.Get(), 1.0f);

  if (menuPage_ != MenuPage::Main) {
    const float center = visuals::kSearchHeight + visuals::kMenuHeaderHeight / 2.0f;
    target->DrawLine(D2D1::Point2F(24.0f, center), D2D1::Point2F(39.0f, center), textBrush.Get(), 1.75f);
    target->DrawLine(D2D1::Point2F(24.0f, center), D2D1::Point2F(30.0f, center - 6.0f), textBrush.Get(), 1.75f);
    target->DrawLine(D2D1::Point2F(24.0f, center), D2D1::Point2F(30.0f, center + 6.0f), textBrush.Get(), 1.75f);
    constexpr std::wstring_view title = L"System";
    target->DrawTextW(title.data(), static_cast<UINT32>(title.size()), resultTextFormat_.Get(),
        D2D1::RectF(visuals::kResultTextLeft, visuals::kSearchHeight,
            visuals::kWindowWidth - visuals::kContentRight, ResultsTop()), mutedBrush.Get());
  }

  if (searchResults_.empty()) {
    const std::wstring message = !hasValidCatalog_ && menuPage_ == MenuPage::Main
        ? L"App list unavailable — F10 opens the menu"
        : L"No matches — F10: Reload app list or Add application";
    target->DrawTextW(message.c_str(), static_cast<UINT32>(message.size()), messageTextFormat_.Get(),
        D2D1::RectF(visuals::kContentRight, ResultsTop(), visuals::kWindowWidth-visuals::kContentRight,
            ResultsTop()+visuals::kResultHeight), mutedBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
  } else {
    for (std::size_t index = firstVisibleResult_; index < std::min(searchResults_.size(), firstVisibleResult_+kMaximumVisibleResults); ++index) {
      const float top = ResultsTop()+static_cast<float>(index-firstVisibleResult_)*visuals::kResultHeight;
      if (index == selectedResult_) {
        target->FillRoundedRectangle(D2D1::RoundedRect(
          D2D1::RectF(visuals::kSelectionHorizontalInset,top+visuals::kSelectionVerticalInset,
              visuals::kWindowWidth-visuals::kSelectionHorizontalInset,top+visuals::kResultHeight-visuals::kSelectionVerticalInset),
          visuals::kSelectionCornerRadius,visuals::kSelectionCornerRadius),selectedBrush.Get());
        target->FillRoundedRectangle(D2D1::RoundedRect(
            D2D1::RectF(visuals::kSelectionHorizontalInset, top + 14.0f,
                visuals::kSelectionHorizontalInset + 2.0f, top + visuals::kResultHeight - 14.0f),
            1.0f, 1.0f), textBrush.Get());
      }
      textBrush->SetColor(index==selectedResult_ ? palette.selectedText : palette.text);
      mutedBrush->SetColor(index==selectedResult_ ? palette.selectedText : palette.muted);
      const auto result = searchResults_[index];
      std::wstring_view label;
      const float center = top+visuals::kResultHeight/2;
      if (result.kind == LauncherResult::Kind::Command) {
        label = MenuItemLabel(result.command);
        DrawSystemIcon(*d2dFactory_.Get(),*target,result.command,D2D1::Point2F(28,center),*mutedBrush.Get(),dpi_);
        const bool navigation = result.command == MenuItemId::System || result.command == MenuItemId::Appearance;
        const std::wstring_view badge = navigation ? L"›" : L"System action";
        target->DrawTextW(badge.data(),static_cast<UINT32>(badge.size()),messageTextFormat_.Get(),
            D2D1::RectF(visuals::kWindowWidth-(navigation ? 26.0f : 110.0f),top,
                visuals::kWindowWidth-visuals::kContentRight,top+visuals::kResultHeight),
            mutedBrush.Get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
      } else {
        if (result.application >= catalog_.applications.size()) continue;
        label = catalog_.applications[result.application].name;
        const auto icon = applicationIcons ? GetApplicationIcon(result.application) : ComPtr<ID2D1Bitmap>{};
        const float iconTop = top+(visuals::kResultHeight-visuals::kApplicationIconSize)/2;
        const auto bounds = D2D1::RectF(visuals::kApplicationIconLeft,iconTop,
            visuals::kApplicationIconLeft+visuals::kApplicationIconSize,iconTop+visuals::kApplicationIconSize);
        if (icon) {
          const auto pixels = icon->GetPixelSize();
          const float slot = static_cast<float>(ScaleForDpiInt(visuals::kApplicationIconSize,dpi_));
          const float scale = slot/static_cast<float>(std::max(pixels.width,pixels.height));
          const float width = std::max(1.0f,std::round(pixels.width*scale));
          const float height = std::max(1.0f,std::round(pixels.height*scale));
          const float left = std::round(ScaleForDpi(bounds.left,dpi_)+(slot-width)/2);
          const float topPixel = std::round(ScaleForDpi(bounds.top,dpi_)+(slot-height)/2);
          const float dipPerPixel = 96.0f/static_cast<float>(dpi_);
          target->DrawBitmap(icon.Get(),D2D1::RectF(left*dipPerPixel,topPixel*dipPerPixel,
              (left+width)*dipPerPixel,(topPixel+height)*dipPerPixel),1,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else target->DrawRoundedRectangle(D2D1::RoundedRect(bounds,
            visuals::kSelectionCornerRadius,visuals::kSelectionCornerRadius),mutedBrush.Get(),1.5f);
      }
      target->DrawTextW(label.data(),static_cast<UINT32>(label.size()),resultTextFormat_.Get(),
          D2D1::RectF(visuals::kResultTextLeft,top,
              visuals::kWindowWidth-(result.kind == LauncherResult::Kind::Command &&
                  result.command != MenuItemId::System && result.command != MenuItemId::Appearance ? 120.0f : 40.0f),
              top+visuals::kResultHeight),
          textBrush.Get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
  }

  const std::wstring& status = !launchError_.empty() ? launchError_ : catalogError_;
  if (!status.empty()) {
    const float top = ResultsTop() +
                      static_cast<float>(VisibleResultRows()) * visuals::kResultHeight;
    const D2D1_RECT_F bounds = D2D1::RectF(
        visuals::kContentRight, top, visuals::kWindowWidth - visuals::kContentRight,
        top + visuals::kStatusHeight);
    target->DrawTextW(status.c_str(), static_cast<UINT32>(status.size()), messageTextFormat_.Get(), bounds,
                             errorBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }
  const float footerTop = ResultsTop() + static_cast<float>(VisibleResultRows()) * visuals::kResultHeight +
      (status.empty() ? 0.0f : visuals::kStatusHeight);
  constexpr std::wstring_view hints = L"↑↓ navigate    ↵ open";
  target->DrawTextW(hints.data(), static_cast<UINT32>(hints.size()), hintTextFormat_.Get(),
      D2D1::RectF(18.0f, footerTop, visuals::kWindowWidth - 64.0f, footerTop + visuals::kFooterHeight),
      mutedBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
  target->DrawTextW(L"Blade", 5, hintTextFormat_.Get(),
      D2D1::RectF(visuals::kWindowWidth - 46.0f, footerTop,
          visuals::kWindowWidth - 18.0f, footerTop + visuals::kFooterHeight),
      mutedBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

ComPtr<ID2D1Bitmap> LauncherApp::GetApplicationIcon(std::size_t applicationIndex) {
  if (iconCache_.size() != catalog_.applications.size()) {
    iconCache_.resize(catalog_.applications.size());
    iconAttempted_.assign(catalog_.applications.size(), false);
  }
  if (applicationIndex >= iconCache_.size()) {
    return {};
  }
  if (iconAttempted_[applicationIndex]) {
    return iconCache_[applicationIndex];
  }
  iconAttempted_[applicationIndex] = true;

  const ApplicationEntry& application = catalog_.applications[applicationIndex];
  const std::wstring key = IconKey(application);
  const auto source = iconSources_.find(key);
  if (source != iconSources_.end()) {
    source->second.lastUse = ++iconUseCounter_;
    const IconImage& image = source->second.image;
    if (!image.pixels.empty()) {
      const auto properties = D2D1::BitmapProperties(
          D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
      const HRESULT result = renderTarget_->CreateBitmap(
          D2D1::SizeU(image.width, image.height), image.pixels.data(), image.width * 4,
          properties, &iconCache_[applicationIndex]);
      if (FAILED(result)) {
        iconAttempted_[applicationIndex] = false;
      }
    }
    return iconCache_[applicationIndex];
  }
  if (pendingIconTargets_.contains(key)) {
    return {};
  }
  const UINT size = std::clamp(static_cast<UINT>(ScaleForDpiInt(visuals::kApplicationIconSize, dpi_)), 1U, 96U);
  const bool submitted = iconTasks_->Submit(
      [this, key, target = application.target, icon = application.icon, size, generation = iconGeneration_]() {
    IconImage image = LoadApplicationIcon(target, icon, size);
    return [this, key, generation, image = std::move(image)]() mutable {
      if (generation != iconGeneration_) {
        return;
      }
      pendingIconTargets_.erase(key);
      if (iconSources_.size() >= kMaximumIconSources) {
        const auto oldest = std::min_element(iconSources_.begin(), iconSources_.end(),
            [](const auto& left, const auto& right) { return left.second.lastUse < right.second.lastUse; });
        iconSources_.erase(oldest);
      }
      iconSources_.insert_or_assign(key, CachedIcon{std::move(image), ++iconUseCounter_});
      iconCache_.clear();
      iconAttempted_.clear();
      if (IsWindowVisible(window_)) {
        InvalidateRect(window_, nullptr, FALSE);
      }
    };
  });
  if (submitted) {
    pendingIconTargets_.insert(key);
    StartBackgroundTimer();
  } else {
    // A full queue is retried on the next completion/repaint, without growing it.
    iconAttempted_[applicationIndex] = false;
  }
  return {};
}

std::wstring LauncherApp::IconKey(const ApplicationEntry& application) const {
  return application.target + L'\0' + application.icon.value_or(L"") + L'\0' + std::to_wstring(dpi_);
}

LRESULT CALLBACK LauncherApp::WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  LauncherApp* app = reinterpret_cast<LauncherApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
    app = static_cast<LauncherApp*>(create->lpCreateParams);
    app->window_ = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
  }
  if (app != nullptr) {
    return app->HandleMessage(message, wParam, lParam);
  }
  return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK LauncherApp::AppearanceProcedure(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR referenceData) {
  auto* app = reinterpret_cast<LauncherApp*>(referenceData);
  if (message == WM_CTLCOLOREDIT && reinterpret_cast<HWND>(lParam) == app->edit_) {
    return app->HandleMessage(message, wParam, lParam);
  }
  if (message == WM_NCDESTROY) RemoveWindowSubclass(window, AppearanceProcedure, 2);
  return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT CALLBACK LauncherApp::EditProcedure(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR referenceData) {
  auto* app = reinterpret_cast<LauncherApp*>(referenceData);
  // TranslateMessage queues these characters before WM_KEYDOWN is dispatched.
  // The actions below consume the keys; the single-line edit would beep on the characters.
  if (message == WM_MOUSEWHEEL) {
    app->ScrollResults(GET_WHEEL_DELTA_WPARAM(wParam));
    return 0;
  }
  if (message == WM_CHAR && (wParam == VK_RETURN || wParam == VK_ESCAPE)) {
    return 0;
  }
  if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && wParam == VK_F10) {
    app->ShowTrayMenu(true);
    return 0;
  }
  if (message == WM_KEYDOWN) {
    switch (wParam) {
      case VK_ESCAPE:
        if ((lParam & (1L << 30)) == 0) app->GoBack();
        return 0;
      case VK_UP:
        app->MoveSelection(-1);
        return 0;
      case VK_DOWN:
        app->MoveSelection(1);
        return 0;
      case VK_RETURN:
        if ((lParam & (1L << 30)) == 0) app->LaunchSelection();
        return 0;
      default:
        break;
    }
  }
  if ((message==WM_PAINT || message==WM_PRINTCLIENT) && GetWindowTextLengthW(window)==0) {
    const auto result=DefSubclassProc(window,message,wParam,lParam);
    const HDC dc=message==WM_PRINTCLIENT ? reinterpret_cast<HDC>(wParam) : GetDC(window);
    if(dc) {
      RECT bounds{};GetClientRect(window,&bounds);
      FillRect(dc,&bounds,app->editBackgroundBrush_);
      SendMessageW(window,EM_GETRECT,0,reinterpret_cast<LPARAM>(&bounds));
      wchar_t cue[256]{};SendMessageW(window,EM_GETCUEBANNER,reinterpret_cast<WPARAM>(cue),256);
      const auto previous=SelectObject(dc,app->editFont_);SetTextColor(dc,blade_appearance_color("muted"));SetBkMode(dc,TRANSPARENT);
      DrawTextW(dc,cue,-1,&bounds,DT_SINGLELINE|DT_TOP|DT_END_ELLIPSIS|DT_NOPREFIX);
      SelectObject(dc,previous);if(message==WM_PAINT)ReleaseDC(window,dc);
    }
    return result;
  }
  if (message == WM_NCDESTROY) {
    RemoveWindowSubclass(window, EditProcedure, 1);
  }
  return DefSubclassProc(window, message, wParam, lParam);
}

LRESULT LauncherApp::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
  if (taskbarCreatedMessage_ != 0 && message == taskbarCreatedMessage_) {
    trayIconAdded_ = false;
    AddTrayIcon();
    if (installedAppsWatcher_) {
      if (!installedAppsWatcher_->Stop()) {
        ShowNotification(L"Blade Launcher", L"Could not restore app-change notifications. Restart Launcher.");
      }
      UpdateInstalledApplicationsWatcher();
      ScheduleInstalledApplicationsRefresh();
    }
    return 0;
  }
  switch (message) {
    case kMessageEnterShellMode:
      EnterShellMode();
      return 1;
    case kMessageSystemPowerAction:
      ExecutePendingPowerAction();
      return 0;
    case kMessageLaunchSelection:
      ExecutePendingLaunch();
      return 0;
    case WM_SETTINGCHANGE:
      blade_appearance_reload();
      [[fallthrough]];
    case WM_THEMECHANGED:
      ApplyTheme();
      ResizeAndPosition(false);
      InvalidateRect(window_,nullptr,FALSE);
      return 0;
    case WM_CREATE:
      edit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                              0, 0, 100, 32, window_, reinterpret_cast<HMENU>(1), instance_, nullptr);
      if (edit_ == nullptr) {
        return -1;
      }
      SetWindowSubclass(edit_, EditProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
      SendMessageW(edit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search applications"));
      dpi_ = GetDpiForWindow(window_);
      ConfigureWindowAppearance();
      LayoutEditControl();
      return 0;

    case WM_COMMAND:
      if (reinterpret_cast<HWND>(lParam) == edit_ && HIWORD(wParam) == EN_CHANGE) {
        launchError_.clear();
        UpdateResults();
      }
      return 0;

    case WM_CTLCOLOREDIT: {
      HDC deviceContext = reinterpret_cast<HDC>(wParam);
      const visuals::Palette palette = visuals::GetPalette(lightTheme_);
      SetTextColor(deviceContext, palette.editText);
      SetBkColor(deviceContext, palette.editBackground);
      return reinterpret_cast<LRESULT>(editBackgroundBrush_);
    }

    case WM_PAINT:
      Render();
      return 0;

    case WM_ERASEBKGND:
      return 1;

    case WM_SIZE:
      if (renderTarget_) {
        renderTarget_->Resize(D2D1::SizeU(LOWORD(lParam), HIWORD(lParam)));
      }
      LayoutEditControl();
      return 0;

    case WM_DPICHANGED: {
      dpi_ = HIWORD(wParam);
      iconCache_.clear();
      iconAttempted_.clear();
      const auto* suggested = reinterpret_cast<const RECT*>(lParam);
      SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                   suggested->right - suggested->left, suggested->bottom - suggested->top,
                   SWP_NOACTIVATE | SWP_NOZORDER);
      if (renderTarget_) {
        renderTarget_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
      }
      LayoutEditControl();
      InvalidateRect(window_, nullptr, FALSE);
      return 0;
    }

    case WM_MOUSEWHEEL:
      ScrollResults(GET_WHEEL_DELTA_WPARAM(wParam));
      return 0;

    case WM_LBUTTONDOWN: {
      const float x = static_cast<float>(GET_X_LPARAM(lParam)) * 96.0f / static_cast<float>(dpi_);
      const float y = static_cast<float>(GET_Y_LPARAM(lParam)) * 96.0f / static_cast<float>(dpi_);
      if (menuPage_ != MenuPage::Main && x >= 8.0f && x < visuals::kSearchEditLeft &&
          y >= visuals::kSearchHeight && y < ResultsTop()) {
        GoBack();
      } else if (const auto result = ResultAtY(y)) {
        selectedResult_ = *result;
        LaunchSelection();
      }
      return 0;
    }

    case WM_ACTIVATE:
      if (LOWORD(wParam) == WA_INACTIVE && IsWindowVisible(window_) && !powerActionActive_) {
        HideLauncher();
      }
      return 0;

    case WM_COPYDATA: {
      const auto data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
      std::wstring query;
      const auto client = reinterpret_cast<HWND>(wParam);
      if (!IsWindow(client) || !ReadDiagnosticRequest(data, query) || diagnosticRequests_.size() >= 8) return 0;
      diagnosticRequests_.push_back({client, data->dwData, std::move(query)});
      if (!PostMessageW(window_, kMessageDeliverDiagnostics, 0, 0)) {
        diagnosticRequests_.pop_back(); return 0;
      }
      return 1;
    }

    case kMessageDeliverDiagnostics: {
      if (diagnosticRequests_.empty()) return 0;
      auto request = std::move(diagnosticRequests_.front());
      diagnosticRequests_.pop_front();
      if (!IsWindow(request.client)) return 0;
      // Windows Runtime/COM work runs after the synchronous input handler.
      try {
        if (request.mode == kSearchRequest) {
          ShowLauncher();
          SetWindowTextW(edit_, request.query.c_str());
          UpdateResults();
          UpdateWindow(window_);
        }
        SendDiagnosticResponse(request.client, window_,
            DiagnosticReport(request.mode == kStatusRequest ? L"" : request.query));
      } catch (...) {
        SendDiagnosticResponse(request.client, window_, L"{\"success\":false,\"error\":\"Launcher could not generate diagnostics.\"}");
      }
      return 0;
    }

    case kMessageToggleLauncher:
      ToggleLauncher();
      return 0;

    case kMessageShowLauncher:
      ShowLauncher();
      return 0;

    case kMessageHideLauncher:
      HideLauncher();
      return 0;

    case kMessageBackgroundComplete:
      ProcessBackgroundResults();
      return 0;

    case kMessageInstalledApplicationsChanged: {
      std::wstring error;
      const bool changed = InstalledAppsWatcher::ConsumeNotification(wParam, lParam, error);
      if (!hasValidCatalog_ || !configuredCatalog_.discoverInstalled) return 0;
      if (!error.empty()) {
        installedApplicationsWatchError_ = error;
        if (catalogFileError_.empty()) RebuildCatalog();
        ShowNotification(L"Blade Launcher", error);
      }
      if (changed || !error.empty()) ScheduleInstalledApplicationsRefresh();
      return 0;
    }

    case kMessageBenchmarkState: {
      if (!benchmarkPresentedEvent_) return 0;
      LRESULT state = kBenchmarkAvailable;
      if (!discoveryTasks_ || !iconTasks_ || discoveryPending_ || discoveryAgain_ || discoveryChangePending_ || discoveryRetryPending_ ||
          discoveryTasks_->HasPending() || iconTasks_->HasPending() ||
          (IsWindowVisible(window_) && GetUpdateRect(window_, nullptr, FALSE))) {
        state |= kBenchmarkPending;
      }
      if (!catalogError_.empty() || !launchError_.empty()) state |= kBenchmarkFailed;
      return state;
    }

    case WM_TIMER:
      if (wParam == kTrayRetryTimer) {
        AddTrayIcon();
      } else if (wParam == kBackgroundTimer) {
        ProcessBackgroundResults();
      } else if (wParam == kInstalledApplicationsChangeTimer) {
        RefreshChangedInstalledApplications();
      } else if (wParam == kInstalledApplicationsRetryTimer) {
        KillTimer(window_, kInstalledApplicationsRetryTimer);
        discoveryRetryPending_ = false;
        if (configuredCatalog_.discoverInstalled) {
          if (discoveryPending_) discoveryAgain_ = true;
          else RequestInstalledApplications();
        }
      }
      return 0;

    case kMessageTray:
      if (lParam == WM_LBUTTONUP) {
        ShowLauncher();
      } else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
        ShowTrayMenu();
      }
      return 0;

    case WM_QUERYENDSESSION:
      return TRUE;

    case WM_DESTROY:
      pendingLaunch_.reset();
      StopBackgroundTasks();
      if (hookManager_) {
        hookManager_->Stop();
      }
      RemoveTrayIcon();
      PostQuitMessage(0);
      return 0;

    default:
      return DefWindowProcW(window_, message, wParam, lParam);
  }
}

}  // namespace quickdial
