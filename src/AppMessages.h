#pragma once

#include <windows.h>

namespace quickdial {

inline constexpr UINT kMessageToggleLauncher = WM_APP + 1;
inline constexpr UINT kMessageShowLauncher = WM_APP + 2;
inline constexpr UINT kMessageTray = WM_APP + 3;
inline constexpr UINT kMessageHideLauncher = WM_APP + 4;
inline constexpr UINT kMessageIconLoaded = WM_APP + 5;

}  // namespace quickdial
