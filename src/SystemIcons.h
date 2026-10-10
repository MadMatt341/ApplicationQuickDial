#pragma once
#include "MainMenu.h"
#include <d2d1.h>
namespace quickdial {
void DrawSystemIcon(ID2D1Factory& factory, ID2D1RenderTarget& target, MenuItemId item,
                    D2D1_POINT_2F center, ID2D1Brush& brush, unsigned dpi);
}
