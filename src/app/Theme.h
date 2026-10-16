#pragma once

#include <imgui.h>

struct ImFont;

namespace app {

namespace colors {
constexpr ImU32 Accent = IM_COL32(198, 244, 50, 255);   // #C6F432
constexpr ImU32 Glsl = IM_COL32(124, 199, 255, 255);    // #7CC7FF
constexpr ImU32 Metal = IM_COL32(255, 159, 67, 255);    // #FF9F43
constexpr ImU32 Error = IM_COL32(255, 110, 110, 255);
constexpr ImU32 Muted = IM_COL32(138, 144, 156, 255);
}  // namespace colors

struct Fonts {
    ImFont* ui = nullptr;
    ImFont* mono = nullptr;
};

void applyTheme();
Fonts loadFonts();  // tries system fonts, falls back to ImGui default

}  // namespace app
