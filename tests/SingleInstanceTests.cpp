#include "SingleInstance.h"

#include <windows.h>

#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr wchar_t kWindowClass[] = L"QuickDial.SingleInstanceTest.Window";
constexpr UINT kShowMessage = WM_APP + 2;
constexpr int kForwarded = 10;
constexpr int kFailed = 11;
HANDLE shownEvent = nullptr;

struct CloseHandleDeleter {
  void operator()(void* handle) const { if (handle) CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, CloseHandleDeleter>;
struct CloseDesktopDeleter {
  void operator()(void* desktop) const { if (desktop) CloseDesktop(static_cast<HDESK>(desktop)); }
};
using Desktop = std::unique_ptr<void, CloseDesktopDeleter>;

bool Signaled(HANDLE event, DWORD timeout = 3000) {
  return WaitForSingleObject(event, timeout) == WAIT_OBJECT_0;
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == kShowMessage) { SetEvent(shownEvent); return 0; }
  return DefWindowProcW(window, message, wParam, lParam);
}

int Fixture(const std::wstring& prefix, std::wstring_view mode) {
  auto open = [&](const wchar_t* suffix) {
    return Handle(OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, (prefix + suffix).c_str()));
  };
  auto acquired = open(L".acquired");
  auto ready = open(L".ready");
  auto shown = open(L".shown");
  auto gate = open(L".gate");
  auto stop = open(L".stop");
  if (!acquired || !ready || !shown || !gate || !stop) return 20;
  shownEvent = shown.get();

  quickdial::SingleInstance instance;
  if (mode != L"legacy") {
    std::wstring error;
    const auto result = instance.Start(kWindowClass, kShowMessage, error);
    if (result == quickdial::InstanceStart::Forwarded) return kForwarded;
    if (result == quickdial::InstanceStart::Failed) {
      std::wcerr << L"Fixture startup error: " << error << L'\n';
      return kFailed;
    }
  }
  if (!SetEvent(acquired.get())) return 21;
  if (mode == L"delayed") {
    const HANDLE waits[] = {gate.get(), stop.get()};
    if (WaitForMultipleObjects(2, waits, FALSE, 10000) != WAIT_OBJECT_0) return 22;
  }

  WNDCLASSW windowClass{};
  windowClass.hInstance = GetModuleHandleW(nullptr);
  windowClass.lpfnWndProc = WindowProcedure;
  windowClass.lpszClassName = kWindowClass;
  if (!RegisterClassW(&windowClass)) return 23;
  HWND window = CreateWindowExW(0, kWindowClass, L"Singleton fixture", WS_POPUP,
      0, 0, 10, 10, nullptr, nullptr, windowClass.hInstance, nullptr);
  if (!window) return 24;
  if (!SetEvent(ready.get())) { DestroyWindow(window); return 25; }
  if (mode == L"stalled") {
    Signaled(stop.get(), 10000);
  } else {
    const HANDLE stopHandle = stop.get();
    const ULONGLONG deadline = GetTickCount64() + 15000;
    while (GetTickCount64() < deadline) {
      const DWORD wait = MsgWaitForMultipleObjects(1, &stopHandle, FALSE, 1000, QS_ALLINPUT);
      if (wait == WAIT_OBJECT_0) break;
      if (wait == WAIT_FAILED) { DestroyWindow(window); return 26; }
      MSG message{};
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
    }
  }
  DestroyWindow(window);
  return 0;
}

class Child {
 public:
  Child(const std::wstring& executable, const std::wstring& desktop, const wchar_t* mode) {
    static unsigned sequence = 0;
    const std::wstring prefix = L"Local\\QuickDialSingletonTest." + std::to_wstring(GetCurrentProcessId()) +
        L"." + std::to_wstring(++sequence);
    auto create = [&](const wchar_t* suffix) {
      return Handle(CreateEventW(nullptr, TRUE, FALSE, (prefix + suffix).c_str()));
    };
    acquired = create(L".acquired"); ready = create(L".ready"); shown = create(L".shown");
    gate = create(L".gate"); stop = create(L".stop");
    if (!acquired || !ready || !shown || !gate || !stop) return;
    std::wstring command = L"\"" + executable + L"\" --fixture \"" + prefix + L"\" " + mode;
    std::wstring desktopName = desktop;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.lpDesktop = desktopName.data();
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION information{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &information)) {
      std::cerr << "CreateProcess failed: " << GetLastError() << '\n';
      return;
    }
    process.reset(information.hProcess);
    CloseHandle(information.hThread);
  }
  ~Child() {
    if (!process) return;
    SetEvent(stop.get());
    if (!Signaled(process.get(), 2000)) {
      if (!TerminateProcess(process.get(), 30)) std::cerr << "Fixture termination failed\n";
      if (!Signaled(process.get(), 2000)) std::cerr << "Fixture cleanup timed out\n";
    }
  }
  bool Exits(DWORD expected) {
    DWORD code = 0;
    return process && Signaled(process.get(), 4000) && GetExitCodeProcess(process.get(), &code) && code == expected;
  }
  Handle process, acquired, ready, shown, gate, stop;
};

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 4 && std::wstring_view(argv[1]) == L"--fixture") return Fixture(argv[2], argv[3]);
  std::vector<wchar_t> executableBuffer(32768);
  const DWORD size = GetModuleFileNameW(nullptr, executableBuffer.data(), static_cast<DWORD>(executableBuffer.size()));
  if (!size || size >= executableBuffer.size()) return 1;
  const std::wstring executable(executableBuffer.data(), size);
  const std::wstring prefix = L"QuickDialTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
      std::to_wstring(GetTickCount64());
  const std::wstring desktopA = prefix + L"-A";
  const std::wstring desktopB = prefix + L"-B";
  Desktop first(CreateDesktopW(desktopA.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr));
  Desktop second(CreateDesktopW(desktopB.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr));
  if (!first || !second) { std::cerr << "Private desktop creation failed: " << GetLastError() << '\n'; return 1; }
  int failures = 0;
  auto check = [&](bool ok, const char* description) {
    if (!ok) { ++failures; std::cerr << "FAILED: " << description << '\n'; }
  };

  {
    // A session-wide legacy mutex must not prevent either desktop's primary.
    Handle legacy(CreateMutexW(nullptr, FALSE, L"Local\\ApplicationQuickDial.SingleInstance"));
    check(legacy != nullptr, "legacy singleton fixture exists");
    Child a(executable, desktopA, L"normal");
    check(Signaled(a.ready.get()), "first desktop starts a primary");
    Child b(executable, desktopB, L"normal");
    check(Signaled(b.ready.get()), "an invisible primary on another desktop cannot block startup");
    Child repeatA(executable, desktopA, L"normal");
    check(repeatA.Exits(kForwarded) && Signaled(a.shown.get()), "same-desktop launch opens the existing window");
    check(!Signaled(b.shown.get(), 0), "forwarding never targets the other desktop");
    Child repeatB(executable, desktopB, L"normal");
    check(repeatB.Exits(kForwarded) && Signaled(b.shown.get()), "each desktop independently forwards repeat launches");
  }
  {
    Child a(executable, desktopA, L"delayed");
    check(Signaled(a.acquired.get()), "delayed primary reserves its desktop");
    Child repeat(executable, desktopA, L"normal");
    check(!Signaled(repeat.process.get(), 150), "repeat launch waits while the primary creates its window");
    check(SetEvent(a.gate.get()) != FALSE, "release delayed window creation");
    check(repeat.Exits(kForwarded) && Signaled(a.shown.get()), "startup race delivers the open request");
  }
  {
    Child a(executable, desktopA, L"delayed");
    check(Signaled(a.acquired.get()), "blocked primary reserves its desktop");
    Child repeat(executable, desktopA, L"normal");
    check(repeat.Exits(kFailed), "a missing window reports bounded failure instead of silent success");
  }
  {
    Child a(executable, desktopA, L"delayed");
    check(Signaled(a.acquired.get()), "crash fixture reserves its desktop");
    Child replacement(executable, desktopA, L"normal");
    check(!Signaled(replacement.process.get(), 150), "replacement waits for the earlier primary");
    check(TerminateProcess(a.process.get(), 31) != FALSE && a.Exits(31), "simulate primary crash before window creation");
    check(Signaled(replacement.ready.get()), "abandoned startup ownership is recovered");
  }
  {
    Child legacy(executable, desktopA, L"legacy");
    check(Signaled(legacy.ready.get()), "older-version window fixture is ready");
    Child repeat(executable, desktopA, L"normal");
    check(repeat.Exits(kForwarded) && Signaled(legacy.shown.get()), "existing older-version window is reused");
  }
  {
    Child stalled(executable, desktopA, L"stalled");
    check(Signaled(stalled.ready.get()), "unresponsive-window fixture is ready");
    Child repeat(executable, desktopA, L"normal");
    check(repeat.Exits(kFailed), "unresponsive forwarding reports bounded failure");
  }
  if (!failures) std::cout << "All quickdial single-instance integration checks passed.\n";
  return failures ? 1 : 0;
}
