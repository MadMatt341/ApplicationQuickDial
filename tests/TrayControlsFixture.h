#pragma once
#include <windows.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <atomic>
#include <chrono>
#include <thread>

// Owned native tray host and application menu. Only use on the private fixture desktop.
class TrayControlsFixture {
 public:
  std::atomic<unsigned> snapshots = 0, menus = 0, unexpectedActions = 0, lastToken = 0;
  std::atomic<int> generation = 1;
  std::atomic<bool> malformed = false, blocked = false, rejectMenu = false;
  std::atomic<HWND> window = nullptr, popup = nullptr;
  TrayControlsFixture() {
    EnumWindows([](HWND value, LPARAM found) -> BOOL {
      if (GetPropW(value,L"Blade.NotificationTray.v1")) *reinterpret_cast<bool*>(found) = true;
      return TRUE;
    },reinterpret_cast<LPARAM>(&foreignHost_));
    if (foreignHost_) return;
    worker_ = std::thread([this] {
      winrt::init_apartment(winrt::apartment_type::single_threaded);
      WNDCLASSW type{}; type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = L"Blade.Tray.Fixture.Host"; type.lpfnWndProc = Procedure;
      if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
      const auto host = CreateWindowW(type.lpszClassName,L"",0,0,0,0,0,nullptr,nullptr,type.hInstance,this);
      const auto menu = CreateWindowW(L"STATIC",L"Application-owned tray menu",WS_POPUP|WS_BORDER,10,10,280,120,
          host,nullptr,type.hInstance,nullptr);
      if (!host || !menu || !SetPropW(host,L"Blade.NotificationTray.v1",reinterpret_cast<HANDLE>(1))) return;
      popup = menu; window = host;
      MSG message{};
      while (GetMessageW(&message,nullptr,0,0)>0) DispatchMessageW(&message);
    });
    const auto limit = std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while (!window && std::chrono::steady_clock::now()<limit) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ~TrayControlsFixture() {
    if (window) PostMessageW(window,WM_CLOSE,0,0);
    if (worker_.joinable()) worker_.join();
  }
  bool Ready() const { return !foreignHost_ && window; }
 private:
  bool foreignHost_ = false;
  std::thread worker_;
  static LRESULT CALLBACK Procedure(HWND h,UINT message,WPARAM w,LPARAM l) {
    using namespace winrt::Windows::Data::Json;
    if (message==WM_NCCREATE) SetWindowLongPtrW(h,GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    const auto self = reinterpret_cast<TrayControlsFixture*>(GetWindowLongPtrW(h,GWLP_USERDATA));
    if (self && message==WM_APP+91) {
      ++self->snapshots;
      if (self->blocked) { std::this_thread::sleep_for(std::chrono::milliseconds(700)); return 0; }
      JsonObject root; root.SetNamedValue(L"version",JsonValue::CreateNumberValue(1));
      root.SetNamedValue(L"revision",JsonValue::CreateNumberValue(self->generation));
      root.SetNamedValue(L"hostSession",JsonValue::CreateStringValue(winrt::to_hstring("fixture-"+std::to_string(self->generation))));
      JsonArray icons;
      for (const unsigned token : {11U,12U}) {
        JsonObject item;
        item.SetNamedValue(L"token",JsonValue::CreateNumberValue(token));
        item.SetNamedValue(L"pid",JsonValue::CreateNumberValue(GetCurrentProcessId()));
        item.SetNamedValue(L"tip",JsonValue::CreateStringValue(token==11 ? L"Hidden Startup Fixture" : L"Status Fixture"));
        std::wstring pixels;
        if (token==11) for (int index=0;index<1024;++index) pixels += L"ff0000ff";
        item.SetNamedValue(L"pixels",JsonValue::CreateStringValue(pixels));
        item.SetNamedValue(L"rect",JsonValue::CreateNullValue());
        icons.Append(item);
      }
      root.SetNamedValue(L"icons",icons);
      const auto bytes = self->malformed ? std::string("{") : winrt::to_string(root.Stringify());
      COPYDATASTRUCT data{0x42545301,static_cast<DWORD>(bytes.size()),const_cast<char*>(bytes.data())};
      DWORD_PTR reply = 0;
      SendMessageTimeoutW(reinterpret_cast<HWND>(w),WM_COPYDATA,reinterpret_cast<WPARAM>(h),
          reinterpret_cast<LPARAM>(&data),SMTO_ABORTIFHUNG,100,&reply);
      return static_cast<LRESULT>(reply);
    }
    if (self && message==WM_COPYDATA && l) {
      const auto data = reinterpret_cast<const COPYDATASTRUCT*>(l);
      if (data->dwData != 0x42544101 || !data->lpData || data->cbData>4096) return 0;
      try {
        const auto action = JsonObject::Parse(winrt::to_hstring(std::string_view(static_cast<const char*>(data->lpData),data->cbData)));
        if (action.GetNamedString(L"action") != L"keyboard-menu") { ++self->unexpectedActions; return 0; }
        const auto token = static_cast<unsigned>(action.GetNamedNumber(L"token"));
        if (self->rejectMenu || (token!=11 && token!=12) ||
            action.GetNamedString(L"hostSession") != winrt::to_hstring("fixture-"+std::to_string(self->generation))) return 0;
        self->lastToken = token; ++self->menus;
        ShowWindow(self->popup,SW_SHOWNOACTIVATE);
        return 1;
      } catch (...) { return 0; }
    }
    if (message==WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h,message,w,l);
  }
};
