#include "LauncherApp.h"

#include "AppMessages.h"
#include "BackgroundTasks.h"
#include "DiscoveryProcess.h"
#include "HookManager.h"
#include "InstalledApps.h"
#include "InstalledAppsWatcher.h"
#include "LauncherVisualStyle.h"
#include "Search.h"
#include "StartupManager.h"

#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
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

constexpr std::size_t kMaximumResults = 6;
constexpr UINT_PTR kTrayRetryTimer = 1;
constexpr UINT_PTR kBackgroundTimer = 2;
constexpr UINT_PTR kInstalledApplicationsChangeTimer = 3;
constexpr UINT kInstalledApplicationsDebounceMs = 750;
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

LauncherApp::LauncherApp(HANDLE benchmarkPresentedEvent, std::filesystem::path catalogPath)
    : benchmarkPresentedEvent_(benchmarkPresentedEvent), catalogPath_(std::move(catalogPath)) {}

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
  if (!hookManager_->Start()) {
    ShowNotification(L"Application Quick Dial", L"Win+Space could not be captured. Use the tray icon to open the launcher.");
  }
  ReloadCatalog();
  return true;
}

int LauncherApp::Run() {
  for (;;) {
    const auto directories = installedAppsWatcher_->DirectoryHandles();
    const DWORD count = static_cast<DWORD>(directories.size());
    const DWORD ready = MsgWaitForMultipleObjectsEx(count, directories.data(), INFINITE,
        QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (ready == WAIT_FAILED) {
      ShowNotification(L"Application Quick Dial", L"Could not wait for Windows events. Restart Quick Dial.");
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
  result = dwriteFactory_->CreateTextFormat(
      fontFamily_.c_str(), nullptr, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL,
      DWRITE_FONT_STRETCH_NORMAL, visuals::kResultFontSize, L"", resultTextFormat_.ReleaseAndGetAddressOf());
  if (FAILED(result)) {
    return false;
  }
  result = dwriteFactory_->CreateTextFormat(
      fontFamily_.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
      DWRITE_FONT_STRETCH_NORMAL, visuals::kMessageFontSize, L"", messageTextFormat_.ReleaseAndGetAddressOf());
  if (FAILED(result)) {
    return false;
  }
  return SUCCEEDED(resultTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP)) &&
         SUCCEEDED(resultTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING)) &&
         SUCCEEDED(resultTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER)) &&
         SUCCEEDED(messageTextFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP)) &&
         SUCCEEDED(messageTextFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING)) &&
         SUCCEEDED(messageTextFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER));
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
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kWindowClassName, L"Application Quick Dial", WS_POPUP,
      CW_USEDEFAULT, CW_USEDEFAULT, static_cast<int>(visuals::kWindowWidth), 160, nullptr, nullptr, instance_, this);
  return window_ != nullptr;
}

void LauncherApp::ConfigureWindowAppearance() {
  const DWM_WINDOW_CORNER_PREFERENCE cornerPreference = DWMWCP_ROUND;
  DwmSetWindowAttribute(window_, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPreference, sizeof(cornerPreference));
  ApplyTheme();
}

void LauncherApp::ApplyTheme() {
  lightTheme_ = ReadLightThemePreference();
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
  const int y = ScaleForDpiInt(visuals::kSearchEditTop, dpi_);
  const int width = ScaleForDpiInt(
      visuals::kWindowWidth - visuals::kSearchEditLeft - visuals::kSearchEditRight, dpi_);
  const int height = ScaleForDpiInt(visuals::kSearchEditHeight, dpi_);
  SetWindowPos(edit_, nullptr, x, y, width, height, SWP_NOACTIVATE | SWP_NOZORDER);

  if (editFont_ != nullptr) {
    DeleteObject(editFont_);
  }
  editFont_ = CreateFontW(
      -ScaleForDpiInt(visuals::kSearchFontSize, dpi_), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, fontFamily_.c_str());
  SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), TRUE);
  SendMessageW(edit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(0, 0));
}

void LauncherApp::ResizeAndPosition(bool chooseMonitor) {
  const bool hasMessage = !catalogError_.empty() || !launchError_.empty();
  const std::size_t visibleRows = std::max<std::size_t>(1, results_.size());
  const float heightDip = visuals::kSearchHeight + static_cast<float>(visibleRows) * visuals::kResultHeight +
                          visuals::kBottomPadding + (hasMessage ? visuals::kStatusHeight : 0.0f);

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

  SetWindowPos(window_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  LayoutEditControl();
}

void LauncherApp::AddTrayIcon() {
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
  wcscpy_s(icon.szTip, L"Application Quick Dial");
  trayIconAdded_ = Shell_NotifyIconW(NIM_ADD, &icon) != FALSE ||
                   Shell_NotifyIconW(NIM_MODIFY, &icon) != FALSE;
  if (trayIconAdded_) {
    KillTimer(window_, kTrayRetryTimer);
  } else if (SetTimer(window_, kTrayRetryTimer, 2000, nullptr) == 0) {
    launchError_ = L"Could not restore the tray icon. Restart Quick Dial when Explorer is available.";
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

void LauncherApp::ShowTrayMenu() {
  HMENU menu = CreatePopupMenu();
  if (menu == nullptr) {
    return;
  }
  AppendMenuW(menu, MF_STRING, kCommandOpen, L"Open Quick Dial");
  AppendMenuW(menu, MF_STRING, kCommandOpenCatalog, L"Open app list");
  AppendMenuW(menu, MF_STRING, kCommandReloadCatalog, L"Reload app list");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING | (IsStartWithWindowsEnabled() ? MF_CHECKED : MF_UNCHECKED),
              kCommandStartWithWindows, L"Start with Windows");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kCommandExit, L"Exit");

  POINT point{};
  GetCursorPos(&point);
  SetForegroundWindow(window_);
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
          ShowNotification(L"Application Quick Dial", L"The app list was reloaded.");
        } else {
          ShowNotification(L"Could not reload app list", catalogError_);
        }
      }
      break;
    case kCommandStartWithWindows: {
      std::wstring error;
      const bool enable = !IsStartWithWindowsEnabled();
      if (!SetStartWithWindowsEnabled(enable, error)) {
        ShowNotification(L"Application Quick Dial", error);
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
  launchError_.clear();
  anchorMonitor_ = nullptr;
  ReloadCatalog();
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
  if (selectedResult_ < results_.size() && results_[selectedResult_] < catalog_.applications.size()) {
    selectedTarget = catalog_.applications[results_[selectedResult_]].target;
  }
  catalog_ = MergeInstalledApplications(configuredCatalog_, installedApplications_);
  catalogError_ = catalog_.discoverInstalled ?
      (installedApplicationsError_.empty() ? installedApplicationsWatchError_ : installedApplicationsError_) :
      std::wstring{};
  iconCache_.clear();
  iconAttempted_.clear();
  UpdateResults();
  for (std::size_t index = 0; index < results_.size(); ++index) {
    if (catalog_.applications[results_[index]].target == selectedTarget) {
      selectedResult_ = index;
      break;
    }
  }
}

void LauncherApp::UpdateInstalledApplicationsWatcher() {
  const std::wstring previousError = installedApplicationsWatchError_;
  if (!hasValidCatalog_ || !configuredCatalog_.discoverInstalled) {
    KillTimer(window_, kInstalledApplicationsChangeTimer);
    discoveryChangePending_ = discoveryAgain_ = false;
    installedApplicationsWatchError_.clear();
    if (!installedAppsWatcher_->Stop()) {
      installedApplicationsWatchError_ = L"Could not stop app-change notifications. Restart Quick Dial.";
    }
  } else {
    installedAppsWatcher_->Start(window_, kMessageInstalledApplicationsChanged, installedApplicationsWatchError_);
  }
  if (!installedApplicationsWatchError_.empty() && installedApplicationsWatchError_ != previousError) {
    ShowNotification(L"Application Quick Dial", installedApplicationsWatchError_);
  }
}

void LauncherApp::ScheduleInstalledApplicationsRefresh() {
  if (!hasValidCatalog_ || !configuredCatalog_.discoverInstalled) return;
  discoveryChangePending_ = true;
  // A one-shot debounce after an event, never a periodic app-list poll.
  if (SetTimer(window_, kInstalledApplicationsChangeTimer, kInstalledApplicationsDebounceMs, nullptr) == 0) {
    installedApplicationsWatchError_ = L"Could not delay an app-list refresh. Restart Quick Dial.";
    ShowNotification(L"Application Quick Dial", installedApplicationsWatchError_);
    RefreshChangedInstalledApplications();
  }
}

void LauncherApp::HandleInstalledApplicationsDirectoryChange(std::size_t index) {
  std::wstring error;
  const bool changed = installedAppsWatcher_->ConsumeDirectoryChange(index, error);
  if (!error.empty()) {
    if (!installedAppsWatcher_->Stop()) error += L" Notification cleanup failed; restart Quick Dial.";
    installedApplicationsWatchError_ = error;
    if (catalogFileError_.empty()) RebuildCatalog();
    ShowNotification(L"Application Quick Dial", error);
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
  if (installed) {
    changed = installed.applications.size() != installedApplications_.size() ||
        !std::equal(installed.applications.begin(), installed.applications.end(), installedApplications_.begin(),
            [](const ApplicationEntry& left, const ApplicationEntry& right) {
              return left.name == right.name && left.target == right.target && left.identity == right.identity;
            });
    installedApplications_ = std::move(installed.applications);
    installedApplicationsError_.clear();
  } else {
    installedApplicationsError_ = std::move(installed.error);
  }
  if (catalogFileError_.empty() && (changed || previousError != installedApplicationsError_)) {
    RebuildCatalog();
  }
  const bool refreshAgain = std::exchange(discoveryAgain_, false);
  if (refreshAgain && configuredCatalog_.discoverInstalled) {
    RequestInstalledApplications();
  } else if (notifyAfterDiscovery_) {
    notifyAfterDiscovery_ = false;
    ShowNotification(L"Application Quick Dial",
                     catalogError_.empty() ? L"The app list was reloaded." : catalogError_);
  } else if (!installedApplicationsError_.empty() && installedApplicationsError_ != previousError &&
             configuredCatalog_.discoverInstalled) {
    ShowNotification(L"Could not discover installed apps", installedApplicationsError_);
  }
}

void LauncherApp::StartBackgroundTimer() {
  if (SetTimer(window_, kBackgroundTimer, 100, nullptr) == 0) {
    launchError_ = L"Could not monitor background work. Try reopening Quick Dial.";
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
    ShowNotification(L"Application Quick Dial", launchError_);
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
    OutputDebugStringW(L"Quick Dial: could not stop app-change notifications during shutdown.\n");
  }
  discoveryCancellation_.request_stop();
  if (iconTasks_) iconTasks_->Stop();
  if (discoveryTasks_) discoveryTasks_->Stop();
  if (window_) {
    KillTimer(window_, kBackgroundTimer);
    KillTimer(window_, kTrayRetryTimer);
    KillTimer(window_, kInstalledApplicationsChangeTimer);
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
  results_ = hasValidCatalog_ ? RankApplications(catalog_, query, kMaximumResults) : std::vector<std::size_t>{};
  selectedResult_ = 0;
  if (IsWindowVisible(window_)) {
    ResizeAndPosition(false);
  }
  InvalidateRect(window_, nullptr, FALSE);
}

void LauncherApp::MoveSelection(int delta) {
  if (results_.empty()) {
    return;
  }
  const int count = static_cast<int>(results_.size());
  int selected = static_cast<int>(selectedResult_);
  selected = (selected + delta + count) % count;
  selectedResult_ = static_cast<std::size_t>(selected);
  InvalidateRect(window_, nullptr, FALSE);
}

void LauncherApp::LaunchSelection() {
  if (results_.empty() || selectedResult_ >= results_.size()) {
    return;
  }
  const std::size_t applicationIndex = results_[selectedResult_];
  if (applicationIndex >= catalog_.applications.size()) {
    return;
  }

  std::wstring error;
  if (LaunchApplication(catalog_.applications[applicationIndex], error)) {
    HideLauncher();
    return;
  }
  launchError_ = std::move(error);
  ResizeAndPosition(false);
  InvalidateRect(window_, nullptr, FALSE);
}

bool LauncherApp::LaunchApplication(const ApplicationEntry& application, std::wstring& error) {
  std::wstring file = application.target;
  std::wstring parameters = JoinArguments(application.arguments);
  if (StartsWithCaseInsensitive(application.target, L"shell:")) {
    file = L"explorer.exe";
    std::wstring shellParameters = QuoteArgument(application.target);
    if (!parameters.empty()) {
      shellParameters += L" ";
      shellParameters += parameters;
    }
    parameters = std::move(shellParameters);
  }

  SHELLEXECUTEINFOW execute{};
  execute.cbSize = sizeof(execute);
  execute.fMask = SEE_MASK_FLAG_NO_UI;
  execute.hwnd = window_;
  execute.lpVerb = L"open";
  execute.lpFile = file.c_str();
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
    ShowNotification(L"Application Quick Dial", error);
    return;
  }
  if (reinterpret_cast<INT_PTR>(ShellExecuteW(window_, L"open", catalogPath_.c_str(), nullptr,
                                              catalogPath_.parent_path().c_str(), SW_SHOWNORMAL)) <= 32) {
    ShowNotification(L"Application Quick Dial", L"Could not open the app list.");
  }
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

  const visuals::Palette palette = visuals::GetPalette(lightTheme_);

  ComPtr<ID2D1SolidColorBrush> textBrush;
  ComPtr<ID2D1SolidColorBrush> mutedBrush;
  ComPtr<ID2D1SolidColorBrush> selectedBrush;
  ComPtr<ID2D1SolidColorBrush> separatorBrush;
  ComPtr<ID2D1SolidColorBrush> errorBrush;
  renderTarget_->CreateSolidColorBrush(palette.text, &textBrush);
  renderTarget_->CreateSolidColorBrush(palette.muted, &mutedBrush);
  renderTarget_->CreateSolidColorBrush(palette.selected, &selectedBrush);
  renderTarget_->CreateSolidColorBrush(palette.separator, &separatorBrush);
  renderTarget_->CreateSolidColorBrush(palette.error, &errorBrush);

  renderTarget_->BeginDraw();
  renderTarget_->Clear(palette.background);

  renderTarget_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(28.0f, 29.0f), 7.0f, 7.0f), mutedBrush.Get(), 1.75f);
  renderTarget_->DrawLine(D2D1::Point2F(33.0f, 34.0f), D2D1::Point2F(39.0f, 40.0f), mutedBrush.Get(), 1.75f);
  renderTarget_->DrawLine(D2D1::Point2F(16.0f, visuals::kSearchHeight),
                          D2D1::Point2F(visuals::kWindowWidth - 16.0f, visuals::kSearchHeight),
                          separatorBrush.Get(), 1.0f);

  if (results_.empty()) {
    const std::wstring message = !hasValidCatalog_
                                     ? L"App list unavailable — use the tray menu to open it"
                                     : (catalog_.applications.empty() ? L"No applications configured" : L"No matching applications");
    const D2D1_RECT_F bounds = D2D1::RectF(visuals::kContentRight, visuals::kSearchHeight,
                                           visuals::kWindowWidth - visuals::kContentRight,
                                           visuals::kSearchHeight + visuals::kResultHeight);
    renderTarget_->DrawTextW(message.c_str(), static_cast<UINT32>(message.size()), messageTextFormat_.Get(),
                             bounds, mutedBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
  } else {
    for (std::size_t resultIndex = 0; resultIndex < results_.size(); ++resultIndex) {
      const float top = visuals::kSearchHeight + static_cast<float>(resultIndex) * visuals::kResultHeight;
      if (resultIndex == selectedResult_) {
        renderTarget_->FillRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF(visuals::kSelectionHorizontalInset, top + visuals::kSelectionVerticalInset,
                            visuals::kWindowWidth - visuals::kSelectionHorizontalInset,
                            top + visuals::kResultHeight - visuals::kSelectionVerticalInset),
                visuals::kSelectionCornerRadius, visuals::kSelectionCornerRadius),
            selectedBrush.Get());
      }

      const std::size_t applicationIndex = results_[resultIndex];
      if (applicationIndex >= catalog_.applications.size()) {
        continue;
      }
      const ApplicationEntry& application = catalog_.applications[applicationIndex];
      const auto icon = GetApplicationIcon(applicationIndex);
      const float iconTop = top + (visuals::kResultHeight - visuals::kApplicationIconSize) / 2.0f;
      const D2D1_RECT_F iconBounds = D2D1::RectF(
          visuals::kApplicationIconLeft, iconTop,
          visuals::kApplicationIconLeft + visuals::kApplicationIconSize,
          iconTop + visuals::kApplicationIconSize);
      if (icon) {
        // Preserve the source proportions and place every edge on a physical
        // pixel, including at fractional monitor scales such as 125% and 150%.
        const auto pixels = icon->GetPixelSize();
        const float slot = static_cast<float>(ScaleForDpiInt(visuals::kApplicationIconSize, dpi_));
        const float scale = slot / static_cast<float>(std::max(pixels.width, pixels.height));
        const float width = std::max(1.0f, std::round(pixels.width * scale));
        const float height = std::max(1.0f, std::round(pixels.height * scale));
        const float left = std::round(ScaleForDpi(iconBounds.left, dpi_) + (slot - width) / 2.0f);
        const float topPixel = std::round(ScaleForDpi(iconBounds.top, dpi_) + (slot - height) / 2.0f);
        const float dipPerPixel = 96.0f / static_cast<float>(dpi_);
        const auto imageBounds = D2D1::RectF(left * dipPerPixel, topPixel * dipPerPixel,
            (left + width) * dipPerPixel, (topPixel + height) * dipPerPixel);
        renderTarget_->DrawBitmap(icon.Get(), imageBounds, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
      } else {
        renderTarget_->DrawRoundedRectangle(
            D2D1::RoundedRect(iconBounds, visuals::kSelectionCornerRadius, visuals::kSelectionCornerRadius),
            mutedBrush.Get(), 1.5f);
      }

      const D2D1_RECT_F textBounds = D2D1::RectF(
          visuals::kResultTextLeft, top, visuals::kWindowWidth - visuals::kContentRight,
          top + visuals::kResultHeight);
      renderTarget_->DrawTextW(application.name.c_str(), static_cast<UINT32>(application.name.size()),
                               resultTextFormat_.Get(), textBounds, textBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
  }

  const std::wstring& status = !launchError_.empty() ? launchError_ : catalogError_;
  if (!status.empty()) {
    const float top = visuals::kSearchHeight +
                      static_cast<float>(std::max<std::size_t>(1, results_.size())) * visuals::kResultHeight;
    const D2D1_RECT_F bounds = D2D1::RectF(
        visuals::kContentRight, top, visuals::kWindowWidth - visuals::kContentRight,
        top + visuals::kStatusHeight);
    renderTarget_->DrawTextW(status.c_str(), static_cast<UINT32>(status.size()), messageTextFormat_.Get(), bounds,
                             errorBrush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }

  const HRESULT result = renderTarget_->EndDraw();
  if (result == D2DERR_RECREATE_TARGET) {
    ReleaseDeviceResources();
  } else if (SUCCEEDED(result) && benchmarkPresentedEvent_ != nullptr) {
    SetEvent(benchmarkPresentedEvent_);
  }
  EndPaint(window_, &paint);
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

LRESULT CALLBACK LauncherApp::EditProcedure(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR referenceData) {
  auto* app = reinterpret_cast<LauncherApp*>(referenceData);
  // TranslateMessage queues these characters before WM_KEYDOWN is dispatched.
  // The actions below consume the keys; the single-line edit would beep on the characters.
  if (message == WM_CHAR && (wParam == VK_RETURN || wParam == VK_ESCAPE)) {
    return 0;
  }
  if (message == WM_KEYDOWN) {
    switch (wParam) {
      case VK_ESCAPE:
        app->HideLauncher();
        return 0;
      case VK_UP:
        app->MoveSelection(-1);
        return 0;
      case VK_DOWN:
        app->MoveSelection(1);
        return 0;
      case VK_RETURN:
        app->LaunchSelection();
        return 0;
      default:
        break;
    }
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
        ShowNotification(L"Application Quick Dial", L"Could not restore app-change notifications. Restart Quick Dial.");
      }
      UpdateInstalledApplicationsWatcher();
      ScheduleInstalledApplicationsRefresh();
    }
    return 0;
  }
  switch (message) {
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

    case WM_LBUTTONDOWN: {
      const float y = static_cast<float>(GET_Y_LPARAM(lParam)) * 96.0f / static_cast<float>(dpi_);
      if (y >= visuals::kSearchHeight) {
        const std::size_t row = static_cast<std::size_t>(
            (y - visuals::kSearchHeight) / visuals::kResultHeight);
        if (row < results_.size()) {
          selectedResult_ = row;
          LaunchSelection();
        }
      }
      return 0;
    }

    case WM_ACTIVATE:
      if (LOWORD(wParam) == WA_INACTIVE && IsWindowVisible(window_)) {
        HideLauncher();
      }
      return 0;

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
        ShowNotification(L"Application Quick Dial", error);
      }
      if (changed || !error.empty()) ScheduleInstalledApplicationsRefresh();
      return 0;
    }

    case kMessageBenchmarkState: {
      if (!benchmarkPresentedEvent_) return 0;
      LRESULT state = kBenchmarkAvailable;
      if (!discoveryTasks_ || !iconTasks_ || discoveryPending_ || discoveryAgain_ || discoveryChangePending_ ||
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
