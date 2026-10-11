#include "DiscoveryProtocol.h"

#include <cstdint>
#include <cstring>
#include <utility>
#include <algorithm>

namespace quickdial {
namespace {

constexpr std::uint32_t kMagic = 0x31445141;  // AQD1
constexpr std::uint32_t kVersion = 2;
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
  if (result.sources.size() > 3 || !writer.Number(static_cast<std::uint32_t>(result.sources.size()))) return false;
  std::uint32_t seen = 0;
  for (const auto& source : result.sources) {
    const auto id = static_cast<std::uint32_t>(source.source);
    if (id < 1 || id > 3 || (seen & (1U << id)) ||
        !writer.Number(id) || !writer.Number(source.succeeded ? 1 : 0) ||
        !writer.Number(source.skipped) || !writer.String(source.error) ||
        (source.succeeded && !source.error.empty())) return false;
    seen |= 1U << id;
  }
  for (const auto& application : result.applications) {
    if (!result.sources.empty() && std::none_of(result.sources.begin(), result.sources.end(),
        [&application](const auto& source) { return source.source == application.source && source.succeeded; })) return false;
    if (application.name.empty() || application.target.empty() ||
        !writer.String(application.name) || !writer.String(application.target) ||
        !writer.Number(application.identity.has_value() ? 1 : 0) ||
        (application.identity && !writer.String(*application.identity)) ||
        static_cast<std::uint32_t>(application.source) > 3 ||
        !writer.Number(static_cast<std::uint32_t>(application.source)) ||
        !writer.String(application.resolvedTarget) || !writer.String(application.shortcutArguments) ||
        !writer.String(application.shortcutWorkingDirectory) ||
        !writer.String(application.launchKey)) {
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
  std::uint32_t sources = 0, seen = 0;
  if (!reader.Number(sources) || sources > 3) return InvalidResult();
  for (std::uint32_t index = 0; index < sources; ++index) {
    DiscoverySourceStatus source;
    std::uint32_t id = 0, succeeded = 0;
    if (!reader.Number(id) || id < 1 || id > 3 || (seen & (1U << id)) ||
        !reader.Number(succeeded) || succeeded > 1 || !reader.Number(source.skipped) ||
        !reader.String(source.error) || (succeeded && !source.error.empty())) return InvalidResult();
    source.source = static_cast<ApplicationSource>(id);
    source.succeeded = succeeded != 0;
    seen |= 1U << id;
    result.sources.push_back(std::move(source));
  }
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
    std::uint32_t source = 0;
    if (!reader.Number(source) || source > 3 || !reader.String(application.resolvedTarget) ||
        !reader.String(application.shortcutArguments) || !reader.String(application.shortcutWorkingDirectory) ||
        !reader.String(application.launchKey)) return InvalidResult();
    application.source = static_cast<ApplicationSource>(source);
    if (sources && std::none_of(result.sources.begin(), result.sources.end(), [source](const auto& status) {
      return static_cast<std::uint32_t>(status.source) == source && status.succeeded;
    })) return InvalidResult();
    result.applications.push_back(std::move(application));
  }
  return reader.offset == size ? std::move(result) : InvalidResult();
}

}  // namespace quickdial
