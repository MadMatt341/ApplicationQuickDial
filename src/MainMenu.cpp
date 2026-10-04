#include "MainMenu.h"

#include "Catalog.h"
#include "Search.h"

#include <algorithm>
#include <cwctype>

namespace quickdial {

std::wstring_view MenuItemLabel(MenuItemId item) {
  switch (item) {
    case MenuItemId::System: return L"System";
    case MenuItemId::ShutDown: return L"Shut down";
    case MenuItemId::Restart: return L"Restart";
    case MenuItemId::SignOut: return L"Sign out";
  }
  return {};
}

std::vector<MenuItemId> MenuItems(MenuPage page, std::wstring_view query) {
  const bool empty = std::all_of(query.begin(), query.end(), [](wchar_t c) { return std::iswspace(c); });
  if (page == MenuPage::Main) return empty ? std::vector{MenuItemId::System} : std::vector<MenuItemId>{};
  if (empty) return {MenuItemId::ShutDown, MenuItemId::Restart, MenuItemId::SignOut};

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

}  // namespace quickdial
