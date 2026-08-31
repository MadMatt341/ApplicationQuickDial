#include "Catalog.h"
#include "HotkeyState.h"
#include "InstalledApps.h"
#include "Search.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <winrt/base.h>

namespace {

int failures = 0;

void Check(bool condition, const char* description) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << description << '\n';
  }
}

quickdial::ApplicationEntry App(
    std::wstring name, std::wstring target, std::vector<std::wstring> aliases = {}) {
  quickdial::ApplicationEntry app;
  app.name = std::move(name);
  app.target = std::move(target);
  app.aliases = std::move(aliases);
  return app;
}

void TestCatalogParsing() {
  SetEnvironmentVariableW(L"QD_TEST_ROOT", L"C:\\QuickDialTest");
  const auto result = quickdial::ParseCatalogJson(R"json({
    "version": 1,
    "discoverInstalled": false,
    "hiddenApplications": ["hidden.app", "Hidden by name"],
    "unknownFutureSetting": true,
    "applications": [
      {
        "name": "Editor",
        "target": "%QD_TEST_ROOT%\\editor.exe",
        "id": "editor.application",
        "aliases": ["text", "notes"],
        "arguments": ["--new", "file with spaces.txt"],
        "workingDirectory": "%QD_TEST_ROOT%",
        "icon": "%QD_TEST_ROOT%\\editor.png",
        "unknown": 42
      }
    ]
  })json");

  Check(static_cast<bool>(result), "valid JSON catalog parses");
  if (result) {
    Check(result.catalog->applications.size() == 1, "catalog has one application");
    Check(!result.catalog->discoverInstalled, "installed-app discovery setting is parsed");
    Check(result.catalog->hiddenApplications.size() == 2 &&
              result.catalog->hiddenApplications[1] == L"Hidden by name",
          "hidden applications are parsed");
    const auto& app = result.catalog->applications.front();
    Check(app.name == L"Editor", "name is parsed");
    Check(app.target == L"C:\\QuickDialTest\\editor.exe", "target expands environment variables");
    Check(app.identity == L"editor.application", "application identity is parsed");
    Check(app.aliases.size() == 2 && app.aliases[1] == L"notes", "aliases are parsed");
    Check(app.arguments.size() == 2 && app.arguments[1] == L"file with spaces.txt", "arguments are parsed");
    Check(app.workingDirectory == L"C:\\QuickDialTest", "working directory expands environment variables");
    Check(app.icon == L"C:\\QuickDialTest\\editor.png", "icon expands environment variables");
  }

  Check(!quickdial::ParseCatalogJson(R"json({"version":2,"applications":[]})json"),
        "unsupported version is rejected");
  Check(!quickdial::ParseCatalogJson(R"json({"version":1})json"),
        "missing applications is rejected");
  Check(!quickdial::ParseCatalogJson(R"json({"version":1,"applications":[{"name":"Missing target"}]})json"),
        "missing required target is rejected");
  Check(!quickdial::ParseCatalogJson(
            R"json({"version":1,"discoverInstalled":"yes","applications":[]})json"),
        "non-boolean discovery setting is rejected");
  Check(!quickdial::ParseCatalogJson(
            R"json({"version":1,"hiddenApplications":[1],"applications":[]})json"),
        "non-string hidden application is rejected");
  Check(!quickdial::ParseCatalogJson("not json"), "malformed JSON is rejected");
  const auto compatible = quickdial::ParseCatalogJson(
      "\xEF\xBB\xBF{\"version\":1,\"applications\":[]}");
  Check(static_cast<bool>(compatible), "UTF-8 BOM is accepted");
  if (compatible) {
    Check(compatible.catalog->discoverInstalled, "installed-app discovery defaults on for version 1 catalogs");
  }
}

void TestDefaultCatalog() {
  const auto testDirectory = std::filesystem::temp_directory_path() /
                             (L"ApplicationQuickDialTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                              std::to_wstring(GetTickCount64()));
  const auto catalogPath = testDirectory / L"apps.json";
  std::wstring error;

  Check(quickdial::EnsureDefaultCatalog(catalogPath, error), "default catalog is created");
  const auto result = quickdial::LoadCatalogFile(catalogPath);
  Check(static_cast<bool>(result), "default catalog parses");
  if (result) {
    const auto& applications = result.catalog->applications;
    Check(result.catalog->discoverInstalled, "default catalog enables installed-app discovery");
    Check(applications.size() == 6, "default catalog contains the curated applications");
    if (applications.size() == 6) {
      Check(applications[1].name == L"Obsidian", "default catalog contains Obsidian");
      Check(applications[2].name == L"Slack" && applications[2].target == L"slack://open",
            "default catalog contains Slack");
      Check(applications[3].name == L"Brave", "default catalog contains Brave");
      Check(applications[4].name == L"P4V" && !applications[4].aliases.empty() &&
                applications[4].aliases[0] == L"p4",
            "default catalog contains P4V with its p4 alias");
      Check(applications[5].name == L"UnrealGameSync" && !applications[5].aliases.empty() &&
                applications[5].aliases[0] == L"ugs",
            "default catalog contains UnrealGameSync with its ugs alias");
    }
  }

  std::error_code cleanupError;
  std::filesystem::remove(catalogPath, cleanupError);
  std::filesystem::remove(testDirectory, cleanupError);
}

void TestInstalledApplicationMerge() {
  quickdial::Catalog configured;
  configured.hiddenApplications = {L"hidden.application", L"Hidden by name"};
  configured.applications = {
      App(L"Manual first", L"manual.exe", {L"priority"}),
      App(L"Brave", L"manual-brave.exe"),
  };

  auto Installed = [](std::wstring name, std::wstring target, std::wstring identity) {
    auto application = App(std::move(name), std::move(target));
    application.identity = std::move(identity);
    return application;
  };

  std::vector<quickdial::ApplicationEntry> installed = {
      Installed(L"Zulu", L"shell:AppsFolder\\zulu", L"zulu.application"),
      Installed(L"Hidden identity", L"shell:AppsFolder\\hidden", L"hidden.application"),
      Installed(L"brave", L"shell:AppsFolder\\brave", L"brave.application"),
      Installed(L"Hidden by name", L"shell:AppsFolder\\hidden-name", L"other.application"),
      Installed(L"Alpha", L"shell:AppsFolder\\alpha", L"alpha.application"),
      Installed(L"Duplicate target", L"manual.exe", L"duplicate.application"),
  };

  const quickdial::Catalog merged =
      quickdial::MergeInstalledApplications(configured, std::move(installed));
  Check(merged.applications.size() == 4, "merge removes hidden and duplicate installed applications");
  if (merged.applications.size() == 4) {
    Check(merged.applications[0].name == L"Manual first" && merged.applications[1].name == L"Brave",
          "merge preserves configured application order");
    Check(merged.applications[2].name == L"Alpha" && merged.applications[3].name == L"Zulu",
          "merge appends discovered applications alphabetically");
  }

  configured.discoverInstalled = false;
  const quickdial::Catalog manualOnly = quickdial::MergeInstalledApplications(
      configured, {Installed(L"Ignored", L"ignored.exe", L"ignored.application")});
  Check(manualOnly.applications.size() == configured.applications.size(),
        "disabled discovery leaves the configured catalog unchanged");
}

void TestSearchRanking() {
  quickdial::Catalog catalog;
  catalog.applications = {
      App(L"Visual Studio Code", L"code.exe", {L"editor"}),
      App(L"ChatGPT", L"chat.exe", {L"codex", L"openai"}),
      App(L"Code Runner", L"runner.exe", {L"execute"}),
      App(L"OpenAI Playground", L"playground.exe"),
      App(L"My Chat Archive", L"archive.exe"),
  };

  auto results = quickdial::RankApplications(catalog, L"", 6);
  Check(results == std::vector<std::size_t>({0, 1, 2, 3, 4}), "empty query preserves JSON order");

  results = quickdial::RankApplications(catalog, L"chat", 6);
  Check(results.size() == 2 && results[0] == 1 && results[1] == 4,
        "name prefix ranks ahead of token prefix");

  results = quickdial::RankApplications(catalog, L"COD", 6);
  Check(results.size() == 3 && results[0] == 2 && results[1] == 0 && results[2] == 1,
        "name matches rank ahead of aliases and matching is case-insensitive");

  results = quickdial::RankApplications(catalog, L"open", 6);
  Check(results.size() == 2 && results[0] == 3 && results[1] == 1,
        "name prefix ranks ahead of alias prefix");

  results = quickdial::RankApplications(catalog, L"missing", 6);
  Check(results.empty(), "unmatched query returns no applications");
}

void TestHotkeyState() {
  using quickdial::HotkeyDisposition;
  quickdial::HotkeyState state;

  Check(state.Handle(VK_LWIN, true) == HotkeyDisposition::Pass, "left Windows down passes through");
  Check(state.Handle(VK_SPACE, true) == HotkeyDisposition::TriggerAndSuppress,
        "Win+Space triggers and suppresses first keydown");
  Check(state.Handle(VK_SPACE, true) == HotkeyDisposition::Suppress, "Space repeat is suppressed without retriggering");
  Check(state.Handle(VK_SPACE, false) == HotkeyDisposition::Suppress, "chord Space keyup is suppressed");
  Check(state.Handle(VK_LWIN, false) == HotkeyDisposition::Pass, "left Windows keyup passes through");

  Check(state.Handle(VK_SPACE, true) == HotkeyDisposition::Pass, "Space alone passes through");
  Check(state.Handle(VK_SPACE, false) == HotkeyDisposition::Pass, "Space-alone keyup passes through");
  Check(state.Handle(VK_RWIN, true) == HotkeyDisposition::Pass, "right Windows down passes through");
  Check(state.Handle('E', true) == HotkeyDisposition::Pass, "unrelated Win shortcut passes through");
  Check(state.Handle('E', false) == HotkeyDisposition::Pass, "unrelated keyup passes through");
  Check(state.Handle(VK_RWIN, false) == HotkeyDisposition::Pass, "right Windows keyup passes through");

  Check(state.Handle(VK_LWIN, true, true) == HotkeyDisposition::Pass, "injected Windows key is ignored");
  Check(state.Handle(VK_SPACE, true, true) == HotkeyDisposition::Pass, "injected Space key is ignored");
  Check(state.Handle(0xE8, true, true) == HotkeyDisposition::Pass, "injected shell mask key is ignored");
}

}  // namespace

int main(int argc, char* argv[]) {
  winrt::init_apartment(winrt::apartment_type::single_threaded);
  if (argc == 2 && std::strcmp(argv[1], "--list-installed") == 0) {
    const auto installed = quickdial::DiscoverInstalledApplications();
    if (!installed) {
      std::cerr << winrt::to_string(winrt::hstring(installed.error)) << '\n';
      return EXIT_FAILURE;
    }
    std::cout << "Discovered " << installed.applications.size() << " applications.\n";
    for (const auto& application : installed.applications) {
      std::cout << winrt::to_string(winrt::hstring(application.name)) << '\t'
                << winrt::to_string(winrt::hstring(application.target)) << '\n';
    }
    return installed.applications.empty() ? EXIT_FAILURE : EXIT_SUCCESS;
  }

  TestCatalogParsing();
  TestDefaultCatalog();
  TestInstalledApplicationMerge();
  TestSearchRanking();
  TestHotkeyState();

  if (failures == 0) {
    std::cout << "All quickdial core tests passed.\n";
    return EXIT_SUCCESS;
  }
  std::cerr << failures << " test(s) failed.\n";
  return EXIT_FAILURE;
}
