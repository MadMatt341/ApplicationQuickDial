#pragma once
#include <windows.h>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace quickdial {
inline constexpr UINT kMessageDeliverDiagnostics = WM_APP + 54;
inline constexpr ULONG_PTR kStatusRequest = 0x424C5301;
inline constexpr ULONG_PTR kLookupRequest = 0x424C5302;
inline constexpr ULONG_PTR kSearchRequest = 0x424C5303;
inline constexpr ULONG_PTR kStatusResponse = 0x424C53FF;
inline constexpr std::size_t kMaximumStatusBytes = 4 * 1024 * 1024;
bool ReadDiagnosticRequest(const COPYDATASTRUCT* data, std::wstring& query);
bool SendDiagnosticResponse(HWND destination, HWND source, std::wstring_view report);
int RunLauncherDiagnostic(ULONG_PTR mode, std::wstring_view query, const std::filesystem::path& output,
                          HWND target = nullptr);
}
