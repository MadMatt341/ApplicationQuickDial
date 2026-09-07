#pragma once

#include "Catalog.h"

#include <string>
#include <vector>

struct IEnumShellItems;

namespace quickdial {

struct InstalledAppsResult {
  std::vector<ApplicationEntry> applications;
  std::wstring error;

  explicit operator bool() const noexcept { return error.empty(); }
};

InstalledAppsResult DiscoverInstalledApplications();
InstalledAppsResult ReadInstalledApplications(IEnumShellItems& enumerator);
Catalog MergeInstalledApplications(Catalog configured, std::vector<ApplicationEntry> installed);

}  // namespace quickdial
