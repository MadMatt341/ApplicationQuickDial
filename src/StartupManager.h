#pragma once

#include <string>

namespace quickdial {

std::wstring GetExecutablePath();
bool IsStartWithWindowsEnabled();
bool SetStartWithWindowsEnabled(bool enabled, std::wstring& error);

}  // namespace quickdial
