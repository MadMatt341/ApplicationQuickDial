#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace quickdial {
struct RegistrationResult {
  bool success = false;
  std::filesystem::path backup;
  std::wstring error;
};
RegistrationResult RegisterApplicationJson(const std::filesystem::path& catalogPath, std::wstring_view applicationJson);
std::wstring CurrentAccountSid();
bool IsPackagedProcess();
// These modes never create UI or forward to the singleton.
int RunCatalogCommand(const std::wstring& mode, const std::filesystem::path& request,
                      const std::filesystem::path& output, std::wstring_view expectedAccount);
}
