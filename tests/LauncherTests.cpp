#include "LauncherApp.h"
#include <dwmapi.h>
#include "LauncherVisualStyle.h"
#include <algorithm>
#include <atomic>
#include "InstalledApps.h"
#include "BackgroundTasks.h"
#include "AppMessages.h"
#include "DiscoveryProcess.h"
#include "InstalledAppsWatcher.h"
#include "LauncherDiagnostics.h"
#include "blade_appearance.h"
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <shellapi.h>
#include <shlobj.h>
#include <propkey.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <cmath>
#include <thread>

#include <winrt/base.h>

namespace quickdial {

class LauncherAppTestAccess {
  template <typename Predicate>
  static bool PumpUntil(LauncherApp& app, Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    do {
      const auto directories = app.installedAppsWatcher_->DirectoryHandles();
      for (std::size_t index = 0; index < directories.size(); ++index) {
        if (WaitForSingleObject(directories[index], 0) == WAIT_OBJECT_0) {
          app.HandleInstalledApplicationsDirectoryChange(index);
          break;
        }
      }
      MSG message{};
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
      app.ProcessBackgroundResults();
      if (ready()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
  }

  static bool Drain(LauncherApp& app) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
      app.ProcessBackgroundResults();
      if (!app.iconTasks_->HasPending() && !app.discoveryTasks_->HasPending()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
  }

  static bool CheckInstallationNotifications(LauncherApp& app, const std::filesystem::path& directory) {
    PWSTR programs = nullptr;
    const HRESULT folderResult = SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs);
    if (FAILED(folderResult)) {
      if (programs) CoTaskMemFree(programs);
      return false;
    }
    const std::wstring name = L"Quick Dial installation check " + std::to_wstring(GetCurrentProcessId());
    const auto shortcut = std::filesystem::path(programs) / (name + L".lnk");
    CoTaskMemFree(programs);
    const auto target = directory / L"installation-check.exe";
    if (std::filesystem::exists(shortcut) || std::filesystem::exists(target)) return false;
    const auto cleanup = [&shortcut, &target](void*) {
      std::error_code error;
      std::filesystem::remove(shortcut, error);
      if (error) std::wcerr << L"Could not clean up test shortcut: " << shortcut << L'\n';
      std::filesystem::remove(target, error);
      if (error) std::wcerr << L"Could not clean up test executable: " << target << L'\n';
    };
    const std::unique_ptr<void, decltype(cleanup)> guard(&app, cleanup);
    wchar_t executable[32'768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (length == 0 || length >= std::size(executable)) return false;
    std::filesystem::copy_file(executable, target);
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(link.ReleaseAndGetAddressOf()))) ||
        FAILED(link->SetPath(target.c_str())) || FAILED(link.As(&file))) return false;
    app.HideLauncher();
    SetWindowTextW(app.edit_, name.c_str());
    // Do not send SHChangeNotify: exercise the actual directory subscription.
    if (FAILED(file->Save(shortcut.c_str(), TRUE))) return false;
    file.Reset();
    link.Reset();
    const auto settled = [&app] {
      return !app.discoveryPending_ && !app.discoveryChangePending_ && !app.discoveryAgain_ &&
          !app.discoveryRetryPending_ && app.installedApplicationsError_.empty();
    };
    if (!PumpUntil(app, [&] { return settled() && !app.results_.empty(); })) return false;
    if (IsWindowVisible(app.window_)) return false;
    std::cout << "A real Start-menu shortcut was discovered while hidden, without polling.\n" << std::flush;
    app.ShowLauncher();
    SetWindowTextW(app.edit_, name.c_str());
    if (!std::filesystem::remove(shortcut)) return false;
    if (!PumpUntil(app, [&] { return settled() && app.results_.empty(); })) return false;
    std::cout << "Removing the shortcut automatically removed its search result.\n" << std::flush;
    return true;
  }

 public:
  static int CheckWindowVisibility(LauncherApp& app) {
    int failures = 0;
    const auto check = [&failures](bool condition, const char* description) {
      if (!condition) { ++failures; std::cerr << "FAILED: " << description << '\n'; }
    };
    DWORD owner = 0;
    GetWindowThreadProcessId(app.window_, &owner);
    check(owner == GetCurrentProcessId(), "visibility checks observe the owned fixture window");
    check(!IsWindowVisible(app.window_), "shell startup remains hidden before visibility checks");
    for (const bool chooseMonitor : {false, true}) {
      app.HideLauncher();
      app.ResizeAndPosition(chooseMonitor);
      check(!IsWindowVisible(app.window_), "layout updates preserve a hidden window");
    }
    app.HideLauncher();
    SendMessageW(app.window_, WM_THEMECHANGED, 0, 0);
    check(!IsWindowVisible(app.window_), "theme notifications preserve a hidden window");
    app.HideLauncher();
    SendMessageW(app.window_, WM_SETTINGCHANGE, SPI_SETWORKAREA, 0);
    check(!IsWindowVisible(app.window_), "work-area notifications preserve a hidden window");

    app.ShowLauncher();
    check(IsWindowVisible(app.window_), "an explicit open still shows the launcher");
    for (const bool chooseMonitor : {false, true}) {
      app.ResizeAndPosition(chooseMonitor);
      check(IsWindowVisible(app.window_), "layout updates preserve a visible window");
    }
    SendMessageW(app.window_, WM_THEMECHANGED, 0, 0);
    check(IsWindowVisible(app.window_), "theme notifications preserve a visible window");
    SendMessageW(app.window_, WM_SETTINGCHANGE, SPI_SETWORKAREA, 0);
    check(IsWindowVisible(app.window_), "work-area notifications preserve a visible window");
    app.EnterShellMode();
    app.ShowNotification(L"Fixture message", L"Visible without a tray");
    check(IsWindowVisible(app.window_) && app.launchError_.find(L"Visible without a tray") != std::wstring::npos,
          "tray-free notifications still show their message");
    app.HideLauncher();
    return failures;
  }

  static int CheckSearch(LauncherApp& app) {
    int failures = 0;
    const auto check = [&failures](bool condition, const char* description) {
      if (!condition) { ++failures; std::cerr << "FAILED: " << description << '\n'; }
    };
    app.ShowLauncher();
    check(app.results_.empty(), "opening shows no application results");
    RECT emptyBounds{};
    GetClientRect(app.window_, &emptyBounds);
    SetWindowTextW(app.edit_, L"Test");
    check(app.results_.size() == 2, "typing shows matching applications");
    RECT populatedBounds{};
    GetClientRect(app.window_, &populatedBounds);
    check(populatedBounds.bottom == emptyBounds.bottom, "two matches retain the two-row main-menu height");
    SetWindowTextW(app.edit_, L"   ");
    RECT clearedBounds{};
    GetClientRect(app.window_, &clearedBounds);
    check(app.results_.empty() && clearedBounds.bottom == emptyBounds.bottom,
          "clearing to whitespace restores the main-menu height");
    SetWindowTextW(app.edit_, L"Test");

    const auto originalCatalog = app.catalog_;
    for (int index = 0; index < 12; ++index) {
      ApplicationEntry entry;
      entry.name = L"Test extra " + std::to_wstring(index);
      entry.target = L"extra-" + std::to_wstring(index) + L".exe";
      app.catalog_.applications.push_back(std::move(entry));
    }
    app.UpdateResults();
    check(app.results_.size() == 14 && app.VisibleResultRows() == 6,
          "all matches are retained within six visible rows");
    for (int index = 0; index < 8; ++index) app.MoveSelection(1);
    check(app.selectedResult_ == 8 && app.firstVisibleResult_ == 3,
          "keyboard navigation scrolls beyond the sixth match");
    check(app.ResultAtY(visuals::kSearchHeight + 1) == 3 &&
              !app.ResultAtY(visuals::kSearchHeight + 6 * visuals::kResultHeight + 1),
          "click hit testing follows the scrolled viewport and excludes rows below it");
    SetWindowTextW(app.edit_, L"Test extra 11");
    check(app.results_.size() == 1 && app.firstVisibleResult_ == 0,
          "narrowing the query resets the viewport");
    SetWindowTextW(app.edit_, L"Test");
    app.MoveSelection(-1);
    check(app.selectedResult_ == 13 && app.firstVisibleResult_ == 8,
          "wrapping upward reveals the final match");
    app.MoveSelection(1);
    check(app.selectedResult_ == 0 && app.firstVisibleResult_ == 0,
          "wrapping downward returns to the first match");
    UINT wheelLines = 0;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &wheelLines, 0);
    app.ScrollResults(-WHEEL_DELTA / 2);
    check(app.firstVisibleResult_ == 0, "partial wheel deltas accumulate");
    app.ScrollResults(-WHEEL_DELTA / 2);
    check(app.firstVisibleResult_ == std::min<std::size_t>(wheelLines == WHEEL_PAGESCROLL ? 6 : wheelLines, 8),
          "mouse wheel scrolls according to Windows preferences");
    app.catalog_ = originalCatalog;
    SetWindowTextW(app.edit_, L"");
    check(app.results_.empty() && app.firstVisibleResult_ == 0 && app.VisibleResultRows() == 2,
          "clearing a scrolled search restores the System row");
    SetWindowTextW(app.edit_, L"Test");

    return failures;
  }

  static int CheckMainMenu(LauncherApp& app) {
    int failures = 0;
    const auto check = [&failures](bool condition, const char* description) {
      if (!condition) { ++failures; std::cerr << "FAILED: " << description << '\n'; }
    };
    app.ShowLauncher();
    check(app.VisibleResultRows() == 2 && app.menuItems_ == std::vector{MenuItemId::System,MenuItemId::Appearance},
          "empty search displays the System row below the divider");
    SendMessageW(app.edit_, WM_KEYDOWN, VK_DOWN, 0);
    SendMessageW(app.edit_, WM_KEYDOWN, VK_DOWN, 0);
    SendMessageW(app.edit_, WM_KEYDOWN, VK_RETURN, 0);
    SendMessageW(app.edit_, WM_KEYDOWN, VK_RETURN, 1L << 30);
    check(!app.pendingPowerAction_, "holding Enter cannot enter a section and activate its first power action");
    check(app.menuPage_ == MenuPage::System && app.VisibleResultRows() == 3 &&
          app.menuItems_ == std::vector{MenuItemId::ShutDown, MenuItemId::Restart, MenuItemId::SignOut},
          "Down and Enter open the System submenu");
    check(!app.ResultAtY(visuals::kSearchHeight + 1) && app.ResultAtY(app.ResultsTop() + 1) == 0,
          "submenu header is separate from action hit testing");
    SendMessageW(app.edit_, WM_KEYDOWN, VK_DOWN, 0);
    check(app.selectedResult_ == 1, "Down selects Restart");
    const bool validCatalog = app.hasValidCatalog_;
    app.hasValidCatalog_ = false;
    app.UpdateResults();
    check(app.menuItems_.size() == 3, "power actions remain available without a valid app catalog");
    app.hasValidCatalog_ = validCatalog;
    SetWindowTextW(app.edit_, L"reboot");
    check(app.menuItems_ == std::vector{MenuItemId::Restart}, "System search filters to the reboot action");
    SendMessageW(app.edit_, WM_KEYDOWN, VK_ESCAPE, 0);
    check(app.menuPage_ == MenuPage::Main && IsWindowVisible(app.window_) && !app.queryHasText_,
          "Escape returns to the empty main menu without hiding");
    SendMessageW(app.edit_, WM_KEYDOWN, VK_ESCAPE, 1L << 30);
    check(IsWindowVisible(app.window_), "holding Escape cannot return to main and immediately dismiss it");
    SendMessageW(app.edit_, WM_KEYDOWN, VK_RETURN, 0);
    const auto click = MAKELPARAM(static_cast<WORD>(32 * app.dpi_ / 96),
        static_cast<WORD>((visuals::kSearchHeight + 18) * app.dpi_ / 96));
    app.HandleMessage(WM_LBUTTONDOWN, 0, click);
    check(app.menuPage_ == MenuPage::Main && IsWindowVisible(app.window_), "clicking the back arrow returns to main");
    SendMessageW(app.edit_, WM_KEYDOWN, VK_ESCAPE, 0);
    check(!IsWindowVisible(app.window_), "Escape on main dismisses the launcher");

    int requests = 0;
    MenuItemId captured = MenuItemId::System;
    app.requestPowerAction_ = [&](MenuItemId action, std::wstring& error) {
      ++requests;
      captured = action;
      // Nested input while a power request is active cannot start another request.
      app.LaunchSelection();
      error = L"Simulated power request failure";
      return false;
    };
    app.ShowLauncher();
    app.LaunchSelection();
    app.LaunchSelection();
    app.MoveSelection(1);
    app.LaunchSelection();
    check(PumpUntil(app, [&] { return !app.pendingPowerAction_; }), "queued power request completes");
    check(requests == 1 && captured == MenuItemId::ShutDown && IsWindowVisible(app.window_),
          "duplicate Enter coalesces and preserves the selected Shut down action");
    app.LaunchSelection();
    check(PumpUntil(app, [&] { return !app.pendingPowerAction_; }), "Restart request completes without confirmation");
    check(requests == 2 && captured == MenuItemId::Restart && IsWindowVisible(app.window_) &&
          app.launchError_ == L"Simulated power request failure", "restart request failures remain visible and actionable");
    app.LaunchSelection();
    app.GoBack();
    check(PumpUntil(app, [&] { return !app.pendingPowerAction_; }) && requests == 2,
          "Back cancels a queued power action before dispatch");
    app.LaunchSelection();
    app.LaunchSelection();
    app.HideLauncher();
    app.ExecutePendingPowerAction();
    check(requests == 2, "hiding cancels a queued power request");
    app.requestPowerAction_ = [&](MenuItemId, std::wstring&) { ++requests; return true; };
    app.ShowLauncher();
    app.LaunchSelection();
    app.LaunchSelection();
    app.ExecutePendingPowerAction();
    check(requests == 3 && !IsWindowVisible(app.window_), "an accepted fake power request dismisses the launcher");
    std::wstring invalidActionError;
    check(!RequestSystemPowerAction(MenuItemId::System, invalidActionError) && !invalidActionError.empty(),
          "the power adapter rejects non-power menu items before touching privileges or Windows shutdown");
    app.requestPowerAction_ = [&](MenuItemId action, std::wstring& error) {
      captured = action; error = L"Simulated sign out failure"; return false;
    };
    app.ShowLauncher(); app.LaunchSelection();
    SetWindowTextW(app.edit_, L"log off"); app.LaunchSelection();
    check(PumpUntil(app, [&] { return !app.pendingPowerAction_; }) && captured == MenuItemId::SignOut &&
          app.launchError_ == L"Simulated sign out failure" && IsWindowVisible(app.window_),
          "sign out is independently selected and request failures stay visible");
    app.ShowLauncher();
    SetWindowTextW(app.edit_, L"restart");
    check(app.searchResults_.size() == 1 && app.searchResults_[0].kind == LauncherResult::Kind::Command &&
        app.searchResults_[0].command == MenuItemId::Restart, "root search returns a typed Restart result");
    app.LaunchSelection();
    check(PumpUntil(app, [&] { return !app.pendingPowerAction_; }) && captured == MenuItemId::Restart,
        "root result dispatches the captured action through the fake power boundary");
    app.requestPowerAction_ = RequestSystemPowerAction;
    app.ShowLauncher();
    check(app.menuPage_ == MenuPage::Main, "reopening always returns to the main menu");
    const auto configured = app.configuredCatalog_;
    ApplicationEntry first;
    first.name = L"Selection first"; first.target = L"first.exe"; first.identity = L"selection-first";
    ApplicationEntry last;
    last.name = L"Selection last"; last.target = L"last.exe"; last.identity = L"selection-last";
    app.configuredCatalog_.applications = {first, last};
    app.configuredCatalog_.discoverInstalled = false;
    app.RebuildCatalog();
    SetWindowTextW(app.edit_, L"Selection");
    app.MoveSelection(1);
    app.configuredCatalog_.applications[1].target = L"replacement.exe";
    app.RebuildCatalog();
    check(app.selectedResult_ == 1 && app.searchResults_[1].kind == LauncherResult::Kind::Application,
        "stable application identity preserves selection when its launch target changes");
    SetWindowTextW(app.edit_, L"reboot");
    app.RebuildCatalog();
    check(app.searchResults_.size() == 1 && app.searchResults_[app.selectedResult_].command == MenuItemId::Restart,
        "catalog refresh preserves the selected typed root command");
    app.configuredCatalog_ = configured;
    app.ShowLauncher();
    return failures;
  }

  static bool CaptureMenu(LauncherApp& app, const std::filesystem::path& path, bool light, UINT dpi) {
    app.lightTheme_ = light;
    if (app.editBackgroundBrush_) DeleteObject(app.editBackgroundBrush_);
    app.editBackgroundBrush_ = CreateSolidBrush(visuals::GetPalette(light).editBackground);
    app.dpi_ = dpi;
    app.ReleaseDeviceResources();
    const auto height = static_cast<int>((app.ResultsTop() + app.VisibleResultRows() * visuals::kResultHeight +
        visuals::kBottomPadding + visuals::kFooterHeight) * dpi / 96.0f);
    SetWindowPos(app.window_, HWND_TOPMOST, 0, 0, static_cast<int>(visuals::kWindowWidth * dpi / 96), height,
                 SWP_NOMOVE | SWP_NOACTIVATE);
    app.LayoutEditControl();
    InvalidateRect(app.window_, nullptr, FALSE);
    InvalidateRect(app.edit_, nullptr, TRUE);
    UpdateWindow(app.window_);
    UpdateWindow(app.edit_);
    DwmFlush();
    RECT bounds{};
    GetClientRect(app.window_, &bounds);
    const int width = bounds.right;
    const int pixelsHigh = bounds.bottom;
    HDC source = GetDC(app.window_);
    HDC destination = source ? CreateCompatibleDC(source) : nullptr;
    HBITMAP bitmap = source ? CreateCompatibleBitmap(source, width, pixelsHigh) : nullptr;
    HGDIOBJ previous = bitmap && destination ? SelectObject(destination, bitmap) : nullptr;
    // Inactive desktop screen copies omit Direct2D presentation. Render the
    // production drawing path to an owned software DC, then print the real edit.
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> target;
    const auto properties=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE),static_cast<float>(dpi),static_cast<float>(dpi));
    bool success=previous && SUCCEEDED(app.d2dFactory_->CreateDCRenderTarget(&properties,&target)) && SUCCEEDED(target->BindDC(destination,&bounds));
    if(success) {
      target->BeginDraw();app.DrawContent(target.Get(),false);success=SUCCEEDED(target->EndDraw());
      RECT editBounds{};GetWindowRect(app.edit_,&editBounds);MapWindowPoints(nullptr,app.window_,reinterpret_cast<POINT*>(&editBounds),2);
      const int saved=SaveDC(destination);SetViewportOrgEx(destination,editBounds.left,editBounds.top,nullptr);
      SendMessageW(app.edit_,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(destination),PRF_CLIENT|PRF_ERASEBKGND);RestoreDC(destination,saved);
    }
    target.Reset();
    if (previous) SelectObject(destination, previous);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = pixelsHigh;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * pixelsHigh);
    success = success && GetDIBits(source, bitmap, 0, pixelsHigh, pixels.data(), &info, DIB_RGB_COLORS) == pixelsHigh;
    if (bitmap) DeleteObject(bitmap);
    if (destination) DeleteDC(destination);
    if (source) ReleaseDC(app.window_, source);
    if (!success) return false;
    BITMAPFILEHEADER header{};
    header.bfType = 0x4d42;
    header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
    header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size() * sizeof(pixels[0]));
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
    output.write(reinterpret_cast<const char*>(pixels.data()), pixels.size() * sizeof(pixels[0]));
    return static_cast<bool>(output);
  }

  static int CheckAppearance(LauncherApp& app, const std::filesystem::path& snapshots) {
    wchar_t source[32768]{};
    if (!GetEnvironmentVariableW(L"BLADE_APPEARANCE_FILE", source, 32768) ||
        std::filesystem::path(source) != snapshots / L"appearance.json") {
      std::cerr << "FAILED: isolated appearance fixture prerequisite\n"; return 1;
    }
    int failures=0;
    const auto check=[&](bool ok,const char* detail){if(!ok){++failures;std::cerr<<"FAILED: "<<detail<<'\n';}};
    const auto write=[&](const std::string& value){
      const std::wstring temporary=std::wstring(source)+L".new";
      {std::ofstream file(temporary,std::ios::binary);file<<value;}
      check(MoveFileExW(temporary.c_str(),source,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0,"atomic appearance replacement");
    };
    const auto status=[](){std::string result(blade_appearance_json(nullptr,0),'\0');blade_appearance_json(result.data(),result.size());result.pop_back();return result;};
    app.ShowLauncher();SetWindowTextW(app.edit_,L"Test");app.MoveSelection(1);
    const auto window=app.window_;const auto edit=app.edit_;const auto selected=app.selectedResult_;
    write(R"({"format":"blade-appearance","schemaVersion":1,"theme":"blade-charcoal","themeVersion":1,"overrides":{"tokens":{"colors":{"background":"#102030"},"typography":{"body":18,"search":22,"families":["Blade Fixture Missing Font","Consolas"]}},"components":{"launcher":{"width":640}}}})");
    check(PumpUntil(app,[&]{return visuals::kWindowWidth==640 && visuals::kSearchFontSize==22;}),"appearance file notification updates launcher metrics");
    wchar_t query[64]{};GetWindowTextW(app.edit_,query,64);
    check(window==app.window_&&edit==app.edit_&&app.selectedResult_==selected&&std::wstring(query)==L"Test","theme reload preserves HWND, query and selection");
    LOGFONTW font{};GetObjectW(app.editFont_,sizeof(font),&font);
    check(std::wstring(font.lfFaceName)==L"Consolas","native search font uses installed fallback");
    check(visuals::GetPalette(false).editBackground==RGB(16,32,48)&&visuals::GetPalette(true).editBackground==RGB(16,32,48),"fixed dark selection ignores independent Windows palette preference");
    std::ofstream(snapshots/L"appearance-status.json",std::ios::binary)<<status();
    write("{invalid");
    check(PumpUntil(app,[&]{return status().find("\"rejection\":null")==std::string::npos;}),"malformed live edit reports rejection");
    check(visuals::kWindowWidth==640&&visuals::kSearchFontSize==22,"malformed live edit retains last valid layout");
    write(R"({"format":"blade-appearance","schemaVersion":1,"theme":"blade-charcoal","themeVersion":1,"overrides":{}})");
    check(PumpUntil(app,[&]{return visuals::kWindowWidth==540&&visuals::kSearchFontSize==16;}),"valid replacement recovers default theme");
    app.ShowLauncher();return failures;
  }

  struct EditInk { int top = -1; int bottom = -1; COLORREF background = CLR_INVALID; };

  static EditInk CaptureEditInk(HWND edit, COLORREF background) {
    RECT bounds{}; GetClientRect(edit, &bounds);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = bounds.right;
    info.bmiHeader.biHeight = -bounds.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* data = nullptr;
    HDC source = GetDC(edit);
    HDC dc = source ? CreateCompatibleDC(source) : nullptr;
    HBITMAP bitmap = dc ? CreateDIBSection(dc, &info, DIB_RGB_COLORS, &data, nullptr, 0) : nullptr;
    HGDIOBJ old = bitmap ? SelectObject(dc, bitmap) : nullptr;
    EditInk ink;
    if (old && data) {
      SendMessageW(edit, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_ERASEBKGND);
      ink.background = GetPixel(dc, bounds.right - 1, bounds.bottom - 1);
      const auto* pixels = static_cast<const std::uint32_t*>(data);
      for (int y = 0; y < bounds.bottom; ++y) {
        for (int x = 0; x < bounds.right; ++x) {
          const auto pixel = pixels[y * bounds.right + x];
          const int red = (pixel >> 16) & 255, green = (pixel >> 8) & 255, blue = pixel & 255;
          if (std::max({std::abs(red - GetRValue(background)), std::abs(green - GetGValue(background)),
                        std::abs(blue - GetBValue(background))}) > 20) {
            if (ink.top < 0) ink.top = y;
            ink.bottom = y;
          }
        }
      }
    }
    if (old) SelectObject(dc, old);
    if (bitmap) DeleteObject(bitmap);
    if (dc) DeleteDC(dc);
    if (source) ReleaseDC(edit, source);
    return ink;
  }

  static int CheckSearchAlignment(LauncherApp& app) {
    int failures = 0;
    const auto check = [&](bool ok, const char* detail) {
      if (!ok) { ++failures; std::cerr << "FAILED: " << detail << '\n'; }
    };
    const UINT originalDpi = app.dpi_;
    app.ShowLauncher();
    SendMessageW(app.edit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"H"));
    for (const UINT dpi : {96U, 120U, 144U, 192U}) {
      for (const float size : {16.0f, 22.0f, 48.0f}) {
        app.dpi_ = dpi;
        visuals::kSearchFontSize = size;
        visuals::kSearchHeight = size == 48.0f ? 72.0f : 52.0f;
        app.LayoutEditControl();
        const COLORREF background = visuals::GetPalette(false).editBackground;
        SetWindowTextW(app.edit_, L"");
        app.dpi_ = dpi;
        app.LayoutEditControl();
        const auto cue = CaptureEditInk(app.edit_, background);
        SetWindowTextW(app.edit_, L"H");
        app.dpi_ = dpi;
        app.LayoutEditControl();
        const auto typed = CaptureEditInk(app.edit_, background);
        check(cue.top >= 0 && typed.top >= 0 && std::abs(cue.top - typed.top) <= 1 &&
                  std::abs(cue.bottom - typed.bottom) <= 1,
              "placeholder and native typed glyph have the same vertical bounds at each scale/font size");
        check(cue.background == background && typed.background == background,
              "empty and typed search use the same theme background");
        RECT rect{}; GetWindowRect(app.edit_, &rect);
        MapWindowPoints(nullptr, app.window_, reinterpret_cast<POINT*>(&rect), 2);
        const int header = static_cast<int>(std::lround(visuals::kSearchHeight * dpi / 96.0f));
        check(rect.top >= 0 && rect.bottom <= header && std::abs(rect.top + rect.bottom - header) <= 1,
              "measured native edit is centered inside the search header without clipping");
      }
    }
    app.dpi_ = originalDpi;
    app.ApplyTheme();
    app.ShowLauncher();
    return failures;
  }

  static int RunMainMenuChecks(const std::filesystem::path& path, const std::filesystem::path& snapshots) {
    LauncherApp app(nullptr, path, true);
    if (!app.Initialize(GetModuleHandleW(nullptr))) {
      std::cerr << "FAILED: main-menu launcher initialization\n";
      return 1;
    }
    int failures = 0;
    if (IsWindowVisible(app.window_) || app.trayIconAdded_) {
      ++failures;
      std::cerr << "FAILED: shell mode starts hidden without a tray icon\n";
    }
    failures += CheckWindowVisibility(app);
    failures += CheckSearch(app);
    failures += CheckMainMenu(app);
    failures += CheckSearchAlignment(app);
    if(!snapshots.empty()) failures += CheckAppearance(app,snapshots);
    const auto reportPath = path.parent_path() / L"live-status.json";
    // The real native client runs in a separate owned process. Target the fixture
    // explicitly so an ordinary-desktop run cannot inspect or open the resident.
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, 32768);
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --diagnostic-fixture " +
        std::to_wstring(reinterpret_cast<UINT_PTR>(app.window_)) + L" \"" + reportPath.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION client{};
    int statusExit = 1;
    if (CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &startup, &client)) {
      CloseHandle(client.hThread);
      if (PumpUntil(app, [&] { return WaitForSingleObject(client.hProcess, 0) == WAIT_OBJECT_0; })) {
        DWORD exitCode = 1;
        GetExitCodeProcess(client.hProcess, &exitCode);
        statusExit = static_cast<int>(exitCode);
      } else {
        TerminateProcess(client.hProcess, 1);
        WaitForSingleObject(client.hProcess, 5000);
      }
      CloseHandle(client.hProcess);
    }
    std::ifstream statusFile(reportPath, std::ios::binary);
    const std::string statusText{std::istreambuf_iterator<char>(statusFile), {}};
    statusFile.close();
    winrt::Windows::Data::Json::JsonObject status;
    const bool parsed = winrt::Windows::Data::Json::JsonObject::TryParse(winrt::to_hstring(statusText), status);
    if (statusExit != 0 || !parsed || status.GetNamedString(L"product", L"") != L"Blade Launcher" ||
        status.GetNamedArray(L"results").Size() != 1 ||
        status.GetNamedArray(L"results").GetObjectAt(0).GetNamedString(L"label") != L"Restart") {
      ++failures; std::cerr << "FAILED: live diagnostic protocol returns the actual typed search result\n";
    }
    std::filesystem::remove(reportPath);
    if (!snapshots.empty()) {
      std::filesystem::create_directories(snapshots);
      for (const bool light : {false, true}) {
        for (const UINT dpi : {96U, 120U, 144U, 192U}) {
          app.ShowLauncher();
          const auto suffix = std::wstring(light ? L"light-" : L"dark-") + std::to_wstring(dpi) + L".bmp";
          if (!CaptureMenu(app, snapshots / (L"main-" + suffix), light, dpi)) ++failures;
          app.LaunchSelection();
          if (!CaptureMenu(app, snapshots / (L"system-" + suffix), light, dpi)) ++failures;
          app.GoBack();
          SetWindowTextW(app.edit_, L"Test");
          if (!CaptureMenu(app, snapshots / (L"search-" + suffix), light, dpi)) ++failures;
        }
      }
    }
    DestroyWindow(app.window_);
    if (failures == 0) std::cout << "All main-menu integration checks passed; no Windows power request was sent.\n";
    return failures == 0 ? 0 : 1;
  }

  static bool CheckShellLaunch(LauncherApp& app, const std::filesystem::path& directory, bool checkAppsFolder) {
    wchar_t executable[32768]{};
    if (!GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)))) return false;
    const auto marker = directory / L"launch result.txt";
    ApplicationEntry fixture;
    fixture.name = L"Quick Dial launch fixture";
    fixture.target = executable;
    fixture.arguments = {L"--launched-fixture", marker.wstring(), L"two words", L"quote\"inside"};
    std::wstring error;
    const auto completed = [&] {
      if (!std::filesystem::exists(marker)) return false;
      std::ifstream input(marker);
      std::string content((std::istreambuf_iterator<char>(input)), {});
      return content == "two words\nquote\"inside\n";
    };
    if (!app.LaunchApplication(fixture, error) || !PumpUntil(app, completed)) return false;
    std::filesystem::remove(marker);
    PWSTR programs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs))) return false;
    const std::wstring name = L"QuickDial-launch-test-" + std::to_wstring(GetCurrentProcessId()) + L".lnk";
    const auto shortcut = std::filesystem::path(programs) / name;
    CoTaskMemFree(programs);
    if (std::filesystem::exists(shortcut)) return false;
    const auto cleanup = [&shortcut, &marker](void*) {
      std::error_code ignored;
      std::filesystem::remove(shortcut, ignored);
      std::filesystem::remove(marker, ignored);
    };
    const std::unique_ptr<void, decltype(cleanup)> guard(&app, cleanup);
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(link.ReleaseAndGetAddressOf()))) ||
        FAILED(link->SetPath(executable)) ||
        FAILED(link->SetArguments((L"--launched-fixture \"" + marker.wstring() + L"\" \"two words\" \"quote\\\"inside\"").c_str())) ||
        FAILED(link.As(&file)) || FAILED(file->Save(shortcut.c_str(), TRUE))) return false;
    Microsoft::WRL::ComPtr<IPropertyStore> properties;
    const std::wstring identity = L"QuickDial.LaunchTest." + std::to_wstring(GetCurrentProcessId());
    PROPVARIANT identityValue{};
    identityValue.vt = VT_LPWSTR;
    identityValue.pwszVal = const_cast<PWSTR>(identity.c_str());
    if (FAILED(link.As(&properties)) || FAILED(properties->SetValue(PKEY_AppUserModel_ID, identityValue)) ||
        FAILED(properties->Commit()) || FAILED(file->Save(shortcut.c_str(), TRUE))) return false;
    SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW, shortcut.c_str(), nullptr);
    fixture.target = shortcut.wstring();
    fixture.arguments.clear();
    if (!app.LaunchApplication(fixture, error) || !PumpUntil(app, completed)) {
      std::wcerr << L"Direct shortcut launch failed: " << error << L'\n';
      return false;
    }
    std::filesystem::remove(marker);
    if (!checkAppsFolder) {
      std::cout << "SKIP: newly registered desktop Apps-folder shortcut needs Explorer; direct shortcut verified\n";
      return true;
    }
    fixture.target = L"shell:AppsFolder\\" + identity;
    if (!PumpUntil(app, [&] { return app.LaunchApplication(fixture, error); })) {
      std::wcerr << L"Shell fixture launch failed: " << error << L'\n';
      return false;
    }
    return PumpUntil(app, completed);
  }

  static bool CheckDeferredActivation(LauncherApp& app, const ApplicationEntry& fixture) {
    // Stay hidden so unrelated foreground changes cannot clear the error state.
    app.HideLauncher();
    app.catalog_.applications = {fixture};
    app.hasValidCatalog_ = true;
    app.menuPage_ = MenuPage::Main;
    SetWindowTextW(app.edit_, fixture.name.c_str());
    std::atomic<bool> senderDone = false;
    std::atomic<bool> sent = false;
    std::thread sender([&] {
      DWORD_PTR reply = 0;
      bool success = true;
      for (int attempt = 0; attempt < 3; ++attempt) {
        success = success && SendMessageTimeoutW(app.edit_, WM_KEYDOWN, VK_RETURN, 0,
                                                 SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &reply) != 0;
      }
      sent = success;
      senderDone = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(7);
    while (!senderDone && std::chrono::steady_clock::now() < deadline) {
      // PeekMessage services sent messages but leaves the posted launch queued.
      MSG message{};
      PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    sender.join();
    if (!sent || !app.pendingLaunch_ || !app.launchError_.empty()) {
      std::wcerr << L"Deferred send state: sent=" << sent << L" pending=" << app.pendingLaunch_.has_value()
                 << L" error=" << app.launchError_ << L'\n';
      return false;
    }
    const auto queuedName = app.pendingLaunch_->name;
    app.catalog_.applications[0].name = L"Changed selection must not launch";
    if (!PumpUntil(app, [&] { return !app.pendingLaunch_.has_value(); })) {
      std::cerr << "FAILED: deferred activation did not finish\n";
      return false;
    }
    const std::wstring expected = L"Could not activate " + queuedName + L": ";
    if (!app.launchError_.starts_with(expected)) {
      std::wcerr << L"Deferred activation error: " << app.launchError_ << L'\n';
      return false;
    }
    // Extra Enter requests must not leave another activation queued.
    MSG remaining{};
    return !PeekMessageW(&remaining, app.window_, kMessageLaunchSelection, kMessageLaunchSelection, PM_NOREMOVE);
  }

  static int CheckApplicationLaunches(LauncherApp& app, const std::filesystem::path& directory,
                                      bool checkAppsFolder = true) {
    int failures = 0;
    const auto check = [&failures](bool condition, const char* description) {
      if (!condition) { ++failures; std::cerr << "FAILED: " << description << '\n'; }
    };
    check(CheckShellLaunch(app, directory, checkAppsFolder), "executable and shortcut launches preserve fixture arguments");
    ApplicationEntry missingPackage;
    missingPackage.name = L"Missing packaged fixture";
    missingPackage.target = L"SHELL:appsfolder\\QuickDial.LaunchTest_123456789abcd!MissingApp";
    std::wstring activationError;
    check(!app.LaunchApplication(missingPackage, activationError) &&
              activationError.starts_with(L"Could not activate Missing packaged fixture: "),
          "packaged Apps-folder entries use direct activation and report failure without Shell fallback");
    ApplicationEntry missing;
    missing.name = L"Missing shell fixture";
    missing.target = L"shell:Programs\\QuickDial-does-not-exist-947362.lnk";
    std::wstring launchError;
    check(!app.LaunchApplication(missing, launchError) && !launchError.empty(),
          "invalid Shell target produces an actionable error");
    check(CheckDeferredActivation(app, missingPackage),
          "synchronous Enter defers activation, coalesces repeats, preserves the selected entry, and retains failure status");
    return failures;
  }

  static int RunLaunchChecks(const std::filesystem::path& path) {
    LauncherApp app(nullptr, path, true);
    if (!app.Initialize(GetModuleHandleW(nullptr))) {
      std::cerr << "FAILED: shell-mode launcher initialization\n";
      return 1;
    }
    const int failures = CheckApplicationLaunches(app, path.parent_path(), false);
    DestroyWindow(app.window_);
    if (!failures) std::cout << "Launcher launch checks passed\n";
    return failures;
  }

  static int RunDiscoveryChecks(const std::filesystem::path& path) {
    int failures = 0;
    const auto check = [&failures](bool condition, const char* description) {
      if (!condition) { ++failures; std::cerr << "FAILED: " << description << '\n'; }
    };
    std::ofstream(path, std::ios::trunc) << R"({"version":1,"applications":[{"name":"Manual fixture","target":"manual.exe"}]})";
    LauncherApp app(nullptr, path, true);
    if (!app.Initialize(GetModuleHandleW(nullptr))) { std::cerr << "FAILED: discovery fixture initialization\n"; return 1; }
    check(Drain(app) && app.installedApplicationsError_.empty(), "real initial discovery completes without Explorer");
    app.ShowLauncher();
    check(!app.discoveryPending_, "fresh healthy reopening reuses the source cache");
    ApplicationEntry installed;
    installed.name = L"Cached fixture"; installed.target = L"cached.exe"; installed.source = ApplicationSource::AppsFolder;
    app.ApplyInstalledApplications({{installed}, {}, {{ApplicationSource::AppsFolder, true, 0, {}},
        {ApplicationSource::UserStartMenu, true, 0, {}}, {ApplicationSource::CommonStartMenu, true, 0, {}}}});
    SetWindowTextW(app.edit_, L"Cached fixture");
    app.ApplyInstalledApplications({{}, L"Fixture total provider failure"});
    check(app.catalog_.applications.size() == 2 && app.results_.size() == 1 && !app.catalogError_.empty(),
        "tray-free failure preserves cache, query and visible error without triggering notification recursion");
    std::ofstream(path, std::ios::trunc) << "invalid JSON";
    app.ReloadCatalog();
    const auto parseError = app.catalogError_;
    auto replacement = installed; replacement.name = L"Replacement";
    app.ApplyInstalledApplications({{replacement}, {}});
    check(app.catalog_.applications.back().name == L"Cached fixture" && app.catalogError_ == parseError,
        "late completion cannot replace a visible catalog after invalid JSON reload");
    std::ofstream(path, std::ios::trunc) << R"({"version":1,"applications":[{"name":"Manual fixture","target":"manual.exe"}]})";
    app.ShowLauncher();
    app.ApplyInstalledApplications({{}, L"Fixture failure before open"});
    app.ShowLauncher();
    check(app.discoveryPending_, "opening after failure starts a bounded catch-up request");
    check(Drain(app) && app.installedApplicationsError_.empty(), "catch-up recovers through the real helper");
    app.HideLauncher();
    check(CheckInstallationNotifications(app, path.parent_path()), "physical shortcut creation and removal trigger discovery while hidden");
    DestroyWindow(app.window_);
    if (!failures) std::cout << "Launcher discovery integration checks passed\n";
    return failures ? 1 : 0;
  }

  static int Run(const std::filesystem::path& path, const std::filesystem::path& iconPath,
                 bool checkInstallation) {
    int failures = 0;
    const auto check = [&failures](bool condition, const char* description) {
      if (!condition) { ++failures; std::cerr << "FAILED: " << description << '\n'; }
    };
    const std::unique_ptr<void, decltype(&CloseHandle)> benchmarkEvent(
        CreateEventW(nullptr, TRUE, FALSE, nullptr), CloseHandle);
    if (!benchmarkEvent) {
      std::cerr << "FAILED: benchmark test event creation\n";
      return 1;
    }
    LauncherApp app(nullptr, path);
    if (!app.Initialize(GetModuleHandleW(nullptr))) {
      std::cerr << "FAILED: launcher initialization\n";
      return 1;
    }
    check(!IsWindowVisible(app.window_), "startup keeps the launcher hidden");
    check(app.trayIconAdded_, "startup registers the tray icon");
    check(app.HandleMessage(kMessageBenchmarkState, 0, 0) == 0,
          "ordinary launches do not expose the benchmark protocol");
    app.benchmarkPresentedEvent_ = benchmarkEvent.get();
    check(app.HandleMessage(kMessageBenchmarkState, 0, 0) == kBenchmarkAvailable,
          "a hidden launcher without discovery reports settled readiness");
    app.discoveryPending_ = true;
    check((app.HandleMessage(kMessageBenchmarkState, 0, 0) & kBenchmarkPending) != 0,
          "the benchmark cannot sample while discovery is unfinished");
    app.discoveryPending_ = false;
    check(app.iconTasks_->Submit([] { return [] {}; }), "benchmark pending-state job is accepted");
    check((app.HandleMessage(kMessageBenchmarkState, 0, 0) & kBenchmarkPending) != 0,
          "queued and undispatched icon work prevents a settled sample");
    check(Drain(app), "benchmark pending-state job drains");
    app.ShowLauncher();
    check(IsWindowVisible(app.window_), "show request makes the launcher visible");
    check((app.HandleMessage(kMessageBenchmarkState, 0, 0) & kBenchmarkPending) != 0,
          "an invalidated visible frame prevents a settled sample");
    failures += CheckSearch(app);

    // Exercise the real completion path without depending on installed software.
    app.configuredCatalog_.discoverInstalled = true;
    app.selectedResult_ = 1;
    ApplicationEntry installed;
    installed.name = L"Test Zulu";
    installed.target = L"zulu.exe";
    app.ApplyInstalledApplications({{installed}, {}});
    check(app.catalog_.applications.size() == 3, "successful discovery extends the catalog");
    check(app.catalog_.applications[app.results_[app.selectedResult_]].name == L"Test Bravo",
          "asynchronous discovery preserves the selected application");
    SetWindowTextW(app.edit_, L"Bravo");
    app.iconAttempted_.assign(3, true);
    app.ApplyInstalledApplications({{installed}, {}});
    check(app.iconAttempted_.size() == 3 && app.iconAttempted_.front(),
          "unchanged discovery does not discard device icon state or rebuild results");
    wchar_t query[32]{};
    GetWindowTextW(app.edit_, query, 32);
    check(std::wstring_view(query) == L"Bravo" && app.results_.size() == 1,
          "asynchronous discovery preserves the current search query");

    SetWindowTextW(app.edit_, L"Newly installed");
    check(app.results_.empty(), "an application is absent before Windows discovers it");
    ApplicationEntry newlyInstalled;
    newlyInstalled.name = L"Newly installed application";
    newlyInstalled.target = L"newly-installed.exe";
    app.ApplyInstalledApplications({{installed, newlyInstalled}, {}});
    GetWindowTextW(app.edit_, query, 32);
    check(std::wstring_view(query) == L"Newly installed" && app.results_.size() == 1 &&
          app.catalog_.applications[app.results_.front()].target == newlyInstalled.target,
          "a newly installed application appears in the active search without reopening");
    app.ApplyInstalledApplications({{installed}, {}});
    check(app.results_.empty(), "refresh removes an uninstalled discovered application");

    app.ApplyInstalledApplications({{}, L"Simulated enumeration failure"});
    check(app.catalog_.applications.size() == 3 && app.installedApplications_.size() == 1,
          "failed discovery preserves the last successful discovery cache and catalog");
    check(!app.catalogError_.empty(), "discovery errors remain visible");
    check((app.HandleMessage(kMessageBenchmarkState, 0, 0) & kBenchmarkFailed) != 0,
          "discovery failure cannot masquerade as successful benchmark completion");
    std::ofstream(path, std::ios::trunc) << "invalid JSON";
    app.ReloadCatalog();
    const std::wstring parseError = app.catalogError_;
    installed.name = L"Test replacement";
    app.ApplyInstalledApplications({{installed}, {}});
    check(app.catalog_.applications.back().name == L"Test Zulu" && app.catalogError_ == parseError,
          "late discovery cannot replace the visible catalog or error after an invalid file reload");

    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = app.window_;
    icon.uID = 1;
    check(Shell_NotifyIconW(NIM_DELETE, &icon) != FALSE, "test can remove its own tray icon");
    app.HandleMessage(app.taskbarCreatedMessage_, 0, 0);
    NOTIFYICONIDENTIFIER identifier{};
    identifier.cbSize = sizeof(identifier);
    identifier.hWnd = app.window_;
    identifier.uID = 1;
    RECT iconRect{};
    check(app.trayIconAdded_ && SUCCEEDED(Shell_NotifyIconGetRect(&identifier, &iconRect)),
          "TaskbarCreated restores the removed tray icon");
    app.HandleMessage(app.taskbarCreatedMessage_, 0, 0);
    check(app.trayIconAdded_, "repeated TaskbarCreated also handles an existing icon");
    check(app.installedAppsWatcher_->IsRunning() && app.discoveryChangePending_,
          "Explorer recovery restores subscriptions and schedules one catch-up refresh");
    app.RefreshChangedInstalledApplications();
    check(Drain(app), "Explorer recovery refresh drains");

    app.HideLauncher();
    check(!IsWindowVisible(app.window_) && !app.renderTarget_, "hide releases presentation resources");
    app.ShowLauncher();
    app.HandleMessage(WM_ACTIVATE, WA_INACTIVE, 0);
    check(!IsWindowVisible(app.window_), "losing activation hides the launcher");

    // Fresh healthy opens reuse the cache. Failed or stale opens catch up once.
    // Idle timer messages still never poll.
    std::ofstream(path, std::ios::trunc)
        << R"({"version":1,"applications":[{"name":"Test Alpha","target":"alpha.exe"}]})";
    app.discoveryRequested_ = true;
    app.ShowLauncher();
    check(!app.discoveryPending_, "opening reuses the discovery cache without scanning");
    app.ShowLauncher();
    app.HandleMessage(WM_TIMER, 3, 0);
    check(!app.discoveryPending_ && !app.discoveryAgain_, "repeated opens and idle timer ticks do not scan");
    app.HideLauncher();
    app.ScheduleInstalledApplicationsRefresh();
    app.ScheduleInstalledApplicationsRefresh();
    check(app.discoveryChangePending_ && !app.discoveryPending_, "notification bursts wait for the debounce");
    app.HandleMessage(WM_TIMER, 3, 0);
    check(app.discoveryPending_ && !app.discoveryChangePending_, "a change refreshes while the launcher is hidden");
    app.ScheduleInstalledApplicationsRefresh();
    app.HandleMessage(WM_TIMER, 3, 0);
    check(app.discoveryAgain_, "a change during scanning schedules a single follow-up");
    check(Drain(app), "event refresh and coalesced follow-up complete through the helper");
    check(app.installedApplicationsError_.empty() && !app.installedApplications_.empty(),
          "event-driven discovery retrieves Windows applications");
    app.HandleMessage(WM_TIMER, 3, 0);
    check(!app.discoveryPending_ && !app.discoveryAgain_, "the debounce timer does not repeat discovery");

    app.ApplyInstalledApplications({{}, L"Simulated automatic discovery failure"});
    app.ShowLauncher();
    check(app.discoveryPending_, "opening after a failed scan requests bounded catch-up discovery");
    app.ScheduleInstalledApplicationsRefresh();
    app.HandleMessage(WM_TIMER, 3, 0);
    check(Drain(app) && app.installedApplicationsError_.empty(), "a later automatic scan recovers from failure");

    // Exercise the Shell's shared-memory delivery path for the virtual Apps
    // folder as well as the direct scheduling checks above.
    PIDLIST_ABSOLUTE appsFolder = nullptr;
    const HRESULT folderResult = SHGetKnownFolderIDList(FOLDERID_AppsFolder, 0, nullptr, &appsFolder);
    check(SUCCEEDED(folderResult), "the virtual Apps folder can be watched");
    if (SUCCEEDED(folderResult)) {
      SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, appsFolder, nullptr);
      ILFree(appsFolder);
      check(PumpUntil(app, [&] { return app.discoveryChangePending_ || app.discoveryPending_; }),
            "an Apps-folder Shell notification reaches the launcher");
      check(PumpUntil(app, [&] { return !app.discoveryChangePending_ && !app.discoveryPending_ &&
                                      !app.discoveryAgain_; }), "the Shell notification refresh settles");
    }
    if (checkInstallation) {
      check(CheckInstallationNotifications(app, path.parent_path()),
            "real shortcut creation and removal update the catalog through Windows notifications");
    }

    std::ofstream(path, std::ios::trunc)
        << R"({"version":1,"discoverInstalled":false,"applications":[]})";
    app.ReloadCatalog();
    app.ShowLauncher();
    app.ScheduleInstalledApplicationsRefresh();
    app.HandleMessage(WM_TIMER, 3, 0);
    check(!app.discoveryPending_ && !app.discoveryChangePending_ && !app.installedAppsWatcher_->IsRunning() &&
          app.catalog_.applications.empty(), "disabled discovery unsubscribes and ignores queued changes");
    app.HideLauncher();

    // Exercise the production source cache beyond its capacity with independent
    // targets and one deterministic image. Never launch a target or edit user data.
    check(Drain(app), "earlier icon work drains before the cache stress check");
    app.catalog_.applications.clear();
    app.iconSources_.clear();
    for (int index = 0; index < 140; ++index) {
      ApplicationEntry entry;
      entry.name = L"Cache test " + std::to_wstring(index);
      entry.target = L"cache-test-" + std::to_wstring(index) + L".exe";
      entry.icon = iconPath.wstring();
      app.catalog_.applications.push_back(std::move(entry));
    }
    check(SUCCEEDED(app.EnsureRenderTarget()), "cache test render target initializes");
    if (app.renderTarget_) {
      app.GetApplicationIcon(0);
      ++app.iconGeneration_;
      app.pendingIconTargets_.clear();
      app.iconCache_.clear();
      app.iconAttempted_.clear();
      check(Drain(app), "old-generation icon job drains");
      check(app.iconSources_.empty(), "late icon results cannot repopulate a refreshed cache");
      for (std::size_t index = 0; index < app.catalog_.applications.size(); ++index) {
        app.GetApplicationIcon(index);
        if (!Drain(app)) { check(false, "cache stress job completes"); break; }
        check(app.GetApplicationIcon(index) != nullptr, "decoded source creates a usable Direct2D bitmap");
        check(app.iconSources_.size() <= 128, "source cache remains bounded across distinct applications");
      }
      const auto firstKey = app.IconKey(app.catalog_.applications[0]);
      const auto retainedKey = app.IconKey(app.catalog_.applications[12]);
      const auto nextOldestKey = app.IconKey(app.catalog_.applications[13]);
      check(app.iconSources_.size() == 128 && !app.iconSources_.contains(firstKey),
            "140 distinct icons evict old sources at the 128-entry capacity");
      app.GetApplicationIcon(12);  // Touch the oldest remaining source.
      app.GetApplicationIcon(0);   // Reload an evicted source.
      check(Drain(app), "an evicted icon can be loaded again");
      check(app.iconSources_.contains(firstKey) && app.iconSources_.contains(retainedKey) &&
            !app.iconSources_.contains(nextOldestKey), "eviction respects recent use, not insertion order");
      app.HideLauncher();
      check(!app.renderTarget_ && app.iconCache_.empty() && app.iconSources_.size() == 128,
            "hide releases device resources and retains only the bounded source cache");
      check(SUCCEEDED(app.EnsureRenderTarget()), "render target can be recreated after hiding");
      if (app.renderTarget_) {
        check(app.GetApplicationIcon(0) != nullptr && !app.iconTasks_->HasPending(),
              "reopening reuses cached pixels without another worker job");
      }
    }
    check(app.HandleMessage(kMessageEnterShellMode, 0, 0) == 1, "shell-mode handoff is acknowledged");
    check(!IsWindowVisible(app.window_) && !app.trayIconAdded_, "shell-mode handoff hides the launcher and removes its tray icon");
    app.ShowLauncher();
    check(IsWindowVisible(app.window_), "an explicit open still shows the shell-mode launcher");
    app.HideLauncher();
    app.HandleMessage(app.taskbarCreatedMessage_, 0, 0);
    check(!app.trayIconAdded_, "Explorer recreation does not add a shell-mode tray icon");
    app.ShowNotification(L"Test notice", L"Visible without Explorer");
    check(IsWindowVisible(app.window_) && app.launchError_.find(L"Visible without Explorer") != std::wstring::npos,
          "tray-free notifications appear in the launcher");
    std::atomic<bool> menuSeen = false;
    std::thread menuObserver([&] {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (std::chrono::steady_clock::now() < deadline) {
        HWND menu = FindWindowW(L"#32768", nullptr);
        DWORD pid = 0;
        if (menu) GetWindowThreadProcessId(menu, &pid);
        if (pid == GetCurrentProcessId()) { menuSeen = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      PostMessageW(app.window_, WM_CANCELMODE, 0, 0);
    });
    SendMessageW(app.edit_, WM_SYSKEYDOWN, VK_F10, 0);
    menuObserver.join();
    check(menuSeen, "F10 opens the real launcher menu without a tray");
    failures += CheckApplicationLaunches(app, path.parent_path());
    failures += CheckMainMenu(app);

    DestroyWindow(app.window_);
    check(!app.iconTasks_->HasPending() && !app.discoveryTasks_->HasPending(),
          "window destruction cancels its background queues");
    check(!app.trayIconAdded_, "window destruction removes the tray icon");
    check(!app.installedAppsWatcher_->IsRunning(), "window destruction releases notification subscriptions");
    if (failures == 0) std::cout << "All quickdial launcher integration checks passed.\n";
    return failures == 0 ? 0 : 1;
  }
};

}  // namespace quickdial

int main(int argc, char** argv) {
  if (argc >= 2 && std::string_view(argv[1]) == "--private-desktop") {
    const std::wstring desktopName = L"BladeLauncherFixture-" + std::to_wstring(GetCurrentProcessId());
    HDESK desktop = CreateDesktopW(desktopName.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop) { std::cerr << "FAILED: private desktop prerequisite " << GetLastError() << '\n'; return 1; }
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
    std::wstring command = L"\"" + std::wstring(executable) + L"\"";
    for (int index = 2; index < argc; ++index) command += L" \"" + std::wstring(winrt::to_hstring(argv[index])) + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    startup.lpDesktop = const_cast<wchar_t*>(desktopName.c_str());
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(executable, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &child)) {
      std::cerr << "FAILED: fixture process creation " << GetLastError() << '\n'; CloseDesktop(desktop); return 1;
    }
    CloseHandle(child.hThread);
    const auto wait = WaitForSingleObject(child.hProcess, 90000);
    DWORD result = 1;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(child.hProcess, &result);
    else { TerminateProcess(child.hProcess, 1); WaitForSingleObject(child.hProcess, 5000); std::cerr << "FAILED: private fixture timed out\n"; }
    CloseHandle(child.hProcess); CloseDesktop(desktop);
    return static_cast<int>(result);
  }
  if (argc >= 2 && std::string_view(argv[1]) == "--launched-fixture") {
    int count = 0;
    PWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments || count != 5) { if (arguments) LocalFree(arguments); return 2; }
    std::ofstream marker(std::filesystem::path(arguments[2]), std::ios::binary);
    marker << winrt::to_string(arguments[3]) << '\n' << winrt::to_string(arguments[4]) << '\n';
    LocalFree(arguments);
    return marker ? 0 : 3;
  }
  if (const auto helperResult = quickdial::RunDiscoveryHelperIfRequested()) return *helperResult;
  winrt::init_apartment(winrt::apartment_type::single_threaded);
  if (argc == 4 && std::string_view(argv[1]) == "--diagnostic-fixture") {
    const auto target = reinterpret_cast<HWND>(std::stoull(argv[2]));
    return quickdial::RunLauncherDiagnostic(quickdial::kSearchRequest, L"restart",
        std::filesystem::path(winrt::to_hstring(argv[3]).c_str()), target);
  }
  const auto directory = std::filesystem::temp_directory_path() /
      (L"QuickDialLauncherTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
  std::filesystem::create_directories(directory);
  const auto path = directory / L"apps.json";
  const auto iconPath = directory / L"icon.bmp";
  BITMAPFILEHEADER header{};
  BITMAPINFOHEADER info{};
  header.bfType = 0x4d42;
  header.bfOffBits = sizeof(header) + sizeof(info);
  header.bfSize = header.bfOffBits + 32 * 32 * 4;
  info.biSize = sizeof(info);
  info.biWidth = info.biHeight = 32;
  info.biPlanes = 1;
  info.biBitCount = 32;
  info.biCompression = BI_RGB;
  {
    std::ofstream bitmap(iconPath, std::ios::binary);
    bitmap.write(reinterpret_cast<const char*>(&header), sizeof(header));
    bitmap.write(reinterpret_cast<const char*>(&info), sizeof(info));
    const std::vector<std::uint32_t> pixels(32 * 32, 0xff336699);
    bitmap.write(reinterpret_cast<const char*>(pixels.data()), pixels.size() * sizeof(pixels[0]));
    if (!bitmap) { std::cerr << "FAILED: cache test image creation\n"; return 1; }
  }
  std::ofstream(path) << R"({"version":1,"discoverInstalled":false,"applications":[)"
                     << R"({"name":"Test Alpha","target":"alpha.exe"},)"
                     << R"({"name":"Test Bravo","target":"bravo.exe"}]})";
  const bool checkInstallation = argc == 2 && std::string_view(argv[1]) == "--check-installation";
  const bool checkLaunching = argc == 2 && std::string_view(argv[1]) == "--check-launching";
  const bool checkMainMenu = argc >= 2 && std::string_view(argv[1]) == "--check-main-menu";
  const int result = checkInstallation ? quickdial::LauncherAppTestAccess::RunDiscoveryChecks(path) : checkMainMenu ? quickdial::LauncherAppTestAccess::RunMainMenuChecks(path, argc == 3 ? std::filesystem::path(winrt::to_hstring(argv[2]).c_str()) : std::filesystem::path{}) : checkLaunching ? quickdial::LauncherAppTestAccess::RunLaunchChecks(path)
                                   : quickdial::LauncherAppTestAccess::Run(path, iconPath, checkInstallation);
  std::filesystem::remove(path);
  std::filesystem::remove(iconPath);
  std::filesystem::remove(directory);
  return result;
}
