#include "MainMenu.h"

#include "Catalog.h"
#include "Search.h"
#include "TextUtilities.h"

#include <algorithm>
#include <cwctype>

namespace quickdial {

std::wstring_view MenuItemLabel(MenuItemId item) {
  switch (item) {
    case MenuItemId::System: return L"System";
    case MenuItemId::Appearance: return L"Appearance";
    case MenuItemId::ShutDown: return L"Shut down";
    case MenuItemId::Restart: return L"Restart";
    case MenuItemId::SignOut: return L"Sign out";
  }
  return {};
}

std::vector<MenuItemId> MenuItems(MenuPage page, std::wstring_view query) {
  const bool empty = std::all_of(query.begin(), query.end(), [](wchar_t c) { return std::iswspace(c); });
  if (page == MenuPage::Main && empty) return {MenuItemId::System,MenuItemId::Appearance};
  if (empty) return {MenuItemId::ShutDown, MenuItemId::Restart, MenuItemId::SignOut};

  if (page==MenuPage::Main && (IsExactMenuQuery(MenuItemId::Appearance,query) || FoldCase(query)==L"theme")) return {MenuItemId::Appearance};

  // Reuse the app search contract for names and aliases inside this section.
  static const Catalog commands = [] {
    Catalog catalog;
    ApplicationEntry shutdown;
    shutdown.name = MenuItemLabel(MenuItemId::ShutDown);
    shutdown.aliases = {L"shutdown", L"power off"};
    ApplicationEntry restart;
    restart.name = MenuItemLabel(MenuItemId::Restart);
    restart.aliases = {L"reboot"};
    ApplicationEntry signout;
    signout.name = MenuItemLabel(MenuItemId::SignOut);
    signout.aliases = {L"log off", L"logoff", L"logout"};
    catalog.applications = {shutdown, restart, signout};
    return catalog;
  }();
  std::vector<MenuItemId> items;
  for (const auto index : RankApplications(commands, query))
    items.push_back(index == 0 ? MenuItemId::ShutDown : index == 1 ? MenuItemId::Restart : MenuItemId::SignOut);
  return items;
}

bool IsExactMenuQuery(MenuItemId item, std::wstring_view query) {
  auto text = FoldCase(query);
  const auto first = text.find_first_not_of(L" \t\r\n");
  if (first == std::wstring::npos) return false;
  text = text.substr(first, text.find_last_not_of(L" \t\r\n") - first + 1);
  if (text == FoldCase(MenuItemLabel(item))) return true;
  switch (item) {
    case MenuItemId::Appearance: return text == L"theme";
    case MenuItemId::ShutDown: return text == L"shutdown" || text == L"power off";
    case MenuItemId::Restart: return text == L"reboot";
    case MenuItemId::SignOut: return text == L"log off" || text == L"logoff" || text == L"logout";
    default: return false;
  }
}

std::vector<LauncherResult> RankLauncherResults(const Catalog& catalog, MenuPage page, std::wstring_view query) {
  const auto commands = MenuItems(page, query);
  std::vector<LauncherResult> results;
  const bool empty = std::all_of(query.begin(), query.end(), [](wchar_t c) { return std::iswspace(c); });
  if (page == MenuPage::System || empty) {
    for (const auto command : commands) results.push_back({LauncherResult::Kind::Command, 0, command});
    return results;
  }
  // Only deliberate exact commands go ahead of apps. Partial command matches follow all app matches.
  for (const auto command : commands) if (IsExactMenuQuery(command, query))
    results.push_back({LauncherResult::Kind::Command, 0, command});
  for (const auto index : RankApplications(catalog, query))
    results.push_back({LauncherResult::Kind::Application, index, MenuItemId::System});
  const auto length = std::count_if(query.begin(), query.end(), [](wchar_t c) { return !std::iswspace(c); });
  if (length >= 3) for (const auto command : commands) if (!IsExactMenuQuery(command, query))
    results.push_back({LauncherResult::Kind::Command, 0, command});
  return results;
}

}  // namespace quickdial
