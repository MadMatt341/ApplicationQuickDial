#pragma once

#include "HotkeyState.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

namespace quickdial {

enum class InputTracePhase { HookReady, MouseHookReady, Key, ToggleReceived, ShowRequested, ShowCompleted, Hide, Deactivate, MouseButton };

struct InputTraceEvent {
  InputTracePhase phase = InputTracePhase::Key;
  long long qpc = 0;
  HWND foreground = nullptr, target = nullptr, focus = nullptr;
  DWORD key = 0, error = 0;
  bool down = false, injected = false, asyncSampled = false, accepted = false;
  SHORT asyncLeft = 0, asyncRight = 0;
  unsigned before = 0, after = 0;
  int disposition = -1, maskSent = -1, posted = -1;
};

// A temporary observer. Hook callbacks only offer fixed-size records to a
// bounded queue. Its worker owns serialization, file I/O and focus sampling.
class StartupInputTrace {
 public:
  static std::unique_ptr<StartupInputTrace> StartFromEnvironment() noexcept;
  ~StartupInputTrace();
  bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }
  static constexpr UINT kStoppedMessage = WM_APP + 12;
  void SetHookThread(DWORD thread) noexcept;
  void Record(InputTraceEvent event) noexcept;
  void Record(InputTracePhase phase, HWND target = nullptr, bool accepted = false, HWND focus = nullptr) noexcept;
  void Stop() noexcept;

 private:
  StartupInputTrace() = default;
  void Run() noexcept;
  std::filesystem::path path_;
  long long origin_ = 0, frequency_ = 0;
  HANDLE file_ = INVALID_HANDLE_VALUE, stop_ = nullptr, wake_ = nullptr;
  std::thread worker_;
  std::atomic_bool enabled_ = false;
  std::atomic<DWORD> hookThread_ = 0;
  std::atomic_uint dropped_ = 0;
  std::mutex mutex_;
  std::array<InputTraceEvent, 256> queue_{};
  std::size_t count_ = 0;
};

}  // namespace quickdial
