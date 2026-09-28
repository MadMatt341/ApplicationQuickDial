#include "SystemPower.h"

#include <memory>

namespace quickdial {
namespace {

std::wstring PowerError(std::wstring_view operation, DWORD code) {
  wchar_t* buffer = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
  std::wstring message = std::wstring(operation) + L" (Windows error " + std::to_wstring(code) + L"): ";
  message += length ? std::wstring(buffer, length) : L"Unknown error";
  if (buffer) LocalFree(buffer);
  while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) message.pop_back();
  return message;
}

}  // namespace

bool RequestSystemPowerAction(MenuItemId action, std::wstring& error) {
  error.clear();
  if (action != MenuItemId::ShutDown && action != MenuItemId::Restart) {
    error = L"Unknown system power action.";
    return false;
  }
  HANDLE rawToken = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &rawToken)) {
    error = PowerError(L"Could not obtain the shutdown privilege", GetLastError());
    return false;
  }
  const std::unique_ptr<void, decltype(&CloseHandle)> token(rawToken, CloseHandle);
  TOKEN_PRIVILEGES privilege{};
  privilege.PrivilegeCount = 1;
  privilege.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  if (!LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &privilege.Privileges[0].Luid)) {
    error = PowerError(L"Could not find the shutdown privilege", GetLastError());
    return false;
  }
  TOKEN_PRIVILEGES previous{};
  DWORD previousSize = sizeof(previous);
  SetLastError(ERROR_SUCCESS);
  const BOOL adjusted = AdjustTokenPrivileges(token.get(), FALSE, &privilege, sizeof(previous), &previous, &previousSize);
  const DWORD adjustmentError = GetLastError();
  if (!adjusted || adjustmentError != ERROR_SUCCESS) {
    error = PowerError(L"Could not enable the shutdown privilege", adjustmentError);
    return false;
  }

  // No FORCE flags: Windows handles applications that need to save or block exit.
  const BOOL requested = ExitWindowsEx(action == MenuItemId::Restart ? EWX_REBOOT : EWX_POWEROFF,
      SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
  const DWORD requestError = requested ? ERROR_SUCCESS : GetLastError();
  const BOOL restored = AdjustTokenPrivileges(token.get(), FALSE, &previous, 0, nullptr, nullptr);
  const DWORD restoreError = restored ? ERROR_SUCCESS : GetLastError();
  if (!requested) {
    error = PowerError(L"Windows could not accept the power request", requestError);
  }
  if (!restored) {
    if (!error.empty()) error += L" ";
    error += PowerError(requested ? L"Windows accepted the request, but could not restore the privilege" :
                                   L"Could not restore the shutdown privilege", restoreError);
  }
  return requested != FALSE;
}

}  // namespace quickdial
