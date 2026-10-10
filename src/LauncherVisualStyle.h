#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <string>

namespace quickdial::visuals {

// All layout values are device-independent pixels.
inline float kWindowWidth = 620.0f;
inline float kSearchHeight = 64.0f;
inline constexpr float kMenuHeaderHeight = 36.0f;
inline float kResultHeight = 50.0f;
inline constexpr float kBottomPadding = 8.0f;
inline constexpr float kStatusHeight = 26.0f;

inline constexpr float kSearchEditLeft = 56.0f;
inline constexpr float kSearchEditTop = 12.0f;
inline constexpr float kSearchEditRight = 20.0f;
inline constexpr float kSearchEditHeight = 40.0f;

inline float kSearchFontSize = 18.0f;
inline float kResultFontSize = 15.0f;
inline float kMessageFontSize = 12.0f;

inline constexpr float kSelectionHorizontalInset = 8.0f;
inline constexpr float kSelectionVerticalInset = 3.0f;
inline float kSelectionCornerRadius = 7.0f;

inline constexpr float kApplicationIconLeft = 18.0f;
inline constexpr float kApplicationIconSize = 28.0f;
inline constexpr float kResultTextLeft = 60.0f;
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
