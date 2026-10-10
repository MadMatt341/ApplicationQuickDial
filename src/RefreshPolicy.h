#pragma once
#include <cstdint>
#include <optional>

namespace quickdial {

// Pure scheduling decisions in monotonic milliseconds, shared by UI and fixtures.
class RefreshPolicy {
 public:
  static constexpr std::uint64_t kDebounceMs = 750;
  static constexpr std::uint64_t kMaximumDelayMs = 3000;
  static constexpr std::uint64_t kStaleMs = 5 * 60 * 1000;
  std::uint64_t Changed(std::uint64_t now);
  void Begin();
  std::optional<std::uint64_t> Completed(std::uint64_t now, bool healthy);
  bool NeedsCatchUp(std::uint64_t now, bool failed) const;
  void Reset();
 private:
  std::optional<std::uint64_t> firstEvent_;
  std::optional<std::uint64_t> lastSuccess_;
  unsigned retries_ = 0;
  bool eventSequence_ = false;
};
}
