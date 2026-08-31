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

 private:
  bool leftWindowsDown_ = false;
  bool rightWindowsDown_ = false;
  bool chordActive_ = false;
};

}  // namespace quickdial
