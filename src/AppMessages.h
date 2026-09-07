#pragma once

#include <windows.h>

namespace quickdial {

inline constexpr UINT kMessageToggleLauncher = WM_APP + 1;
inline constexpr UINT kMessageShowLauncher = WM_APP + 2;
inline constexpr UINT kMessageTray = WM_APP + 3;
inline constexpr UINT kMessageHideLauncher = WM_APP + 4;
inline constexpr UINT kMessageBackgroundComplete = WM_APP + 5;
// Read-only diagnostic, enabled only for launches with benchmark events.
inline constexpr UINT kMessageBenchmarkState = WM_APP + 6;
inline constexpr LRESULT kBenchmarkAvailable = 1;
inline constexpr LRESULT kBenchmarkPending = 2;
inline constexpr LRESULT kBenchmarkFailed = 4;

}  // namespace quickdial
