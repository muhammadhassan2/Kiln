// Menus, dock layout, editor tabs, console, uniforms panel, overlay mode.
#include "app/App.h"

#include "app/Common.h"
#include "app/Templates.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>

namespace app {

namespace {
constexpr const char* kWinEditor = "Editor";
constexpr const char* kWinPreview = "Preview";
constexpr const char* kWinConsole = "Console";
constexpr const char* kWinTextures = "Textures";
constexpr const char* kWinUniforms = "Uniforms";
const char* const kAllWindows[] = {"Editor", "Preview", "Console", "Timeline", "History", "AI Assistant", "Textures",
                                   "Library", "Uniforms", "Transition", "Mobile / Metal", "Performance"};
}  // namespace

void App::setupAutocomplete(TextEditor& e) { e.SetAutoCompleteConfig(&autocomplete_); }

void App::handleShortcuts() {
    if (batch_.active) return;  // the user's project is set aside while a batch renders
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
        if (ImGui::GetIO().KeyShift) compileAll("Shift+F5");
        else compilePass(activePass(), "F5");
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) setOverlay(!overlay_);
    if (overlay_ && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) setOverlay(false);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, ImGuiInputFlags_RouteGlobal))
        pendingDialog_ = Dialog::SaveProjectAs;
    else if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal)) {
        if (projectPath_.empty()) pendingDialog_ = Dialog::SaveProjectAs;
        else saveProject(projectPath_);
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, ImGuiInputFlags_RouteGlobal)) pendingDialog_ = Dialog::OpenProject;
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_E, ImGuiInputFlags_RouteGlobal)) showExport_ = true;
}

void App::setOverlay(bool on) {
    if (on && !overlay_) {
        // Remember which tab is selected in each dock node; hidden windows lose it otherwise.
        tabsToSelect_.clear();
        for (const char* name : kAllWindows) {
            ImGuiWindow* w = ImGui::FindWindowByName(name);
            if (w && w->DockNode && w->DockNode->TabBar && w->DockNode->TabBar->SelectedTabId == w->TabId)
                tabsToSelect_.push_back(name);
        }
    }
    if (!on && overlay_) {
        focusDefaults_ = 5;
        mouse_ = {};  // overlay mouse coordinates don't apply to the preview
    }
    overlay_ = on;
    auto p = editorPalette_;
    if (on) {
        p[static_cast<size_t>(TextEditor::Color::background)] = IM_COL32(0, 0, 0, 0);
        p[static_cast<size_t>(TextEditor::Color::currentLineHighlight)] = IM_COL32(0, 0, 0, 80);
    }
    for (auto& pass : passes_) pass->editor->SetPalette(p);
}

void App::drawUI() {
    if (overlay_) {
        drawOverlay();
        return;
    }
    drawMenuBar();
    drawDockspace();
    drawEditor();
    drawPreview();
    drawConsole();
    drawTextures();
    drawUniforms();
    drawTimeline();
    drawTransition();
    drawHistory();
    drawLibrary();
    drawAI();
    drawCrossCompile();
    drawProfiler();
    drawExport();
    if (focusDefaults_ > 0) {
        // Select the default tabs (after every window was submitted), then give the editor focus.
        // Select the default tab in each dock node (internal API: the tab bar's next selection).
        for (const auto& name : tabsToSelect_) {
            ImGuiWindow* w = ImGui::FindWindowByName(name.c_str());
            if (w && w->DockNode && w->DockNode->TabBar) w->DockNode->TabBar->NextSelectedTabId = w->TabId;
        }
        ImGui::SetWindowFocus(kWinEditor);  // a focused docked window would keep its own tab selected
        --focusDefaults_;
    }
}

// ====================================================================== menu bar

void App::drawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    ImGui::TextColored(col(colors::Accent), "Shader Studio");
    ImGui::Separator();

    if (ImGui::BeginMenu("File", !batch_.active)) {
        if (ImGui::BeginMenu("New from template")) {
            const auto& all = templates();
            for (size_t i = 0; i < all.size(); ++i) {
                if (ImGui::MenuItem(all[i].name)) newProject(static_cast<int>(i));
                ImGui::SetItemTooltip("%s", all[i].description);
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Open Project...", "Cmd/Ctrl+O")) pendingDialog_ = Dialog::OpenProject;
        if (ImGui::BeginMenu("Open Recent", !recent_.empty())) {
            for (const auto& r : recent_)
                if (ImGui::MenuItem(r.filename().string().c_str())) openProject(r);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Save Project", "Cmd/Ctrl+S")) {
            if (projectPath_.empty()) pendingDialog_ = Dialog::SaveProjectAs;
            else saveProject(projectPath_);
        }
        if (ImGui::MenuItem("Save Project As...", "Cmd/Ctrl+Shift+S")) pendingDialog_ = Dialog::SaveProjectAs;
        ImGui::Separator();
        if (ImGui::MenuItem("Open Shader into Current Pass...")) pendingDialog_ = Dialog::OpenShader;
        if (ImGui::MenuItem("Save Current Pass As...")) pendingDialog_ = Dialog::SaveShaderAs;
        if (ImGui::MenuItem("Paste Shadertoy Code into Current Pass")) {
            if (const char* clip = ImGui::GetClipboardText(); clip && *clip) {
                activePass().editor->SetText(clip);
                compilePass(activePass(), "pasted");
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Export...", "Cmd/Ctrl+E")) showExport_ = true;
        if (ImGui::MenuItem("Save to Library")) {
            showLibrary_ = true;
            ImGui::SetWindowFocus("Library");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) quit_ = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Pass")) {
        for (char c = 'A'; c <= 'D'; ++c) {
            const std::string name = std::string("Buffer ") + c;
            if (ImGui::MenuItem(("Add " + name).c_str(), nullptr, false, findPass(name) == nullptr)) {
                addPass(name, std::string("#version 410 core\n// ") + name +
                                  ": read it from other passes as u_buffer" + c +
                                  "; u_prev is this buffer's previous frame.\n"
                                  "uniform vec2 u_resolution;\nuniform float u_time;\nuniform sampler2D u_prev;\n"
                                  "out vec4 fragColor;\n\nvoid main() {\n    vec2 uv = gl_FragCoord.xy / u_resolution;\n"
                                  "    fragColor = vec4(texture(u_prev, uv).rgb * 0.98 + 0.02 * vec3(uv, 0.5 + 0.5 * sin(u_time)), 1.0);\n}\n");
                compilePass(activePass(), "new pass");
            }
        }
        ImGui::Separator();
        Pass& ap = activePass();
        if (ImGui::MenuItem("Remove Current Buffer", nullptr, false, !ap.isImage)) removePass(activePass_);
        if (!ap.isImage) ImGui::MenuItem("Enabled", nullptr, &ap.enabled);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Shader")) {
        if (ImGui::MenuItem("Compile Current Pass", "F5")) compilePass(activePass(), "menu");
        if (ImGui::MenuItem("Compile All Passes", "Shift+F5")) compileAll("menu");
        ImGui::MenuItem("Auto-compile while typing", nullptr, &autoCompile_);
        ImGui::SetNextItemWidth(140.0f);
        if (ImGui::SliderInt("Typing delay", &compileDelayMs_, 0, 1000, "%d ms")) {
            applyCompileDelay();
            saveSettings();
        }
        ImGui::SetItemTooltip("How long to wait after your last keystroke before auto-compiling.\n"
                              "0 = compile on every keystroke. Changing a value under Defines\n"
                              "(Uniforms panel) always compiles immediately.");
        ImGui::Separator();
        if (ImGui::MenuItem("UV origin top-left (video frames)", nullptr, uvTopLeft_)) {
            uvTopLeft_ = !uvTopLeft_;
            textures_->setFlipAll(!uvTopLeft_);
            compileAll("UV origin changed");
        }
        ImGui::SetItemTooltip("For mobile/video effect shaders (varying tc / v_texCoord):\n"
                              "tc.y = 0 at the top and images/videos are uploaded without a Y flip.");
        if (ImGui::MenuItem("Mobile GLSL ES validation", nullptr, mobileCheck_)) {
            mobileCheck_ = !mobileCheck_;
            compileAll("mobile check toggled");
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Overlay mode", "F2", overlay_)) setOverlay(!overlay_);
        drawOutputWindowMenu();
        ImGui::Separator();
        ImGui::MenuItem("Timeline", nullptr, &showTimeline_);
        ImGui::MenuItem("Transition", nullptr, &showTransition_);
        ImGui::MenuItem("History", nullptr, &showHistory_);
        ImGui::MenuItem("Library", nullptr, &showLibrary_);
        ImGui::MenuItem("AI Assistant", nullptr, &showAI_);
        ImGui::MenuItem("Mobile / Metal", nullptr, &showCrossCompile_);
        ImGui::MenuItem("Performance", nullptr, &showProfiler_);
        ImGui::MenuItem("Export", nullptr, &showExport_);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset layout")) resetLayout_ = true;
        ImGui::EndMenu();
    }

    ImGui::Separator();
    static const char* kPresets[] = {"Desktop", "Android phone", "iPhone"};
    int preset = static_cast<int>(preset_);
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::Combo("##preset", &preset, kPresets, 3)) applyPreset(static_cast<Preset>(preset));
    ImGui::SetItemTooltip("Device preset: output size, UV origin and GLSL ES validation");

    ImGui::Separator();
    if (ImGui::Button("|<")) {
        time_ = 0.0;
        frame_ = 0;
        timeJumped_ = true;
    }
    if (ImGui::Button(playing_ ? "Pause" : "Play ")) playing_ = !playing_;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%06.2f", timelineTime());
    ImGui::PushFont(fonts_.mono, 0.0f);
    ImGui::TextUnformatted(buf);
    std::snprintf(buf, sizeof(buf), "%.0f fps", ImGui::GetIO().Framerate);
    ImGui::TextColored(col(colors::Accent), "%s", buf);
    ImGui::PopFont();

    ImGui::Separator();
    ImGui::SetNextItemWidth(150.0f);
    if (ImGui::BeginCombo("##output", kOutputs[outputMode_].label)) {
        for (int i = 0; i < kOutputCount; ++i)
            if (ImGui::Selectable(kOutputs[i].label, i == outputMode_)) outputMode_ = i;
        ImGui::EndCombo();
    }
    pushAccentButton();
    if (ImGui::Button("Compile")) compileAll("button");
    popAccentButton();
    if (ImGui::Button("Export")) showExport_ = true;
    if (ImGui::Button("Overlay")) setOverlay(true);

    ImGui::EndMainMenuBar();
}

void App::drawDockspace() {
    const ImGuiID dockId = ImHashStr("ShaderStudioDockspace");
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (!layoutBuilt_ || resetLayout_) {
        layoutBuilt_ = true;
        ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockId);
        if (resetLayout_ || node == nullptr || node->IsLeafNode()) {
            resetLayout_ = false;
            ImGui::DockBuilderRemoveNode(dockId);
            ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockId, vp->WorkSize);
            ImGuiID left = 0, right = 0, leftBottom = 0, rightTop = 0, rightRest = 0, rightMid = 0, rightBottom = 0;
            ImGui::DockBuilderSplitNode(dockId, ImGuiDir_Left, 0.55f, &left, &right);
            ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.30f, &leftBottom, &left);
            ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.45f, &rightTop, &rightRest);
            ImGui::DockBuilderSplitNode(rightRest, ImGuiDir_Up, 0.45f, &rightMid, &rightBottom);
            ImGui::DockBuilderDockWindow(kWinEditor, left);
            ImGui::DockBuilderDockWindow(kWinConsole, leftBottom);
            ImGui::DockBuilderDockWindow("Timeline", leftBottom);
            ImGui::DockBuilderDockWindow("History", leftBottom);
            ImGui::DockBuilderDockWindow("AI Assistant", leftBottom);
            ImGui::DockBuilderDockWindow(kWinPreview, rightTop);
            ImGui::DockBuilderDockWindow(kWinTextures, rightMid);
            ImGui::DockBuilderDockWindow("Library", rightMid);
            ImGui::DockBuilderDockWindow(kWinUniforms, rightBottom);
            ImGui::DockBuilderDockWindow("Transition", rightBottom);
            ImGui::DockBuilderDockWindow("Mobile / Metal", rightBottom);
            ImGui::DockBuilderDockWindow("Performance", rightBottom);
            ImGui::DockBuilderFinish(dockId);
            tabsToSelect_ = {kWinTextures, kWinUniforms, kWinConsole};
            focusDefaults_ = 10;
        }
    }
    ImGui::DockSpaceOverViewport(dockId, vp);
}

// ====================================================================== editor

void App::drawChannelPopup(Pass& p) {
    if (!ImGui::BeginPopup("##channels")) return;
    ImGui::TextDisabled("iChannel routing for %s", p.name.c_str());
    auto& slots = textures_->slots();
    for (int c = 0; c < 4; ++c) {
        int& route = p.channels[static_cast<size_t>(c)];
        std::string preview = route < 0 ? "Texture slot " + std::to_string(c) + " (default)"
                              : route < 100 ? "Texture slot " + std::to_string(route)
                                            : std::string("Buffer ") + static_cast<char>('A' + route - 100);
        ImGui::SetNextItemWidth(220);
        if (ImGui::BeginCombo(("iChannel" + std::to_string(c)).c_str(), preview.c_str())) {
            if (ImGui::Selectable("Texture slot (default)", route < 0)) route = -1;
            for (int i = 0; i < static_cast<int>(slots.size()); ++i) {
                const std::string n = "Texture " + std::to_string(i) + ": " + slots[static_cast<size_t>(i)].name;
                if (ImGui::Selectable(n.c_str(), route == i)) route = i;
            }
            for (int b = 0; b < 4; ++b) {
                const std::string n = std::string("Buffer ") + static_cast<char>('A' + b);
                if (ImGui::Selectable(n.c_str(), route == 100 + b)) route = 100 + b;
            }
            ImGui::EndCombo();
        }
    }
    ImGui::EndPopup();
}

void App::drawEditor() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (ImGui::Begin(kWinEditor)) {
        const bool pendingSelect = selectTab_;
        if (ImGui::BeginTabBar("##passes", ImGuiTabBarFlags_None)) {
            for (size_t i = 0; i < passes_.size(); ++i) {
                Pass& p = *passes_[i];
                std::string label = p.name;
                if (!p.file.empty()) label += " · " + p.file.filename().string();
                if (!p.file.empty() && p.editor->GetUndoIndex() != p.savedUndo) label += " *";
                if (!p.program) label += " (!)";
                label += "###tab" + p.name;
                const ImGuiTabItemFlags flags = (selectTab_ && i == activePass_) ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem(label.c_str(), nullptr, flags)) {
                    if (!pendingSelect) activePass_ = i;
                    // Pass toolbar
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8);
                    ImGui::TextDisabled("%s", p.dialect.empty() ? "" : p.dialect.c_str());
                    if (!p.isImage) {
                        ImGui::SameLine();
                        ImGui::Checkbox("Enabled", &p.enabled);
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("iChannels...")) ImGui::OpenPopup("##channels");
                    drawChannelPopup(p);
                    ImGui::SameLine();
                    if (ImGui::SmallButton(p.file.empty() ? "Save to file..." : "Save file")) {
                        if (p.file.empty()) pendingDialog_ = Dialog::SaveShaderAs;
                        else if (saveShaderFromPass(p, p.file)) log(Level::Ok, "Saved " + p.file.string());
                    }
                    if (!p.isImage) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Remove pass")) {
                            removePass(i);
                            ImGui::EndTabItem();
                            break;
                        }
                    }
                    ImGui::PushFont(fonts_.mono, 0.0f);
                    p.editor->Render("##code");
                    ImGui::PopFont();
                    ImGui::EndTabItem();
                }
            }
            if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing)) ImGui::OpenPopup("##addpass");
            if (ImGui::BeginPopup("##addpass")) {
                for (char c = 'A'; c <= 'D'; ++c) {
                    const std::string name = std::string("Buffer ") + c;
                    if (ImGui::MenuItem(name.c_str(), nullptr, false, findPass(name) == nullptr)) {
                        addPass(name, std::string("#version 410 core\nuniform vec2 u_resolution;\nuniform float u_time;\n"
                                                  "uniform sampler2D u_prev;  // this buffer's previous frame\n"
                                                  "out vec4 fragColor;\n\nvoid main() {\n"
                                                  "    vec2 uv = gl_FragCoord.xy / u_resolution;\n"
                                                  "    fragColor = vec4(uv, 0.5 + 0.5 * sin(u_time), 1.0);\n}\n"));
                        compilePass(activePass(), "new pass");
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::EndTabBar();
        }
        if (pendingSelect) selectTab_ = false;
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

// ====================================================================== console

void App::drawConsole() {
    if (ImGui::Begin(kWinConsole)) {
        int errors = 0;
        for (const auto& p : passes_)
            for (const auto& d : p->diagnostics)
                if (d.isError) ++errors;
        if (ImGui::SmallButton("Clear")) log_.clear();
        ImGui::SameLine();
        ImGui::TextDisabled("Problems %d", errors);
        ImGui::SameLine();
        ImGui::Checkbox("Auto-compile", &autoCompile_);
        ImGui::SameLine();
        ImGui::Checkbox("Mobile ES check", &mobileCheck_);
        ImGui::Separator();
        ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PushFont(fonts_.mono, 0.0f);
        for (const auto& e : log_) {
            ImU32 c = IM_COL32(183, 188, 198, 255);
            const char* tag = "INFO";
            switch (e.level) {
                case Level::Info: c = colors::Glsl; tag = "INFO"; break;
                case Level::Ok: c = colors::Accent; tag = "OK  "; break;
                case Level::Warn: c = IM_COL32(255, 200, 90, 255); tag = "WARN"; break;
                case Level::Error: c = colors::Error; tag = "ERR "; break;
            }
            ImGui::TextColored(col(colors::Muted), "%s", e.time.c_str());
            ImGui::SameLine();
            ImGui::TextColored(col(c), "%s", tag);
            ImGui::SameLine();
            ImGui::TextUnformatted(e.text.c_str());
        }
        if (scrollLog_) {
            ImGui::SetScrollHereY(1.0f);
            scrollLog_ = false;
        }
        ImGui::PopFont();
        ImGui::EndChild();
    }
    ImGui::End();
}

// ====================================================================== uniforms

// "#ifndef NAME / #define NAME value" pairs become controls; changing one compiles at once
// (no typing delay), and variants compiled before come straight from the program cache.
void App::drawDefines() {
    std::vector<const shader::DefineInfo*> list;
    auto add = [&](const Pass& p) {
        for (const auto& d : p.defines)
            if (std::none_of(list.begin(), list.end(), [&](const auto* x) { return x->name == d.name; }))
                list.push_back(&d);
    };
    add(activePass());
    for (const auto& p : passes_) add(*p);
    if (list.empty()) return;

    auto parseInt = [](const std::string& s, int& out) {
        const char* b = s.data();
        const char* e = b + s.size();
        auto [ptr, ec] = std::from_chars(b, e, out);
        return ec == std::errc() && ptr == e;
    };
    auto parseFloat = [](const std::string& s, float& out) {
        char* end = nullptr;
        out = std::strtof(s.c_str(), &end);
        return !s.empty() && end == s.c_str() + s.size();
    };

    std::pair<std::string, std::string> pending;  // name, new value
    std::string resetName;
    ImGui::TextDisabled("Defines");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    ImGui::SetItemTooltip("From \"#ifndef NAME\" + \"#define NAME value\" in your code.\n"
                          "Changing a value here recompiles instantly; variants you already\n"
                          "compiled are reused from a cache. Mouse wheel over a number steps it.\n"
                          "Editing the #define line in the code takes over again.");
    for (const auto* d : list) {
        const std::string* ov = defineOverride(d->name);
        const std::string current = ov ? *ov : d->value;
        ImGui::PushID(d->name.c_str());
        const float resetW = ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2;
        const float labelW = ImGui::CalcTextSize(d->name.c_str()).x + ImGui::GetStyle().ItemInnerSpacing.x;
        ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - labelW - resetW -
                                                    ImGui::GetStyle().ItemSpacing.x));
        int iv = 0;
        float fv = 0.0f;
        if (parseInt(current, iv)) {
            bool changed = ImGui::InputInt(d->name.c_str(), &iv, 1, 10);
            if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
                iv += ImGui::GetIO().MouseWheel > 0.0f ? 1 : -1;
                changed = true;
            }
            if (changed) pending = {d->name, std::to_string(iv)};
        } else if (parseFloat(current, fv)) {
            ImGui::DragFloat(d->name.c_str(), &fv, 0.01f, 0.0f, 0.0f, "%.3f");
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%.6g", fv);
                std::string s = buf;
                if (s.find_first_of(".eE") == std::string::npos) s += ".0";  // keep it a float literal
                pending = {d->name, s};
            }
        } else {
            char buf[256];
            std::snprintf(buf, sizeof(buf), "%s", current.c_str());
            if (ImGui::InputText(d->name.c_str(), buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue) && buf[0])
                pending = {d->name, buf};
        }
        ImGui::SetItemTooltip("Line %d · default in code: %s", d->line, d->value.c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(!ov);
        if (ImGui::SmallButton("Reset")) {
            pending = {d->name, d->value};
            resetName = d->name;
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::Separator();

    // Applied after the loop: compiling rebuilds the passes' define lists that `list` points into.
    if (!pending.first.empty()) {
        if (!resetName.empty()) {
            std::erase_if(defineOverrides_, [&](const auto& o) { return o.first == resetName; });
            for (auto& p : passes_)
                if (std::any_of(p->defines.begin(), p->defines.end(), [&](const auto& x) { return x.name == resetName; }))
                    compilePass(*p, "define reset");
        } else {
            setDefine(pending.first, pending.second);
        }
    }
}

void App::drawUniforms() {
    if (ImGui::Begin(kWinUniforms)) {
        ImGui::PushFont(fonts_.mono, 0.0f);
        ImGui::TextDisabled("time %.2f · timeline %.2f · frame %d · mouse %.0f,%.0f", time_, timelineTime(), frame_,
                            mouse_[0], mouse_[1]);
        ImGui::PopFont();
        ImGui::Separator();
        drawDefines();

        const auto uniforms = userUniforms();
        for (const auto* up : uniforms) {
            const auto& u = *up;
            auto& v = uniformValues_[u.name];
            const shader::Annotation* ap = annotationFor(u.name);
            const shader::Annotation a = ap ? *ap : shader::Annotation{};
            using K = shader::Annotation::Kind;
            const char* label = u.name.c_str();
            const bool isFloat = u.type == rhi::UniformType::Float;
            const int components = u.type == rhi::UniformType::Vec2 ? 2 : u.type == rhi::UniformType::Vec3 ? 3
                                 : u.type == rhi::UniformType::Vec4 ? 4 : 1;

            ImGui::PushID(label);
            const float driveW = 92.0f, secW = 60.0f, keyW = 34.0f;
            const float labelW = ImGui::CalcTextSize(label).x + ImGui::GetStyle().ItemInnerSpacing.x;
            const float extra = driveW + keyW + (v.drive == DriveLoop ? secW : 0.0f) + 3 * ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - labelW - extra));

            const bool driven = v.drive != DriveManual;
            if (driven) {
                std::array<float, 4> shown{};
                for (int c = 0; c < 4; ++c) shown[static_cast<size_t>(c)] = evalUniform(v, c);
                ImGui::BeginDisabled();
                if (a.kind == K::Color && components >= 3) ImGui::ColorEdit4("##driven", shown.data(), components == 3 ? ImGuiColorEditFlags_NoAlpha : 0);
                else ImGui::DragScalarN("##driven", ImGuiDataType_Float, shown.data(), components, 0.0f, nullptr, nullptr, "%.3f");
                ImGui::EndDisabled();
            } else {
                switch (u.type) {
                    case rhi::UniformType::Float:
                        if (a.kind == K::Slider) ImGui::SliderFloat("##v", &v.f[0], a.min, a.max);
                        else if (a.kind == K::Toggle) {
                            bool b = v.f[0] > 0.5f;
                            if (ImGui::Checkbox("##v", &b)) v.f[0] = b ? 1.0f : 0.0f;
                        } else ImGui::DragFloat("##v", &v.f[0], a.speed);
                        break;
                    case rhi::UniformType::Vec2:
                        if (a.kind == K::Slider) ImGui::SliderFloat2("##v", v.f.data(), a.min, a.max);
                        else ImGui::DragFloat2("##v", v.f.data(), a.speed);
                        break;
                    case rhi::UniformType::Vec3:
                        if (a.kind == K::Color) ImGui::ColorEdit3("##v", v.f.data());
                        else if (a.kind == K::Slider) ImGui::SliderFloat3("##v", v.f.data(), a.min, a.max);
                        else ImGui::DragFloat3("##v", v.f.data(), a.speed);
                        break;
                    case rhi::UniformType::Vec4:
                        if (a.kind == K::Color) ImGui::ColorEdit4("##v", v.f.data());
                        else if (a.kind == K::Slider) ImGui::SliderFloat4("##v", v.f.data(), a.min, a.max);
                        else ImGui::DragFloat4("##v", v.f.data(), a.speed);
                        break;
                    case rhi::UniformType::Int:
                        if (a.kind == K::Slider) ImGui::SliderInt("##v", &v.i, static_cast<int>(a.min), static_cast<int>(a.max));
                        else ImGui::DragInt("##v", &v.i);
                        v.f[0] = static_cast<float>(v.i);
                        break;
                    case rhi::UniformType::Bool: {
                        bool b = v.i != 0;
                        if (ImGui::Checkbox("##v", &b)) v.i = b ? 1 : 0;
                        v.f[0] = static_cast<float>(v.i);
                        break;
                    }
                    default: ImGui::TextDisabled("(unsupported type)"); break;
                }
            }
            // Drive mode: floats get all modes, other types Manual / Keyframes.
            ImGui::SameLine();
            ImGui::SetNextItemWidth(driveW);
            if (isFloat) {
                ImGui::Combo("##drive", &v.drive, kDriveNames, 5);
            } else {
                int d = v.drive == DriveKeys ? 1 : 0;
                static const char* kTwo[] = {"Manual", "Keyframes"};
                if (ImGui::Combo("##drive", &d, kTwo, 2)) v.drive = d ? DriveKeys : DriveManual;
            }
            if (v.drive == DriveLoop) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(secW);
                ImGui::DragFloat("##secs", &v.loopSeconds, 0.05f, 0.1f, 120.0f, "%.1fs");
            }
            ImGui::SameLine();
            if (ImGui::Button("+key", ImVec2(keyW + 8, 0))) {
                addKeyframe(u.name, v);
                showTimeline_ = true;
            }
            ImGui::SetItemTooltip("Add a keyframe at the current timeline time");
            ImGui::SameLine();
            ImGui::TextUnformatted(label);
            ImGui::PopID();
        }
        if (uniforms.empty())
            ImGui::TextDisabled("Declare a uniform to get a control, e.g.\nuniform float u_amount; // @slider 0..1 0.5");
        ImGui::Spacing();
        if (ImGui::SmallButton("Reset to defaults")) {
            uniformValues_.clear();
            for (auto& p : passes_) syncUniformDefaults(*p);
        }
    }
    ImGui::End();
}

// ====================================================================== overlay

void App::drawOverlay() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
    desiredW_ = std::max(1, static_cast<int>(vp->Size.x * scale.x));
    desiredH_ = std::max(1, static_cast<int>(vp->Size.y * scale.y));

    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    const Pass& img = imagePass();
    if (img.program && img.targets[img.latest])
        bg->AddImage(texId(img.targets[img.latest]->color()), vp->Pos,
                     ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), ImVec2(0, 1), ImVec2(1, 0));
    bg->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y),
                      IM_COL32(0, 0, 0, static_cast<int>(overlayDim_ * 255.0f)));

    const ImVec2 m = ImGui::GetIO().MousePos;
    mouse_[0] = (m.x - vp->Pos.x) * scale.x;
    mouse_[1] = static_cast<float>(desiredH_) - (m.y - vp->Pos.y) * scale.y;

    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##overlay", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    if (ImGui::Button("Exit overlay (Esc)")) setOverlay(false);
    ImGui::SameLine();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f s · %.0f fps", timelineTime(), ImGui::GetIO().Framerate);
    ImGui::TextColored(col(colors::Accent), "%s", buf);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("Dim", &overlayDim_, 0.0f, 0.9f, "%.2f");
    ImGui::SameLine();
    if (ImGui::Button(playing_ ? "Pause" : "Play ")) playing_ = !playing_;
    ImGui::SameLine();
    // Pass switcher
    for (size_t i = 0; i < passes_.size(); ++i) {
        ImGui::SameLine();
        if (ImGui::RadioButton(passes_[i]->name.c_str(), activePass_ == i)) {
            activePass_ = i;
            selectTab_ = true;
        }
    }
    ImGui::SameLine();
    pushAccentButton();
    if (ImGui::Button("Compile (F5)")) compilePass(activePass(), "button");
    popAccentButton();

    ImGui::PushFont(fonts_.mono, 18.0f);
    activePass().editor->Render("##overlay_code");
    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
}

}  // namespace app
