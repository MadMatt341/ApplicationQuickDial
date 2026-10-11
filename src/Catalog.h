#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace quickdial {

enum class ApplicationSource { Manual, AppsFolder, UserStartMenu, CommonStartMenu };

struct ApplicationEntry {
  std::wstring name;
  std::wstring target;
  std::optional<std::wstring> identity;
  std::vector<std::wstring> aliases;
  std::vector<std::wstring> arguments;
  std::optional<std::wstring> workingDirectory;
  std::optional<std::wstring> icon;
  // Discovery metadata stays in memory; launching a shortcut retains its semantics.
  ApplicationSource source = ApplicationSource::Manual;
  std::wstring resolvedTarget;
  std::wstring shortcutArguments;
  std::wstring shortcutWorkingDirectory;
  std::wstring launchKey;
};

struct Catalog {
  bool discoverInstalled = true;
  std::vector<std::wstring> hiddenApplications;
  std::vector<std::wstring> includedApplications;
  std::vector<ApplicationEntry> applications;
};

struct CatalogResult {
  std::optional<Catalog> catalog;
  std::wstring error;

  explicit operator bool() const noexcept { return catalog.has_value(); }
};

CatalogResult ParseCatalogJson(std::string_view json);
CatalogResult LoadCatalogFile(const std::filesystem::path& path);
bool EnsureDefaultCatalog(const std::filesystem::path& path, std::wstring& error);
std::filesystem::path GetCatalogPath();
std::wstring ExpandEnvironment(std::wstring_view value);

}  // namespace quickdial
