#include "StartupInputTrace.h"
#include <winrt/base.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
using namespace winrt::Windows::Data::Json;
int failures = 0;
void Check(bool condition, const char* message) { if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; } }
std::wstring Environment(const wchar_t* name) {
  const auto size = GetEnvironmentVariableW(name, nullptr, 0);
  if (!size) return {};
  std::wstring value(size, L'\0');
  value.resize(GetEnvironmentVariableW(name, value.data(), size)); return value;
}
struct Fixture {
  std::filesystem::path root, run, marker, log;
  std::wstring oldDirectory = Environment(L"BLADE_STARTUP_DIR"), oldOrigin = Environment(L"BLADE_STARTUP_QPC"), oldFrequency = Environment(L"BLADE_STARTUP_FREQUENCY");
  LARGE_INTEGER origin{}, frequency{};
  Fixture() {
    root = std::filesystem::temp_directory_path() / (L"QuickDialInputTrace-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    run = root / L"run"; std::filesystem::create_directories(run);
    marker = root / L"trace-launcher-input.once";
    log = run / (L"launcher-input-" + std::to_wstring(GetCurrentProcessId()) + L".jsonl");
    QueryPerformanceCounter(&origin); QueryPerformanceFrequency(&frequency);
    SetEnvironmentVariableW(L"BLADE_STARTUP_DIR", run.c_str());
    SetEnvironmentVariableW(L"BLADE_STARTUP_QPC", std::to_wstring(origin.QuadPart).c_str());
    SetEnvironmentVariableW(L"BLADE_STARTUP_FREQUENCY", std::to_wstring(frequency.QuadPart).c_str());
  }
  void Arm() { std::ofstream(marker) << "one capture\n"; }
  ~Fixture() {
    SetEnvironmentVariableW(L"BLADE_STARTUP_DIR", oldDirectory.empty() ? nullptr : oldDirectory.c_str());
    SetEnvironmentVariableW(L"BLADE_STARTUP_QPC", oldOrigin.empty() ? nullptr : oldOrigin.c_str());
    SetEnvironmentVariableW(L"BLADE_STARTUP_FREQUENCY", oldFrequency.empty() ? nullptr : oldFrequency.c_str());
    std::filesystem::remove_all(root);
  }
};
std::vector<JsonObject> Rows(const std::filesystem::path& path) {
  std::ifstream file(path); std::string line; std::vector<JsonObject> rows;
  while (std::getline(file, line)) {
    JsonObject row;
    Check(JsonObject::TryParse(winrt::to_hstring(line), row), "every retained row is complete JSON");
    if (row) rows.push_back(row);
  }
  return rows;
}
void TestArmingAndFailure() {
  Fixture fixture;
  Check(!quickdial::StartupInputTrace::StartFromEnvironment(), "an unarmed launcher starts no trace");
  Check(!std::filesystem::exists(fixture.log), "an unarmed startup creates no trace file");
  fixture.Arm();
  SetEnvironmentVariableW(L"BLADE_STARTUP_FREQUENCY", L"0");
  Check(!quickdial::StartupInputTrace::StartFromEnvironment(), "invalid shared clock disables tracing");
  Check(std::filesystem::exists(fixture.marker), "invalid context retains the next-start request");
  SetEnvironmentVariableW(L"BLADE_STARTUP_FREQUENCY", std::to_wstring(fixture.frequency.QuadPart).c_str());
  SetEnvironmentVariableW(L"BLADE_STARTUP_QPC", std::to_wstring(fixture.origin.QuadPart - 121 * fixture.frequency.QuadPart).c_str());
  Check(!quickdial::StartupInputTrace::StartFromEnvironment(), "a shared clock older than two minutes starts no capture");
  Check(std::filesystem::exists(fixture.marker), "an expired context retains the request");
  SetEnvironmentVariableW(L"BLADE_STARTUP_QPC", std::to_wstring(fixture.origin.QuadPart).c_str());
  std::ofstream(fixture.log) << "existing evidence\n";
  Check(!quickdial::StartupInputTrace::StartFromEnvironment(), "an existing trace file is never overwritten");
  Check(std::filesystem::exists(fixture.marker), "file creation failure retains the request");
  std::ifstream previous(fixture.log); std::string line; std::getline(previous, line);
  Check(line == "existing evidence", "failed startup preserves original evidence");
}
void TestContentsAndOneShot() {
  Fixture fixture; fixture.Arm();
  auto trace = quickdial::StartupInputTrace::StartFromEnvironment();
  Check(static_cast<bool>(trace), "armed valid context creates a trace");
  if (!trace) return;
  Check(!std::filesystem::exists(fixture.marker), "successful startup consumes the one-shot marker");
  quickdial::InputTraceEvent key;
  key.key = VK_SPACE; key.down = true; key.before = 1; key.after = 0; key.asyncSampled = true;
  key.disposition = static_cast<int>(quickdial::HotkeyDisposition::Pass);
  trace->Record(key);
  key.key = 'A'; trace->Record(key);
  trace->Record(quickdial::InputTracePhase::ShowCompleted, reinterpret_cast<HWND>(123), false, reinterpret_cast<HWND>(456));
  trace->Stop(); trace.reset();
  const auto rows = Rows(fixture.log);
  Check(rows.size() >= 5, "trace retains entry, native focus, offered events and footer");
  bool sawKey = false, sawShow = false, sawFocus = false;
  unsigned sequence = 0; long long previousQpc = 0;
  for (const auto& row : rows) {
    Check(row.GetNamedString(L"originQpc") == std::to_wstring(fixture.origin.QuadPart), "rows preserve the supplied shared clock");
    Check(row.GetNamedNumber(L"sequence") == sequence++, "persisted rows have contiguous sequence numbers");
    Check(row.GetNamedNumber(L"elapsedUs") >= 0, "timestamps do not precede shared startup");
    const auto qpc = std::stoll(std::wstring(row.GetNamedString(L"qpc")));
    Check(qpc >= previousQpc, "writer timestamps stay monotonic when hook observations arrive from another thread"); previousQpc = qpc;
    const auto phase = row.GetNamedString(L"phase"); const auto details = row.GetNamedObject(L"details");
    if (phase == L"key_observed") {
      sawKey = true;
      Check(details.GetNamedNumber(L"virtualKey") == VK_SPACE, "ordinary character keys never enter the log");
      Check(details.GetNamedNumber(L"trackedBefore") == 1 && details.GetNamedNumber(L"trackedAfter") == 0 && details.GetNamedNumber(L"asyncLeft") == 0,
          "trace preserves a Windows press cleared by a zero asynchronous sample");
      Check(details.GetNamedNumber(L"disposition") == 0, "trace retains the actual shortcut decision");
      Check(std::stoll(std::wstring(details.GetNamedString(L"observedQpc"))) <= qpc,
          "the retained callback timestamp is independent of the later writer timestamp");
    }
    if (phase == L"show_completed") {
      sawShow = true;
      Check(!details.GetNamedBoolean(L"accepted") && details.GetNamedNumber(L"target") == 123 && details.GetNamedNumber(L"focus") == 456,
          "failed activation and delivered focus remain distinct observations");
    }
    if (phase == L"focus_snapshot") { sawFocus = true; Check(details.HasKey(L"foreground") && details.HasKey(L"inputDesktop"), "native focus snapshot includes desktop and foreground"); }
    Check(!details.HasKey(L"title") && !details.HasKey(L"text") && !details.HasKey(L"coordinates"), "trace schema excludes user content and pointer position");
  }
  Check(sawKey && sawShow && sawFocus, "offered key, activation and native focus records reach disk");
  Check(!rows.empty() && rows.back().GetNamedString(L"phase") == L"capture_closed", "stopping the observer writes its termination reason");
  std::filesystem::remove(fixture.log);
  Check(!quickdial::StartupInputTrace::StartFromEnvironment(), "the next startup does not capture without another request");
}
void TestUnconsumedMarker() {
  Fixture fixture; fixture.Arm();
  Check(SetFileAttributesW(fixture.marker.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE, "owned marker can reject deletion");
  auto trace = quickdial::StartupInputTrace::StartFromEnvironment();
  Check(!trace && std::filesystem::exists(fixture.marker), "an unconsumed marker cannot silently enable repeated capture");
  SetFileAttributesW(fixture.marker.c_str(), FILE_ATTRIBUTE_NORMAL);
}
void TestBudgetAndHookCaller() {
  Fixture fixture; fixture.Arm();
  auto trace = quickdial::StartupInputTrace::StartFromEnvironment();
  Check(static_cast<bool>(trace), "bounded capture fixture starts"); if (!trace) return;
  quickdial::InputTraceEvent event; event.key = VK_SPACE; event.down = true;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (trace->enabled() && std::chrono::steady_clock::now() < deadline) {
    for (int index = 0; index < 256; ++index) trace->Record(event);
    std::this_thread::yield();
  }
  Check(!trace->enabled(), "a sustained offered event stream stops at the byte budget within five seconds");
  trace->Stop(); trace.reset();
  Check(std::filesystem::file_size(fixture.log) <= 256 * 1024, "the complete log including its footer stays within 256 KiB");
  const auto rows = Rows(fixture.log);
  Check(!rows.empty() && rows.back().GetNamedString(L"phase") == L"capture_closed", "byte exhaustion retains a complete footer");
}
}  // namespace
int main() {
  winrt::init_apartment(winrt::apartment_type::single_threaded);
  try { TestArmingAndFailure(); TestContentsAndOneShot(); TestUnconsumedMarker(); TestBudgetAndHookCaller(); }
  catch (const winrt::hresult_error& error) { ++failures; std::cerr << winrt::to_string(error.message()) << '\n'; }
  catch (const std::exception& error) { ++failures; std::cerr << error.what() << '\n'; }
  if (!failures) std::cout << "All startup input trace tests passed.\n";
  return failures ? 1 : 0;
}
