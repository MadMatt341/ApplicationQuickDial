#include "DiscoveryProcess.h"

#include "DiscoveryProtocol.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

namespace quickdial {
namespace {

constexpr wchar_t kHelperArgument[] = L"--discover-apps";

struct HandleCloser {
  void operator()(void* handle) const noexcept {
    if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
  }
};
struct ViewCloser {
  void operator()(void* view) const noexcept { if (view) UnmapViewOfFile(view); }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;
using UniqueView = std::unique_ptr<void, ViewCloser>;

struct Attributes {
  std::vector<std::byte> storage;
  LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;
  ~Attributes() { if (list) DeleteProcThreadAttributeList(list); }

  bool Initialize(HANDLE* mapping, HANDLE* job) {
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 2, 0, &size);
    if (!size) return false;
    storage.resize(size);
    auto* candidate = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(candidate, 2, 0, &size)) return false;
    list = candidate;
    return UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                      mapping, sizeof(HANDLE), nullptr, nullptr) &&
           UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                      job, sizeof(HANDLE), nullptr, nullptr);
  }
};

InstalledAppsResult Failure(std::wstring_view operation, DWORD error = GetLastError()) {
  return {{}, std::wstring(operation) + L" (Windows error " + std::to_wstring(error) +
              L"). Try Reload app list."};
}

std::filesystem::path ExecutablePath() {
  std::wstring path(512, L'\0');
  while (path.size() <= 32'768) {
    const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!size) return {};
    if (size < path.size()) { path.resize(size); return path; }
    path.resize(path.size() * 2);
  }
  SetLastError(ERROR_FILENAME_EXCED_RANGE);
  return {};
}

int RunHelper(HANDLE mapping) {
  // A crashed provider must not leave an invisible helper waiting on a dialog.
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  UniqueHandle inheritedMapping(mapping);
  UniqueView view(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, kDiscoveryBufferBytes));
  if (!view) return 2;
  const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  InstalledAppsResult result;
  try {
    result = SUCCEEDED(apartment) ? DiscoverInstalledApplications() :
        Failure(L"Could not initialize Windows app discovery", static_cast<DWORD>(apartment));
  } catch (...) {
    result.error = L"Windows app discovery failed. Try Reload app list.";
    result.applications.clear();
  }
  if (SUCCEEDED(apartment)) CoUninitialize();
  return WriteDiscoveryResult({static_cast<std::byte*>(view.get()), kDiscoveryBufferBytes}, result) ? 0 : 3;
}

}  // namespace

InstalledAppsResult DiscoverInstalledApplicationsIsolated(
    std::stop_token cancellation, const std::filesystem::path& executable, std::chrono::milliseconds timeout) {
  if (cancellation.stop_requested()) return {{}, L"Windows app discovery was cancelled."};
  const auto path = executable.empty() ? ExecutablePath() : executable;
  if (path.empty()) return Failure(L"Could not locate the app discovery helper");
  if (timeout.count() <= 0 || timeout.count() >= INFINITE) {
    return Failure(L"Invalid app discovery timeout", ERROR_INVALID_PARAMETER);
  }

  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  UniqueHandle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0,
                                         static_cast<DWORD>(kDiscoveryBufferBytes), nullptr));
  if (!mapping) return Failure(L"Could not create the app discovery buffer");
  UniqueHandle cancelled(CreateEventW(nullptr, TRUE, FALSE, nullptr));
  if (!cancelled) return Failure(L"Could not monitor app discovery cancellation");
  std::stop_callback cancelCallback(cancellation, [event = cancelled.get()] { SetEvent(event); });

  UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
  if (!job) return Failure(L"Could not supervise the app discovery helper");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
    return Failure(L"Could not configure app discovery cleanup");
  }
  HANDLE mappingHandle = mapping.get(), jobHandle = job.get();
  Attributes attributes;
  if (!attributes.Initialize(&mappingHandle, &jobHandle)) {
    return Failure(L"Could not isolate the app discovery helper");
  }
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
  startup.StartupInfo.wShowWindow = SW_HIDE;
  startup.lpAttributeList = attributes.list;
  std::wstring command = L"\"" + path.wstring() + L"\" " + kHelperArgument + L" " +
      std::to_wstring(reinterpret_cast<std::uintptr_t>(mapping.get()));
  PROCESS_INFORMATION processInfo{};
  // Assignment to the kill-on-close job is atomic with process creation: even
  // parent termination during startup cannot orphan a suspended child.
  if (!CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, TRUE,
                       EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr,
                       &startup.StartupInfo, &processInfo)) {
    return Failure(L"Could not start the app discovery helper");
  }
  UniqueHandle process(processInfo.hProcess), thread(processInfo.hThread);
  const HANDLE waits[] = {cancelled.get(), process.get()};
  const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, static_cast<DWORD>(timeout.count()));
  if (wait != WAIT_OBJECT_0 + 1) {
    const DWORD waitError = GetLastError();
    job.reset();  // Terminate only this request's helper and descendants.
    if (WaitForSingleObject(process.get(), 1000) != WAIT_OBJECT_0) {
      return Failure(L"Could not finish app discovery helper cleanup", ERROR_TIMEOUT);
    }
    if (wait == WAIT_OBJECT_0) return {{}, L"Windows app discovery was cancelled."};
    if (wait == WAIT_TIMEOUT) return {{}, L"Windows app discovery timed out. Try Reload app list."};
    return Failure(L"Could not wait for app discovery", waitError);
  }
  DWORD exitCode = 0;
  if (!GetExitCodeProcess(process.get(), &exitCode)) return Failure(L"Could not read app discovery exit status");
  if (exitCode != 0) return {{}, L"The app discovery helper exited unexpectedly (code " +
                               std::to_wstring(exitCode) + L"). Try Reload app list."};
  UniqueView view(MapViewOfFile(mapping.get(), FILE_MAP_READ, 0, 0, kDiscoveryBufferBytes));
  if (!view) return Failure(L"Could not read the app discovery buffer");
  return ReadDiscoveryResult({static_cast<const std::byte*>(view.get()), kDiscoveryBufferBytes});
}

std::optional<int> RunDiscoveryHelperIfRequested() {
  int count = 0;
  auto* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
  if (!arguments) return 2;
  const std::unique_ptr<void, decltype(&LocalFree)> guard(arguments, LocalFree);
  if (count < 2 || std::wstring_view(arguments[1]) != kHelperArgument) return std::nullopt;
  if (count != 3 || !*arguments[2]) return 2;
  for (const wchar_t* digit = arguments[2]; *digit; ++digit) {
    if (*digit < L'0' || *digit > L'9') return 2;
  }
  errno = 0;
  wchar_t* end = nullptr;
  const auto value = std::wcstoull(arguments[2], &end, 10);
  if (errno == ERANGE || *end || value == 0 || value >= std::numeric_limits<std::uintptr_t>::max()) return 2;
  try {
    return RunHelper(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(value)));
  } catch (...) {
    return 3;
  }
}

}  // namespace quickdial
