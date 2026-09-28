#pragma once

#include <string_view>
#include <vector>

namespace quickdial {

enum class MenuPage { Main, System };
enum class MenuItemId { System, ShutDown, Restart };

// Built-in menu entries are commands, never application catalog targets.
std::wstring_view MenuItemLabel(MenuItemId item);
std::vector<MenuItemId> MenuItems(MenuPage page, std::wstring_view query);

}  // namespace quickdial
