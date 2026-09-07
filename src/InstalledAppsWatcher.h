#pragma once

#include <windows.h>

#include <array>
#include <string>
#include <span>

namespace quickdial {

// Window-thread change subscriptions only: no enumeration, polling, or worker.
class InstalledAppsWatcher {
 public:
  InstalledAppsWatcher() = default;
  ~InstalledAppsWatcher();
  InstalledAppsWatcher(const InstalledAppsWatcher&) = delete;
  InstalledAppsWatcher& operator=(const InstalledAppsWatcher&) = delete;
  bool Start(HWND window, UINT message, std::wstring& error);
  bool Stop();
  bool IsRunning() const noexcept { return running_; }
  std::span<const HANDLE> DirectoryHandles() const noexcept;
  bool ConsumeDirectoryChange(std::size_t index, std::wstring& error);
  static bool ConsumeNotification(WPARAM wParam, LPARAM lParam, std::wstring& error);

 private:
  std::array<ULONG, 2> registrations_{};
  std::array<HANDLE, 2> directories_{};
  bool running_ = false;
};

}  // namespace quickdial
