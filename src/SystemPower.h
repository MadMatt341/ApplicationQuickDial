#pragma once

#include "MainMenu.h"

#include <windows.h>
#include <string>

namespace quickdial {

bool RequestSystemPowerAction(MenuItemId action, std::wstring& error);

}  // namespace quickdial
