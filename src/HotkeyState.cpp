#include "HotkeyState.h"

#include <windows.h>

namespace quickdial {

void HotkeyState::ClearReleasedWindowsKeys(bool leftDown, bool rightDown) noexcept {
  // Only clear stale state: an injected Windows press must never arm the shortcut.
  leftWindowsDown_ = leftWindowsDown_ && leftDown;
  rightWindowsDown_ = rightWindowsDown_ && rightDown;
}

HotkeyDisposition HotkeyState::Handle(std::uint32_t virtualKey, bool keyDown, bool injected) noexcept {
  if (injected) {
    return HotkeyDisposition::Pass;
  }

  if (virtualKey == VK_LWIN) {
    leftWindowsDown_ = keyDown;
    return HotkeyDisposition::Pass;
  }
  if (virtualKey == VK_RWIN) {
    rightWindowsDown_ = keyDown;
    return HotkeyDisposition::Pass;
  }
  if (virtualKey != VK_SPACE) {
    return HotkeyDisposition::Pass;
  }

  if (chordActive_) {
    if (!keyDown) {
      chordActive_ = false;
    }
    return HotkeyDisposition::Suppress;
  }
  if (keyDown && (leftWindowsDown_ || rightWindowsDown_)) {
    chordActive_ = true;
    return HotkeyDisposition::TriggerAndSuppress;
  }
  return HotkeyDisposition::Pass;
}

}  // namespace quickdial
