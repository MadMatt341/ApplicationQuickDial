#include "RefreshPolicy.h"
#include <algorithm>
#include <array>

namespace quickdial {
std::uint64_t RefreshPolicy::Changed(std::uint64_t now) {
  if (!firstEvent_) { firstEvent_ = now; retries_ = 0; }
  eventSequence_ = true;
  const auto remaining = now - *firstEvent_ >= kMaximumDelayMs ? 1 : kMaximumDelayMs - (now - *firstEvent_);
  return std::min(kDebounceMs, remaining);
}
void RefreshPolicy::Begin() { firstEvent_.reset(); }
std::optional<std::uint64_t> RefreshPolicy::Completed(std::uint64_t now, bool healthy) {
  if (healthy) lastSuccess_ = now;
  if (!healthy || eventSequence_) {
    // Registration/index readiness can lag a successful first event scan.
    constexpr std::array<std::uint64_t, 2> delays{2000, 5000};
    if (retries_ < delays.size()) return delays[retries_++];
  }
  retries_ = 0;
  eventSequence_ = false;
  return std::nullopt;
}
bool RefreshPolicy::NeedsCatchUp(std::uint64_t now, bool failed) const {
  return failed || !lastSuccess_ || now - *lastSuccess_ >= kStaleMs;
}
void RefreshPolicy::Reset() { *this = {}; }
}
