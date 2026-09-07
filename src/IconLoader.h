#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace quickdial {

struct IconImage {
  unsigned int width = 0;
  unsigned int height = 0;
  std::vector<std::uint8_t> pixels;
};

// Returns bounded, premultiplied BGRA pixels with no apartment/device ownership.
// Must be called off the UI thread; an empty image uses the UI's placeholder.
IconImage LoadApplicationIcon(
    const std::wstring& target, const std::optional<std::wstring>& icon, unsigned int size);

}  // namespace quickdial
