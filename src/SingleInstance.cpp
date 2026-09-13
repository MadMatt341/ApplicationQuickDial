#include "SingleInstance.h"

#include <vector>

namespace quickdial {
namespace {

bool ObjectName(HANDLE object, std::wstring& name, std::wstring& error) {
  DWORD bytes = 0;
  GetUserObjectInformationW(object, UOI_NAME, nullptr, 0, &bytes);
  if (bytes == 0) {
    error = L"Could not identify the Windows desktop (Windows error " + std::to_wstring(GetLastError()) + L").";
    return false;
  }
  std::vector<wchar_t> buffer((bytes + sizeof(wchar_t) - 1) / sizeof(wchar_t));
  if (!GetUserObjectInformationW(object, UOI_NAME, buffer.data(), bytes, &bytes)) {
    error = L"Could not read the Windows desktop name (Windows error " + std::to_wstring(GetLastError()) + L").";
    return false;
  }
  name = buffer.data();
  return true;
}

InstanceStart Forward(HWND window, UINT message, std::wstring& error) {
  DWORD_PTR result = 0;
  SetLastError(ERROR_SUCCESS);
  if (SendMessageTimeoutW(window, message, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result)) {
    return InstanceStart::Forwarded;
  }
  error = L"The existing Quick Dial window could not be opened (Windows error " +
      std::to_wstring(GetLastError()) + L"). Close that instance and try again.";
  return InstanceStart::Failed;
}

}  // namespace

SingleInstance::~SingleInstance() {
  if (owned_) ReleaseMutex(mutex_);
  if (mutex_) CloseHandle(mutex_);
}

InstanceStart SingleInstance::Start(const wchar_t* windowClass, UINT showMessage, std::wstring& error) {
  error.clear();
  std::wstring station;
  std::wstring desktop;
  if (!ObjectName(GetProcessWindowStation(), station, error) ||
      !ObjectName(GetThreadDesktop(GetCurrentThreadId()), desktop, error)) {
    return InstanceStart::Failed;
  }

  // FindWindow searches only this desktop. Match that scope instead of letting
  // an invisible process elsewhere in the session block the interactive copy.
  // The length prefix keeps station/desktop name pairs unambiguous.
  const std::wstring name = L"Local\\ApplicationQuickDial.SingleInstance." +
      std::to_wstring(station.size()) + L":" + station + L":" + desktop;
  mutex_ = CreateMutexW(nullptr, TRUE, name.c_str());
  const DWORD creationError = GetLastError();
  if (!mutex_) {
    error = L"Could not establish the Quick Dial instance for this desktop (Windows error " +
        std::to_wstring(creationError) + L").";
    return InstanceStart::Failed;
  }
  owned_ = creationError != ERROR_ALREADY_EXISTS;

  const ULONGLONG deadline = GetTickCount64() + 2000;
  for (;;) {
    // Also recognize an older version using the former session-wide mutex.
    if (HWND existing = FindWindowW(windowClass, nullptr)) return Forward(existing, showMessage, error);
    if (owned_) return InstanceStart::Primary;

    const DWORD wait = WaitForSingleObject(mutex_, 25);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
      owned_ = true;
    } else if (wait == WAIT_FAILED) {
      error = L"Could not wait for the existing Quick Dial instance (Windows error " +
          std::to_wstring(GetLastError()) + L").";
      return InstanceStart::Failed;
    } else if (GetTickCount64() >= deadline) {
      error = L"Quick Dial is already starting on this desktop, but its window is not ready. Try again shortly; "
              L"if the problem persists, close that instance and restart Quick Dial.";
      return InstanceStart::Failed;
    }
  }
}

}  // namespace quickdial
