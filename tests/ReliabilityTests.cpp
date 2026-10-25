#include "CatalogRegistration.h"
#include "Catalog.h"
#include "InstalledApps.h"
#include "InstalledAppsWatcher.h"
#include "MainMenu.h"
#include "RefreshPolicy.h"
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <winrt/base.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

namespace {
int failures = 0;
void Check(bool condition, const char* description) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << description << '\n'; }
}
void Write(const std::filesystem::path& path, std::string_view text) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
}
std::string Read(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), {}};
}
quickdial::ApplicationEntry App(std::wstring name, std::wstring target, quickdial::ApplicationSource source) {
  quickdial::ApplicationEntry app;
  app.name = std::move(name); app.target = std::move(target); app.source = source;
  return app;
}
void TestPowerSearch() {
  using namespace quickdial;
  Catalog catalog;
  catalog.applications.push_back(App(L"Reboot notebook", L"app.exe", ApplicationSource::Manual));
  for (const auto [query, action] : {std::pair{L"shutdown", MenuItemId::ShutDown}, {L"  POWER OFF  ", MenuItemId::ShutDown},
      {L"restart", MenuItemId::Restart}, {L"reboot", MenuItemId::Restart}, {L"log off", MenuItemId::SignOut}}) {
    const auto result = RankLauncherResults(catalog, MenuPage::Main, query);
    Check(!result.empty() && result.front().kind == LauncherResult::Kind::Command && result.front().command == action,
        "exact root action names and aliases outrank colliding apps");
  }
  const auto partial = RankLauncherResults(catalog, MenuPage::Main, L"reb");
  Check(partial.size() == 2 && partial[0].kind == LauncherResult::Kind::Application &&
      partial[1].kind == LauncherResult::Kind::Command && partial[1].command == MenuItemId::Restart,
      "partial commands follow apps with explicit result types");
  for (const auto query : {L"re", L"r", L"shutdwon", L"restarrt"}) {
    const auto results = RankLauncherResults(catalog, MenuPage::Main, query);
    Check(std::none_of(results.begin(), results.end(), [](const auto& item) { return item.kind == LauncherResult::Kind::Command; }),
        "short and fuzzy root queries never produce power commands");
  }
  const auto empty = RankLauncherResults(catalog, MenuPage::Main, L"");
  Check(empty.size() == 2 && empty[0].command == MenuItemId::System && empty[1].command == MenuItemId::Appearance, "empty root retains System navigation");
}
void TestRefresh() {
  quickdial::RefreshPolicy policy;
  Check(policy.NeedsCatchUp(0, false), "first open without a healthy cache requests discovery");
  Check(policy.Changed(100) == 750 && policy.Changed(2800) == 300 && policy.Changed(3200) == 1,
      "continuous events cannot postpone discovery beyond the maximum debounce");
  policy.Begin();
  Check(policy.Completed(4000, true) == 2000, "an installation event has a bounded readiness retry after success");
  policy.Begin();
  Check(policy.Completed(6000, false) == 5000, "a failed source advances the same retry budget");
  policy.Begin();
  Check(!policy.Completed(11000, false), "retry exhaustion stops polling even when Windows remains unavailable");
  Check(!policy.NeedsCatchUp(12000, false) && policy.NeedsCatchUp(12000, true) && policy.NeedsCatchUp(304000, false),
      "healthy fresh cache avoids scanning; failures and stale cache catch up on open");
  Check(policy.Changed(12000) == 750, "a later installation starts a fresh bounded sequence");
  policy.Reset();
  Check(policy.NeedsCatchUp(12000, false), "disabling discovery clears refresh readiness");
}
void TestPartialWatcher(const std::filesystem::path& directory) {
  quickdial::InstalledAppsWatcher watcher;
  std::wstring error;
  // One nonexistent owned tree fails; the other subscription must stay live.
  const HWND window = CreateWindowW(L"STATIC", L"Watcher fixture", 0, 0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  Check(window != nullptr, "watcher fixture creates a valid owned HWND");
  if (!window) return;
  const auto working = directory / L"Watched";
  std::filesystem::create_directory(working);
  const std::filesystem::path paths[] = {working, directory / L"Missing watch tree"};
  Check(watcher.Start(window, WM_APP + 91, error, paths) && !error.empty() &&
      watcher.IsRunning() && !watcher.DirectoryHandles().empty(),
      "failed watch registration retains independent physical Start-menu subscriptions");
  Write(working / L"change.txt", "event");
  Check(watcher.DirectoryHandles().size() == 1 && WaitForSingleObject(watcher.DirectoryHandles()[0], 3000) == WAIT_OBJECT_0 &&
      watcher.ConsumeDirectoryChange(0, error), "the surviving subscription delivers a real native change");
  Check(watcher.Stop() && !watcher.IsRunning() && watcher.DirectoryHandles().empty(),
      "partial subscriptions release all owned handles");
  DestroyWindow(window);
}
void TestSourcesAndMaintenance() {
  using namespace quickdial;
  const auto oldShell = App(L"Old Windows app", L"shell:old", ApplicationSource::AppsFolder);
  const auto oldShortcut = App(L"Removed shortcut", L"old.lnk", ApplicationSource::UserStartMenu);
  const auto added = App(L"Added shortcut", L"new.lnk", ApplicationSource::UserStartMenu);
  InstalledAppsResult refresh{{added}, {}, {{ApplicationSource::AppsFolder, false, 0, L"fixture failure"},
      {ApplicationSource::UserStartMenu, true, 1, {}}}};
  const auto reconciled = ReconcileDiscoverySources({oldShell, oldShortcut}, refresh);
  Check(reconciled.size() == 2 && std::any_of(reconciled.begin(), reconciled.end(), [](const auto& app) { return app.target == L"shell:old"; }) &&
      std::none_of(reconciled.begin(), reconciled.end(), [](const auto& app) { return app.target == L"old.lnk"; }),
      "failed provider keeps its cache while a healthy provider adds and removes shortcuts");
  Check(ReconcileDiscoverySources(reconciled, {{}, L"total failure"}).size() == 2, "total failure preserves every cached source");
  Catalog catalog;
  auto uninstaller = App(L"Odinstaluj - fixture", L"remove.lnk", ApplicationSource::UserStartMenu);
  uninstaller.resolvedTarget = L"C:\\Fixture\\unins000.exe";
  auto wrapper = App(L"Uninstall Notes", L"notes.lnk", ApplicationSource::UserStartMenu);
  wrapper.resolvedTarget = L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
  wrapper.shortcutArguments = L"-NoProfile -File \"C:\\Notes\\Uninstall.ps1\"";
  auto tool = App(L"Uninstall Tool", L"shell:tool", ApplicationSource::AppsFolder);
  tool.identity = L"Vendor.UninstallTool!App";
  auto docs = App(L"Uninstall instructions", L"instructions.url", ApplicationSource::UserStartMenu);
  docs.resolvedTarget = L"https://example.invalid/uninstall";
  Check(IsMaintenanceApplication(uninstaller) && IsMaintenanceApplication(wrapper) &&
      !IsMaintenanceApplication(tool) && !IsMaintenanceApplication(docs),
      "maintenance classification uses command evidence and preserves legitimate uninstall tools and docs");
  auto merged = MergeInstalledApplications(catalog, {uninstaller, wrapper, tool, docs});
  Check(merged.applications.size() == 2, "automatic maintenance entries are hidden by default");
  catalog.includedApplications = {L"REMOVE.LNK"};
  Check(MergeInstalledApplications(catalog, {uninstaller, wrapper, tool, docs}).applications.size() == 3,
      "case-insensitive inclusion overrides maintenance filtering");
  catalog.hiddenApplications = {L"remove.lnk"};
  Check(MergeInstalledApplications(catalog, {uninstaller}, true).applications.empty(),
      "explicit hidden rules win over maintenance visibility and inclusion");
  catalog = {};
  auto manual = uninstaller; manual.source = ApplicationSource::Manual;
  catalog.applications = {manual};
  Check(MergeInstalledApplications(catalog, {uninstaller}).applications.size() == 1, "manual maintenance entries are preserved");
  Check(EquivalentLaunchKey(L"C:/APP.exe", L"--book \"two words\"") == EquivalentLaunchKey(L"c:\\app.EXE", L"--book \"two words\"") &&
      EquivalentLaunchKey(L"c:\\app.exe", L"--book A") != EquivalentLaunchKey(L"c:\\app.exe", L"--book a"),
      "launch identity normalizes paths while preserving argument case");
  auto one = added; one.resolvedTarget = L"C:\\app.exe"; one.shortcutArguments = L"--book one";
  one.launchKey = EquivalentLaunchKey(one.resolvedTarget, one.shortcutArguments);
  auto equivalent = one; equivalent.target = L"other.lnk"; equivalent.source = ApplicationSource::AppsFolder;
  auto different = one; different.target = L"two.lnk"; different.shortcutArguments = L"--book two";
  different.launchKey = EquivalentLaunchKey(different.resolvedTarget, different.shortcutArguments);
  Check(MergeInstalledApplications({}, {one, equivalent, different}).applications.size() == 2,
      "equivalent launch commands deduplicate while different arguments remain separate apps");
  auto differentDirectory = one;
  differentDirectory.target = L"different-directory.lnk";
  differentDirectory.shortcutWorkingDirectory = L"C:\\Different library";
  differentDirectory.launchKey = EquivalentLaunchKey(differentDirectory.resolvedTarget,
      differentDirectory.shortcutArguments, differentDirectory.shortcutWorkingDirectory);
  Check(MergeInstalledApplications({}, {one, differentDirectory}).applications.size() == 2,
      "working directory variants remain separate launchable apps");
  one.identity = L"same-app-id";
  different.identity = L"same-app-id";
  Check(MergeInstalledApplications({}, {one, different}).applications.size() == 2,
      "discovered AppUserModelID variants with different arguments do not hide each other");
}
void TestIndexedShortcutMerge() {
  using namespace quickdial;
  auto shortcut = App(L"Fixture Browser", L"browser.lnk", ApplicationSource::UserStartMenu);
  shortcut.resolvedTarget = L"C:\\Browser\\browser.exe";
  shortcut.shortcutArguments = L"--profile-directory=\"Default profile\"";
  shortcut.shortcutWorkingDirectory = L"C:\\Browser";
  shortcut.launchKey = EquivalentLaunchKey(shortcut.resolvedTarget,
      shortcut.shortcutArguments, shortcut.shortcutWorkingDirectory);
  auto indexed = shortcut;
  indexed.target = L"shell:AppsFolder\\Microsoft.AutoGenerated.Browser";
  indexed.identity = L"Microsoft.AutoGenerated.Browser";
  indexed.source = ApplicationSource::AppsFolder;
  indexed.name = L"FIXTURE BROWSER";
  indexed.resolvedTarget = L"c:/browser/BROWSER.EXE";
  indexed.shortcutWorkingDirectory.clear();
  indexed.launchKey = EquivalentLaunchKey(indexed.resolvedTarget, indexed.shortcutArguments);
  const auto merged = MergeInstalledApplications({}, {indexed, shortcut});
  Check(merged.applications.size() == 1 && merged.applications[0].target == L"browser.lnk" &&
      merged.applications[0].shortcutWorkingDirectory == L"C:\\Browser",
      "indexed app with an unknown working folder uses its matching physical shortcut");
  auto variant = shortcut;
  variant.target = L"other-folder.lnk";
  variant.shortcutWorkingDirectory = L"C:\\Other folder";
  variant.launchKey = EquivalentLaunchKey(variant.resolvedTarget,
      variant.shortcutArguments, variant.shortcutWorkingDirectory);
  const auto variants = MergeInstalledApplications({}, {indexed, variant, shortcut});
  Check(variants.applications.size() == 2 && std::all_of(variants.applications.begin(), variants.applications.end(),
      [](const auto& app) { return app.source == ApplicationSource::UserStartMenu; }),
      "both physical working-folder variants survive without an extra indexed copy");
  auto otherName = indexed; otherName.name = L"Different app";
  Check(MergeInstalledApplications({}, {shortcut, otherName}).applications.size() == 2,
      "missing indexed working folder does not merge unrelated app names by executable alone");
  auto otherArguments = indexed; otherArguments.shortcutArguments = L"--profile-directory=Other";
  otherArguments.launchKey = EquivalentLaunchKey(otherArguments.resolvedTarget, otherArguments.shortcutArguments);
  Check(MergeInstalledApplications({}, {shortcut, otherArguments}).applications.size() == 2,
      "indexed argument variants remain distinct even with a matching display name");
  auto otherTarget = indexed; otherTarget.resolvedTarget = L"C:\\Other\\browser.exe";
  otherTarget.launchKey = EquivalentLaunchKey(otherTarget.resolvedTarget, otherTarget.shortcutArguments);
  Check(MergeInstalledApplications({}, {shortcut, otherTarget}).applications.size() == 2,
      "same-name indexed apps with different executables remain distinct");
  auto knownFolder = indexed; knownFolder.shortcutWorkingDirectory = L"C:\\Other folder";
  knownFolder.launchKey = EquivalentLaunchKey(knownFolder.resolvedTarget,
      knownFolder.shortcutArguments, knownFolder.shortcutWorkingDirectory);
  Check(MergeInstalledApplications({}, {shortcut, knownFolder}).applications.size() == 2,
      "known indexed working-folder differences remain distinct");
  auto opaque = indexed; opaque.resolvedTarget.clear(); opaque.launchKey.clear();
  Check(MergeInstalledApplications({}, {shortcut, opaque}).applications.size() == 2,
      "matching names without command or app-ID evidence never merge");
  shortcut.identity = indexed.identity;
  otherName.resolvedTarget.clear(); otherName.launchKey.clear();
  const auto byId = MergeInstalledApplications({}, {otherName, shortcut});
  Check(byId.applications.size() == 1 && byId.applications[0].target == L"browser.lnk",
      "matching app ID and arguments join renamed indexed apps to physical shortcuts");
  auto hidden = shortcut; hidden.target = L"hidden.lnk";
  Catalog configured; configured.hiddenApplications = {L"hidden.lnk"};
  const auto hiddenMerge = MergeInstalledApplications(configured, {hidden, indexed});
  Check(hiddenMerge.applications.size() == 1 && hiddenMerge.applications[0].source == ApplicationSource::AppsFolder,
      "excluded physical shortcuts do not suppress a usable indexed entry");
}
void TestDuplicateLaunchPatterns() {
  using namespace quickdial;
  const auto shortcut = [](std::wstring name, std::wstring link, std::wstring target,
                           std::wstring args, std::wstring directory, std::wstring id) {
    auto app = App(std::move(name), std::move(link), ApplicationSource::UserStartMenu);
    app.resolvedTarget = std::move(target); app.shortcutArguments = std::move(args);
    app.shortcutWorkingDirectory = std::move(directory); app.identity = std::move(id);
    app.launchKey = EquivalentLaunchKey(app.resolvedTarget, app.shortcutArguments, app.shortcutWorkingDirectory);
    return app;
  };
  const auto merge = [](Catalog configured, std::vector<ApplicationEntry> apps) {
    for (auto& app : apps)
      app.launchKey = EquivalentLaunchKey(app.resolvedTarget, app.shortcutArguments, app.shortcutWorkingDirectory);
    return MergeInstalledApplications(std::move(configured), std::move(apps));
  };
  const auto browser = shortcut(L"Browser", L"browser.lnk", L"C:\\Brave\\brave.exe",
      L"--source-shortcut-location=start-menu", L"C:\\Brave", L"Brave");
  auto indexed = browser; indexed.source = ApplicationSource::AppsFolder;
  indexed.target = L"shell:AppsFolder\\Brave"; indexed.shortcutArguments.clear();
  indexed.shortcutWorkingDirectory.clear(); indexed.launchKey.clear();
  for (const auto& inventory : {std::vector{browser, indexed}, std::vector{indexed, browser}}) {
    const auto merged = merge({}, inventory);
    Check(merged.applications.size() == 1 && merged.applications[0].target == L"browser.lnk" &&
        merged.applications[0].shortcutArguments == L"--source-shortcut-location=start-menu",
        "Chromium launch metrics do not add an indexed copy or change the retained shortcut command");
  }
  for (const auto* arguments : {L"--profile-directory=Work", L"--app-id=remote", L"--incognito",
       L"https://example.invalid/Work", L"--source-shortcut-location=unknown",
       L"-- --source-shortcut-location=start-menu"}) {
    auto variant = browser; variant.target = L"variant.lnk"; variant.shortcutArguments = arguments;
    const auto variants = merge({}, {browser, indexed, variant});
    Check(variants.applications.size() == 2 &&
        std::any_of(variants.applications.begin(), variants.applications.end(), [](const auto& app) { return app.target == L"browser.lnk"; }) &&
        std::any_of(variants.applications.begin(), variants.applications.end(), [](const auto& app) { return app.target == L"variant.lnk"; }),
        "browser profiles, web apps, private mode, URLs, unknown flags and positional arguments remain distinct");
  }
  auto chrome = shortcut(L"Chrome Remote Desktop", L"chrome-remote.lnk", L"C:\\Chrome\\chrome_proxy.exe",
      L"--profile-directory=Default --app-id=remote", L"", L"shared-browser-app-id");
  auto brave = chrome; brave.target = L"brave-remote.lnk"; brave.resolvedTarget = L"C:\\Brave\\chrome_proxy.exe";
  Check(merge({}, {brave, chrome}).applications.size() == 2,
      "Chrome and Brave web apps remain separate even if their names, arguments and app IDs collide");
  auto profile = chrome; profile.target = L"work-remote.lnk";
  profile.shortcutArguments = L"--profile-directory=Work --app-id=remote";
  auto anotherApp = chrome; anotherApp.target = L"another-app.lnk";
  anotherApp.shortcutArguments = L"--profile-directory=Default --app-id=another";
  Check(merge({}, {chrome, profile, anotherApp}).applications.size() == 3,
      "different profiles and web-app IDs in one browser remain separate");
  Check(EquivalentLaunchKey(L"C:\\tool.exe", L"--source-shortcut-location=start-menu") !=
      EquivalentLaunchKey(L"C:\\tool.exe", L""), "Chromium metrics normalization never applies to unrelated executables");
  Check(EquivalentLaunchKey(L"https://example.invalid/chrome.exe", L"--source-shortcut-location=start-menu") !=
      EquivalentLaunchKey(L"https://example.invalid/chrome.exe", L""), "a URL ending in a browser filename is not a browser command");
  auto manual = App(L"My browser", L"C:\\Brave\\brave.exe", ApplicationSource::Manual);
  manual.arguments = {L"--source-shortcut-location=start-menu"}; manual.workingDirectory = L"C:\\Brave";
  Catalog configured; configured.applications = {manual};
  const auto manualMerge = merge(configured, {browser});
  Check(manualMerge.applications.size() == 1 && manualMerge.applications[0].name == L"My browser",
      "configured argument vectors use the same command identity and retain precedence over equivalent discovered shortcuts");

  const auto updater = shortcut(L"Chat", L"z-stable-chat.lnk", L"C:\\Chat\\Update.exe",
      L"--processStart Chat.exe", L"C:\\Chat\\app-1.2.3", L"com.squirrel.Chat.Chat");
  auto direct = shortcut(L"Chat", L"a-versioned-chat.lnk", L"C:\\Chat\\app-1.2.3\\Chat.exe",
      L"", L"C:\\Chat\\app-1.2.3", L"com.squirrel.Chat.Chat");
  for (const auto& inventory : {std::vector{direct, updater}, std::vector{updater, direct}}) {
    const auto merged = merge({}, inventory);
    Check(merged.applications.size() == 1 && merged.applications[0].target == L"z-stable-chat.lnk" &&
        merged.applications[0].resolvedTarget == L"C:\\Chat\\Update.exe",
        "equivalent Squirrel launch paths retain the stable updater shortcut regardless of enumeration or name order");
  }
  auto forwarded = updater; forwarded.target = L"forwarded.lnk";
  forwarded.shortcutArguments = L"--processStart Chat.exe --process-start-args \"--profile Work\"";
  auto work = direct; work.target = L"work.lnk"; work.shortcutArguments = L"--profile Work";
  const auto forwardedMerge = merge({}, {direct, work, updater, forwarded});
  Check(forwardedMerge.applications.size() == 2 && std::all_of(forwardedMerge.applications.begin(),
      forwardedMerge.applications.end(), [](const auto& app) { return app.resolvedTarget == L"C:\\Chat\\Update.exe"; }),
      "Squirrel forwarded arguments merge only with their equivalent direct command and preserve the default variant");
  for (int change = 0; change < 5; ++change) {
    auto variant = updater; variant.target = L"different-updater.lnk";
    if (change == 0) variant.shortcutArguments += L" --unknown";
    if (change == 1) variant.shortcutArguments = L"--processStart Other.exe";
    if (change == 2) variant.shortcutWorkingDirectory = L"C:\\Other\\app-1.2.3";
    if (change == 3) variant.identity = L"unrelated.updater";
    if (change == 4) variant.shortcutArguments = L"--processStart ..\\Chat.exe";
    Check(merge({}, {direct, variant}).applications.size() == 2,
        "unknown updater syntax, other executables, foreign folders, unrelated identities and traversal never merge");
  }
  auto oldVersion = direct; oldVersion.target = L"old-version.lnk";
  oldVersion.resolvedTarget = L"C:\\Chat\\app-1.0.0\\Chat.exe";
  oldVersion.shortcutWorkingDirectory = L"C:\\Chat\\app-1.0.0";
  Check(merge({}, {updater, oldVersion}).applications.size() == 2,
      "an explicitly different application version and working folder remain distinct");
  Catalog hidden; hidden.hiddenApplications = {updater.target};
  const auto withoutUpdater = merge(hidden, {updater, direct});
  Check(withoutUpdater.applications.size() == 1 && withoutUpdater.applications[0].target == direct.target,
      "an excluded updater never suppresses the available direct shortcut");

  auto url = shortcut(L"Release notes", L"release-notes.url", L"https://example.invalid/Notes?Version=A", L"", L"", L"");
  auto urlIndex = url; urlIndex.target = L"shell:AppsFolder\\https://example.invalid/Notes?Version=A";
  urlIndex.source = ApplicationSource::AppsFolder;
  Check(merge({}, {url, urlIndex}).applications.size() == 1,
      "physical and indexed entries for the identical URL merge");
  auto otherUrl = urlIndex; otherUrl.target = L"shell:AppsFolder\\https://example.invalid/notes?Version=A";
  otherUrl.resolvedTarget = L"https://example.invalid/notes?Version=A";
  Check(merge({}, {urlIndex, otherUrl}).applications.size() == 2,
      "URI path case is preserved even when indexed target strings differ only by case");
}
void TestRegistration(const std::filesystem::path& directory) {
  using namespace winrt::Windows::Data::Json;
  const auto catalog = directory / L"apps.json";
  const std::string original = R"({"version":1,"discoverInstalled":false,"hiddenApplications":["hidden"],"includedApplications":["included"],"custom":{"keep":7},"applications":[{"id":"keep","name":"Keep","target":"same.exe","arguments":["one"]},{"id":"update","name":"Old","target":"old.exe","aliases":["books"],"extra":{"keep":true}}]})";
  Write(catalog, original);
  const auto result = quickdial::RegisterApplicationJson(catalog, LR"({"id":"update","name":"New","target":"same.exe","arguments":["two"]})");
  Check(result.success && Read(result.backup) == original, "atomic registration keeps an exact pre-update backup");
  const auto updated = JsonObject::Parse(winrt::to_hstring(Read(catalog)));
  const auto apps = updated.GetNamedArray(L"applications");
  Check(!updated.GetNamedBoolean(L"discoverInstalled") && updated.GetNamedObject(L"custom").GetNamedNumber(L"keep") == 7 &&
      updated.GetNamedArray(L"hiddenApplications").Size() == 1 && updated.GetNamedArray(L"includedApplications").Size() == 1,
      "registration preserves discovery settings, hidden rules, inclusion rules and unknown root fields");
  Check(apps.Size() == 2 && apps.GetAt(0).GetObject().GetNamedString(L"id") == L"keep" &&
      apps.GetAt(1).GetObject().GetNamedObject(L"extra").GetNamedBoolean(L"keep") &&
      apps.GetAt(1).GetObject().GetNamedArray(L"aliases").GetStringAt(0) == L"books",
      "registration preserves different commands, app order, unspecified aliases and unknown app fields");
  const auto before = Read(catalog);
  Check(!quickdial::RegisterApplicationJson(catalog, LR"({"name":"Missing identity","target":"bad.exe"})").success && Read(catalog) == before,
      "invalid registration leaves the catalog byte-for-byte unchanged");
  Write(catalog, "invalid JSON");
  Check(!quickdial::RegisterApplicationJson(catalog, LR"({"id":"valid","name":"Valid","target":"app.exe"})").success && Read(catalog) == "invalid JSON",
      "registration refuses to replace an invalid existing catalog");
}
void TestShortcuts(const std::filesystem::path& directory) {
  using namespace quickdial;
  const auto programs = directory / L"Programs";
  std::filesystem::create_directories(programs / L"Nested");
  Microsoft::WRL::ComPtr<IShellLinkW> link;
  HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.ReleaseAndGetAddressOf()));
  Check(SUCCEEDED(hr), "shortcut fixture has a real Windows Shell-link provider");
  if (FAILED(hr)) return;
  const auto target = directory / L"fixture.exe";
  Write(target, "fixture is never launched");
  link->SetPath(target.c_str()); link->SetArguments(L"--fixture \"two words\""); link->SetWorkingDirectory(directory.c_str());
  Microsoft::WRL::ComPtr<IPersistFile> file;
  link.As(&file);
  const auto shortcut = programs / L"Nested" / L"Fixture.lnk";
  Check(SUCCEEDED(file->Save(shortcut.c_str(), TRUE)), "fixture shortcut is persisted");
  Write(programs / L"Website.url", "[InternetShortcut]\r\nURL=https://example.invalid/fixture\r\n");
  Write(programs / L"Broken.lnk", "invalid shortcut");
  const auto discovered = ReadStartMenuApplications(programs, ApplicationSource::UserStartMenu);
  Check(discovered && discovered.applications.size() == 2 && discovered.sources.size() == 1 && discovered.sources[0].skipped == 1,
      "recursive physical discovery finds shortcuts and URLs without a Shell index and skips one broken item");
  const auto found = std::find_if(discovered.applications.begin(), discovered.applications.end(), [&](const auto& app) { return app.target == shortcut.wstring(); });
  Check(found != discovered.applications.end() && found->resolvedTarget == target.wstring() && found->shortcutArguments == L"--fixture \"two words\"" &&
      found->shortcutWorkingDirectory == directory.wstring(),
      "the original shortcut remains the launch target and its command metadata survives discovery");
  Microsoft::WRL::ComPtr<IShellLinkW> webLink;
  Check(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&webLink))),
      "URL shortcut fixture has a Shell-link provider");
  const wchar_t* url = L"https://example.invalid/ReleaseNotes?Version=A";
  PIDLIST_ABSOLUTE urlId = nullptr;
  const auto parsed = SHParseDisplayName(url, nullptr, &urlId, 0, nullptr);
  Check(SUCCEEDED(parsed) && urlId && webLink && SUCCEEDED(webLink->SetIDList(urlId)),
      "URL shortcut fixture stores a real namespace target without a file path");
  if (urlId) CoTaskMemFree(urlId);
  if (webLink && SUCCEEDED(parsed)) {
    Microsoft::WRL::ComPtr<IPersistFile> webFile;
    const auto urlShortcut = programs / L"Release notes.lnk";
    Check(SUCCEEDED(webLink.As(&webFile)) && SUCCEEDED(webFile->Save(urlShortcut.c_str(), TRUE)),
        "namespace URL shortcut is persisted");
    const auto urls = ReadStartMenuApplications(programs, ApplicationSource::UserStartMenu);
    const auto foundUrl = std::find_if(urls.applications.begin(), urls.applications.end(),
        [&](const auto& app) { return app.target == urlShortcut.wstring(); });
    Check(urls && foundUrl != urls.applications.end() && foundUrl->resolvedTarget == url &&
        foundUrl->launchKey == EquivalentLaunchKey(url, L""),
        "namespace URL metadata survives physical shortcut discovery for cross-source duplicate matching");
  }
  Check(!ReadStartMenuApplications(directory / L"Missing", ApplicationSource::UserStartMenu),
      "source-level directory failure cannot publish a successful partial refresh");
}
}
int main() {
  winrt::init_apartment(winrt::apartment_type::single_threaded);
  wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH, temp);
  const auto directory = std::filesystem::path(temp) / (L"BladeLauncherReliability-" + std::to_wstring(GetCurrentProcessId()));
  try {
    std::filesystem::create_directories(directory);
    TestPowerSearch(); TestRefresh(); TestPartialWatcher(directory); TestSourcesAndMaintenance(); TestIndexedShortcutMerge();
    TestDuplicateLaunchPatterns(); TestRegistration(directory); TestShortcuts(directory);
  } catch (const winrt::hresult_error& error) { ++failures; std::cerr << winrt::to_string(error.message()) << '\n'; }
    catch (const std::exception& error) { ++failures; std::cerr << error.what() << '\n'; }
  std::error_code ignored; std::filesystem::remove_all(directory, ignored);
  if (!failures) std::cout << "Launcher reliability checks passed\n";
  return failures ? 1 : 0;
}
