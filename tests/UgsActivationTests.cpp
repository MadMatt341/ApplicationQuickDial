#include "UgsActivation.h"
#include <windows.h>
#include <shobjidl.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

namespace {
using namespace std::chrono_literals;
using namespace winrt::Windows::Data::Json;
constexpr UINT kOpen = WM_APP + 3;
std::atomic<HWND> hostWindow = nullptr;
std::atomic<int> mode = 0, requests = 0, actions = 0;
HWND applicationWindow = nullptr;

LRESULT CALLBACK AppProcedure(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == kOpen) { ShowWindow(window, SW_SHOW); return 0; }
  return DefWindowProcW(window, message, w, l);
}

LRESULT CALLBACK HostProcedure(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_APP + 91) {
    ++requests;
    const int scenario = mode;
    if (scenario == 8) { std::this_thread::sleep_for(700ms); return 0; }
    std::string bytes = "{\"version\":" + std::to_string(scenario == 5 ? 2 : 1) + ",\"icons\":[";
    const auto icon = "{\"token\":42,\"pid\":" + std::to_string(GetCurrentProcessId()) +
        (scenario == 6 ? ".5" : "") + ",\"tip\":\"" + (scenario == 4 ? "Other app" : "UnrealGameSync") +
        "\",\"rect\":" + (scenario == 3 ? "null" : scenario == 7 ? "[100,2,99,30]" : "[100,2,128,30]") + "}";
    bytes += icon + (scenario == 2 ? "," + icon : "") + "]}";
    if (scenario == 1) bytes = "{";
    COPYDATASTRUCT data{0x42545301, static_cast<DWORD>(bytes.size()), bytes.data()};
    DWORD_PTR reply = 0;
    SendMessageTimeoutW(reinterpret_cast<HWND>(w), WM_COPYDATA, reinterpret_cast<WPARAM>(window),
                        reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 100, &reply);
    return static_cast<LRESULT>(reply);
  }
  if (message == WM_COPYDATA && l) {
    const auto data = reinterpret_cast<const COPYDATASTRUCT*>(l);
    if (data->dwData != 0x42544101 || !data->lpData || data->cbData > 4096) return 0;
    try {
      const auto action = JsonObject::Parse(winrt::to_hstring(std::string_view(static_cast<const char*>(data->lpData), data->cbData)));
      const auto rect = action.GetNamedArray(L"rect");
      if (action.GetNamedNumber(L"token") != 42 || action.GetNamedString(L"action") != L"double" || rect.Size() != 4 ||
          rect.GetNumberAt(0) != 100 || rect.GetNumberAt(1) != 2 || rect.GetNumberAt(2) != 128 || rect.GetNumberAt(3) != 30) return 0;
      if (mode == 9) return 0;
      ++actions;
      PostMessageW(applicationWindow, kOpen, 0, 0);
      return 1;
    } catch (...) { return 0; }
  }
  if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
  return DefWindowProcW(window, message, w, l);
}

bool PumpVisible() {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  do {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
    if (IsWindowVisible(applicationWindow)) return true;
    std::this_thread::sleep_for(1ms);
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}

int CheckFixture(const std::filesystem::path& root) {
  winrt::init_apartment(winrt::apartment_type::single_threaded);
  int failures = 0;
  const auto check = [&](bool value, const char* description) {
    if (!value) { ++failures; std::cerr << "FAILED: " << description << '\n'; }
  };
  WNDCLASSW appClass{};
  appClass.hInstance = GetModuleHandleW(nullptr);
  appClass.lpszClassName = L"BladeUgsFixture.Application";
  appClass.lpfnWndProc = AppProcedure;
  if (!RegisterClassW(&appClass)) return 2;
  applicationWindow = CreateWindowW(appClass.lpszClassName, L"Owned UGS activation fixture", WS_OVERLAPPEDWINDOW,
      10, 10, 300, 200, nullptr, nullptr, appClass.hInstance, nullptr);
  if (!applicationWindow) return 2;
  quickdial::ApplicationEntry entry;
  entry.name = L"UGS fixture";
  entry.target = (root / L"Latest/UnrealGameSync.exe").wstring();
  check(!quickdial::TryActivateRunningUgs(entry), "missing Blade host retains ordinary launch");
  std::thread host([] {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    WNDCLASSW type{};
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"BladeUgsFixture.Host";
    type.lpfnWndProc = HostProcedure;
    if (!RegisterClassW(&type)) return;
    const auto window = CreateWindowW(type.lpszClassName, L"", 0, 0, 0, 0, 0, nullptr, nullptr, type.hInstance, nullptr);
    if (!window || !SetPropW(window, L"Blade.NotificationTray.v1", reinterpret_cast<HANDLE>(1))) return;
    hostWindow = window;
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
  });
  const auto ready = std::chrono::steady_clock::now() + 3s;
  while (!hostWindow && std::chrono::steady_clock::now() < ready) std::this_thread::sleep_for(1ms);
  if (!hostWindow) { std::cerr << "FAILED: host prerequisite\n"; std::terminate(); }

  check(quickdial::TryActivateRunningUgs(entry) && PumpVisible() && actions == 1,
        "verified direct UGS instance opens through the native tray transport");
  ShowWindow(applicationWindow, SW_HIDE);
  entry.target = (root / L"Launcher/UnrealGameSync.exe").wstring();
  check(quickdial::TryActivateRunningUgs(entry) && PumpVisible() && actions == 2,
        "stable UGS launcher opens its running Latest sibling");

  const auto accepted = actions.load();
  const auto before = requests.load();
  entry.target = (root / L"AudioControl.exe").wstring();
  check(!quickdial::TryActivateRunningUgs(entry), "unrelated tray app keeps its normal launch behavior");
  entry.target = L"UnrealGameSync.exe";
  check(!quickdial::TryActivateRunningUgs(entry), "relative image does not select a process");
  entry.target = (root / L"Launcher/UnrealGameSync.exe").wstring();
  entry.arguments = {L"-Project=fixture.uproject"};
  check(!quickdial::TryActivateRunningUgs(entry), "explicit app arguments keep the Shell launch path");
  entry.arguments.clear();
  entry.shortcutArguments = L"-RestoreState";
  check(!quickdial::TryActivateRunningUgs(entry), "startup arguments are preserved");
  entry.shortcutArguments.clear();
  check(requests == before && actions == accepted, "excluded commands never contact the tray host");

  entry.target = (root / L"OtherInstallation/Launcher/UnrealGameSync.exe").wstring();
  check(!quickdial::TryActivateRunningUgs(entry) && actions == accepted, "same filename in another installation is rejected");
  entry.target = (root / L"Launcher/UnrealGameSync.exe").wstring();
  for (const int scenario : {1, 2, 3, 4, 5, 6, 7, 9}) {
    mode = scenario;
    check(!quickdial::TryActivateRunningUgs(entry) && actions == accepted,
          "invalid, ambiguous or rejected tray data retains ordinary launch");
  }
  mode = 8;
  const auto start = std::chrono::steady_clock::now();
  check(!quickdial::TryActivateRunningUgs(entry) && std::chrono::steady_clock::now() - start < 600ms,
        "unresponsive host has a bounded timeout");
  // Wait for the owned host's deliberately blocked request before the next test.
  DWORD_PTR pong = 0;
  check(SendMessageTimeoutW(hostWindow, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 1000, &pong) != 0,
        "owned host recovered after timeout fixture");
  mode = 0;

  const auto shortcut = root / L"UGS.lnk";
  winrt::com_ptr<IShellLinkW> link;
  check(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.put()))), "shortcut prerequisite");
  if (link) {
    const auto file = link.as<IPersistFile>();
    check(SUCCEEDED(link->SetPath(entry.target.c_str())) && SUCCEEDED(file->Save(shortcut.c_str(), TRUE)), "owned shortcut created");
    entry.resolvedTarget = entry.target;
    entry.target = shortcut.wstring();
    ShowWindow(applicationWindow, SW_HIDE);
    check(quickdial::TryActivateRunningUgs(entry) && PumpVisible() && actions == accepted + 1,
          "plain Start-menu shortcut opens its verified running instance");
    check(SUCCEEDED(link->SetArguments(L"-Project=fixture.uproject")) && SUCCEEDED(file->Save(shortcut.c_str(), TRUE)), "shortcut arguments changed");
    const auto after = requests.load();
    check(!quickdial::TryActivateRunningUgs(entry) && requests == after,
          "fresh shortcut read preserves arguments absent from cached discovery metadata");
    check(SUCCEEDED(link->SetArguments(L"")) && SUCCEEDED(link->SetShowCmd(SW_SHOWMINNOACTIVE)) &&
          SUCCEEDED(file->Save(shortcut.c_str(), TRUE)), "shortcut display state changed");
    check(!quickdial::TryActivateRunningUgs(entry) && requests == after,
          "a shortcut's explicit minimized state keeps the Shell launch path");
  }

  PostMessageW(hostWindow, WM_CLOSE, 0, 0);
  host.join();
  DestroyWindow(applicationWindow);
  std::filesystem::remove(shortcut);
  if (!failures) std::cout << "UGS activation boundaries passed on a private desktop.\n";
  return failures ? 1 : 0;
}

std::wstring DesktopName(HDESK desktop) {
  wchar_t name[256]{};
  DWORD needed = 0;
  if (!desktop || !GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &needed)) return {};
  return name;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 3 && std::wstring_view(argv[1]) == L"--fixture") return CheckFixture(argv[2]);
  const auto input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
  const auto before = DesktopName(input);
  if (input) CloseDesktop(input);
  if (before.empty()) return 2;
  const auto suffix = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
  const auto root = std::filesystem::temp_directory_path() / (L"BladeUgsActivation-" + suffix);
  std::filesystem::create_directories(root / L"Latest");
  wchar_t self[32768]{};
  if (!GetModuleFileNameW(nullptr, self, 32768)) return 2;
  const auto executable = root / L"Latest/UnrealGameSync.exe";
  std::filesystem::copy_file(self, executable);
  const auto desktopName = L"BladeUgsActivation-" + suffix;
  const auto desktop = CreateDesktopW(desktopName.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
  STARTUPINFOW startup{sizeof(startup)};
  startup.lpDesktop = const_cast<wchar_t*>(desktopName.c_str());
  PROCESS_INFORMATION child{};
  std::wstring command = L"\"" + executable.wstring() + L"\" --fixture \"" + root.wstring() + L"\"";
  DWORD result = 2;
  if (desktop && CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &child)) {
    CloseHandle(child.hThread);
    if (WaitForSingleObject(child.hProcess, 20000) == WAIT_OBJECT_0) GetExitCodeProcess(child.hProcess, &result);
    else { TerminateProcess(child.hProcess, 2); WaitForSingleObject(child.hProcess, 5000); }
    CloseHandle(child.hProcess);
  } else { std::cerr << "FAILED: private desktop process prerequisite " << GetLastError() << '\n'; }
  if (desktop) CloseDesktop(desktop);
  const auto after = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
  if (DesktopName(after) != before) { std::cerr << "FAILED: input desktop changed\n"; result = 2; }
  if (after) CloseDesktop(after);
  // The checked absolute fixture root contains only files created above.
  std::filesystem::remove(root / L"UGS.lnk");
  std::filesystem::remove(executable);
  std::filesystem::remove(root / L"Latest");
  std::filesystem::remove(root);
  return static_cast<int>(result);
}
