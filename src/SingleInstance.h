#pragma once

#include <windows.h>

#include <string>

namespace quickdial {

enum class InstanceStart { Primary, Forwarded, Failed };

// Keep alive until the primary window and its message loop have shut down.
class SingleInstance {
 public:
  SingleInstance() = default;
  ~SingleInstance();
  SingleInstance(const SingleInstance&) = delete;
  SingleInstance& operator=(const SingleInstance&) = delete;

  InstanceStart Start(const wchar_t* windowClass, UINT showMessage, std::wstring& error);

 private:
  HANDLE mutex_ = nullptr;
  bool owned_ = false;
};

}  // namespace quickdial
