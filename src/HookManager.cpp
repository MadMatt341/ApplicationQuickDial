#include "HookManager.h"

#include "AppMessages.h"

namespace quickdial {
namespace {

constexpr WORD kWindowsMenuMaskKey = 0xE8;  // Unassigned by Windows.

void MaskWindowsKeyTap() {
  // Space is deliberately swallowed, so Explorer would otherwise see only a
  // Windows-key down/up pair and open Start. Sending an unassigned virtual key
  // while Windows is still held marks it as a chord without invoking a real
  // function-key shortcut or disturbing ordinary Windows shortcuts.
  INPUT inputs[2]{};
  inputs[0].type = INPUT_KEYBOARD;
  inputs[0].ki.wVk = kWindowsMenuMaskKey;
  inputs[1] = inputs[0];
  inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
  SendInput(static_cast<UINT>(sizeof(inputs) / sizeof(inputs[0])), inputs, sizeof(INPUT));
}

}  // namespace

HookManager* HookManager::instance_ = nullptr;

HookManager::HookManager(HWND notificationWindow) : notificationWindow_(notificationWindow) {}

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

  {
    std::scoped_lock lock(mutex_);
    ready_ = true;
  }
  condition_.notify_one();

  if (hook_ != nullptr) {
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    UnhookWindowsHookEx(hook_);
    hook_ = nullptr;
  }
  instance_ = nullptr;
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
  if (!injected && event->vkCode == VK_SPACE) {
    // Recover from Windows keyups missed across desktop changes or other hooks.
    // Query on Space, not on the Windows event itself: asynchronous key state is
    // updated only after its low-level hook callback. Zero also safely disarms
    // the shortcut when Windows cannot expose the key state on this desktop.
    instance_->state_.ClearReleasedWindowsKeys(
        (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0,
        (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0);
  }
  const HotkeyDisposition disposition = instance_->state_.Handle(event->vkCode, keyDown, injected);
  if (disposition == HotkeyDisposition::TriggerAndSuppress) {
    MaskWindowsKeyTap();
    PostMessageW(instance_->notificationWindow_, kMessageToggleLauncher, 0, 0);
  }
  if (disposition != HotkeyDisposition::Pass) {
    return 1;
  }
  return CallNextHookEx(nullptr, code, wParam, lParam);
}

}  // namespace quickdial
