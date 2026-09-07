#include "InstalledApps.h"

#include <windows.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <propsys.h>
#include <propkey.h>
#include <wrl/client.h>

#include <algorithm>
#include <limits>
#include <string_view>
#include <unordered_set>

namespace quickdial {
namespace {

using Microsoft::WRL::ComPtr;

std::wstring HResultMessage(std::wstring_view prefix, HRESULT result) {
  wchar_t* buffer = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, static_cast<DWORD>(result), 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
  std::wstring message(prefix);
  if (length > 0 && buffer != nullptr) {
    message += L": ";
    message.append(buffer, length);
  }
  if (buffer != nullptr) {
    LocalFree(buffer);
  }
  while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
    message.pop_back();
  }
  return message;
}

std::wstring FoldCase(std::wstring_view value) {
  if (value.empty()) {
    return {};
  }
  if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return std::wstring(value);
  }
  const int length = static_cast<int>(value.size());
  const int needed = LCMapStringEx(
      LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(), length, nullptr, 0, nullptr, nullptr, 0);
  if (needed <= 0) {
    return std::wstring(value);
  }
  std::wstring result(static_cast<std::size_t>(needed), L'\0');
  if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(), length,
                    result.data(), needed, nullptr, nullptr, 0) <= 0) {
    return std::wstring(value);
  }
  return result;
}

void AddApplicationKeys(
    const ApplicationEntry& application, std::unordered_set<std::wstring>& keys) {
  keys.insert(L"name:" + FoldCase(application.name));
  keys.insert(L"target:" + FoldCase(application.target));
  if (application.identity && !application.identity->empty()) {
    keys.insert(L"id:" + FoldCase(*application.identity));
  }
}

bool HasApplicationKey(
    const ApplicationEntry& application, const std::unordered_set<std::wstring>& keys) {
  if (keys.contains(L"name:" + FoldCase(application.name)) ||
      keys.contains(L"target:" + FoldCase(application.target))) {
    return true;
  }
  return application.identity && !application.identity->empty() &&
         keys.contains(L"id:" + FoldCase(*application.identity));
}

bool IsHidden(
    const ApplicationEntry& application, const std::unordered_set<std::wstring>& hidden) {
  if (hidden.contains(FoldCase(application.name)) || hidden.contains(FoldCase(application.target))) {
    return true;
  }
  return application.identity && hidden.contains(FoldCase(*application.identity));
}

bool ApplicationNameLess(const ApplicationEntry& left, const ApplicationEntry& right) {
  const int nameResult = CompareStringOrdinal(
      left.name.c_str(), static_cast<int>(left.name.size()),
      right.name.c_str(), static_cast<int>(right.name.size()), TRUE);
  if (nameResult != CSTR_EQUAL) {
    return nameResult == CSTR_LESS_THAN;
  }
  return CompareStringOrdinal(
             left.target.c_str(), static_cast<int>(left.target.size()),
             right.target.c_str(), static_cast<int>(right.target.size()), TRUE) == CSTR_LESS_THAN;
}

}  // namespace

InstalledAppsResult DiscoverInstalledApplications() {
  ComPtr<IShellItem> appsFolder;
  HRESULT result = SHCreateItemFromParsingName(
      L"shell:AppsFolder", nullptr, IID_PPV_ARGS(appsFolder.ReleaseAndGetAddressOf()));
  if (FAILED(result)) {
    return {{}, HResultMessage(L"Could not open Windows' installed-apps folder", result)};
  }

  ComPtr<IEnumShellItems> enumerator;
  result = appsFolder->BindToHandler(
      nullptr, BHID_EnumItems, IID_PPV_ARGS(enumerator.ReleaseAndGetAddressOf()));
  if (FAILED(result)) {
    return {{}, HResultMessage(L"Could not enumerate Windows' installed apps", result)};
  }

  return ReadInstalledApplications(*enumerator.Get());
}

InstalledAppsResult ReadInstalledApplications(IEnumShellItems& enumerator) {
  std::vector<ApplicationEntry> applications;
  while (true) {
    ComPtr<IShellItem> item;
    ULONG fetched = 0;
    const HRESULT result = enumerator.Next(1, item.ReleaseAndGetAddressOf(), &fetched);
    if (FAILED(result)) {
      return {{}, HResultMessage(L"Could not finish enumerating Windows' installed apps", result)};
    }
    if (result == S_FALSE) {
      break;
    }
    if (fetched != 1 || !item) {
      return {{}, L"Windows returned an incomplete installed-app entry. Try Reload app list."};
    }

    PWSTR rawName = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &rawName)) || rawName == nullptr || *rawName == L'\0') {
      if (rawName != nullptr) {
        CoTaskMemFree(rawName);
      }
      continue;
    }
    std::wstring name(rawName);
    CoTaskMemFree(rawName);

    std::optional<std::wstring> identity;
    ComPtr<IShellItem2> item2;
    if (SUCCEEDED(item.As(&item2))) {
      PWSTR rawIdentity = nullptr;
      if (SUCCEEDED(item2->GetString(PKEY_AppUserModel_ID, &rawIdentity)) &&
          rawIdentity != nullptr && *rawIdentity != L'\0') {
        identity = rawIdentity;
      }
      if (rawIdentity != nullptr) {
        CoTaskMemFree(rawIdentity);
      }
    }

    std::wstring target;
    if (identity) {
      target = L"shell:AppsFolder\\" + *identity;
    } else {
      PWSTR rawTarget = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &rawTarget)) && rawTarget != nullptr) {
        target = rawTarget;
      }
      if (rawTarget != nullptr) {
        CoTaskMemFree(rawTarget);
      }
    }
    if (target.empty()) {
      continue;
    }

    ApplicationEntry application;
    application.name = std::move(name);
    application.target = std::move(target);
    application.identity = std::move(identity);
    applications.emplace_back(std::move(application));
  }

  std::stable_sort(applications.begin(), applications.end(), ApplicationNameLess);
  return {std::move(applications), {}};
}

Catalog MergeInstalledApplications(Catalog configured, std::vector<ApplicationEntry> installed) {
  if (!configured.discoverInstalled) {
    return configured;
  }

  std::unordered_set<std::wstring> hidden;
  hidden.reserve(configured.hiddenApplications.size());
  for (const auto& value : configured.hiddenApplications) {
    hidden.insert(FoldCase(value));
  }

  std::unordered_set<std::wstring> keys;
  keys.reserve((configured.applications.size() + installed.size()) * 3);
  for (const auto& application : configured.applications) {
    AddApplicationKeys(application, keys);
  }

  std::stable_sort(installed.begin(), installed.end(), ApplicationNameLess);
  for (auto& application : installed) {
    if (IsHidden(application, hidden) || HasApplicationKey(application, keys)) {
      continue;
    }
    AddApplicationKeys(application, keys);
    configured.applications.emplace_back(std::move(application));
  }
  return configured;
}

}  // namespace quickdial
