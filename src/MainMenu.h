#pragma once

#include <string_view>
#include <vector>
#include <cstddef>

namespace quickdial {

enum class MenuPage { Main, System };
enum class MenuItemId { System, Appearance, Startup, ShutDown, Restart, SignOut };

// Built-in menu entries are commands, never application catalog targets.
std::wstring_view MenuItemLabel(MenuItemId item);
std::vector<MenuItemId> MenuItems(MenuPage page, std::wstring_view query);
bool IsExactMenuQuery(MenuItemId item, std::wstring_view query);
struct LauncherResult {
  enum class Kind { Application, Command, TrayControl } kind = Kind::Application;
  std::size_t application = 0;
  MenuItemId command = MenuItemId::System;
  bool operator==(const LauncherResult&) const = default;
};
struct Catalog;
std::vector<LauncherResult> RankLauncherResults(const Catalog& catalog, MenuPage page, std::wstring_view query);

}  // namespace quickdial
