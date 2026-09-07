#pragma once

#include "InstalledApps.h"

#include <cstddef>
#include <span>

namespace quickdial {

// Private, versioned UTF-16 transport between two copies of the same executable.
// No catalog parsing/expansion is applied to values returned by Windows.
inline constexpr std::size_t kDiscoveryBufferBytes = 4 * 1024 * 1024;
bool WriteDiscoveryResult(std::span<std::byte> buffer, const InstalledAppsResult& result);
InstalledAppsResult ReadDiscoveryResult(std::span<const std::byte> buffer);

}  // namespace quickdial
