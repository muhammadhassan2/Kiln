// Preview panel: pass viewer, channel isolation, pixel inspector with loupe.
#include "app/App.h"

#include "app/Common.h"

#include <imgui.h>

#include <cmath>
#include <cstdio>

namespace app {

namespace {
const char* kViewShader = R"(#version 410 core
uniform sampler2D u_src;
uniform int u_mode;
uniform vec2 u_resolution;
out vec4 o;
void main() {
    vec4 c = texture(u_src, gl_FragCoord.xy / u_resolution);
    vec3 r = c.rgb;
    if (u_mode == 1) r = vec3(c.r);
    else if (u_mode == 2) r = vec3(c.g);
    else if (u_mode == 3) r = vec3(c.b);
    else if (u_mode == 4) r = vec3(c.a);
    else if (u_mode == 5) r = vec3(dot(c.rgb, vec3(0.2126, 0.7152, 0.0722)));
    o = vec4(r, 1.0);
}
)";
}  // namespace

void App::ensureViewProgram() {
    if (viewProgram_) return;
    auto r = device_->compileFragment(shader::translateFragment(kViewShader), false);
    viewProgram_ = std::move(r.program);
    if (!viewProgram_) log(Level::Error, "internal view shader failed: " + r.log);
}

void App::drawPreview() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
    if (ImGui::Begin("Preview")) {
        // ---- toolbar
        std::vector<std::string> names;
        for (const auto& p : passes_) names.push_back(p->name);
        int viewIdx = -1;
        for (size_t i = 0; i < passes_.size(); ++i)
            if ((viewPass_ < 0 && passes_[i]->isImage) || static_cast<int>(i) == viewPass_) viewIdx = static_cast<int>(i);
        ImGui::SetNextItemWidth(110);
        if (ImGui::BeginCombo("##viewpass", viewIdx >= 0 ? names[static_cast<size_t>(viewIdx)].c_str() : "Image")) {
            for (size_t i = 0; i < names.size(); ++i)
                if (ImGui::Selectable(names[i].c_str(), static_cast<int>(i) == viewIdx))
                    viewPass_ = passes_[i]->isImage ? -1 : static_cast<int>(i);
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Which pass to show (inspect buffers of a multipass shader)");
        ImGui::SameLine();
        static const char* kChannels[] = {"RGB", "R", "G", "B", "A", "Luma"};
        ImGui::SetNextItemWidth(70);
        ImGui::Combo("##channel", &channelMode_, kChannels, 6);
        ImGui::SameLine();
        ImGui::Checkbox("Inspect", &inspect_);
        ImGui::SetItemTooltip("Hover the preview to read pixel values with a zoomed loupe");

        if (viewPass_ >= static_cast<int>(passes_.size())) viewPass_ = -1;
        const Pass& src = viewPass_ < 0 ? imagePass() : *passes_[static_cast<size_t>(viewPass_)];
        rhi::RenderTarget* rt = src.targets[src.latest].get();

        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
        if (outputMode_ == 0 && !export_.active) {
            desiredW_ = std::max(1, static_cast<int>(avail.x * scale.x));
            desiredH_ = std::max(1, static_cast<int>(avail.y * scale.y));
        }
        if (!rt) {
            ImGui::TextDisabled("Nothing rendered yet.");
            ImGui::End();
            ImGui::PopStyleVar();
            return;
        }

        // Channel isolation renders through a tiny internal shader.
        const rhi::Texture* shown = &rt->color();
        if (channelMode_ != 0) {
            ensureViewProgram();
            if (viewProgram_) {
                if (!viewTarget_) viewTarget_ = device_->createRenderTarget(rt->width(), rt->height(), rhi::Format::RGBA8);
                viewTarget_->resize(rt->width(), rt->height());
                std::vector<rhi::UniformValue> vals;
                std::vector<rhi::TextureBinding> binds;
                for (const auto& u : viewProgram_->uniforms()) {
                    if (u.name == "u_src") binds.push_back({u.location, 0, &rt->color()});
                    rhi::UniformValue v;
                    v.location = u.location;
                    v.type = u.type;
                    if (u.name == "u_mode") v.i = channelMode_;
                    if (u.name == "u_resolution") {
                        v.f[0] = static_cast<float>(rt->width());
                        v.f[1] = static_cast<float>(rt->height());
                    }
                    if (u.type != rhi::UniformType::Sampler2D) vals.push_back(v);
                }
                device_->draw(*viewProgram_, *viewTarget_, vals, binds);
                shown = &viewTarget_->color();
            }
        }

        const float rtW = static_cast<float>(rt->width());
        const float rtH = static_cast<float>(rt->height());
        const float s = std::min(avail.x / rtW, avail.y / rtH);
        const ImVec2 size(std::max(1.0f, rtW * s), std::max(1.0f, rtH * s));
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        const ImVec2 origin(cursor.x + (avail.x - size.x) * 0.5f, cursor.y + (avail.y - size.y) * 0.5f);
        ImGui::SetCursorScreenPos(origin);
        ImGui::Image(texId(*shown), size, ImVec2(0, 1), ImVec2(1, 0));

        if (ImGui::IsItemHovered()) {
            const ImVec2 m = ImGui::GetIO().MousePos;
            const float mx = (m.x - origin.x) / size.x * rtW;
            const float my = (1.0f - (m.y - origin.y) / size.y) * rtH;
            if (!inspect_) {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    mouse_[0] = mx;
                    mouse_[1] = my;
                }
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    mouse_[2] = mx;
                    mouse_[3] = my;
                }
            } else {
                // Loupe: read an 11x11 block of float pixels around the cursor.
                constexpr int kN = 11;
                const int px = std::clamp(static_cast<int>(mx), 0, rt->width() - 1);
                const int py = std::clamp(static_cast<int>(my), 0, rt->height() - 1);
                const int x0 = std::clamp(px - kN / 2, 0, std::max(0, rt->width() - kN));
                const int y0 = std::clamp(py - kN / 2, 0, std::max(0, rt->height() - kN));
                const int w = std::min(kN, rt->width()), h = std::min(kN, rt->height());
                std::vector<float> block;
                device_->readPixels(*rt, x0, y0, w, h, block);
                ImGui::BeginTooltip();
                const ImVec2 lo = ImGui::GetCursorScreenPos();
                const float cell = 11.0f;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                for (int yy = 0; yy < h; ++yy)
                    for (int xx = 0; xx < w; ++xx) {
                        const float* c = &block[static_cast<size_t>((yy * w + xx) * 4)];
                        const ImVec2 a(lo.x + xx * cell, lo.y + (h - 1 - yy) * cell);
                        dl->AddRectFilled(a, ImVec2(a.x + cell, a.y + cell),
                                          ImGui::ColorConvertFloat4ToU32(ImVec4(std::clamp(c[0], 0.f, 1.f), std::clamp(c[1], 0.f, 1.f),
                                                                                std::clamp(c[2], 0.f, 1.f), 1.0f)));
                        if (x0 + xx == px && y0 + yy == py)
                            dl->AddRect(a, ImVec2(a.x + cell, a.y + cell), IM_COL32(255, 255, 255, 255), 0, 0, 2.0f);
                    }
                ImGui::Dummy(ImVec2(cell * w, cell * h));
                const float* c = &block[static_cast<size_t>(((py - y0) * w + (px - x0)) * 4)];
                ImGui::PushFont(fonts_.mono, 0.0f);
                ImGui::Text("%s  x %d  y %d", src.name.c_str(), px, py);
                ImGui::Text("R %.4f\nG %.4f\nB %.4f\nA %.4f", c[0], c[1], c[2], c[3]);
                auto to8 = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
                ImGui::Text("#%02X%02X%02X%02X", to8(c[0]), to8(c[1]), to8(c[2]), to8(c[3]));
                ImGui::PopFont();
                ImGui::EndTooltip();
            }
        }

        char buf[128];
        double gpu = 0;
        for (size_t i = 0; i < passes_.size(); ++i) gpu += device_->timerMs(static_cast<int>(i));
        std::snprintf(buf, sizeof(buf), "%s · %dx%d · GPU %.2f ms%s", src.name.c_str(), rt->width(), rt->height(), gpu,
                      export_.active ? " · EXPORTING" : "");
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 ts = ImGui::CalcTextSize(buf);
        const ImVec2 p0(origin.x + 8, origin.y + 8);
        dl->AddRectFilled(p0, ImVec2(p0.x + ts.x + 12, p0.y + ts.y + 6), IM_COL32(10, 11, 14, 200), 5.0f);
        dl->AddText(ImVec2(p0.x + 6, p0.y + 3), IM_COL32(242, 244, 247, 255), buf);
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

}  // namespace app
