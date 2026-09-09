#pragma once

#include "Catalog.h"

#include <cstddef>
#include <limits>
#include <string_view>
#include <vector>

namespace quickdial {

std::vector<std::size_t> RankApplications(
    const Catalog& catalog, std::wstring_view query, std::size_t limit = std::numeric_limits<std::size_t>::max());

}  // namespace quickdial
