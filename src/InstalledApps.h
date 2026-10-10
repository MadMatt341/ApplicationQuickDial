#pragma once

#include "Catalog.h"

#include <string>
#include <vector>
#include <cstdint>
#include <filesystem>

struct IEnumShellItems;

namespace quickdial {

struct DiscoverySourceStatus {
  ApplicationSource source = ApplicationSource::AppsFolder;
  bool succeeded = false;
  std::uint32_t skipped = 0;
  std::wstring error;
};

struct InstalledAppsResult {
  std::vector<ApplicationEntry> applications;
  std::wstring error;
  std::vector<DiscoverySourceStatus> sources;

  explicit operator bool() const noexcept { return error.empty(); }
};

InstalledAppsResult DiscoverInstalledApplications();
InstalledAppsResult DiscoverApplicationSource(ApplicationSource source);
InstalledAppsResult ReadInstalledApplications(IEnumShellItems& enumerator);
InstalledAppsResult ReadStartMenuApplications(const std::filesystem::path& directory, ApplicationSource source);
std::vector<ApplicationEntry> ReconcileDiscoverySources(
    std::vector<ApplicationEntry> previous, const InstalledAppsResult& refresh);
Catalog MergeInstalledApplications(Catalog configured, std::vector<ApplicationEntry> installed,
                                  bool showMaintenance = false);
bool IsMaintenanceApplication(const ApplicationEntry& application);
bool MatchesApplication(const ApplicationEntry& application, const std::vector<std::wstring>& values);
std::wstring EquivalentLaunchKey(std::wstring_view target, std::wstring_view arguments,
                                 std::wstring_view workingDirectory = {});
std::wstring_view ApplicationSourceName(ApplicationSource source);

}  // namespace quickdial
