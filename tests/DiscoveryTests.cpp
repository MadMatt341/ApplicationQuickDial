#include "DiscoveryProcess.h"
#include "DiscoveryProtocol.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <thread>

#include <winrt/base.h>

namespace {

using namespace std::chrono_literals;
int failures = 0;
void Check(bool condition, const char* description) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << description << '\n'; }
}
struct HandleCloser {
  void operator()(void* handle) const { if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, HandleCloser>;

std::wstring Environment(const wchar_t* name) {
  wchar_t value[32'768]{};
  const DWORD length = GetEnvironmentVariableW(name, value, static_cast<DWORD>(std::size(value)));
  return length < std::size(value) ? std::wstring(value, length) : std::wstring{};
}

std::filesystem::path Executable() {
  wchar_t path[32'768]{};
  const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
  return length && length < std::size(path) ? std::filesystem::path(path) : std::filesystem::path{};
}

quickdial::InstalledAppsResult Fixture() {
  quickdial::ApplicationEntry first;
  first.name = L"Za\u017c\u00f3\u0142\u0107 \u65e5\u672c\U0001f680";
  first.target = L"shell:AppsFolder\\Example.App_123!App";
  first.identity = L"Example.App_123!App";
  quickdial::ApplicationEntry second;
  second.name = L"Literal target";
  second.target = L"C:\\%USERPROFILE%\\quoted \"name\".exe";
  return {{first, second}, {}};
}

bool SameApplications(const quickdial::InstalledAppsResult& left, const quickdial::InstalledAppsResult& right) {
  if (left.error != right.error || left.applications.size() != right.applications.size()) return false;
  for (std::size_t index = 0; index < left.applications.size(); ++index) {
    const auto& a = left.applications[index];
    const auto& b = right.applications[index];
    if (a.name != b.name || a.target != b.target || a.identity != b.identity) return false;
  }
  return true;
}

void TestProtocol() {
  std::vector<std::byte> buffer(1024);
  const auto expected = Fixture();
  Check(quickdial::WriteDiscoveryResult(buffer, expected), "localized discovery data can be serialized");
  Check(SameApplications(quickdial::ReadDiscoveryResult(buffer), expected),
        "transport preserves Unicode, identifiers, ordering and literal targets without expansion");
  std::uint32_t used = 0;
  std::memcpy(&used, buffer.data() + 8, sizeof(used));
  for (std::size_t size = 0; size < used; ++size) {
    const auto truncated = quickdial::ReadDiscoveryResult(std::span(buffer).first(size));
    Check(!truncated && truncated.applications.empty(), "every truncated payload is rejected without partial entries");
  }
  for (const auto [offset, value] : {std::pair{0U, 0U}, {4U, 999U}, {8U, 0xffffffffU}, {12U, 0xffffffffU},
                                    {16U, 0xffffffffU}, {20U, 0xffffffffU}}) {
    auto corrupt = buffer;
    std::memcpy(corrupt.data() + offset, &value, sizeof(value));
    const auto result = quickdial::ReadDiscoveryResult(corrupt);
    Check(!result && result.applications.empty(), "malformed header/count/string lengths never publish partial data");
  }
  quickdial::InstalledAppsResult failed{{}, L"Simulated discovery failure"};
  Check(quickdial::WriteDiscoveryResult(buffer, failed) &&
        quickdial::ReadDiscoveryResult(buffer).error == failed.error, "helper errors survive transport");
  Check(quickdial::WriteDiscoveryResult(buffer, {}) &&
        static_cast<bool>(quickdial::ReadDiscoveryResult(buffer)), "empty successful discovery remains distinct from failure");
  auto oversized = expected;
  oversized.applications.front().name.assign(32'769, L'x');
  Check(!quickdial::WriteDiscoveryResult(buffer, oversized) && !quickdial::ReadDiscoveryResult(buffer),
        "oversized strings invalidate the response instead of leaving a previous success");
  auto embeddedNull = expected;
  embeddedNull.applications.back().target.push_back(L'\0');
  Check(!quickdial::WriteDiscoveryResult(buffer, embeddedNull), "embedded NULs cannot truncate launch targets");
  auto tooMany = expected;
  tooMany.applications.resize(16'385, expected.applications.front());
  Check(!quickdial::WriteDiscoveryResult(buffer, tooMany), "entry count is bounded before serialization");
  auto tooLarge = expected;
  tooLarge.applications.resize(512, expected.applications.front());
  for (auto& entry : tooLarge.applications) entry.name.assign(8192, L'x');
  buffer.resize(quickdial::kDiscoveryBufferBytes);
  Check(!quickdial::WriteDiscoveryResult(buffer, tooLarge) && !quickdial::ReadDiscoveryResult(buffer),
        "the shared buffer cannot overflow or publish a partial catalog");
}

// Fixture modes exist only in this test executable, never in the launcher.
int RunFixture(int argc, wchar_t** argv) {
  if (argc != 3) return 2;
  const auto mode = Environment(L"QUICKDIAL_TEST_DISCOVERY_MODE");
  if (mode == L"crash") return 17;
  if (mode == L"hang") {
    std::ofstream(std::filesystem::path(Environment(L"QUICKDIAL_TEST_DISCOVERY_PID"))) << GetCurrentProcessId();
    Sleep(INFINITE);
    return 1;
  }
  if (mode == L"inherit") {
    const auto sentinel = std::wcstoull(Environment(L"QUICKDIAL_TEST_DISCOVERY_SENTINEL").c_str(), nullptr, 10);
    SetEvent(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(sentinel)));
  }
  if (mode == L"empty") return 0;
  const auto value = std::wcstoull(argv[2], nullptr, 10);
  void* view = MapViewOfFile(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(value)),
                            FILE_MAP_WRITE, 0, 0, quickdial::kDiscoveryBufferBytes);
  if (!view) return 2;
  auto result = mode == L"error" ? quickdial::InstalledAppsResult{{}, L"Simulated discovery failure"} : Fixture();
  const bool written = quickdial::WriteDiscoveryResult(
      {static_cast<std::byte*>(view), quickdial::kDiscoveryBufferBytes}, result);
  if (mode == L"corrupt") std::memset(view, 0xff, 16);
  UnmapViewOfFile(view);
  return written ? 0 : 3;
}

void Mode(const wchar_t* mode) {
  Check(SetEnvironmentVariableW(L"QUICKDIAL_TEST_DISCOVERY_MODE", mode) != FALSE, "fixture mode is configured");
}

Handle WaitForFixture(const std::filesystem::path& pidPath) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    DWORD pid = 0;
    std::ifstream(pidPath) >> pid;
    if (pid) return Handle(OpenProcess(SYNCHRONIZE, FALSE, pid));
    std::this_thread::sleep_for(5ms);
  }
  return {};
}

void TestProcesses(const std::filesystem::path& self, const std::filesystem::path& pidPath) {
  Mode(L"success");
  Check(SameApplications(quickdial::DiscoverInstalledApplicationsIsolated({}, self), Fixture()),
        "a real child process returns complete discovery data");
  Mode(L"error");
  Check(quickdial::DiscoverInstalledApplicationsIsolated({}, self).error == L"Simulated discovery failure",
        "provider failure is preserved by the process boundary");
  for (const auto* mode : {L"crash", L"empty", L"corrupt"}) {
    Mode(mode);
    const auto result = quickdial::DiscoverInstalledApplicationsIsolated({}, self);
    Check(!result && result.applications.empty(), "crashed, empty or corrupt helper cannot replace a valid list");
  }
  Check(!quickdial::DiscoverInstalledApplicationsIsolated({}, self.parent_path() / L"missing-helper.exe"),
        "failure to create the helper is actionable");

  Mode(L"inherit");
  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  Handle sentinel(CreateEventW(&security, TRUE, FALSE, nullptr));
  Check(sentinel != nullptr, "inheritable sentinel event is created");
  const auto sentinelValue = std::to_wstring(reinterpret_cast<std::uintptr_t>(sentinel.get()));
  Check(SetEnvironmentVariableW(L"QUICKDIAL_TEST_DISCOVERY_SENTINEL", sentinelValue.c_str()) != FALSE,
        "inheritance sentinel is configured");
  Check(static_cast<bool>(quickdial::DiscoverInstalledApplicationsIsolated({}, self)), "restricted inheritance helper completes");
  Check(WaitForSingleObject(sentinel.get(), 0) == WAIT_TIMEOUT, "unrelated inheritable handles do not reach the helper");

  Mode(L"hang");
  const auto started = std::chrono::steady_clock::now();
  const auto timedOut = quickdial::DiscoverInstalledApplicationsIsolated({}, self, 150ms);
  Check(!timedOut && timedOut.error.find(L"timed out") != std::wstring::npos &&
        std::chrono::steady_clock::now() - started < 2s, "a hung helper times out and is cleaned up promptly");
  std::filesystem::remove(pidPath);
  std::stop_source cancellation;
  auto pending = std::async(std::launch::async, [&] {
    return quickdial::DiscoverInstalledApplicationsIsolated(cancellation.get_token(), self);
  });
  Handle child = WaitForFixture(pidPath);
  Check(child != nullptr, "cancellation fixture reaches its blocked operation");
  const auto cancelledAt = std::chrono::steady_clock::now();
  cancellation.request_stop();
  Check(!pending.get() && std::chrono::steady_clock::now() - cancelledAt < 2s,
        "cancellation does not wait for a stalled Shell operation");
  Check(child && WaitForSingleObject(child.get(), 1000) == WAIT_OBJECT_0, "cancellation terminates the actual helper process");
  std::filesystem::remove(pidPath);

  // The helper must also die if the GUI process exits before its worker cleans up.
  std::wstring command = L"\"" + self.wstring() + L"\" --fixture-owner";
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION ownerInfo{};
  const bool launched = CreateProcessW(self.c_str(), command.data(), nullptr, nullptr, FALSE,
                                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &ownerInfo) != FALSE;
  Check(launched, "parent-exit fixture starts");
  if (launched) {
    Handle owner(ownerInfo.hProcess), ownerThread(ownerInfo.hThread);
    child = WaitForFixture(pidPath);
    Check(child != nullptr, "parent-exit fixture starts a supervised helper");
    Check(TerminateProcess(owner.get(), 99) != FALSE, "test terminates only its fixture parent");
    Check(WaitForSingleObject(owner.get(), 1000) == WAIT_OBJECT_0, "fixture parent exits");
    Check(child && WaitForSingleObject(child.get(), 2000) == WAIT_OBJECT_0,
          "the job object prevents an orphan after abrupt parent exit");
  }
  std::filesystem::remove(pidPath);
  Mode(L"success");
  DWORD before = 0, after = 0;
  Check(GetProcessHandleCount(GetCurrentProcess(), &before) != FALSE, "initial discovery handles can be measured");
  for (int index = 0; index < 10; ++index) {
    Check(static_cast<bool>(quickdial::DiscoverInstalledApplicationsIsolated({}, self)),
          "discovery can recover after failure and repeat successfully");
  }
  Check(GetProcessHandleCount(GetCurrentProcess(), &after) != FALSE && after <= before,
        "repeated discovery does not retain mapping, process, thread, event or job handles");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc >= 2 && std::wstring_view(argv[1]) == L"--discover-apps") return RunFixture(argc, argv);
  if (argc == 2 && std::wstring_view(argv[1]) == L"--fixture-owner") {
    return quickdial::DiscoverInstalledApplicationsIsolated() ? 0 : 1;
  }
  const auto self = Executable();
  if (self.empty()) return 1;
  const auto pidPath = self.parent_path() / (L"discovery-test-" + std::to_wstring(GetCurrentProcessId()) + L".pid");
  Check(SetEnvironmentVariableW(L"QUICKDIAL_TEST_DISCOVERY_PID", pidPath.c_str()) != FALSE, "fixture PID file is configured");
  TestProtocol();
  TestProcesses(self, pidPath);

  // Compare the real launcher's private mode with the existing Shell enumerator.
  // The GUI singleton is deliberately irrelevant to helper invocations.
  winrt::init_apartment(winrt::apartment_type::single_threaded);
  const auto direct = quickdial::DiscoverInstalledApplications();
  const auto isolated = quickdial::DiscoverInstalledApplicationsIsolated({}, self.parent_path() / L"ApplicationQuickDial.exe");
  Check(direct && isolated && SameApplications(direct, isolated),
        "the production helper preserves every discovered name, target, identifier and ordering");
  std::filesystem::remove(pidPath);
  if (!failures) std::cout << "All quickdial discovery checks passed (" << isolated.applications.size() << " installed apps).\n";
  return failures ? 1 : 0;
}
