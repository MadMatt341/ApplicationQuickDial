#include "HookManager.h"

#include "AppMessages.h"
#include "StartupInputTrace.h"

namespace quickdial {
namespace {

constexpr WORD kWindowsMenuMaskKey = 0xE8;  // Unassigned by Windows.

UINT MaskWindowsKeyTap() {
  // Space is deliberately swallowed, so Explorer would otherwise see only a
  // Windows-key down/up pair and open Start. Sending an unassigned virtual key
  // while Windows is still held marks it as a chord without invoking a real
  // function-key shortcut or disturbing ordinary Windows shortcuts.
  INPUT inputs[2]{};
  inputs[0].type = INPUT_KEYBOARD;
  inputs[0].ki.wVk = kWindowsMenuMaskKey;
  inputs[1] = inputs[0];
  inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
  return SendInput(static_cast<UINT>(sizeof(inputs) / sizeof(inputs[0])), inputs, sizeof(INPUT));
}

}  // namespace

HookManager* HookManager::instance_ = nullptr;

HookManager::HookManager(HWND notificationWindow, StartupInputTrace* inputTrace)
    : notificationWindow_(notificationWindow), inputTrace_(inputTrace) {}

HookManager::~HookManager() {
  Stop();
}

bool HookManager::Start() {
  if (thread_.joinable()) {
    return hook_ != nullptr;
  }

  {
    std::scoped_lock lock(mutex_);
    ready_ = false;
    error_ = ERROR_SUCCESS;
  }

  thread_ = std::thread(&HookManager::ThreadMain, this);
  std::unique_lock lock(mutex_);
  condition_.wait(lock, [this] { return ready_; });
  return hook_ != nullptr;
}

void HookManager::Stop() {
  if (!thread_.joinable()) {
    return;
  }
  if (threadId_ != 0) {
    PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
  }
  thread_.join();
  threadId_ = 0;
}

void HookManager::ThreadMain() {
  threadId_ = GetCurrentThreadId();

  MSG message{};
  PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
  instance_ = this;
  hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, HookProcedure, GetModuleHandleW(nullptr), 0);
  if (hook_ == nullptr) {
    error_ = GetLastError();
  }
  if (inputTrace_) {
    InputTraceEvent event; event.phase = InputTracePhase::HookReady;
    event.accepted = hook_ != nullptr; event.error = error_;
    inputTrace_->Record(event);
  }
  HHOOK mouse = nullptr;
  if (inputTrace_ && inputTrace_->enabled()) {
    inputTrace_->SetHookThread(threadId_);
    mouse = SetWindowsHookExW(WH_MOUSE_LL, MouseProcedure, GetModuleHandleW(nullptr), 0);
    InputTraceEvent event; event.phase = InputTracePhase::MouseHookReady;
    event.accepted = mouse != nullptr; event.error = mouse ? ERROR_SUCCESS : GetLastError();
    inputTrace_->Record(event);
  }

  {
    std::scoped_lock lock(mutex_);
    ready_ = true;
  }
  condition_.notify_one();

  if (hook_ != nullptr) {
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
      if (message.message == StartupInputTrace::kStoppedMessage) {
        if (mouse) { UnhookWindowsHookEx(mouse); mouse = nullptr; }
        continue;
      }
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    UnhookWindowsHookEx(hook_);
    hook_ = nullptr;
  }
  if (mouse) UnhookWindowsHookEx(mouse);
  if (inputTrace_) inputTrace_->SetHookThread(0);
  instance_ = nullptr;
}

LRESULT CALLBACK HookManager::MouseProcedure(int code, WPARAM message, LPARAM data) {
  if (code >= HC_ACTION && instance_ && instance_->inputTrace_ && instance_->inputTrace_->enabled() &&
      (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN)) {
    const auto* mouse = reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
    if (!(mouse->flags & LLMHF_INJECTED)) {
      InputTraceEvent event; event.phase = InputTracePhase::MouseButton; event.key = static_cast<DWORD>(message);
      instance_->inputTrace_->Record(event);
    }
  }
  return CallNextHookEx(nullptr, code, message, data);
}

LRESULT CALLBACK HookManager::HookProcedure(int code, WPARAM wParam, LPARAM lParam) {
  if (code < 0 || instance_ == nullptr) {
    return CallNextHookEx(nullptr, code, wParam, lParam);
  }

  const auto* event = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
  const bool keyDown = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
  const bool keyUp = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
  if (!keyDown && !keyUp) {
    return CallNextHookEx(nullptr, code, wParam, lParam);
  }

  const bool injected = (event->flags & LLKHF_INJECTED) != 0;
  auto* trace = instance_->inputTrace_;
  const bool tracing = trace && trace->enabled() &&
      (event->vkCode == VK_LWIN || event->vkCode == VK_RWIN || event->vkCode == VK_SPACE);
  InputTraceEvent observation;
  if (tracing) {
    LARGE_INTEGER observed{}; QueryPerformanceCounter(&observed); observation.qpc = observed.QuadPart;
    observation.foreground = GetForegroundWindow();
    observation.key = event->vkCode; observation.down = keyDown; observation.injected = injected;
    observation.before = instance_->state_.TraceBits();
  }
  if (!injected && event->vkCode == VK_SPACE) {
    // Recover from Windows keyups missed across desktop changes or other hooks.
    // Query on Space, not on the Windows event itself: asynchronous key state is
    // updated only after its low-level hook callback. Zero also safely disarms
    // the shortcut when Windows cannot expose the key state on this desktop.
    const auto left = GetAsyncKeyState(VK_LWIN), right = GetAsyncKeyState(VK_RWIN);
    if (tracing) { observation.asyncSampled = true; observation.asyncLeft = left; observation.asyncRight = right; }
    instance_->state_.ClearReleasedWindowsKeys((left & 0x8000) != 0, (right & 0x8000) != 0);
  }
  const HotkeyDisposition disposition = instance_->state_.Handle(event->vkCode, keyDown, injected);
  if (disposition == HotkeyDisposition::TriggerAndSuppress) {
    const auto sent = MaskWindowsKeyTap();
    const auto maskError = sent == 2 ? ERROR_SUCCESS : GetLastError();
    const auto posted = PostMessageW(instance_->notificationWindow_, kMessageToggleLauncher, 0, 0);
    if (tracing) { observation.maskSent = static_cast<int>(sent); observation.posted = posted != FALSE;
      observation.error = posted ? maskError : GetLastError(); }
  }
  if (tracing) { observation.after = instance_->state_.TraceBits(); observation.disposition = static_cast<int>(disposition); trace->Record(observation); }
  if (disposition != HotkeyDisposition::Pass) {
    return 1;
  }
  return CallNextHookEx(nullptr, code, wParam, lParam);
}

}  // namespace quickdial
