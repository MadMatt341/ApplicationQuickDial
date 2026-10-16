#include "UgsActivation.h"

#include <windows.h>
#include <shobjidl.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <array>
#include <cstdint>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <type_traits>

namespace quickdial {
namespace {
using namespace winrt::Windows::Data::Json;
constexpr wchar_t kClientClass[] = L"Blade.Tray.Client.v1";
constexpr UINT kSnapshot = WM_APP + 91;
constexpr ULONG_PTR kSnapshotData = 0x42545301;
constexpr ULONG_PTR kActionData = 0x42544101;

bool Equal(std::wstring_view left, std::wstring_view right) {
  return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                              static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

std::filesystem::path PlainTarget(const ApplicationEntry& application) {
  if (!application.arguments.empty() || !application.shortcutArguments.empty()) return {};
  auto target = std::filesystem::path(application.resolvedTarget.empty() ? application.target : application.resolvedTarget).lexically_normal();
  if (!target.is_absolute() || !Equal(target.filename().native(), L"UnrealGameSync.exe")) return {};
  const auto launch = std::filesystem::path(application.target);
  if (Equal(launch.extension().native(), L".lnk")) {
    // Read the accepted shortcut again: cached discovery metadata must not drop
    // arguments added since the catalog refresh. Do not call ShellLink::Resolve.
    winrt::com_ptr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.put())))) return {};
    const auto file = link.as<IPersistFile>();
    std::array<wchar_t, 32768> buffer{};
    int show = 0;
    if (FAILED(file->Load(launch.c_str(), STGM_READ)) ||
        FAILED(link->GetShowCmd(&show)) || show != SW_SHOWNORMAL ||
        FAILED(link->GetArguments(buffer.data(), static_cast<int>(buffer.size()))) || buffer[0]) return {};
    if (FAILED(link->GetPath(buffer.data(), static_cast<int>(buffer.size()), nullptr, SLGP_RAWPATH))) return {};
    target = std::filesystem::path(ExpandEnvironment(buffer.data())).lexically_normal();
  } else if (!Equal(launch.lexically_normal().native(), target.native())) {
    return {};
  }
  return target.is_absolute() && Equal(target.filename().native(), L"UnrealGameSync.exe") ? target : std::filesystem::path{};
}

bool SameImage(const std::filesystem::path& target, DWORD pid) {
  std::wstring image(32768, L'\0');
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return false;
  DWORD length = static_cast<DWORD>(image.size());
  const bool read = QueryFullProcessImageNameW(process, 0, image.data(), &length) != FALSE;
  CloseHandle(process);
  if (!read) return false;
  image.resize(length);
  const auto path = std::filesystem::path(image).lexically_normal();
  if (Equal(target.native(), path.native())) return true;
  // The UGS updater starts Latest beside its stable Launcher directory.
  return Equal(path.filename().native(), L"UnrealGameSync.exe") &&
      Equal(target.parent_path().filename().native(), L"Launcher") &&
      Equal(path.parent_path().filename().native(), L"Latest") &&
      Equal(target.parent_path().parent_path().native(), path.parent_path().parent_path().native());
}

struct Snapshot {
  HWND host = nullptr;
  std::string bytes;
};

LRESULT CALLBACK ClientProcedure(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_NCCREATE) {
    const auto create = reinterpret_cast<CREATESTRUCTW*>(l);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
  }
  if (message == WM_COPYDATA && l) {
    const auto state = reinterpret_cast<Snapshot*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    const auto data = reinterpret_cast<const COPYDATASTRUCT*>(l);
    if (state && reinterpret_cast<HWND>(w) == state->host && data->dwData == kSnapshotData &&
        data->lpData && data->cbData && data->cbData <= 1'500'000) {
      try {
        state->bytes.assign(static_cast<const char*>(data->lpData), data->cbData);
        return 1;
      } catch (...) { return 0; }
    }
    return 0;
  }
  return DefWindowProcW(window, message, w, l);
}

BOOL CALLBACK FindHost(HWND window, LPARAM parameter) {
  if (GetPropW(window, L"Blade.NotificationTray.v1")) {
    *reinterpret_cast<HWND*>(parameter) = window;
    return FALSE;
  }
  return TRUE;
}

std::optional<std::int64_t> Integer(const IJsonValue& value, std::int64_t minimum, std::int64_t maximum) {
  if (value.ValueType() != JsonValueType::Number) return {};
  const double number = value.GetNumber();
  if (!std::isfinite(number) || number != std::floor(number) ||
      number < static_cast<double>(minimum) || number > static_cast<double>(maximum)) return {};
  return static_cast<std::int64_t>(number);
}

bool Activate(const std::filesystem::path& target) {
  Snapshot state;
  EnumWindows(FindHost, reinterpret_cast<LPARAM>(&state.host));
  if (!state.host) return false;
  const auto instance = GetModuleHandleW(nullptr);
  WNDCLASSW type{};
  type.hInstance = instance;
  type.lpszClassName = kClientClass;
  type.lpfnWndProc = ClientProcedure;
  if (!RegisterClassW(&type)) {
    WNDCLASSW existing{};
    if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS || !GetClassInfoW(instance, kClientClass, &existing) ||
        existing.lpfnWndProc != ClientProcedure) return false;
  }
  const auto destroy = [](HWND value) { if (value) DestroyWindow(value); };
  const std::unique_ptr<std::remove_pointer_t<HWND>, decltype(destroy)> client(
      CreateWindowExW(0, kClientClass, L"", 0, 0, 0, 0, 0, nullptr, nullptr, instance, &state), destroy);
  if (!client) return false;
  DWORD_PTR result = 0;
  if (!SendMessageTimeoutW(state.host, kSnapshot, reinterpret_cast<WPARAM>(client.get()), 0,
                           SMTO_ABORTIFHUNG, 250, &result) || result != 1 || state.bytes.empty()) return false;

  const auto snapshot = JsonObject::Parse(winrt::to_hstring(state.bytes));
  if (snapshot.GetNamedNumber(L"version") != 1) return false;
  const auto icons = snapshot.GetNamedArray(L"icons");
  if (icons.Size() > 128) return false;
  std::optional<JsonObject> selected;
  DWORD selectedPid = 0;
  for (const auto& value : icons) {
    const auto icon = value.GetObject();
    if (icon.GetNamedString(L"tip", L"") != L"UnrealGameSync") continue;
    const auto pid = Integer(icon.GetNamedValue(L"pid"), 1, MAXDWORD);
    if (!pid || !SameImage(target, static_cast<DWORD>(*pid))) continue;
    if (selected) return false;
    selected = icon;
    selectedPid = static_cast<DWORD>(*pid);
  }
  if (!selected) return false;
  const auto token = Integer(selected->GetNamedValue(L"token"), 1, MAXDWORD);
  if (!token) return false;
  const auto rect = selected->GetNamedArray(L"rect");
  if (rect.Size() != 4) return false;
  std::array<std::int64_t, 4> coordinates{};
  for (std::uint32_t index = 0; index != 4; ++index) {
    const auto number = Integer(rect.GetAt(index), std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
    if (!number) return false;
    coordinates[index] = *number;
  }
  if (coordinates[2] <= coordinates[0] || coordinates[3] <= coordinates[1] ||
      coordinates[2] - coordinates[0] > 512 || coordinates[3] - coordinates[1] > 512) return false;
  JsonObject action;
  action.SetNamedValue(L"token", JsonValue::CreateNumberValue(static_cast<double>(*token)));
  action.SetNamedValue(L"rect", rect);
  action.SetNamedValue(L"action", JsonValue::CreateStringValue(L"double"));
  const auto bytes = winrt::to_string(action.Stringify());
  if (!SameImage(target, selectedPid)) return false;
  AllowSetForegroundWindow(selectedPid);
  COPYDATASTRUCT data{kActionData, static_cast<DWORD>(bytes.size()), const_cast<char*>(bytes.data())};
  result = 0;
  return SendMessageTimeoutW(state.host, WM_COPYDATA, reinterpret_cast<WPARAM>(client.get()),
      reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &result) && result == 1;
}
}  // namespace

bool TryActivateRunningUgs(const ApplicationEntry& application) noexcept {
  try {
    const auto target = PlainTarget(application);
    if (target.empty()) return false;
    return Activate(target);
  } catch (...) {
    return false;
  }
}
}  // namespace quickdial
