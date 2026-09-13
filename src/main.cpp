#include "AppMessages.h"
#include "LauncherApp.h"
#include "DiscoveryProcess.h"
#include "SingleInstance.h"

#include <windows.h>
#include <shellapi.h>

#include <memory>
#include <string_view>

#include <winrt/base.h>

namespace {

struct HandleCloser {
  void operator()(void* handle) const noexcept {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
  }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;

struct BenchmarkEvents {
  UniqueHandle ready;
  UniqueHandle presented;
  bool requested = false;
};

BenchmarkEvents OpenBenchmarkEvents() {
  BenchmarkEvents events;
  int argumentCount = 0;
  PWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
  if (arguments == nullptr) {
    return events;
  }

  for (int index = 1; index + 2 < argumentCount; ++index) {
    if (std::wstring_view(arguments[index]) != L"--benchmark-events") {
      continue;
    }
    events.requested = true;
    events.ready.reset(OpenEventW(EVENT_MODIFY_STATE, FALSE, arguments[index + 1]));
    events.presented.reset(OpenEventW(EVENT_MODIFY_STATE, FALSE, arguments[index + 2]));
    break;
  }
  LocalFree(arguments);
  return events;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  if (const auto helperExit = quickdial::RunDiscoveryHelperIfRequested()) return *helperExit;
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  winrt::init_apartment(winrt::apartment_type::single_threaded);

  BenchmarkEvents benchmarkEvents = OpenBenchmarkEvents();
  if (benchmarkEvents.requested && (!benchmarkEvents.ready || !benchmarkEvents.presented)) {
    return 2;
  }

  quickdial::SingleInstance singleInstance;
  std::wstring instanceError;
  const auto instanceStart = singleInstance.Start(quickdial::kWindowClassName, quickdial::kMessageShowLauncher,
                                                 instanceError);
  if (instanceStart == quickdial::InstanceStart::Failed) {
    MessageBoxW(nullptr, instanceError.c_str(), L"Application Quick Dial", MB_OK | MB_ICONERROR);
    return 1;
  }
  if (instanceStart == quickdial::InstanceStart::Forwarded) return 0;

  quickdial::LauncherApp application(benchmarkEvents.presented.get());
  if (!application.Initialize(instance)) {
    MessageBoxW(nullptr, L"Application Quick Dial could not be initialized.", L"Application Quick Dial",
                MB_OK | MB_ICONERROR);
    return 1;
  }
  if (benchmarkEvents.ready) {
    SetEvent(benchmarkEvents.ready.get());
  }
  return application.Run();
}
