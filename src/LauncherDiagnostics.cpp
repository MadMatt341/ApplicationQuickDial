#include "LauncherDiagnostics.h"
#include "LauncherApp.h"
#include <fstream>
#include <winrt/base.h>
#include <winrt/Windows.Data.Json.h>

namespace quickdial {
namespace {
LRESULT CALLBACK ResponseProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == WM_COPYDATA) {
    const auto data = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
    auto report = reinterpret_cast<std::wstring*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!report || !data || data->dwData != kStatusResponse || !data->lpData ||
        data->cbData < sizeof(wchar_t) || data->cbData > kMaximumStatusBytes ||
        data->cbData % sizeof(wchar_t) != 0) return 0;
    const auto text = static_cast<const wchar_t*>(data->lpData);
    const auto length = data->cbData / sizeof(wchar_t);
    if (text[length - 1] != L'\0') return 0;
    report->assign(text, length - 1);
    return 1;
  }
  return DefWindowProcW(window, message, wParam, lParam);
}
}
bool ReadDiagnosticRequest(const COPYDATASTRUCT* data, std::wstring& query) {
  if (!data || !data->lpData || data->cbData < sizeof(wchar_t) ||
      data->cbData > 4096 * sizeof(wchar_t) || data->cbData % sizeof(wchar_t) != 0 ||
      (data->dwData != kStatusRequest && data->dwData != kLookupRequest && data->dwData != kSearchRequest)) return false;
  const auto text = static_cast<const wchar_t*>(data->lpData);
  const auto length = data->cbData / sizeof(wchar_t);
  if (text[length - 1] != L'\0') return false;
  query.assign(text, length - 1);
  return query.find(L'\0') == std::wstring::npos;
}
bool SendDiagnosticResponse(HWND destination, HWND source, std::wstring_view report) {
  if (!IsWindow(destination) || (report.size() + 1) * sizeof(wchar_t) > kMaximumStatusBytes) return false;
  const std::wstring text(report);
  COPYDATASTRUCT data{kStatusResponse, static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)),
                      const_cast<wchar_t*>(text.c_str())};
  DWORD_PTR accepted = 0;
  return SendMessageTimeoutW(destination, WM_COPYDATA, reinterpret_cast<WPARAM>(source),
      reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &accepted) && accepted == 1;
}
int RunLauncherDiagnostic(ULONG_PTR mode, std::wstring_view query, const std::filesystem::path& output, HWND target) {
  std::wstring report;
  WNDCLASSW windowClass{};
  windowClass.lpfnWndProc = ResponseProcedure;
  windowClass.hInstance = GetModuleHandleW(nullptr);
  windowClass.lpszClassName = L"ApplicationQuickDial.StatusClient";
  RegisterClassW(&windowClass);
  HWND client = CreateWindowW(windowClass.lpszClassName, L"", 0, 0, 0, 0, 0,
                              nullptr, nullptr, windowClass.hInstance, nullptr);
  if (!client) return 1;
  SetWindowLongPtrW(client, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&report));
  const std::wstring text(query);
  COPYDATASTRUCT request{mode, static_cast<DWORD>((text.size() + 1) * sizeof(wchar_t)),
                         const_cast<wchar_t*>(text.c_str())};
  const auto launcher = target ? target : FindWindowW(kWindowClassName, nullptr);
  DWORD_PTR accepted = 0;
  // The receiver copies the request, then generates its response outside sent input.
  bool success = launcher && SendMessageTimeoutW(launcher, WM_COPYDATA,
      reinterpret_cast<WPARAM>(client), reinterpret_cast<LPARAM>(&request),
      SMTO_ABORTIFHUNG, 5000, &accepted) && accepted == 1;
  const auto deadline = GetTickCount64() + 5000;
  while (success && report.empty() && GetTickCount64() < deadline) {
    MsgWaitForMultipleObjectsEx(0, nullptr, 50, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message); DispatchMessageW(&message);
    }
  }
  success = success && !report.empty();
  DestroyWindow(client);
  if (!success) report = L"{\"success\":false,\"error\":\"The running Launcher is unavailable or predates diagnostics. Update it, then retry.\"}";
  const auto bytes = winrt::to_string(winrt::hstring(report));
  std::ofstream file(output, std::ios::binary | std::ios::trunc);
  file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  winrt::Windows::Data::Json::JsonObject result;
  const bool healthy = winrt::Windows::Data::Json::JsonObject::TryParse(report, result) && result.GetNamedBoolean(L"success", false);
  return success && healthy && file.good() ? 0 : 1;
}
}
