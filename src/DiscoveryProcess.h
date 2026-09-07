#pragma once

#include "InstalledApps.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <stop_token>

namespace quickdial {

// Run on a background worker. The default helper is this executable in a private
// mode; executable/timeout overrides allow deterministic process-failure tests.
InstalledAppsResult DiscoverInstalledApplicationsIsolated(
    std::stop_token cancellation = {}, const std::filesystem::path& executable = {},
    std::chrono::milliseconds timeout = std::chrono::seconds(30));

// Call before normal GUI/COM/single-instance initialization. nullopt means a
// normal launch; otherwise return this exit code without starting the launcher.
std::optional<int> RunDiscoveryHelperIfRequested();

}  // namespace quickdial
