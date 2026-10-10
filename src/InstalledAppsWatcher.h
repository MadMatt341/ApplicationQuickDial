#pragma once

#include <windows.h>

#include <array>
#include <string>
#include <span>
#include <vector>
#include <filesystem>

namespace quickdial {

// Window-thread change subscriptions only: no enumeration, polling, or worker.
class InstalledAppsWatcher {
 public:
  InstalledAppsWatcher() = default;
  ~InstalledAppsWatcher();
  InstalledAppsWatcher(const InstalledAppsWatcher&) = delete;
  InstalledAppsWatcher& operator=(const InstalledAppsWatcher&) = delete;
  // Optional paths let native fixtures own both watched trees.
  bool Start(HWND window, UINT message, std::wstring& error,
             std::span<const std::filesystem::path> directoryPaths = {});
  bool Stop();
  bool IsRunning() const noexcept { return running_; }
  std::span<const HANDLE> DirectoryHandles() const noexcept;
  bool ConsumeDirectoryChange(std::size_t index, std::wstring& error);
  static bool ConsumeNotification(WPARAM wParam, LPARAM lParam, std::wstring& error);

 private:
  std::array<ULONG, 2> registrations_{};
  std::vector<HANDLE> directories_;
  bool running_ = false;
  std::wstring error_;
};

}  // namespace quickdial
