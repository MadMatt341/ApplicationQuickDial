#include "InstalledAppsWatcher.h"

#include <shlobj.h>
#include <algorithm>

namespace quickdial {
namespace {

constexpr LONG kFolderEvents = SHCNE_CREATE | SHCNE_DELETE | SHCNE_RENAMEITEM |
    SHCNE_MKDIR | SHCNE_RMDIR | SHCNE_RENAMEFOLDER | SHCNE_UPDATEDIR | SHCNE_UPDATEITEM;

}  // namespace

InstalledAppsWatcher::~InstalledAppsWatcher() {
  if (!Stop()) OutputDebugStringW(L"Launcher: could not release app-change subscriptions.\n");
}

bool InstalledAppsWatcher::Start(HWND window, UINT message, std::wstring& error,
                                std::span<const std::filesystem::path> directoryPaths) {
  error.clear();
  if (!directoryPaths.empty() && directoryPaths.size() != 2) {
    error = L"App-change notifications require both Start-menu paths.";
    return false;
  }
  if (running_ && error_.empty()) return true;
  if (!Stop()) {
    error = L"Could not reset app-change notifications. Restart Launcher.";
    return false;
  }
  PIDLIST_ABSOLUTE folder = nullptr;
  const HRESULT folderResult = SHGetKnownFolderIDList(FOLDERID_AppsFolder, 0, nullptr, &folder);
  if (FAILED(folderResult)) {
    error = L"Could not locate the app-change notification folder (HRESULT " +
        std::to_wstring(static_cast<unsigned long>(folderResult)) + L"). Try Reload app list.";
  } else {
    const SHChangeNotifyEntry entry{folder, TRUE};
    registrations_.front() = SHChangeNotifyRegister(window, SHCNRF_ShellLevel | SHCNRF_NewDelivery,
        kFolderEvents, message, 1, &entry);
    if (registrations_.front() == 0) {
      error = L"Could not listen for application-list changes. Try Reload app list or restart Launcher.";
    }
  }
  if (folder) ILFree(folder);
  {
    // Protocol/association registration is global and has no item PIDL.
    const SHChangeNotifyEntry entry{nullptr, TRUE};
    registrations_.back() = SHChangeNotifyRegister(window, SHCNRF_ShellLevel | SHCNRF_NewDelivery,
        SHCNE_ASSOCCHANGED, message, 1, &entry);
    if (registrations_.back() == 0) {
      error = L"Could not listen for application-registration changes. Try Reload app list or restart Launcher.";
    }
  }
  // Path lookup and kernel notifications avoid loading filesystem Shell
  // extensions merely to watch Start-menu shortcuts. The main message loop
  // waits on these handles, so no polling or extra resident thread is needed.
  const KNOWNFOLDERID* directories[] = {&FOLDERID_StartMenu, &FOLDERID_CommonStartMenu};
  for (std::size_t index = 0; index < std::size(directories); ++index) {
    PWSTR path = nullptr;
    const HRESULT result = directoryPaths.empty() ?
        SHGetKnownFolderPath(*directories[index], 0, nullptr, &path) : S_OK;
    if (FAILED(result)) {
      error = L"Could not locate a Start-menu notification folder (HRESULT " +
          std::to_wstring(static_cast<unsigned long>(result)) + L"). Try Reload app list.";
    } else {
      const HANDLE notification = FindFirstChangeNotificationW(
          directoryPaths.empty() ? path : directoryPaths[index].c_str(), TRUE,
          FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE);
      if (notification == INVALID_HANDLE_VALUE) {
        error = L"Could not listen for Start-menu changes (Windows error " + std::to_wstring(GetLastError()) +
            L"). Try Reload app list.";
      } else {
        directories_.push_back(notification);
      }
    }
    if (path) CoTaskMemFree(path);
  }
  running_ = registrations_.front() != 0 || registrations_.back() != 0 || !directories_.empty();
  error_ = error;
  return running_;
}

bool InstalledAppsWatcher::Stop() {
  running_ = false;
  bool success = true;
  for (auto& registration : registrations_) {
    if (registration != 0) {
      if (SHChangeNotifyDeregister(registration)) registration = 0;
      else success = false;
    }
  }
  for (auto& directory : directories_) {
    if (directory) {
      if (FindCloseChangeNotification(directory)) directory = nullptr;
      else success = false;
    }
  }
  std::erase(directories_, nullptr);
  error_.clear();
  return success;
}

std::span<const HANDLE> InstalledAppsWatcher::DirectoryHandles() const noexcept {
  return running_ ? std::span<const HANDLE>(directories_) : std::span<const HANDLE>{};
}

bool InstalledAppsWatcher::ConsumeDirectoryChange(std::size_t index, std::wstring& error) {
  error.clear();
  if (!running_ || index >= directories_.size()) return false;
  if (!FindNextChangeNotification(directories_[index])) {
    error = L"Could not continue listening for Start-menu changes (Windows error " +
        std::to_wstring(GetLastError()) + L"). Try Reload app list.";
    if (FindCloseChangeNotification(directories_[index]))
      directories_.erase(directories_.begin() + static_cast<std::ptrdiff_t>(index));
    error_ = error;
    running_ = registrations_.front() != 0 || registrations_.back() != 0 || !directories_.empty();
    return false;
  }
  return true;
}

bool InstalledAppsWatcher::ConsumeNotification(WPARAM wParam, LPARAM lParam, std::wstring& error) {
  error.clear();
  PIDLIST_ABSOLUTE* items = nullptr;
  LONG events = 0;
  HANDLE notification = SHChangeNotification_Lock(
      reinterpret_cast<HANDLE>(wParam), static_cast<DWORD>(lParam), &items, &events);
  if (!notification) {
    error = L"Could not read an app-change notification. Try Reload app list.";
    return false;
  }
  if (!SHChangeNotification_Unlock(notification)) {
    error = L"Could not release an app-change notification. Restart Launcher.";
  }
  return (events & (kFolderEvents | SHCNE_ASSOCCHANGED)) != 0;
}

}  // namespace quickdial
