#include "StartupManager.h"

#include <windows.h>

#include <vector>

namespace quickdial {
namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"ApplicationQuickDial";

std::wstring QuotedExecutablePath() {
  return L"\"" + GetExecutablePath() + L"\"";
}

std::wstring ErrorMessage(DWORD error) {
  wchar_t* buffer = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
  std::wstring result = length > 0 ? std::wstring(buffer, length) : L"Unknown Windows error";
  if (buffer != nullptr) {
    LocalFree(buffer);
  }
  while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) {
    result.pop_back();
  }
  return result;
}

}  // namespace

std::wstring GetExecutablePath() {
  std::vector<wchar_t> buffer(512);
  while (true) {
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) {
      return {};
    }
    if (length < buffer.size() - 1) {
      return std::wstring(buffer.data(), length);
    }
    buffer.resize(buffer.size() * 2);
  }
}

bool IsStartWithWindowsEnabled() {
  DWORD type = 0;
  DWORD size = 0;
  LONG result = RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, &type, nullptr, &size);
  if (result != ERROR_SUCCESS || size < sizeof(wchar_t)) {
    return false;
  }

  std::wstring value(size / sizeof(wchar_t), L'\0');
  result = RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, &type, value.data(), &size);
  if (result != ERROR_SUCCESS) {
    return false;
  }
  if (!value.empty() && value.back() == L'\0') {
    value.pop_back();
  }
  return _wcsicmp(value.c_str(), QuotedExecutablePath().c_str()) == 0;
}

bool SetStartWithWindowsEnabled(bool enabled, std::wstring& error) {
  if (!enabled) {
    const LONG result = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue);
    if (result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND) {
      return true;
    }
    error = L"Could not remove the sign-in entry: " + ErrorMessage(result);
    return false;
  }

  const std::wstring value = QuotedExecutablePath();
  const LONG result = RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, REG_SZ, value.c_str(),
                                     static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  if (result != ERROR_SUCCESS) {
    error = L"Could not create the sign-in entry: " + ErrorMessage(result);
    return false;
  }
  return true;
}

}  // namespace quickdial
