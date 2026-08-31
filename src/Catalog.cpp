#include "Catalog.h"

#include <windows.h>
#include <shlobj.h>

#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/base.h>

namespace quickdial {
namespace {

using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValueType;

constexpr std::string_view kDefaultCatalog = R"json({
  "version": 1,
  "discoverInstalled": true,
  "applications": [
    {
      "name": "ChatGPT",
      "target": "shell:AppsFolder\\OpenAI.Codex_2p2nqsd0c76g0!App",
      "aliases": ["codex", "openai"]
    },
    {
      "name": "Obsidian",
      "target": "%APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs\\Obsidian.lnk",
      "aliases": ["notes", "vault", "markdown"]
    },
    {
      "name": "Slack",
      "target": "slack://open",
      "aliases": ["chat", "messages"]
    },
    {
      "name": "Brave",
      "target": "%ProgramData%\\Microsoft\\Windows\\Start Menu\\Programs\\Brave.lnk",
      "aliases": ["browser", "web"]
    },
    {
      "name": "P4V",
      "target": "%ProgramData%\\Microsoft\\Windows\\Start Menu\\Programs\\Perforce\\P4V.lnk",
      "aliases": ["p4", "perforce"]
    },
    {
      "name": "UnrealGameSync",
      "target": "%APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs\\Divide\\UnrealGameSync - Divide.lnk",
      "aliases": ["ugs", "unreal game sync", "divide"]
    }
  ]
}
)json";

std::wstring ErrorWithPrefix(std::wstring_view prefix, const winrt::hresult_error& error) {
  std::wstring result(prefix);
  if (!result.empty()) {
    result += L": ";
  }
  result += error.message().c_str();
  return result;
}

std::optional<std::wstring> OptionalString(const JsonObject& object, std::wstring_view key) {
  const winrt::hstring name(key);
  if (!object.HasKey(name)) {
    return std::nullopt;
  }

  auto value = object.GetNamedValue(name);
  if (value.ValueType() != JsonValueType::String) {
    throw winrt::hresult_invalid_argument(L"Optional value must be a string");
  }
  return ExpandEnvironment(value.GetString().c_str());
}

std::optional<std::wstring> OptionalRawString(const JsonObject& object, std::wstring_view key) {
  const winrt::hstring name(key);
  if (!object.HasKey(name)) {
    return std::nullopt;
  }

  auto value = object.GetNamedValue(name);
  if (value.ValueType() != JsonValueType::String || value.GetString().empty()) {
    throw winrt::hresult_invalid_argument(L"Optional value must be a non-empty string");
  }
  return value.GetString().c_str();
}

bool OptionalBoolean(const JsonObject& object, std::wstring_view key, bool defaultValue) {
  const winrt::hstring name(key);
  if (!object.HasKey(name)) {
    return defaultValue;
  }

  auto value = object.GetNamedValue(name);
  if (value.ValueType() != JsonValueType::Boolean) {
    throw winrt::hresult_invalid_argument(L"Optional value must be a boolean");
  }
  return value.GetBoolean();
}

std::vector<std::wstring> OptionalStringArray(const JsonObject& object, std::wstring_view key) {
  const winrt::hstring name(key);
  if (!object.HasKey(name)) {
    return {};
  }

  auto value = object.GetNamedValue(name);
  if (value.ValueType() != JsonValueType::Array) {
    throw winrt::hresult_invalid_argument(L"Optional value must be an array");
  }

  std::vector<std::wstring> values;
  for (const auto& item : value.GetArray()) {
    if (item.ValueType() != JsonValueType::String) {
      throw winrt::hresult_invalid_argument(L"Array entries must be strings");
    }
    values.emplace_back(item.GetString().c_str());
  }
  return values;
}

std::wstring RequiredString(const JsonObject& object, std::wstring_view key) {
  const winrt::hstring name(key);
  if (!object.HasKey(name)) {
    std::wstring message(L"Missing required field: ");
    message += key;
    throw winrt::hresult_invalid_argument(message);
  }

  auto value = object.GetNamedValue(name);
  if (value.ValueType() != JsonValueType::String || value.GetString().empty()) {
    std::wstring message(L"Required field must be a non-empty string: ");
    message += key;
    throw winrt::hresult_invalid_argument(message);
  }
  return value.GetString().c_str();
}

std::wstring Win32ErrorMessage(DWORD error) {
  wchar_t* buffer = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
  std::wstring message = length > 0 ? std::wstring(buffer, length) : L"Unknown Windows error";
  if (buffer != nullptr) {
    LocalFree(buffer);
  }
  while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
    message.pop_back();
  }
  return message;
}

}  // namespace

std::wstring ExpandEnvironment(std::wstring_view value) {
  if (value.empty()) {
    return {};
  }

  std::wstring input(value);
  const DWORD needed = ExpandEnvironmentStringsW(input.c_str(), nullptr, 0);
  if (needed == 0) {
    return input;
  }

  std::wstring expanded(needed, L'\0');
  const DWORD written = ExpandEnvironmentStringsW(input.c_str(), expanded.data(), needed);
  if (written == 0 || written > needed) {
    return input;
  }
  expanded.resize(written - 1);
  return expanded;
}

CatalogResult ParseCatalogJson(std::string_view json) {
  try {
    if (json.size() >= 3 && static_cast<unsigned char>(json[0]) == 0xEF &&
        static_cast<unsigned char>(json[1]) == 0xBB && static_cast<unsigned char>(json[2]) == 0xBF) {
      json.remove_prefix(3);
    }

    JsonObject root = JsonObject::Parse(winrt::to_hstring(json));
    if (!root.HasKey(L"version") || root.GetNamedValue(L"version").ValueType() != JsonValueType::Number ||
        root.GetNamedNumber(L"version") != 1.0) {
      return {std::nullopt, L"The catalog must declare numeric version 1"};
    }
    if (!root.HasKey(L"applications") ||
        root.GetNamedValue(L"applications").ValueType() != JsonValueType::Array) {
      return {std::nullopt, L"The catalog must contain an applications array"};
    }

    Catalog catalog;
    catalog.discoverInstalled = OptionalBoolean(root, L"discoverInstalled", true);
    catalog.hiddenApplications = OptionalStringArray(root, L"hiddenApplications");
    JsonArray applications = root.GetNamedArray(L"applications");
    catalog.applications.reserve(applications.Size());

    for (uint32_t index = 0; index < applications.Size(); ++index) {
      const auto item = applications.GetAt(index);
      if (item.ValueType() != JsonValueType::Object) {
        return {std::nullopt, L"Each application entry must be an object"};
      }

      const JsonObject object = item.GetObject();
      ApplicationEntry application;
      application.name = RequiredString(object, L"name");
      application.target = ExpandEnvironment(RequiredString(object, L"target"));
      application.identity = OptionalRawString(object, L"id");
      application.aliases = OptionalStringArray(object, L"aliases");
      application.arguments = OptionalStringArray(object, L"arguments");
      application.workingDirectory = OptionalString(object, L"workingDirectory");
      application.icon = OptionalString(object, L"icon");
      catalog.applications.emplace_back(std::move(application));
    }

    return {std::move(catalog), {}};
  } catch (const winrt::hresult_error& error) {
    return {std::nullopt, ErrorWithPrefix(L"Could not parse the app catalog", error)};
  } catch (const std::exception&) {
    return {std::nullopt, L"Could not parse the app catalog"};
  }
}

CatalogResult LoadCatalogFile(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return {std::nullopt, L"Could not open " + path.wstring()};
  }

  std::string contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  if (!stream.good() && !stream.eof()) {
    return {std::nullopt, L"Could not read " + path.wstring()};
  }
  return ParseCatalogJson(contents);
}

bool EnsureDefaultCatalog(const std::filesystem::path& path, std::wstring& error) {
  std::error_code existsError;
  if (std::filesystem::exists(path, existsError)) {
    return true;
  }
  if (existsError) {
    const std::string narrowMessage = existsError.message();
    error = L"Could not inspect the app catalog: " +
            std::wstring(narrowMessage.begin(), narrowMessage.end());
    return false;
  }

  std::error_code directoryError;
  std::filesystem::create_directories(path.parent_path(), directoryError);
  if (directoryError) {
    error = L"Could not create the configuration directory";
    return false;
  }

  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    error = L"Could not create " + path.wstring();
    return false;
  }
  stream.write(kDefaultCatalog.data(), static_cast<std::streamsize>(kDefaultCatalog.size()));
  if (!stream) {
    error = L"Could not write " + path.wstring();
    return false;
  }
  return true;
}

std::filesystem::path GetCatalogPath() {
  PWSTR localAppData = nullptr;
  const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &localAppData);
  if (FAILED(result) || localAppData == nullptr) {
    if (localAppData != nullptr) {
      CoTaskMemFree(localAppData);
    }
    return std::filesystem::current_path() / L"apps.json";
  }

  std::filesystem::path path(localAppData);
  CoTaskMemFree(localAppData);
  return path / L"ApplicationQuickDial" / L"apps.json";
}

}  // namespace quickdial
