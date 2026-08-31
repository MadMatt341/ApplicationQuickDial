#include "LauncherApp.h"

#include "AppMessages.h"
#include "HookManager.h"
#include "InstalledApps.h"
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

struct LoadedShellIcon {
  std::wstring target;
  HBITMAP bitmap = nullptr;
};

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

HBITMAP LoadShellIconBitmap(const std::wstring& target) {
  ComPtr<IShellItem> shellItem;
  if (FAILED(SHCreateItemFromParsingName(target.c_str(), nullptr, IID_PPV_ARGS(&shellItem)))) {
    return nullptr;
  }
  ComPtr<IShellItemImageFactory> imageFactory;
  if (FAILED(shellItem.As(&imageFactory))) {
    return nullptr;
  }
  HBITMAP bitmap = nullptr;
  const SIZE requestedSize{32, 32};
  if (FAILED(imageFactory->GetImage(requestedSize, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &bitmap))) {
    return nullptr;
  }
  return bitmap;
}

}  // namespace

LauncherApp::LauncherApp(HANDLE benchmarkPresentedEvent)
    : benchmarkPresentedEvent_(benchmarkPresentedEvent) {}

LauncherApp::~LauncherApp() {
  shuttingDown_ = true;
  for (auto& thread : iconLoaderThreads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
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
  for (const auto& [target, bitmap] : shellIconSources_) {
    static_cast<void>(target);
    if (bitmap != nullptr) {
      DeleteObject(bitmap);
    }
  }
}

bool LauncherApp::Initialize(HINSTANCE instance) {
  instance_ = instance;
  catalogPath_ = GetCatalogPath();

  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&controls);

  if (!CreateFactories() || !CreateMainWindow()) {
    return false;
  }

  ReloadCatalog(true);
  AddTrayIcon();

  hookManager_ = std::make_unique<HookManager>(window_);
  if (!hookManager_->Start()) {
    ShowNotification(L"Application Quick Dial", L"Win+Space could not be captured. Use the tray icon to open the launcher.");
  }
  return true;
}

int LauncherApp::Run() {
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return static_cast<int>(message.wParam);
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
  result = CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                            IID_PPV_ARGS(wicFactory_.ReleaseAndGetAddressOf()));
  if (FAILED(result)) {
    result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(wicFactory_.ReleaseAndGetAddressOf()));
  }
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
  NOTIFYICONDATAW icon{};
  icon.cbSize = sizeof(icon);
  icon.hWnd = window_;
  icon.uID = kTrayIconId;
  icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  icon.uCallbackMessage = kMessageTray;
  icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  wcscpy_s(icon.szTip, L"Application Quick Dial");
  trayIconAdded_ = Shell_NotifyIconW(NIM_ADD, &icon) != FALSE;
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
      ReloadCatalog(true);
      UpdateResults();
      if (IsWindowVisible(window_)) {
        ResizeAndPosition(false);
        InvalidateRect(window_, nullptr, FALSE);
      }
      if (catalogError_.empty()) {
        ShowNotification(L"Application Quick Dial", L"The app list was reloaded.");
      } else {
        ShowNotification(L"Could not reload app list", catalogError_);
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
    catalogError_ = createError;
    return;
  }

  CatalogResult result = LoadCatalogFile(catalogPath_);
  if (!result) {
    catalogError_ = result.error;
    return;
  }

  Catalog configured = std::move(*result.catalog);
  if (configured.discoverInstalled &&
      (refreshInstalledApplications || !installedApplicationsLoaded_)) {
    InstalledAppsResult installed = DiscoverInstalledApplications();
    if (installed) {
      installedApplications_ = std::move(installed.applications);
      installedApplicationsLoaded_ = true;
      installedApplicationsError_.clear();
    } else {
      installedApplicationsError_ = std::move(installed.error);
    }
  }

  catalog_ = configured.discoverInstalled
                 ? MergeInstalledApplications(std::move(configured), installedApplications_)
                 : std::move(configured);
  hasValidCatalog_ = true;
  catalogError_ = catalog_.discoverInstalled ? installedApplicationsError_ : std::wstring{};
  iconCache_.clear();
  iconAttempted_.clear();
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
        renderTarget_->DrawBitmap(icon.Get(), iconBounds, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
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
  if (application.icon) {
    iconCache_[applicationIndex] = LoadBitmapFromFile(*application.icon);
  }
  if (!iconCache_[applicationIndex]) {
    iconCache_[applicationIndex] = LoadBitmapFromShellTarget(application.target);
  }
  if (!iconCache_[applicationIndex]) {
    if (!failedIconTargets_.contains(application.target)) {
      QueueShellIconLoad(application.target);
      return {};
    }
  }
  iconAttempted_[applicationIndex] = true;
  return iconCache_[applicationIndex];
}

ComPtr<ID2D1Bitmap> LauncherApp::LoadBitmapFromFile(const std::wstring& path) {
  ComPtr<IWICBitmapDecoder> decoder;
  HRESULT result = wicFactory_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                          WICDecodeMetadataCacheOnLoad, &decoder);
  if (FAILED(result)) {
    return {};
  }
  ComPtr<IWICBitmapFrameDecode> frame;
  result = decoder->GetFrame(0, &frame);
  if (FAILED(result)) {
    return {};
  }
  ComPtr<IWICFormatConverter> converter;
  result = wicFactory_->CreateFormatConverter(&converter);
  if (FAILED(result)) {
    return {};
  }
  result = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                 0.0, WICBitmapPaletteTypeCustom);
  if (FAILED(result)) {
    return {};
  }
  ComPtr<ID2D1Bitmap> bitmap;
  if (FAILED(renderTarget_->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &bitmap))) {
    return {};
  }
  return bitmap;
}

ComPtr<ID2D1Bitmap> LauncherApp::LoadBitmapFromShellTarget(const std::wstring& target) {
  const auto source = shellIconSources_.find(target);
  if (source == shellIconSources_.end() || source->second == nullptr) {
    return {};
  }

  ComPtr<IWICBitmap> wicBitmap;
  const HRESULT result = wicFactory_->CreateBitmapFromHBITMAP(
      source->second, nullptr, WICBitmapUsePremultipliedAlpha, &wicBitmap);
  if (FAILED(result)) {
    return {};
  }
  ComPtr<ID2D1Bitmap> bitmap;
  if (FAILED(renderTarget_->CreateBitmapFromWicBitmap(wicBitmap.Get(), nullptr, &bitmap))) {
    return {};
  }
  return bitmap;
}

void LauncherApp::QueueShellIconLoad(const std::wstring& target) {
  if (pendingIconTargets_.contains(target) || shellIconSources_.contains(target) ||
      failedIconTargets_.contains(target)) {
    return;
  }
  pendingIconTargets_.insert(target);
  iconLoaderThreads_.emplace_back([this, target] {
    const HRESULT apartmentResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HBITMAP bitmap = LoadShellIconBitmap(target);
    if (SUCCEEDED(apartmentResult)) {
      CoUninitialize();
    }

    if (shuttingDown_) {
      if (bitmap != nullptr) {
        DeleteObject(bitmap);
      }
      return;
    }

    auto* result = new LoadedShellIcon{target, bitmap};
    if (!PostMessageW(window_, kMessageIconLoaded, 0, reinterpret_cast<LPARAM>(result))) {
      if (bitmap != nullptr) {
        DeleteObject(bitmap);
      }
      delete result;
    }
  });
}

void LauncherApp::HandleLoadedShellIcon(LPARAM payload) {
  std::unique_ptr<LoadedShellIcon> result(reinterpret_cast<LoadedShellIcon*>(payload));
  if (!result) {
    return;
  }
  pendingIconTargets_.erase(result->target);
  if (result->bitmap == nullptr) {
    failedIconTargets_.insert(result->target);
  } else {
    auto existing = shellIconSources_.find(result->target);
    if (existing != shellIconSources_.end() && existing->second != nullptr) {
      DeleteObject(existing->second);
    }
    shellIconSources_[result->target] = result->bitmap;
    result->bitmap = nullptr;
  }

  if (iconCache_.size() == catalog_.applications.size()) {
    for (std::size_t index = 0; index < catalog_.applications.size(); ++index) {
      if (catalog_.applications[index].target == result->target) {
        iconCache_[index].Reset();
        iconAttempted_[index] = false;
      }
    }
  }
  if (IsWindowVisible(window_)) {
    InvalidateRect(window_, nullptr, FALSE);
  }
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

    case kMessageIconLoaded:
      HandleLoadedShellIcon(lParam);
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
