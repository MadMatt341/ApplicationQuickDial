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

bool HasArgument(std::wstring_view name) {
  int count = 0;
  PWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
  if (!arguments) return false;
  bool requested = false;
  for (int i = 1; i < count; ++i) requested |= std::wstring_view(arguments[i]) == name;
  LocalFree(arguments);
  return requested;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  // The shell installer probes this before registration. Keep it free of COM,
  // singleton forwarding, and UI so an incompatible launcher fails closed.
  if (HasArgument(L"--blade-contract-v1")) return 73;
  if (const auto helperExit = quickdial::RunDiscoveryHelperIfRequested()) return *helperExit;
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  winrt::init_apartment(winrt::apartment_type::single_threaded);

  BenchmarkEvents benchmarkEvents = OpenBenchmarkEvents();
  if (benchmarkEvents.requested && (!benchmarkEvents.ready || !benchmarkEvents.presented)) {
    return 2;
  }

  const bool shellMode = HasArgument(L"--shell-mode");
  quickdial::SingleInstance singleInstance;
  std::wstring instanceError;
  DWORD_PTR forwardedReply = 0;
  const auto instanceStart = singleInstance.Start(quickdial::kWindowClassName,
      shellMode ? quickdial::kMessageEnterShellMode : quickdial::kMessageShowLauncher,
      instanceError, shellMode ? &forwardedReply : nullptr);
  if (instanceStart == quickdial::InstanceStart::Failed) {
    MessageBoxW(nullptr, instanceError.c_str(), L"Application Quick Dial", MB_OK | MB_ICONERROR);
    return 1;
  }
  if (instanceStart == quickdial::InstanceStart::Forwarded) {
    if (shellMode && forwardedReply != 1) {
      MessageBoxW(nullptr, L"The running Quick Dial does not support shell mode. Exit it and run this version again.",
                  L"Application Quick Dial", MB_OK | MB_ICONERROR);
      return 1;
    }
    return 0;
  }

  quickdial::LauncherApp application(benchmarkEvents.presented.get(), {}, shellMode);
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
