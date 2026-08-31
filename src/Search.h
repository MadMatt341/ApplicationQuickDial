#pragma once

#include "Catalog.h"

#include <cstddef>
#include <string_view>
#include <vector>

namespace quickdial {

std::vector<std::size_t> RankApplications(
    const Catalog& catalog, std::wstring_view query, std::size_t limit = 6);

}  // namespace quickdial
