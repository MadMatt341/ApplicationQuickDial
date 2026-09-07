#include "AppMessages.h"
#include "Catalog.h"
#include "LauncherApp.h"
#include "Search.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <winrt/base.h>

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kStartupTrials = 10;
constexpr int kShowTrials = 20;
constexpr auto kReadyTimeout = std::chrono::seconds(5);
constexpr auto kPresentationTimeout = std::chrono::seconds(2);
constexpr auto kBackgroundTimeout = std::chrono::seconds(10);
constexpr auto kSettledInterval = std::chrono::milliseconds(250);
constexpr auto kIdleSampleDuration = std::chrono::seconds(3);

// Performance contract for the personal Windows 11 x64 prototype.
constexpr double kStartupP95BudgetMs = 100.0;
constexpr double kFirstShowBudgetMs = 100.0;
constexpr double kWarmShowP95BudgetMs = 50.0;
constexpr double kHiddenInitialWorkingSetBudgetMb = 20.0;
constexpr double kHiddenInitialPrivateWorkingSetBudgetMb = 5.0;
constexpr double kHiddenInitialPrivateBudgetMb = 5.0;
constexpr double kVisibleWorkingSetBudgetMb = 64.0;
constexpr double kVisiblePrivateWorkingSetBudgetMb = 20.0;
constexpr double kVisiblePrivateBudgetMb = 20.0;
constexpr double kHiddenAfterUseWorkingSetBudgetMb = 64.0;
constexpr double kHiddenAfterUsePrivateWorkingSetBudgetMb = 20.0;
constexpr double kHiddenAfterUsePrivateBudgetMb = 16.0;
constexpr double kHiddenIdleCpuBudgetMs = 10.0;
constexpr double kVisibleIdleCpuBudgetMs = 10.0;
constexpr double kCatalogParseAverageBudgetMs = 2.0;
constexpr double kSearchAverageBudgetMs = 0.25;
constexpr double kExecutableSizeBudgetKb = 256.0;

struct HandleCloser {
  void operator()(void* handle) const noexcept {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
  }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;

struct MemorySample {
  double workingSetMb = -1.0;
  double privateWorkingSetMb = -1.0;
  double privateMb = -1.0;
};

struct ChildProcess {
  UniqueHandle process;
  DWORD id = 0;
  HWND window = nullptr;
  double startupMs = 0.0;
};

struct BenchmarkResults {
  std::vector<double> startupTimesMs;
  double firstShowMs = 0.0;
  std::vector<double> warmShowTimesMs;
  std::vector<double> shutdownTimesMs;
  double discoverySettledMs = 0.0;
  MemorySample hiddenInitial;
  MemorySample visible;
  MemorySample hiddenAfterUse;
  double hiddenIdleCpuMs = 0.0;
  double visibleIdleCpuMs = 0.0;
  double catalogParseAverageMs = 0.0;
  double searchAverageMs = 0.0;
  double executableSizeKb = 0.0;
};

std::filesystem::path CurrentExecutablePath() {
  std::wstring buffer(512, L'\0');
  while (true) {
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) {
      return {};
    }
    if (length < buffer.size() - 1) {
      buffer.resize(length);
      return buffer;
    }
    buffer.resize(buffer.size() * 2);
  }
}

std::wstring Quote(std::wstring_view value) {
  return L"\"" + std::wstring(value) + L"\"";
}

double ElapsedMilliseconds(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

double Percentile(std::vector<double> values, double percentile) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const std::size_t rank = static_cast<std::size_t>(std::ceil(percentile * values.size()));
  return values[std::clamp<std::size_t>(rank, 1, values.size()) - 1];
}

double Average(const std::vector<double>& values) {
  if (values.empty()) {
    return 0.0;
  }
  return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
}

std::uint64_t FileTimeValue(const FILETIME& value) {
  ULARGE_INTEGER converted{};
  converted.LowPart = value.dwLowDateTime;
  converted.HighPart = value.dwHighDateTime;
  return converted.QuadPart;
}

std::optional<std::uint64_t> ProcessCpuTime100ns(HANDLE process) {
  FILETIME created{};
  FILETIME exited{};
  FILETIME kernel{};
  FILETIME user{};
  if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) {
    return std::nullopt;
  }
  return FileTimeValue(kernel) + FileTimeValue(user);
}

double MeasureIdleCpu(HANDLE process, std::chrono::milliseconds duration) {
  const auto before = ProcessCpuTime100ns(process);
  std::this_thread::sleep_for(duration);
  const auto after = ProcessCpuTime100ns(process);
  if (!before || !after || *after < *before) {
    return -1.0;
  }
  return static_cast<double>(*after - *before) / 10'000.0;
}

MemorySample ReadMemory(HANDLE process) {
  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  if (!GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
    return {};
  }

  std::vector<ULONG_PTR> workingSetBuffer(1024);
  PSAPI_WORKING_SET_INFORMATION* workingSet = nullptr;
  bool sampled = false;
  for (int attempt = 0; attempt < 5; ++attempt) {
    workingSet = reinterpret_cast<PSAPI_WORKING_SET_INFORMATION*>(workingSetBuffer.data());
    if (QueryWorkingSet(process, workingSet, static_cast<DWORD>(workingSetBuffer.size() * sizeof(ULONG_PTR)))) {
      sampled = true;
      break;
    }
    if (GetLastError() != ERROR_BAD_LENGTH) break;
    const std::size_t needed = workingSet->NumberOfEntries + 1024;
    if (needed > 16'777'216) break;
    workingSetBuffer.resize(std::max(workingSetBuffer.size() * 2, needed));
  }
  std::size_t privateResidentPages = 0;
  if (sampled) {
    for (ULONG_PTR index = 0; index < workingSet->NumberOfEntries; ++index) {
      if (workingSet->WorkingSetInfo[index].Shared == 0) {
        ++privateResidentPages;
      }
    }
  }

  SYSTEM_INFO systemInfo{};
  GetSystemInfo(&systemInfo);
  constexpr double bytesPerMegabyte = 1024.0 * 1024.0;
  return {
      static_cast<double>(counters.WorkingSetSize) / bytesPerMegabyte,
      sampled ? static_cast<double>(privateResidentPages) * systemInfo.dwPageSize / bytesPerMegabyte : -1.0,
      static_cast<double>(counters.PrivateUsage) / bytesPerMegabyte,
  };
}

std::optional<DWORD_PTR> ReadBackgroundState(const ChildProcess& child) {
  DWORD_PTR state = 0;
  if (WaitForSingleObject(child.process.get(), 0) != WAIT_TIMEOUT ||
      !SendMessageTimeoutW(child.window, quickdial::kMessageBenchmarkState, 0, 0,
                           SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &state) ||
      (state & quickdial::kBenchmarkAvailable) == 0 || (state & quickdial::kBenchmarkFailed) != 0) {
    return std::nullopt;
  }
  return state;
}

bool WaitForBackgroundIdle(const ChildProcess& child, bool visible) {
  const auto deadline = Clock::now() + kBackgroundTimeout;
  std::optional<Clock::time_point> idleSince;
  while (Clock::now() < deadline) {
    const auto state = ReadBackgroundState(child);
    if (!state || (IsWindowVisible(child.window) != FALSE) != visible) return false;
    if ((*state & quickdial::kBenchmarkPending) != 0) {
      idleSince.reset();
    } else if (!idleSince) {
      idleSince = Clock::now();
    } else if (Clock::now() - *idleSince >= kSettledInterval) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

bool HideAndWait(const ChildProcess& child) {
  DWORD_PTR ignored = 0;
  return SendMessageTimeoutW(child.window, quickdial::kMessageHideLauncher, 0, 0,
                             SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &ignored) != 0 &&
         WaitForBackgroundIdle(child, false);
}

ChildProcess LaunchQuickDial(
    const std::filesystem::path& applicationPath, HANDLE readyEvent,
    std::wstring_view readyEventName, std::wstring_view presentedEventName) {
  if (!ResetEvent(readyEvent)) return {};
  std::wstring commandLine = Quote(applicationPath.wstring()) + L" --benchmark-events " +
                             Quote(readyEventName) + L" " + Quote(presentedEventName);
  std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
  mutableCommand.push_back(L'\0');

  STARTUPINFOW startupInfo{};
  startupInfo.cb = sizeof(startupInfo);
  PROCESS_INFORMATION processInfo{};
  const auto started = Clock::now();
  if (!CreateProcessW(applicationPath.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, applicationPath.parent_path().c_str(), &startupInfo, &processInfo)) {
    return {};
  }
  UniqueHandle thread(processInfo.hThread);
  UniqueHandle process(processInfo.hProcess);

  const HANDLE waitHandles[] = {readyEvent, process.get()};
  const DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE,
      static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(kReadyTimeout).count()));
  if (waitResult != WAIT_OBJECT_0) {
    TerminateProcess(process.get(), 3);
    WaitForSingleObject(process.get(), 1000);
    return {};
  }

  HWND window = nullptr;
  const auto windowDeadline = Clock::now() + std::chrono::milliseconds(250);
  while (window == nullptr && Clock::now() < windowDeadline) {
    window = FindWindowW(quickdial::kWindowClassName, nullptr);
    DWORD windowProcessId = 0;
    if (window && (!GetWindowThreadProcessId(window, &windowProcessId) ||
                   windowProcessId != processInfo.dwProcessId)) {
      window = nullptr;
    }
    if (window == nullptr) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  ChildProcess child;
  child.process = std::move(process);
  child.id = processInfo.dwProcessId;
  child.window = window;
  child.startupMs = ElapsedMilliseconds(started, Clock::now());
  return child;
}

std::optional<double> CloseChild(ChildProcess& child) {
  if (!child.process) {
    return std::nullopt;
  }
  const auto started = Clock::now();
  const bool closeRequested = WaitForSingleObject(child.process.get(), 0) == WAIT_TIMEOUT &&
      child.window && PostMessageW(child.window, WM_CLOSE, 0, 0);
  const bool exited = WaitForSingleObject(child.process.get(), 3000) == WAIT_OBJECT_0;
  DWORD exitCode = 0;
  const bool cleanExit = closeRequested && exited &&
      GetExitCodeProcess(child.process.get(), &exitCode) && exitCode == 0;
  const double elapsed = ElapsedMilliseconds(started, Clock::now());
  if (!exited) {
    std::cerr << "Launcher did not exit within 3 seconds; forced cleanup is a benchmark failure.\n";
    if (!TerminateProcess(child.process.get(), 4) ||
        WaitForSingleObject(child.process.get(), 1000) != WAIT_OBJECT_0) {
      std::cerr << "Could not clean up benchmark child PID " << child.id << ".\n";
    }
  }
  child.process.reset();
  return cleanExit ? std::optional<double>(elapsed) : std::nullopt;
}

std::optional<double> ShowAndMeasure(HWND window, HANDLE presentedEvent) {
  if (!ResetEvent(presentedEvent)) return std::nullopt;
  const auto started = Clock::now();
  if (!PostMessageW(window, quickdial::kMessageShowLauncher, 0, 0)) {
    return std::nullopt;
  }
  const DWORD waitResult = WaitForSingleObject(
      presentedEvent,
      static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(kPresentationTimeout).count()));
  if (waitResult != WAIT_OBJECT_0 || !IsWindowVisible(window)) {
    return std::nullopt;
  }
  return ElapsedMilliseconds(started, Clock::now());
}

std::string BuildSyntheticCatalogJson(std::size_t count) {
  std::string json = R"({"version":1,"applications":[)";
  for (std::size_t index = 0; index < count; ++index) {
    if (index != 0) {
      json.push_back(',');
    }
    json += R"({"name":"Application )" + std::to_string(index) +
            R"(","target":"app)" + std::to_string(index) +
            R"(.exe","aliases":["tool )" + std::to_string(index) + R"("]})";
  }
  json += "]}";
  return json;
}

double BenchmarkCatalogParsing() {
  constexpr int iterations = 200;
  const std::string json = BuildSyntheticCatalogJson(100);
  std::size_t parsedEntries = 0;
  const auto started = Clock::now();
  for (int iteration = 0; iteration < iterations; ++iteration) {
    const auto result = quickdial::ParseCatalogJson(json);
    if (!result) {
      return -1.0;
    }
    parsedEntries += result.catalog->applications.size();
  }
  const double elapsed = ElapsedMilliseconds(started, Clock::now());
  if (parsedEntries != static_cast<std::size_t>(iterations) * 100) {
    return -1.0;
  }
  return elapsed / iterations;
}

double BenchmarkSearch() {
  quickdial::Catalog catalog;
  for (int index = 0; index < 100; ++index) {
    quickdial::ApplicationEntry application;
    application.name = L"Application " + std::to_wstring(index);
    application.target = L"app" + std::to_wstring(index) + L".exe";
    application.aliases.push_back(L"tool " + std::to_wstring(index));
    catalog.applications.emplace_back(std::move(application));
  }

  constexpr int iterations = 10'000;
  std::size_t resultChecksum = 0;
  const auto started = Clock::now();
  for (int iteration = 0; iteration < iterations; ++iteration) {
    const auto results = quickdial::RankApplications(catalog, L"application 9", 6);
    resultChecksum += results.size();
  }
  const double elapsed = ElapsedMilliseconds(started, Clock::now());
  if (resultChecksum == 0) {
    return -1.0;
  }
  return elapsed / iterations;
}

bool CheckMaximum(std::string_view label, double value, double budget, std::string_view unit) {
  const bool passed = value >= 0.0 && value <= budget;
  std::cout << std::left << std::setw(35) << label << std::right << std::setw(10) << std::fixed
            << std::setprecision(2) << value << " " << std::setw(4) << unit << "   <= "
            << std::setw(8) << budget << "   " << (passed ? "PASS" : "FAIL") << '\n';
  return passed;
}

bool PrintAndEvaluate(const BenchmarkResults& results) {
  const double startupMedian = Percentile(results.startupTimesMs, 0.50);
  const double startupP95 = Percentile(results.startupTimesMs, 0.95);
  const double warmShowMedian = Percentile(results.warmShowTimesMs, 0.50);
  const double warmShowP95 = Percentile(results.warmShowTimesMs, 0.95);

  std::cout << "\nApplication Quick Dial performance contract\n";
  std::cout << "Fresh-process startup median: " << std::fixed << std::setprecision(2) << startupMedian << " ms\n";
  std::cout << "Warm show median:            " << warmShowMedian << " ms\n";
  std::cout << "Discovery settled after ready (includes 250 ms quiet): " << results.discoverySettledMs << " ms\n";
  std::cout << "Graceful shutdown p95:       " << Percentile(results.shutdownTimesMs, 0.95) << " ms\n\n";

  bool passed = true;
  passed &= CheckMaximum("Fresh-process startup p95", startupP95, kStartupP95BudgetMs, "ms");
  passed &= CheckMaximum("First show to first paint", results.firstShowMs, kFirstShowBudgetMs, "ms");
  passed &= CheckMaximum("Warm show to first paint p95", warmShowP95, kWarmShowP95BudgetMs, "ms");
  passed &= CheckMaximum("Hidden initial working set", results.hiddenInitial.workingSetMb,
                         kHiddenInitialWorkingSetBudgetMb, "MB");
  passed &= CheckMaximum("Hidden initial private working set", results.hiddenInitial.privateWorkingSetMb,
                         kHiddenInitialPrivateWorkingSetBudgetMb, "MB");
  passed &= CheckMaximum("Hidden initial private bytes", results.hiddenInitial.privateMb,
                         kHiddenInitialPrivateBudgetMb, "MB");
  passed &= CheckMaximum("Visible working set", results.visible.workingSetMb,
                         kVisibleWorkingSetBudgetMb, "MB");
  passed &= CheckMaximum("Visible private working set", results.visible.privateWorkingSetMb,
                         kVisiblePrivateWorkingSetBudgetMb, "MB");
  passed &= CheckMaximum("Visible private bytes", results.visible.privateMb,
                         kVisiblePrivateBudgetMb, "MB");
  passed &= CheckMaximum("Hidden-after-use working set", results.hiddenAfterUse.workingSetMb,
                         kHiddenAfterUseWorkingSetBudgetMb, "MB");
  passed &= CheckMaximum("Hidden-after-use private working set", results.hiddenAfterUse.privateWorkingSetMb,
                         kHiddenAfterUsePrivateWorkingSetBudgetMb, "MB");
  passed &= CheckMaximum("Hidden-after-use private bytes", results.hiddenAfterUse.privateMb,
                         kHiddenAfterUsePrivateBudgetMb, "MB");
  passed &= CheckMaximum("Hidden idle CPU over 3 seconds", results.hiddenIdleCpuMs,
                         kHiddenIdleCpuBudgetMs, "ms");
  passed &= CheckMaximum("Visible idle CPU over 3 seconds", results.visibleIdleCpuMs,
                         kVisibleIdleCpuBudgetMs, "ms");
  passed &= CheckMaximum("Parse 100-app catalog average", results.catalogParseAverageMs,
                         kCatalogParseAverageBudgetMs, "ms");
  passed &= CheckMaximum("Search 100 apps average", results.searchAverageMs,
                         kSearchAverageBudgetMs, "ms");
  passed &= CheckMaximum("Release executable size", results.executableSizeKb,
                         kExecutableSizeBudgetKb, "KB");

  std::cout << "\nOverall: " << (passed ? "PASS" : "FAIL") << '\n';
  return passed;
}

}  // namespace

int main() {
  winrt::init_apartment(winrt::apartment_type::single_threaded);

  if (FindWindowW(quickdial::kWindowClassName, nullptr) != nullptr) {
    std::cerr << "Application Quick Dial is already running. Exit it before benchmarking.\n";
    return 2;
  }

  const std::filesystem::path benchmarkPath = CurrentExecutablePath();
  const std::filesystem::path applicationPath = benchmarkPath.parent_path() / L"ApplicationQuickDial.exe";
  if (!std::filesystem::exists(applicationPath)) {
    std::cerr << "ApplicationQuickDial.exe was not found next to the benchmark executable.\n";
    return 2;
  }

  const std::wstring suffix = std::to_wstring(GetCurrentProcessId());
  const std::wstring readyEventName = L"Local\\ApplicationQuickDial.Benchmark.Ready." + suffix;
  const std::wstring presentedEventName = L"Local\\ApplicationQuickDial.Benchmark.Presented." + suffix;
  UniqueHandle readyEvent(CreateEventW(nullptr, TRUE, FALSE, readyEventName.c_str()));
  UniqueHandle presentedEvent(CreateEventW(nullptr, TRUE, FALSE, presentedEventName.c_str()));
  if (!readyEvent || !presentedEvent) {
    std::cerr << "Could not create benchmark synchronization events.\n";
    return 2;
  }

  BenchmarkResults results;
  results.catalogParseAverageMs = BenchmarkCatalogParsing();
  results.searchAverageMs = BenchmarkSearch();
  results.executableSizeKb = static_cast<double>(std::filesystem::file_size(applicationPath)) / 1024.0;

  for (int trial = 0; trial < kStartupTrials; ++trial) {
    ChildProcess child = LaunchQuickDial(
        applicationPath, readyEvent.get(), readyEventName, presentedEventName);
    if (!child.process || child.window == nullptr) {
      std::cerr << "Could not start or find the benchmark launcher process.\n";
      CloseChild(child);
      return 2;
    }
    results.startupTimesMs.push_back(child.startupMs);

    if (trial == 0) {
      const auto discoveryStarted = Clock::now();
      if (!WaitForBackgroundIdle(child, false)) {
        std::cerr << "Startup discovery failed or did not settle within 10 seconds.\n";
        CloseChild(child);
        return 2;
      }
      results.discoverySettledMs = ElapsedMilliseconds(discoveryStarted, Clock::now());
      results.hiddenInitial = ReadMemory(child.process.get());

      const auto firstShow = ShowAndMeasure(child.window, presentedEvent.get());
      if (!firstShow) {
        std::cerr << "The first launcher frame did not arrive.\n";
        CloseChild(child);
        return 2;
      }
      results.firstShowMs = *firstShow;
      if (!WaitForBackgroundIdle(child, true) || !HideAndWait(child)) {
        std::cerr << "First-show icon loading or hide did not complete successfully.\n";
        CloseChild(child);
        return 2;
      }

      for (int showTrial = 0; showTrial < kShowTrials; ++showTrial) {
        const auto showTime = ShowAndMeasure(child.window, presentedEvent.get());
        if (!showTime) {
          std::cerr << "A warm launcher frame did not arrive.\n";
          CloseChild(child);
          return 2;
        }
        results.warmShowTimesMs.push_back(*showTime);
        if (!WaitForBackgroundIdle(child, true) || !HideAndWait(child)) {
          std::cerr << "Warm-show icon loading or hide did not complete successfully.\n";
          CloseChild(child);
          return 2;
        }
      }

      if (!ShowAndMeasure(child.window, presentedEvent.get())) {
        std::cerr << "Could not show the launcher for its visible resource sample.\n";
        CloseChild(child);
        return 2;
      }
      if (!WaitForBackgroundIdle(child, true)) {
        std::cerr << "Visible launcher did not settle for its resource sample.\n";
        CloseChild(child);
        return 2;
      }
      results.visible = ReadMemory(child.process.get());
      results.visibleIdleCpuMs = MeasureIdleCpu(child.process.get(), kIdleSampleDuration);
      const auto visibleState = ReadBackgroundState(child);
      if (!visibleState || (*visibleState & quickdial::kBenchmarkPending) != 0 || !IsWindowVisible(child.window)) {
        std::cerr << "Visible idle sample was interrupted. Keep the benchmark launcher in the foreground.\n";
        CloseChild(child);
        return 2;
      }

      if (!HideAndWait(child)) {
        std::cerr << "Launcher did not hide and settle for its resource sample.\n";
        CloseChild(child);
        return 2;
      }
      results.hiddenAfterUse = ReadMemory(child.process.get());
      results.hiddenIdleCpuMs = MeasureIdleCpu(child.process.get(), kIdleSampleDuration);
      const auto hiddenState = ReadBackgroundState(child);
      if (!hiddenState || (*hiddenState & quickdial::kBenchmarkPending) != 0 || IsWindowVisible(child.window)) {
        std::cerr << "Hidden idle sample was interrupted.\n";
        CloseChild(child);
        return 2;
      }
    }

    const auto shutdown = CloseChild(child);
    if (!shutdown) {
      std::cerr << "Benchmark launcher did not shut down normally.\n";
      return 2;
    }
    results.shutdownTimesMs.push_back(*shutdown);
  }

  return PrintAndEvaluate(results) ? 0 : 1;
}
