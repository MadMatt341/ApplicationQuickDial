#include "StartupInputTrace.h"

#include <winrt/base.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <algorithm>
#include <string>
#include <utility>

namespace quickdial {
namespace {
using namespace winrt::Windows::Data::Json;
constexpr unsigned long long kSeconds = 120;
constexpr std::size_t kBytes = 256 * 1024;

long long Counter() noexcept { LARGE_INTEGER value{}; QueryPerformanceCounter(&value); return value.QuadPart; }
std::wstring Environment(const wchar_t* name) {
  const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
  if (size == 0 || size > 32768) return {};
  std::wstring value(size, L'\0');
  const auto written = GetEnvironmentVariableW(name, value.data(), size);
  if (!written || written >= size) return {};
  value.resize(written); return value;
}
long long Integer(const std::wstring& text) {
  if (text.empty() || text.find_first_not_of(L"0123456789") != std::wstring::npos || text.size() > 18) return 0;
  return std::stoll(text);
}
void Number(JsonObject& object, const wchar_t* name, long long value) {
  object.SetNamedValue(name, JsonValue::CreateNumberValue(static_cast<double>(value)));
}
void Boolean(JsonObject& object, const wchar_t* name, bool value) {
  object.SetNamedValue(name, JsonValue::CreateBooleanValue(value));
}
void String(JsonObject& object, const wchar_t* name, std::wstring_view value) {
  object.SetNamedValue(name, JsonValue::CreateStringValue(value));
}
std::wstring ObjectName(HANDLE handle) {
  wchar_t name[256]{}; DWORD bytes = 0;
  return handle && GetUserObjectInformationW(handle, UOI_NAME, name, sizeof(name), &bytes) ? name : L"unavailable";
}
struct FocusState {
  HWND foreground = nullptr, active = nullptr, focus = nullptr;
  DWORD pid = 0, tid = 0, guiError = 0;
  bool visible = false, enabled = false, iconic = false;
  std::wstring inputDesktop;
  bool operator==(const FocusState&) const = default;
};
FocusState Focus() {
  FocusState state;
  state.foreground = GetForegroundWindow();
  state.tid = GetWindowThreadProcessId(state.foreground, &state.pid);
  GUITHREADINFO gui{sizeof(gui)};
  if (state.tid && GetGUIThreadInfo(state.tid, &gui)) { state.active = gui.hwndActive; state.focus = gui.hwndFocus; }
  else state.guiError = state.tid ? GetLastError() : ERROR_INVALID_WINDOW_HANDLE;
  state.visible = IsWindowVisible(state.foreground) != FALSE;
  state.enabled = IsWindowEnabled(state.foreground) != FALSE;
  state.iconic = IsIconic(state.foreground) != FALSE;
  const auto desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
  state.inputDesktop = ObjectName(desktop);
  if (desktop) CloseDesktop(desktop);
  return state;
}
JsonObject FocusDetails(const FocusState& state) {
  JsonObject details;
  Number(details, L"foreground", reinterpret_cast<INT_PTR>(state.foreground));
  Number(details, L"active", reinterpret_cast<INT_PTR>(state.active));
  Number(details, L"focus", reinterpret_cast<INT_PTR>(state.focus));
  Number(details, L"pid", state.pid); Number(details, L"tid", state.tid); Number(details, L"guiError", state.guiError);
  Boolean(details, L"visible", state.visible); Boolean(details, L"enabled", state.enabled); Boolean(details, L"minimized", state.iconic);
  String(details, L"inputDesktop", state.inputDesktop);
  String(details, L"observerDesktop", ObjectName(GetThreadDesktop(GetCurrentThreadId())));
  wchar_t className[256]{}; GetClassNameW(state.foreground, className, 256);
  String(details, L"windowClass", className);
  const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, state.pid);
  if (process) {
    wchar_t path[32768]{}; DWORD length = 32768;
    if (QueryFullProcessImageNameW(process, 0, path, &length)) String(details, L"executable", std::wstring_view(path, length));
    HANDLE token = nullptr;
    if (OpenProcessToken(process, TOKEN_QUERY, &token)) {
      alignas(TOKEN_MANDATORY_LABEL) BYTE storage[256]{}; DWORD bytes = 0;
      if (GetTokenInformation(token, TokenIntegrityLevel, storage, sizeof(storage), &bytes)) {
        const auto sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(storage)->Label.Sid;
        const auto count = *GetSidSubAuthorityCount(sid);
        if (count) Number(details, L"integrityRid", *GetSidSubAuthority(sid, count - 1));
      }
      CloseHandle(token);
    }
    CloseHandle(process);
  }
  return details;
}
const wchar_t* Phase(InputTracePhase phase) {
  switch (phase) {
    case InputTracePhase::HookReady: return L"keyboard_hook_ready";
    case InputTracePhase::MouseHookReady: return L"mouse_hook_ready";
    case InputTracePhase::Key: return L"key_observed";
    case InputTracePhase::ToggleReceived: return L"toggle_received";
    case InputTracePhase::ShowRequested: return L"show_requested";
    case InputTracePhase::ShowCompleted: return L"show_completed";
    case InputTracePhase::Hide: return L"hide_requested";
    case InputTracePhase::Deactivate: return L"deactivated";
    case InputTracePhase::MouseButton: return L"mouse_button";
  }
  return L"unknown";
}
JsonObject Details(const InputTraceEvent& event) {
  JsonObject details;
  Number(details, L"foreground", reinterpret_cast<INT_PTR>(event.foreground));
  Number(details, L"target", reinterpret_cast<INT_PTR>(event.target));
  Number(details, L"focus", reinterpret_cast<INT_PTR>(event.focus));
  Number(details, L"error", event.error); Boolean(details, L"accepted", event.accepted);
  if (event.phase == InputTracePhase::Key) {
    Number(details, L"virtualKey", event.key); Boolean(details, L"down", event.down); Boolean(details, L"injected", event.injected);
    Number(details, L"trackedBefore", event.before); Number(details, L"trackedAfter", event.after);
    Boolean(details, L"asyncSampled", event.asyncSampled);
    Number(details, L"asyncLeft", event.asyncLeft); Number(details, L"asyncRight", event.asyncRight);
    Number(details, L"disposition", event.disposition); Number(details, L"maskSent", event.maskSent); Number(details, L"togglePosted", event.posted);
  } else if (event.phase == InputTracePhase::MouseButton) Number(details, L"buttonMessage", event.key);
  return details;
}
}  // namespace

std::unique_ptr<StartupInputTrace> StartupInputTrace::StartFromEnvironment() noexcept {
  try {
    auto trace = std::unique_ptr<StartupInputTrace>(new StartupInputTrace);
    trace->origin_ = Integer(Environment(L"BLADE_STARTUP_QPC"));
    trace->frequency_ = Integer(Environment(L"BLADE_STARTUP_FREQUENCY"));
    const std::filesystem::path directory(Environment(L"BLADE_STARTUP_DIR"));
    if (!directory.is_absolute() || !std::filesystem::is_directory(directory) || trace->origin_ <= 0 || trace->frequency_ <= 0) return {};
    const auto now = Counter();
    if (now < trace->origin_ || static_cast<unsigned long long>((now - trace->origin_) / trace->frequency_) >= kSeconds) return {};
    const auto marker = directory.parent_path() / L"trace-launcher-input.once";
    if (!std::filesystem::is_regular_file(marker)) return {};
    trace->path_ = directory / (L"launcher-input-" + std::to_wstring(GetCurrentProcessId()) + L".jsonl");
    trace->file_ = CreateFileW(trace->path_.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (trace->file_ == INVALID_HANDLE_VALUE) return {};
    trace->stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    trace->wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!trace->stop_ || !trace->wake_) return {};
    trace->enabled_ = true;
    trace->worker_ = std::thread(&StartupInputTrace::Run, trace.get());
    // Claim only after the writer and worker exist. Do not silently leave a
    // repeated capture enabled if this one-shot request cannot be consumed.
    if (!DeleteFileW(marker.c_str())) return {};
    return trace;
  } catch (...) { return {}; }
}

StartupInputTrace::~StartupInputTrace() {
  Stop();
  if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
  if (stop_) CloseHandle(stop_);
  if (wake_) CloseHandle(wake_);
}
void StartupInputTrace::Stop() noexcept {
  enabled_ = false;
  if (stop_) SetEvent(stop_);
  if (worker_.joinable()) worker_.join();
}
void StartupInputTrace::SetHookThread(DWORD thread) noexcept {
  hookThread_ = thread;
  if (thread && !enabled()) PostThreadMessageW(thread, kStoppedMessage, 0, 0);
}
void StartupInputTrace::Record(InputTracePhase phase, HWND target, bool accepted, HWND focus) noexcept {
  InputTraceEvent event; event.phase = phase; event.target = target; event.accepted = accepted; event.focus = focus;
  Record(event);
}
void StartupInputTrace::Record(InputTraceEvent event) noexcept {
  if (!enabled_.load(std::memory_order_relaxed)) return;
  if (event.phase == InputTracePhase::Key && event.key != VK_LWIN && event.key != VK_RWIN && event.key != VK_SPACE) return;
  if (!event.qpc) { event.qpc = Counter(); event.foreground = GetForegroundWindow(); }
  std::unique_lock lock(mutex_, std::try_to_lock);
  if (!lock || count_ == queue_.size()) { ++dropped_; return; }
  queue_[count_++] = event;
  SetEvent(wake_);
}
void StartupInputTrace::Run() noexcept {
  bool apartment = false;
  try {
    winrt::init_apartment(winrt::apartment_type::multi_threaded); apartment = true;
    std::size_t bytes = 0; unsigned sequence = 0;
    const auto write = [&](const wchar_t* phase, const JsonObject& details, long long qpc) {
      JsonObject row;
      Number(row, L"schemaVersion", 1); String(row, L"originQpc", std::to_wstring(origin_));
      String(row, L"qpcFrequency", std::to_wstring(frequency_)); String(row, L"qpc", std::to_wstring(qpc));
      Number(row, L"elapsedUs", (qpc - origin_) * 1000000 / frequency_);
      Number(row, L"emitterPid", GetCurrentProcessId()); String(row, L"role", L"launcher-input");
      Number(row, L"sequence", sequence); String(row, L"component", L"launcher-input"); String(row, L"phase", phase);
      row.SetNamedValue(L"subjectPid", JsonValue::CreateNullValue()); row.SetNamedValue(L"resources", JsonValue::CreateNullValue());
      row.SetNamedValue(L"details", details);
      auto line = winrt::to_string(row.Stringify()); line += '\n';
      const bool closing = std::wstring_view(phase) == L"capture_closed";
      if (bytes + line.size() > kBytes - (closing ? 0 : 4096)) return false;
      DWORD written = 0;
      if (!WriteFile(file_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) || written != line.size()) return false;
      bytes += written; ++sequence; return true;
    };
    JsonObject entry; wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
    String(entry, L"identity", executable); Number(entry, L"maximumSeconds", kSeconds); Number(entry, L"maximumBytes", kBytes);
    String(entry, L"scope", L"Win/Space events, mouse-button timing and native focus metadata; no typed text, titles or coordinates");
    bool writable = write(L"process_entry", entry, Counter());
    FocusState previous = Focus();
    writable = writable && write(L"focus_snapshot", FocusDetails(previous), Counter());
    const HANDLE handles[] = {stop_, wake_};
    const wchar_t* reason = L"owner_stopped";
    for (;;) {
      const auto wait = WaitForMultipleObjects(2, handles, FALSE, 250);
      std::array<InputTraceEvent, 256> pending{}; std::size_t count;
      { std::scoped_lock lock(mutex_); count = count_; std::copy_n(queue_.begin(), count, pending.begin()); count_ = 0; }
      for (std::size_t index = 0; index < count && writable; ++index) {
        if ((pending[index].qpc - origin_) / frequency_ < static_cast<long long>(kSeconds)) {
          auto details = Details(pending[index]);
          String(details, L"observedQpc", std::to_wstring(pending[index].qpc));
          Number(details, L"observedElapsedUs", (pending[index].qpc - origin_) * 1000000 / frequency_);
          // Keep the stream's writer clock monotonic. Callback time is separate
          // because another thread can offer an older observation after a row.
          writable = write(Phase(pending[index].phase), details, Counter());
        }
      }
      const auto state = Focus();
      if (!(state == previous)) { writable = writable && write(L"focus_snapshot", FocusDetails(state), Counter()); previous = state; }
      if (!writable) { reason = L"size_or_write_limit"; break; }
      if (wait == WAIT_OBJECT_0) break;
      if (wait == WAIT_FAILED) { reason = L"wait_failed"; break; }
      if (static_cast<unsigned long long>((Counter() - origin_) / frequency_) >= kSeconds) { reason = L"time_limit"; break; }
    }
    enabled_ = false;
    JsonObject closed; String(closed, L"reason", reason); Number(closed, L"eventsDropped", dropped_.load());
    write(L"capture_closed", closed, Counter());
  } catch (...) { enabled_ = false; }
  if (const auto thread = hookThread_.load()) PostThreadMessageW(thread, kStoppedMessage, 0, 0);
  if (apartment) winrt::uninit_apartment();
  CloseHandle(file_); file_ = INVALID_HANDLE_VALUE;
}
}  // namespace quickdial
