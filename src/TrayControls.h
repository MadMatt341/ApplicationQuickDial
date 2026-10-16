#pragma once

#include "IconLoader.h"
#include <array>
#include <cstdint>
#include <stop_token>

namespace quickdial {
struct TrayControl {
  std::uintptr_t host = 0;
  std::string hostSession;
  std::uint32_t token = 0, pid = 0;
  std::wstring name, processPath;
  IconImage image;
  bool SameRegistration(const TrayControl& other) const {
    return host == other.host && hostSession == other.hostSession && token == other.token && pid == other.pid;
  }
};
struct TrayControlsSnapshot {
  std::vector<TrayControl> items;
  std::wstring error;
  bool connected = false;
  std::uintptr_t host = 0;
  std::string hostSession;
  double revision = 0;
};
// Bounded native calls and pixel decoding run on the existing background queue.
TrayControlsSnapshot ReadTrayControls() noexcept;
bool RequestTrayControlMenu(const TrayControl& selected, const std::array<int, 4>& anchor,
                           std::stop_token cancelled, std::wstring& error) noexcept;
}  // namespace quickdial
