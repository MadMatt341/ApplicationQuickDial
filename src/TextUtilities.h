#pragma once

#include <windows.h>

#include <limits>
#include <string>
#include <string_view>

namespace quickdial {

inline std::wstring FoldCase(std::wstring_view value) {
  if (value.empty()) {
    return {};
  }
  if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return std::wstring(value);
  }
  const int length = static_cast<int>(value.size());
  const int needed = LCMapStringEx(
      LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(), length, nullptr, 0, nullptr, nullptr, 0);
  if (needed <= 0) {
    return std::wstring(value);
  }
  std::wstring result(static_cast<std::size_t>(needed), L'\0');
  if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(), length,
                    result.data(), needed, nullptr, nullptr, 0) <= 0) {
    return std::wstring(value);
  }
  return result;
}

}  // namespace quickdial
