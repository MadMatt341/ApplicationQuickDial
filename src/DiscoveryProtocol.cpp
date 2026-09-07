#include "DiscoveryProtocol.h"

#include <cstdint>
#include <cstring>
#include <utility>

namespace quickdial {
namespace {

constexpr std::uint32_t kMagic = 0x31445141;  // AQD1
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kMaximumEntries = 16'384;
constexpr std::uint32_t kMaximumString = 32'768;
constexpr std::size_t kHeaderBytes = 4 * sizeof(std::uint32_t);
static_assert(sizeof(wchar_t) == 2);

struct Writer {
  std::span<std::byte> buffer;
  std::size_t offset = kHeaderBytes;

  bool Bytes(const void* data, std::size_t bytes) {
    if (bytes > buffer.size() - offset) return false;
    if (bytes) std::memcpy(buffer.data() + offset, data, bytes);
    offset += bytes;
    return true;
  }
  bool Number(std::uint32_t value) { return Bytes(&value, sizeof(value)); }
  bool String(std::wstring_view value) {
    return value.size() <= kMaximumString && value.find(L'\0') == std::wstring_view::npos &&
           Number(static_cast<std::uint32_t>(value.size())) && Bytes(value.data(), value.size() * sizeof(wchar_t));
  }
};

struct Reader {
  std::span<const std::byte> buffer;
  std::size_t offset = 0;

  bool Bytes(void* data, std::size_t bytes) {
    if (bytes > buffer.size() - offset) return false;
    if (bytes) std::memcpy(data, buffer.data() + offset, bytes);
    offset += bytes;
    return true;
  }
  bool Number(std::uint32_t& value) { return Bytes(&value, sizeof(value)); }
  bool String(std::wstring& value) {
    std::uint32_t length = 0;
    if (!Number(length) || length > kMaximumString || length > (buffer.size() - offset) / sizeof(wchar_t)) {
      return false;
    }
    value.resize(length);
    return Bytes(value.data(), value.size() * sizeof(wchar_t)) && value.find(L'\0') == std::wstring::npos;
  }
};

InstalledAppsResult InvalidResult() {
  return {{}, L"Windows app discovery returned invalid data. Try Reload app list."};
}

}  // namespace

bool WriteDiscoveryResult(std::span<std::byte> buffer, const InstalledAppsResult& result) {
  if (buffer.size() < kHeaderBytes || buffer.size() > kDiscoveryBufferBytes) return false;
  // A failed write never leaves a valid header, including when reusing a buffer.
  std::memset(buffer.data(), 0, kHeaderBytes);
  if (result.applications.size() > kMaximumEntries || (!result && !result.applications.empty())) return false;
  Writer writer{buffer};
  if (!writer.String(result.error)) return false;
  for (const auto& application : result.applications) {
    if (application.name.empty() || application.target.empty() ||
        !writer.String(application.name) || !writer.String(application.target) ||
        !writer.Number(application.identity.has_value() ? 1 : 0) ||
        (application.identity && !writer.String(*application.identity))) {
      return false;
    }
  }
  const std::uint32_t header[] = {kMagic, kVersion, static_cast<std::uint32_t>(writer.offset),
                                 static_cast<std::uint32_t>(result.applications.size())};
  std::memcpy(buffer.data(), header, sizeof(header));
  return true;
}

InstalledAppsResult ReadDiscoveryResult(std::span<const std::byte> buffer) {
  Reader reader{buffer};
  std::uint32_t magic = 0, version = 0, size = 0, count = 0;
  if (!reader.Number(magic) || magic != kMagic || !reader.Number(version) || version != kVersion ||
      !reader.Number(size) || size < kHeaderBytes || size > buffer.size() || size > kDiscoveryBufferBytes ||
      !reader.Number(count) || count > kMaximumEntries) {
    return InvalidResult();
  }
  reader.buffer = buffer.first(size);
  InstalledAppsResult result;
  if (!reader.String(result.error) || (!result && count != 0)) return InvalidResult();
  for (std::uint32_t index = 0; index < count; ++index) {
    ApplicationEntry application;
    std::uint32_t hasIdentity = 0;
    if (!reader.String(application.name) || application.name.empty() ||
        !reader.String(application.target) || application.target.empty() ||
        !reader.Number(hasIdentity) || hasIdentity > 1) {
      return InvalidResult();
    }
    if (hasIdentity) {
      application.identity.emplace();
      if (!reader.String(*application.identity)) return InvalidResult();
    }
    result.applications.push_back(std::move(application));
  }
  return reader.offset == size ? std::move(result) : InvalidResult();
}

}  // namespace quickdial
