#pragma once

#include "Catalog.h"
#include "IconLoader.h"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace quickdial {

class HookManager;
class BackgroundTasks;
struct InstalledAppsResult;

inline constexpr wchar_t kWindowClassName[] = L"ApplicationQuickDial.LauncherWindow";

class LauncherApp {
 public:
  explicit LauncherApp(HANDLE benchmarkPresentedEvent = nullptr, std::filesystem::path catalogPath = {});
  ~LauncherApp();

  LauncherApp(const LauncherApp&) = delete;
  LauncherApp& operator=(const LauncherApp&) = delete;

  bool Initialize(HINSTANCE instance);
  int Run();

 private:
  friend class LauncherAppTestAccess;
  static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
  static LRESULT CALLBACK EditProcedure(
      HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR referenceData);

  LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
  bool CreateFactories();
  bool CreateMainWindow();
  void ConfigureWindowAppearance();
  void ApplyTheme();
  void LayoutEditControl();
  void ResizeAndPosition(bool chooseMonitor);

  void AddTrayIcon();
  void RemoveTrayIcon();
  void ShowTrayMenu();
  void HandleTrayCommand(UINT command);
  void ShowNotification(std::wstring_view title, std::wstring_view message);

  void ShowLauncher();
  void HideLauncher();
  void ToggleLauncher();
  void ReloadCatalog(bool refreshInstalledApplications = false);
  void RebuildCatalog();
  void RequestInstalledApplications();
  void ApplyInstalledApplications(InstalledAppsResult installed);
  void ProcessBackgroundResults();
  void StartBackgroundTimer();
  void StopBackgroundTasks();
  void UpdateResults();
  void MoveSelection(int delta);
  void LaunchSelection();
  bool LaunchApplication(const ApplicationEntry& application, std::wstring& error);
  void OpenCatalog();

  HRESULT EnsureRenderTarget();
  void ReleaseDeviceResources();
  void Render();
  Microsoft::WRL::ComPtr<ID2D1Bitmap> GetApplicationIcon(std::size_t applicationIndex);
  std::wstring IconKey(const ApplicationEntry& application) const;

  HINSTANCE instance_ = nullptr;
  HWND window_ = nullptr;
  HWND edit_ = nullptr;
  HMONITOR anchorMonitor_ = nullptr;
  UINT dpi_ = 96;
  HFONT editFont_ = nullptr;
  HBRUSH editBackgroundBrush_ = nullptr;
  std::wstring fontFamily_;
  bool lightTheme_ = false;
  bool trayIconAdded_ = false;
  UINT taskbarCreatedMessage_ = 0;
  HANDLE benchmarkPresentedEvent_ = nullptr;

  std::filesystem::path catalogPath_;
  Catalog catalog_;
  Catalog configuredCatalog_;
  std::vector<ApplicationEntry> installedApplications_;
  bool discoveryRequested_ = false;
  bool discoveryPending_ = false;
  bool discoveryAgain_ = false;
  bool notifyAfterDiscovery_ = false;
  std::wstring installedApplicationsError_;
  std::wstring catalogFileError_;
  bool hasValidCatalog_ = false;
  std::wstring catalogError_;
  std::wstring launchError_;
  std::vector<std::size_t> results_;
  std::size_t selectedResult_ = 0;

  std::unique_ptr<HookManager> hookManager_;
  std::unique_ptr<BackgroundTasks> iconTasks_;
  std::unique_ptr<BackgroundTasks> discoveryTasks_;

  Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory_;
  Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory_;
  Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> renderTarget_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> resultTextFormat_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> messageTextFormat_;
  std::vector<Microsoft::WRL::ComPtr<ID2D1Bitmap>> iconCache_;
  std::vector<bool> iconAttempted_;
  struct CachedIcon {
    IconImage image;
    std::size_t lastUse = 0;
  };
  std::unordered_map<std::wstring, CachedIcon> iconSources_;
  std::unordered_set<std::wstring> pendingIconTargets_;
  std::size_t iconGeneration_ = 0;
  std::size_t iconUseCounter_ = 0;
};

}  // namespace quickdial
