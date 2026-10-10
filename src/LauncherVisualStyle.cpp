#include "LauncherVisualStyle.h"
#include "blade_appearance.h"

namespace quickdial::visuals {
namespace {
D2D1_COLOR_F Color(const char* role) {
    const COLORREF c=blade_appearance_color(role);
    return D2D1::ColorF(GetRValue(c)/255.0f,GetGValue(c)/255.0f,GetBValue(c)/255.0f);
}
}
void Refresh() {
    kWindowWidth=static_cast<float>(blade_appearance_number("components.launcher.width"));
    kSearchHeight=static_cast<float>(blade_appearance_number("components.launcher.searchHeight"));
    kResultHeight=static_cast<float>(blade_appearance_number("components.launcher.resultHeight"));
    kSearchFontSize=static_cast<float>(blade_appearance_number("tokens.typography.search"));
    kResultFontSize=static_cast<float>(blade_appearance_number("tokens.typography.result"));
    kMessageFontSize=static_cast<float>(blade_appearance_number("tokens.typography.compact"));
    kSelectionCornerRadius=static_cast<float>(blade_appearance_number("tokens.geometry.controlRadius"));
}
Palette GetPalette(bool) {
    return {Color("background"),Color("text"),Color("muted"),Color("selected"),Color("selectedText"),Color("border"),Color("error"),
            blade_appearance_color("background"),blade_appearance_color("text")};
}
std::wstring ResolveFontFamily(IDWriteFactory*) {
    uint16_t family[256]{}; blade_appearance_font(family,256,0);
    return reinterpret_cast<const wchar_t*>(family);
}
} // namespace quickdial::visuals
