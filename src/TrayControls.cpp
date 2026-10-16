#include "TrayControls.h"
#include <windows.h>
#include <roapi.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <memory>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace quickdial {
namespace {
using namespace winrt::Windows::Data::Json;
constexpr wchar_t kClientClass[] = L"Blade.Tray.Client.v1";
struct Apartment {
  HRESULT result = RoInitialize(RO_INIT_MULTITHREADED);
  ~Apartment() { if (SUCCEEDED(result)) RoUninitialize(); }
};
struct Receiver { HWND host = nullptr; std::string bytes; };
LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_NCCREATE) SetWindowLongPtrW(window, GWLP_USERDATA,
      reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
  if (message == WM_COPYDATA && l) {
    const auto receiver = reinterpret_cast<Receiver*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    const auto data = reinterpret_cast<const COPYDATASTRUCT*>(l);
    if (receiver && reinterpret_cast<HWND>(w) == receiver->host && data->dwData == 0x42545301 &&
        data->lpData && data->cbData && data->cbData <= 1'500'000) {
      try { receiver->bytes.assign(static_cast<const char*>(data->lpData), data->cbData); return 1; }
      catch (...) { return 0; }
    }
    return 0;
  }
  return DefWindowProcW(window, message, w, l);
}
BOOL CALLBACK FindHost(HWND window, LPARAM parameter) {
  if (GetPropW(window, L"Blade.NotificationTray.v1")) {
    *reinterpret_cast<HWND*>(parameter) = window; return FALSE;
  }
  return TRUE;
}
struct Client {
  Receiver receiver;
  HWND window = nullptr;
  explicit Client(HWND host = nullptr) {
    receiver.host = host;
    if (!host) EnumWindows(FindHost, reinterpret_cast<LPARAM>(&receiver.host));
    if (!receiver.host || !GetPropW(receiver.host, L"Blade.NotificationTray.v1")) return;
    const auto module = GetModuleHandleW(nullptr);
    WNDCLASSW type{}; type.hInstance = module; type.lpszClassName = kClientClass; type.lpfnWndProc = Procedure;
    if (!RegisterClassW(&type)) {
      WNDCLASSW existing{};
      if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS || !GetClassInfoW(module, kClientClass, &existing) ||
          existing.lpfnWndProc != Procedure) return;
    }
    window = CreateWindowW(kClientClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, module, &receiver);
  }
  ~Client() { if (window) DestroyWindow(window); }
  Client(const Client&) = delete;
  bool Snapshot() {
    DWORD_PTR result = 0;
    return window && SendMessageTimeoutW(receiver.host, WM_APP + 91, reinterpret_cast<WPARAM>(window), 0,
        SMTO_ABORTIFHUNG, 250, &result) && result == 1 && !receiver.bytes.empty();
  }
};
std::uint32_t PositiveInteger(const IJsonValue& value) {
  const auto number = value.GetNumber();
  if (!std::isfinite(number) || number != std::floor(number) || number < 1 || number > MAXDWORD)
    throw std::invalid_argument("Invalid tray identity");
  return static_cast<std::uint32_t>(number);
}
std::wstring ProcessPath(DWORD pid) {
  std::wstring path(32768, L'\0');
  const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return {};
  DWORD size = static_cast<DWORD>(path.size());
  const bool read = QueryFullProcessImageNameW(process, 0, path.data(), &size) != FALSE;
  CloseHandle(process);
  if (!read) return {};
  path.resize(size); return path;
}
unsigned Hex(wchar_t c) {
  if (c >= L'0' && c <= L'9') return c - L'0';
  if (c >= L'a' && c <= L'f') return c - L'a' + 10;
  throw std::invalid_argument("Invalid tray pixels");
}
TrayControlsSnapshot Decode(Client& client) {
  const auto root = JsonObject::Parse(winrt::to_hstring(client.receiver.bytes));
  if (root.GetNamedNumber(L"version") != 1) throw std::invalid_argument("Unsupported tray snapshot");
  const auto session = winrt::to_string(root.GetNamedString(L"hostSession"));
  if (session.empty() || session.size() > 128 || session.find('\0') != std::string::npos)
    throw std::invalid_argument("Missing tray session");
  const auto icons = root.GetNamedArray(L"icons");
  if (icons.Size() > 128) throw std::invalid_argument("Too many tray icons");
  TrayControlsSnapshot snapshot; snapshot.connected = true;
  snapshot.host = reinterpret_cast<std::uintptr_t>(client.receiver.host);
  snapshot.hostSession = session;
  snapshot.revision = root.GetNamedNumber(L"revision");
  if (!std::isfinite(snapshot.revision) || snapshot.revision < 0 || snapshot.revision > 9007199254740991.0 ||
      snapshot.revision != std::floor(snapshot.revision)) throw std::invalid_argument("Invalid tray revision");
  std::set<std::uint32_t> tokens;
  for (const auto& value : icons) {
    const auto icon = value.GetObject();
    TrayControl item;
    item.host = reinterpret_cast<std::uintptr_t>(client.receiver.host);
    item.hostSession = session;
    item.token = PositiveInteger(icon.GetNamedValue(L"token"));
    item.pid = PositiveInteger(icon.GetNamedValue(L"pid"));
    if (!tokens.insert(item.token).second) throw std::invalid_argument("Duplicate tray token");
    item.name = icon.GetNamedString(L"tip");
    if (item.name.size() > 128 || item.name.find(L'\0') != std::wstring::npos) throw std::invalid_argument("Invalid tray label");
    for (auto& c : item.name) if (c < L' ') c = L' ';
    item.processPath = ProcessPath(item.pid);
    if (item.name.empty()) item.name = std::filesystem::path(item.processPath).stem().wstring();
    if (item.name.empty()) item.name = L"Application notification icon";
    const auto pixels = icon.GetNamedString(L"pixels");
    if (!pixels.empty()) {
      if (pixels.size() != 8192) throw std::invalid_argument("Invalid tray image size");
      item.image.width = item.image.height = 32;
      item.image.pixels.reserve(4096);
      for (std::uint32_t index = 0; index != pixels.size(); index += 2)
        item.image.pixels.push_back(static_cast<std::uint8_t>(Hex(pixels[index]) * 16 + Hex(pixels[index + 1])));
    }
    snapshot.items.push_back(std::move(item));
  }
  return snapshot;
}
}  // namespace

TrayControlsSnapshot ReadTrayControls() noexcept {
  try {
    Apartment apartment;
    Client client;
    if (!client.receiver.host) return {};
    if (!client.Snapshot()) return {{}, L"Blade tray did not respond.", false};
    return Decode(client);
  } catch (...) { return {{}, L"Blade tray returned an unsupported or incorrect snapshot.", false}; }
}

bool RequestTrayControlMenu(const TrayControl& selected, const std::array<int, 4>& anchor,
                           std::stop_token cancelled, std::wstring& error) noexcept {
  error = L"This tray control is no longer available. Search again.";
  try {
    if (cancelled.stop_requested()) return false;
    Apartment apartment;
    Client client(reinterpret_cast<HWND>(selected.host));
    if (!client.Snapshot()) return false;
    const auto snapshot = Decode(client);
    const auto found = std::find_if(snapshot.items.begin(), snapshot.items.end(), [&](const auto& item) {
      return selected.SameRegistration(item);
    });
    if (found == snapshot.items.end() || cancelled.stop_requested()) return false;
    JsonObject action;
    action.SetNamedValue(L"hostSession", JsonValue::CreateStringValue(winrt::to_hstring(selected.hostSession)));
    action.SetNamedValue(L"token", JsonValue::CreateNumberValue(selected.token));
    action.SetNamedValue(L"action", JsonValue::CreateStringValue(L"keyboard-menu"));
    JsonArray rect;
    for (const int value : anchor) rect.Append(JsonValue::CreateNumberValue(value));
    action.SetNamedValue(L"rect", rect);
    const auto bytes = winrt::to_string(action.Stringify());
    COPYDATASTRUCT data{0x42544101, static_cast<DWORD>(bytes.size()), const_cast<char*>(bytes.data())};
    DWORD_PTR result = 0;
    if (!cancelled.stop_requested() && SendMessageTimeoutW(client.receiver.host, WM_COPYDATA,
        reinterpret_cast<WPARAM>(client.window), reinterpret_cast<LPARAM>(&data),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &result) && result == 1) { error.clear(); return true; }
  } catch (...) {}
  return false;
}
}  // namespace quickdial
