#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <string>

namespace quickdial::visuals {

// All layout values are device-independent pixels.
inline float kWindowWidth = 540.0f;
inline float kSearchHeight = 52.0f;
inline constexpr float kMenuHeaderHeight = 36.0f;
inline float kResultHeight = 44.0f;
inline constexpr float kBottomPadding = 8.0f;
inline constexpr float kFooterHeight = 24.0f;
inline constexpr float kStatusHeight = 26.0f;

inline constexpr float kSearchEditLeft = 48.0f;
inline constexpr float kSearchEditRight = 64.0f;

inline float kSearchFontSize = 16.0f;
inline float kResultFontSize = 15.0f;
inline float kMessageFontSize = 12.0f;
inline float kHintFontSize = 11.0f;

inline constexpr float kSelectionHorizontalInset = 8.0f;
inline constexpr float kSelectionVerticalInset = 3.0f;
inline float kSelectionCornerRadius = 7.0f;

inline constexpr float kApplicationIconLeft = 18.0f;
inline constexpr float kApplicationIconSize = 20.0f;
inline constexpr float kResultTextLeft = 48.0f;
inline constexpr float kContentRight = 20.0f;

struct Palette {
  D2D1_COLOR_F background;
  D2D1_COLOR_F text;
  D2D1_COLOR_F muted;
  D2D1_COLOR_F selected;
  D2D1_COLOR_F selectedText;
  D2D1_COLOR_F separator;
  D2D1_COLOR_F error;
  COLORREF editBackground;
  COLORREF editText;
};

void Refresh();
Palette GetPalette(bool lightTheme);
std::wstring ResolveFontFamily(IDWriteFactory* dwriteFactory);

}  // namespace quickdial::visuals
