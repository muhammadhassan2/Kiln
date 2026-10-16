#include "app/Theme.h"

#include <filesystem>
#include <initializer_list>

namespace app {

namespace {

ImVec4 hex(unsigned v, float a = 1.0f) {
    return ImVec4(((v >> 16) & 0xFF) / 255.0f, ((v >> 8) & 0xFF) / 255.0f, (v & 0xFF) / 255.0f, a);
}

ImFont* tryLoad(ImGuiIO& io, std::initializer_list<const char*> paths, float size) {
    for (const char* p : paths) {
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) {
            if (ImFont* f = io.Fonts->AddFontFromFileTTF(p, size)) return f;
        }
    }
    return nullptr;
}

}  // namespace

void applyTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.0f;
    s.ChildRounding = 6.0f;
    s.FrameRounding = 6.0f;
    s.PopupRounding = 8.0f;
    s.GrabRounding = 6.0f;
    s.TabRounding = 4.0f;
    s.ScrollbarRounding = 6.0f;
    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize = 1.0f;
    s.WindowPadding = ImVec2(12, 10);
    s.FramePadding = ImVec2(8, 5);
    s.ItemSpacing = ImVec2(8, 7);
    s.ScrollbarSize = 12.0f;
    s.DockingSeparatorSize = 1.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = hex(0xD7DAE0);
    c[ImGuiCol_TextDisabled] = hex(0x8A909C);
    c[ImGuiCol_WindowBg] = hex(0x121418);
    c[ImGuiCol_ChildBg] = hex(0x0E0F12);
    c[ImGuiCol_PopupBg] = hex(0x16181D);
    c[ImGuiCol_Border] = hex(0x23262D);
    c[ImGuiCol_BorderShadow] = hex(0x000000, 0.0f);
    c[ImGuiCol_FrameBg] = hex(0x1C1F25);
    c[ImGuiCol_FrameBgHovered] = hex(0x23272F);
    c[ImGuiCol_FrameBgActive] = hex(0x2A2E36);
    c[ImGuiCol_TitleBg] = hex(0x0E0F12);
    c[ImGuiCol_TitleBgActive] = hex(0x121418);
    c[ImGuiCol_TitleBgCollapsed] = hex(0x0E0F12);
    c[ImGuiCol_MenuBarBg] = hex(0x0B0C0F);
    c[ImGuiCol_ScrollbarBg] = hex(0x0E0F12);
    c[ImGuiCol_ScrollbarGrab] = hex(0x2A2E36);
    c[ImGuiCol_ScrollbarGrabHovered] = hex(0x3A3F48);
    c[ImGuiCol_ScrollbarGrabActive] = hex(0x4A505B);
    c[ImGuiCol_CheckMark] = hex(0xC6F432);
    c[ImGuiCol_SliderGrab] = hex(0xC6F432);
    c[ImGuiCol_SliderGrabActive] = hex(0xE2FF7A);
    c[ImGuiCol_Button] = hex(0x1C1F25);
    c[ImGuiCol_ButtonHovered] = hex(0x2A2E36);
    c[ImGuiCol_ButtonActive] = hex(0x343944);
    c[ImGuiCol_Header] = hex(0x1F232A);
    c[ImGuiCol_HeaderHovered] = hex(0x262A31);
    c[ImGuiCol_HeaderActive] = hex(0x2E333C);
    c[ImGuiCol_Separator] = hex(0x23262D);
    c[ImGuiCol_SeparatorHovered] = hex(0xC6F432, 0.6f);
    c[ImGuiCol_SeparatorActive] = hex(0xC6F432);
    c[ImGuiCol_ResizeGrip] = hex(0x000000, 0.0f);
    c[ImGuiCol_ResizeGripHovered] = hex(0xC6F432, 0.5f);
    c[ImGuiCol_ResizeGripActive] = hex(0xC6F432);
    c[ImGuiCol_Tab] = hex(0x121418);
    c[ImGuiCol_TabHovered] = hex(0x1F232A);
    c[ImGuiCol_TabSelected] = hex(0x0E0F12);
    c[ImGuiCol_TabSelectedOverline] = hex(0xC6F432);
    c[ImGuiCol_TabDimmed] = hex(0x121418);
    c[ImGuiCol_TabDimmedSelected] = hex(0x0E0F12);
    c[ImGuiCol_TabDimmedSelectedOverline] = hex(0x3A3F48);
    c[ImGuiCol_DockingPreview] = hex(0xC6F432, 0.35f);
    c[ImGuiCol_DockingEmptyBg] = hex(0x0E0F12);
    c[ImGuiCol_TextSelectedBg] = hex(0xC6F432, 0.25f);
    c[ImGuiCol_NavCursor] = hex(0xC6F432);
}

Fonts loadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    Fonts f;
    f.ui = tryLoad(io,
                   {"/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/Helvetica.ttc",
                    "C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"},
                   15.0f);
    if (!f.ui) f.ui = io.Fonts->AddFontDefault();
    f.mono = tryLoad(io,
                     {"/System/Library/Fonts/SFNSMono.ttf", "/System/Library/Fonts/Menlo.ttc",
                      "C:/Windows/Fonts/consola.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"},
                     15.0f);
    if (!f.mono) f.mono = f.ui;
    return f;
}

}  // namespace app
