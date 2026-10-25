#pragma once

#include "HotkeyState.h"

#include <windows.h>

#include <condition_variable>
#include <mutex>
#include <thread>

namespace quickdial {
class StartupInputTrace;

class HookManager {
 public:
  explicit HookManager(HWND notificationWindow, StartupInputTrace* inputTrace = nullptr);
  ~HookManager();

  HookManager(const HookManager&) = delete;
  HookManager& operator=(const HookManager&) = delete;

  bool Start();
  void Stop();
  DWORD error() const noexcept { return error_; }

 private:
  static LRESULT CALLBACK HookProcedure(int code, WPARAM wParam, LPARAM lParam);
  static LRESULT CALLBACK MouseProcedure(int code, WPARAM message, LPARAM data);
  void ThreadMain();

  static HookManager* instance_;

  HWND notificationWindow_ = nullptr;
  StartupInputTrace* inputTrace_ = nullptr;
  HHOOK hook_ = nullptr;
  DWORD threadId_ = 0;
  DWORD error_ = ERROR_SUCCESS;
  HotkeyState state_;
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable condition_;
  bool ready_ = false;
};

}  // namespace quickdial
