#include "LauncherApp.h"
#include "InstalledApps.h"
#include "BackgroundTasks.h"
#include "AppMessages.h"
#include "DiscoveryProcess.h"
#include "InstalledAppsWatcher.h"

#include <shellapi.h>
#include <shlobj.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
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
          app.installedApplicationsError_.empty();
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
    check(app.results_.size() == 2, "manual applications are immediately available");

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

    // Opening and idle timer messages must never poll. Only an app-change
    // event should queue the bounded discovery helper, even while hidden.
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
    check(!app.discoveryPending_, "opening after a failed scan does not resume polling");
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
  if (const auto helperResult = quickdial::RunDiscoveryHelperIfRequested()) return *helperResult;
  winrt::init_apartment(winrt::apartment_type::single_threaded);
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
  const int result = quickdial::LauncherAppTestAccess::Run(path, iconPath, checkInstallation);
  std::filesystem::remove(path);
  std::filesystem::remove(iconPath);
  std::filesystem::remove(directory);
  return result;
}
