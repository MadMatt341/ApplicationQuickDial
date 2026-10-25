#include "InstalledApps.h"
#include "TextUtilities.h"

#include <windows.h>
#include <shobjidl.h>
#include <shlguid.h>
#include <propsys.h>
#include <shlobj.h>
#include <shellapi.h>
#include <propkey.h>
#include <wrl/client.h>
#include <msi.h>

#include <algorithm>
#include <string_view>
#include <unordered_set>
#include <filesystem>
#include <cwctype>

namespace quickdial {
namespace {

using Microsoft::WRL::ComPtr;

std::wstring NormalizedPath(std::wstring_view value) {
  auto path = FoldCase(value);
  std::replace(path.begin(), path.end(), L'/', L'\\');
  return path;
}

std::wstring ComparisonTarget(std::wstring_view value) {
  return value.find(L"://") == std::wstring_view::npos ? FoldCase(value) : std::wstring(value);
}

std::vector<std::wstring> CommandArguments(std::wstring_view arguments) {
  const std::wstring command = L"launcher " + std::wstring(arguments);
  int count = 0;
  auto* parts = CommandLineToArgvW(command.c_str(), &count);
  std::vector<std::wstring> result;
  if (parts) {
    for (int index = 1; index < count; ++index) result.emplace_back(parts[index]);
    LocalFree(parts);
  }
  return result;
}

std::wstring CommandKey(std::wstring_view target, const std::vector<std::wstring>& arguments, std::wstring_view workingDirectory) {
  // URI paths and query values are case-sensitive; do not treat them as files.
  const bool uri = target.find(L"://") != std::wstring_view::npos;
  const auto normalized = uri ? std::wstring(target) : NormalizedPath(target);
  std::wstring key = std::to_wstring(normalized.size()) + L":" + normalized;
  const auto directory = NormalizedPath(workingDirectory);
  key += L"|wd:" + std::to_wstring(directory.size()) + L":" + directory;
  // Argument case is significant; length prefixes preserve token boundaries.
  const auto leaf = FoldCase(std::filesystem::path(target).filename().wstring());
  const bool chromium = !uri && (leaf == L"brave.exe" || leaf == L"chrome.exe" || leaf == L"chrome_proxy.exe" ||
      leaf == L"msedge.exe" || leaf == L"msedge_proxy.exe" || leaf == L"chromium.exe");
  bool switches = true;
  for (const auto& part : arguments) {
    if (part == L"--") switches = false;
    // Chromium uses this switch for launch metrics, never for app/profile selection.
    if (chromium && switches && (part == L"--source-shortcut-location=start-menu" ||
        part == L"--source-shortcut-location=desktop" || part == L"--source-shortcut-location=taskbar")) continue;
    key += L"|" + std::to_wstring(part.size()) + L":" + part;
  }
  return key;
}

struct ShortcutCommand {
  std::wstring target;
  std::wstring arguments;
};

std::optional<ShortcutCommand> SquirrelCommand(const ApplicationEntry& app) {
  // Only recognize the documented process-start wrapper, not arbitrary updaters.
  if (!app.identity || !FoldCase(*app.identity).starts_with(L"com.squirrel.")) return {};
  const std::filesystem::path updater(app.resolvedTarget), directory(app.shortcutWorkingDirectory);
  if (FoldCase(updater.filename().wstring()) != L"update.exe" || !directory.is_absolute() ||
      !FoldCase(directory.filename().wstring()).starts_with(L"app-") ||
      NormalizedPath(directory.parent_path().wstring()) != NormalizedPath(updater.parent_path().wstring())) return {};
  const auto args = CommandArguments(app.shortcutArguments);
  if ((args.size() != 2 && args.size() != 4) || args[0] != L"--processStart" ||
      (args.size() == 4 && args[2] != L"--process-start-args")) return {};
  const std::filesystem::path executable(args[1]);
  if (executable.empty() || executable.has_parent_path() ||
      FoldCase(executable.extension().wstring()) != L".exe") return {};
  return ShortcutCommand{(directory / executable).wstring(), args.size() == 4 ? args[3] : L""};
}

std::wstring ShortcutLaunchKey(const ApplicationEntry& app, bool withDirectory = true) {
  const auto command = SquirrelCommand(app);
  return EquivalentLaunchKey(command ? command->target : app.resolvedTarget,
      command ? command->arguments : app.shortcutArguments,
      withDirectory ? app.shortcutWorkingDirectory : L"");
}

std::wstring ShortcutTarget(const std::filesystem::path& path, IShellLinkW& link) {
  // Installer queries read registration only; never Resolve, repair, or install.
  wchar_t product[39]{}, component[39]{};
  std::vector<wchar_t> value(32'768);
  if (MsiGetShortcutTargetW(path.c_str(), product, nullptr, component) == ERROR_SUCCESS) {
    DWORD size = static_cast<DWORD>(value.size());
    if (MsiGetComponentPathW(product, component, value.data(), &size) == INSTALLSTATE_LOCAL) {
      const std::wstring target(value.data());
      // MSI can also return registry keys. Accept only installed local files.
      if (target.size() > 3 && std::iswalpha(target[0]) && target[1] == L':' &&
          (target[2] == L'\\' || target[2] == L'/')) {
        const auto attributes = GetFileAttributesW(target.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return target;
      }
    }
  }
  value.front() = L'\0';
  if (SUCCEEDED(link.GetPath(value.data(), static_cast<int>(value.size()), nullptr, SLGP_RAWPATH)) && value.front())
    return ExpandEnvironment(value.data());
  // URL/namespace links can have a PIDL but no file-system path.
  PIDLIST_ABSOLUTE target = nullptr;
  std::wstring result;
  if (SUCCEEDED(link.GetIDList(&target)) && target) {
    PWSTR parsing = nullptr;
    if (SUCCEEDED(SHGetNameFromIDList(target, SIGDN_DESKTOPABSOLUTEPARSING, &parsing)) && parsing) result = parsing;
    if (parsing) CoTaskMemFree(parsing);
  }
  if (target) CoTaskMemFree(target);
  return result;
}

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

void AddApplicationKeys(
    const ApplicationEntry& application, std::unordered_set<std::wstring>& keys) {
  keys.insert(L"target:" + ComparisonTarget(application.target));
  if (application.identity && !application.identity->empty()) {
    if (application.source == ApplicationSource::Manual) keys.insert(L"override-id:" + FoldCase(*application.identity));
    else keys.insert(L"id:" + FoldCase(*application.identity) + EquivalentLaunchKey(
        application.resolvedTarget, application.shortcutArguments, application.shortcutWorkingDirectory));
  }
  const auto launchKey = application.resolvedTarget.empty() ? application.launchKey : ShortcutLaunchKey(application);
  if (!launchKey.empty()) keys.insert(L"launch:" + launchKey);
  if (application.source == ApplicationSource::UserStartMenu ||
      application.source == ApplicationSource::CommonStartMenu) {
    // AppsFolder does not supply working folders. Only physical shortcuts add
    // these directional keys; their complete commands still distinguish variants.
    if (!application.resolvedTarget.empty()) keys.insert(L"indexed-command:" +
        ShortcutLaunchKey(application, false) +
        L"|name:" + FoldCase(application.name));
    if (!application.resolvedTarget.empty() && application.identity) keys.insert(L"indexed-command-id:" +
        ShortcutLaunchKey(application, false) + L"|id:" + FoldCase(*application.identity));
    if (application.identity && !application.identity->empty()) keys.insert(L"indexed-id:" +
        FoldCase(*application.identity) + EquivalentLaunchKey(L"", application.shortcutArguments));
  }
  if (application.source == ApplicationSource::Manual) {
    keys.insert(L"launch:" + CommandKey(application.target, application.arguments, application.workingDirectory.value_or(L"")));
  }
}

bool HasApplicationKey(
    const ApplicationEntry& application, const std::unordered_set<std::wstring>& keys) {
  if (keys.contains(L"target:" + ComparisonTarget(application.target))) {
    return true;
  }
  const auto launchKey = application.resolvedTarget.empty() ? application.launchKey : ShortcutLaunchKey(application);
  if (!launchKey.empty() && keys.contains(L"launch:" + launchKey)) return true;
  if (application.source == ApplicationSource::AppsFolder && application.shortcutWorkingDirectory.empty()) {
    if (!application.resolvedTarget.empty() && keys.contains(L"indexed-command:" +
        ShortcutLaunchKey(application, false) +
        L"|name:" + FoldCase(application.name))) return true;
    if (!application.resolvedTarget.empty() && application.identity && keys.contains(L"indexed-command-id:" +
        ShortcutLaunchKey(application, false) + L"|id:" + FoldCase(*application.identity))) return true;
    if (application.resolvedTarget.empty() && application.identity && !application.identity->empty() && keys.contains(L"indexed-id:" +
        FoldCase(*application.identity) + EquivalentLaunchKey(L"", application.shortcutArguments))) return true;
  }
  return application.identity && !application.identity->empty() &&
         (keys.contains(L"override-id:" + FoldCase(*application.identity)) ||
          keys.contains(L"id:" + FoldCase(*application.identity) + EquivalentLaunchKey(
              application.resolvedTarget, application.shortcutArguments, application.shortcutWorkingDirectory)));
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

InstalledAppsResult DiscoverApplicationSource(ApplicationSource source) {
  InstalledAppsResult result;
  if (source == ApplicationSource::AppsFolder) {
    result = []() -> InstalledAppsResult {
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
    }();
  } else if (source == ApplicationSource::UserStartMenu || source == ApplicationSource::CommonStartMenu) {
    PWSTR path = nullptr;
    const auto& folder = source == ApplicationSource::UserStartMenu ? FOLDERID_Programs : FOLDERID_CommonPrograms;
    const HRESULT located = SHGetKnownFolderPath(folder, 0, nullptr, &path);
    if (SUCCEEDED(located) && path) result = ReadStartMenuApplications(path, source);
    else result.error = HResultMessage(L"Could not locate a Start-menu source", located);
    if (path) CoTaskMemFree(path);
  } else result.error = L"Invalid discovery source.";
  if (result.sources.empty()) result.sources.push_back({source, static_cast<bool>(result), 0, result.error});
  return result;
}

InstalledAppsResult DiscoverInstalledApplications() {
  InstalledAppsResult combined;
  for (const auto source : {ApplicationSource::AppsFolder, ApplicationSource::UserStartMenu, ApplicationSource::CommonStartMenu}) {
    auto result = DiscoverApplicationSource(source);
    combined.sources.push_back(result.sources.front());
    if (result) combined.applications.insert(combined.applications.end(),
        std::make_move_iterator(result.applications.begin()), std::make_move_iterator(result.applications.end()));
  }
  if (std::none_of(combined.sources.begin(), combined.sources.end(), [](const auto& source) { return source.succeeded; }))
    combined.error = L"No application source could be read. Reload the app list or open Diagnostics.";
  std::stable_sort(combined.applications.begin(), combined.applications.end(), ApplicationNameLess);
  return combined;
}

InstalledAppsResult ReadInstalledApplications(IEnumShellItems& enumerator) {
  std::vector<ApplicationEntry> applications;
  std::uint32_t skipped = 0;
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
      ++skipped;
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
      ++skipped;
      continue;
    }

    ApplicationEntry application;
    application.name = std::move(name);
    application.target = std::move(target);
    application.identity = std::move(identity);
    application.source = ApplicationSource::AppsFolder;
    if (item2) {
      const auto property = [&item2](REFPROPERTYKEY key) {
        PWSTR value = nullptr;
        std::wstring text;
        if (SUCCEEDED(item2->GetString(key, &value)) && value) text = value;
        if (value) CoTaskMemFree(value);
        return text;
      };
      application.resolvedTarget = property(PKEY_Link_TargetParsingPath);
      application.shortcutArguments = property(PKEY_Link_Arguments);
    }
    if (application.resolvedTarget.empty() && application.identity &&
        std::filesystem::path(*application.identity).is_absolute()) application.resolvedTarget = *application.identity;
    if (!application.resolvedTarget.empty())
      application.launchKey = EquivalentLaunchKey(application.resolvedTarget, application.shortcutArguments);
    applications.emplace_back(std::move(application));
  }

  std::stable_sort(applications.begin(), applications.end(), ApplicationNameLess);
  return {std::move(applications), {}, {{ApplicationSource::AppsFolder, true, skipped, {}}}};
}

InstalledAppsResult ReadStartMenuApplications(const std::filesystem::path& directory, ApplicationSource source) {
  InstalledAppsResult result;
  std::error_code error;
  const bool exists = std::filesystem::exists(directory, error);
  if (error || !exists) return {{}, L"Start-menu folder is unavailable: " + directory.wstring()};
  // No permission-skipping iterator: a partially inaccessible source must retain its last good cache.
  std::filesystem::recursive_directory_iterator iterator(directory, error), end;
  std::uint32_t skipped = 0;
  while (!error && iterator != end) {
    const auto path = iterator->path();
    const auto extension = FoldCase(path.extension().wstring());
    if (iterator->is_regular_file(error) && (extension == L".lnk" || extension == L".url" || extension == L".appref-ms")) {
      ApplicationEntry app;
      app.name = path.stem().wstring();
      app.target = path.wstring();
      app.source = source;
      if (extension == L".lnk") {
        ComPtr<IShellLinkW> link;
        ComPtr<IPersistFile> persist;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
            FAILED(link.As(&persist)) || FAILED(persist->Load(path.c_str(), STGM_READ))) {
          ++skipped;
          iterator.increment(error);
          continue;
        }
        std::vector<wchar_t> value(32'768);
        app.resolvedTarget = ShortcutTarget(path, *link.Get());
        value.front() = L'\0';
        if (SUCCEEDED(link->GetArguments(value.data(), static_cast<int>(value.size())))) app.shortcutArguments = value.data();
        value.front() = L'\0';
        if (SUCCEEDED(link->GetWorkingDirectory(value.data(), static_cast<int>(value.size()))))
          app.shortcutWorkingDirectory = ExpandEnvironment(value.data());
        ComPtr<IPropertyStore> properties;
        if (SUCCEEDED(link.As(&properties))) {
          PROPVARIANT identity{};
          if (SUCCEEDED(properties->GetValue(PKEY_AppUserModel_ID, &identity)) && identity.vt == VT_LPWSTR &&
              identity.pwszVal && *identity.pwszVal) app.identity = identity.pwszVal;
          PropVariantClear(&identity);
        }
        // Advertised/namespace shortcuts remain launchable via the original .lnk even without an executable path.
        if (!app.resolvedTarget.empty()) app.launchKey = ShortcutLaunchKey(app);
      } else if (extension == L".url") {
        std::vector<wchar_t> url(32'768);
        if (GetPrivateProfileStringW(L"InternetShortcut", L"URL", L"", url.data(),
              static_cast<DWORD>(url.size()), path.c_str())) {
          app.resolvedTarget = url.data();
          app.launchKey = EquivalentLaunchKey(app.resolvedTarget, L"");
        } else { ++skipped; iterator.increment(error); continue; }
      }
      result.applications.push_back(std::move(app));
    }
    if (!error) iterator.increment(error);
  }
  if (error) return {{}, L"Could not finish reading Start-menu shortcuts: " + directory.wstring()};
  std::stable_sort(result.applications.begin(), result.applications.end(), ApplicationNameLess);
  result.sources.push_back({source, true, skipped, {}});
  return result;
}

std::wstring EquivalentLaunchKey(std::wstring_view target, std::wstring_view arguments, std::wstring_view workingDirectory) {
  return CommandKey(target, CommandArguments(arguments), workingDirectory);
}

bool MatchesApplication(const ApplicationEntry& application, const std::vector<std::wstring>& values) {
  return std::any_of(values.begin(), values.end(), [&application](const auto& value) {
    const auto folded = FoldCase(value);
    return folded == FoldCase(application.name) || folded == FoldCase(application.target) ||
           (application.identity && folded == FoldCase(*application.identity));
  });
}

bool IsMaintenanceApplication(const ApplicationEntry& application) {
  const auto leaf = [](std::wstring_view path) { return FoldCase(std::filesystem::path(path).filename().wstring()); };
  const std::wstring target = leaf(application.resolvedTarget.empty() ? application.target : application.resolvedTarget);
  if (target == L"uninstall.exe" || target == L"uninstall.bat" || target == L"uninstall.cmd" ||
      target == L"uninstall.ps1" || (target.starts_with(L"unins") && target.ends_with(L".exe") &&
      target.size() == 12 && std::all_of(target.begin() + 5, target.begin() + 8, [](wchar_t c) { return c >= L'0' && c <= L'9'; })))
    return true;
  const auto name = FoldCase(application.name);
  const bool labelled = name == L"uninstall" || name.starts_with(L"uninstall ") || name.starts_with(L"uninstall -") ||
      name == L"odinstaluj" || name.starts_with(L"odinstaluj ") || name.starts_with(L"deinstallieren ") ||
      name.starts_with(L"d\u00e9sinstaller ");
  if (!labelled) return false;
  // A label alone cannot distinguish an uninstaller from an app such as Uninstall Tool.
  // Opaque installer identities need both a maintenance label and an explicit identity token.
  if (application.resolvedTarget.empty()) {
    const auto identity = FoldCase(application.identity.value_or(L""));
    return identity.ends_with(L".uninstall") || identity.ends_with(L"!uninstall") ||
           identity.find(L"\\uninstall.") != std::wstring::npos;
  }
  const auto arguments = FoldCase(application.shortcutArguments);
  if (target == L"msiexec.exe") return arguments.find(L"/x") != std::wstring::npos || arguments.find(L"-x") != std::wstring::npos;
  return (target == L"powershell.exe" || target == L"pwsh.exe" || target == L"cmd.exe") &&
      (arguments.find(L"uninstall.ps1") != std::wstring::npos || arguments.find(L"uninstall.bat") != std::wstring::npos ||
       arguments.find(L"uninstall.cmd") != std::wstring::npos);
}

std::wstring_view ApplicationSourceName(ApplicationSource source) {
  switch (source) {
    case ApplicationSource::Manual: return L"Manual";
    case ApplicationSource::AppsFolder: return L"Windows apps";
    case ApplicationSource::UserStartMenu: return L"User Start menu";
    case ApplicationSource::CommonStartMenu: return L"Shared Start menu";
  }
  return L"Unknown";
}

std::vector<ApplicationEntry> ReconcileDiscoverySources(std::vector<ApplicationEntry> previous, const InstalledAppsResult& refresh) {
  if (refresh.sources.empty()) return refresh ? refresh.applications : previous;
  std::erase_if(previous, [&refresh](const auto& app) {
    return std::any_of(refresh.sources.begin(), refresh.sources.end(), [&app](const auto& source) {
      return source.source == app.source && source.succeeded;
    });
  });
  previous.insert(previous.end(), refresh.applications.begin(), refresh.applications.end());
  std::stable_sort(previous.begin(), previous.end(), ApplicationNameLess);
  return previous;
}

Catalog MergeInstalledApplications(Catalog configured, std::vector<ApplicationEntry> installed, bool showMaintenance) {
  if (!configured.discoverInstalled) {
    return configured;
  }
  const auto manualCount = configured.applications.size();

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
  // Physical shortcuts win equivalent launch identities: they do not need Explorer's index.
  const auto indexed = std::stable_partition(installed.begin(), installed.end(), [](const auto& app) {
    return app.source != ApplicationSource::AppsFolder;
  });
  // A stable updater shortcut survives application upgrades; a versioned executable does not.
  std::stable_partition(installed.begin(), indexed, [](const auto& app) {
    return SquirrelCommand(app).has_value();
  });
  for (auto& application : installed) {
    if (IsHidden(application, hidden) || HasApplicationKey(application, keys)) {
      continue;
    }
    if (!showMaintenance && IsMaintenanceApplication(application) &&
        !MatchesApplication(application, configured.includedApplications)) continue;
    AddApplicationKeys(application, keys);
    configured.applications.emplace_back(std::move(application));
  }
  std::stable_sort(configured.applications.begin() + static_cast<std::ptrdiff_t>(manualCount),
                   configured.applications.end(), ApplicationNameLess);
  return configured;
}

}  // namespace quickdial
