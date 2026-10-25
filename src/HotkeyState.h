#pragma once

#include <cstdint>

namespace quickdial {

enum class HotkeyDisposition {
  Pass,
  Suppress,
  TriggerAndSuppress,
};

class HotkeyState {
 public:
  HotkeyDisposition Handle(std::uint32_t virtualKey, bool keyDown, bool injected = false) noexcept;
  void ClearReleasedWindowsKeys(bool leftDown, bool rightDown) noexcept;
  unsigned TraceBits() const noexcept {
    return (leftWindowsDown_ ? 1U : 0U) | (rightWindowsDown_ ? 2U : 0U) | (chordActive_ ? 4U : 0U);
  }

 private:
  bool leftWindowsDown_ = false;
  bool rightWindowsDown_ = false;
  bool chordActive_ = false;
};

}  // namespace quickdial
