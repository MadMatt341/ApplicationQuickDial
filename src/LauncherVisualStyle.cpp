#include "LauncherVisualStyle.h"

#include <array>
#include <string_view>
#include <wrl/client.h>

namespace quickdial::visuals {
namespace {

D2D1_COLOR_F Color(unsigned int rgb, float alpha = 1.0f) {
  return D2D1::ColorF(
      static_cast<float>((rgb >> 16) & 0xFF) / 255.0f,
      static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
      static_cast<float>(rgb & 0xFF) / 255.0f,
      alpha);
}

}  // namespace

Palette GetPalette(bool lightTheme) {
  if (lightTheme) {
    return {
        Color(0xFAFAFA), Color(0x1F2328), Color(0x68707C),
        Color(0xE8EDF5), Color(0xE1E4E8), Color(0xA4262C),
        RGB(250, 250, 250), RGB(31, 35, 40),
    };
  }

  return {
      Color(0x1E1F22), Color(0xF2F3F5), Color(0x9DA3AE),
      Color(0x2B313B), Color(0x34373D), Color(0xFF99A4),
      RGB(30, 31, 34), RGB(242, 243, 245),
  };
}

std::wstring ResolveFontFamily(IDWriteFactory* dwriteFactory) {
  // Prefer Nerd Font family names from current and older releases, then
  // progressively fall back to common Windows monospace fonts.
  constexpr std::array<std::wstring_view, 6> candidates = {
      L"JetBrainsMono Nerd Font Mono",
      L"JetBrainsMono Nerd Font",
      L"JetBrainsMono NFM",
      L"JetBrains Mono",
      L"Cascadia Mono",
      L"Consolas",
  };

  if (dwriteFactory == nullptr) {
    return std::wstring(candidates.back());
  }

  Microsoft::WRL::ComPtr<IDWriteFontCollection> fonts;
  if (FAILED(dwriteFactory->GetSystemFontCollection(&fonts, FALSE))) {
    return std::wstring(candidates.back());
  }

  for (const std::wstring_view candidate : candidates) {
    UINT32 familyIndex = 0;
    BOOL exists = FALSE;
    if (SUCCEEDED(fonts->FindFamilyName(candidate.data(), &familyIndex, &exists)) && exists) {
      return std::wstring(candidate);
    }
  }
  return std::wstring(candidates.back());
}

}  // namespace quickdial::visuals
