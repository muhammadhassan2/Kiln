// Timeline, keyframes, transition panel and device presets.
#include "app/App.h"

#include "app/Common.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>
#include <cstdio>

namespace app {

double App::timelineTime() const {
    const double d = std::max(0.01f, duration_);
    if (loop_) return std::fmod(std::max(0.0, time_), d);
    return std::clamp(time_, 0.0, d);
}

float App::ease(int type, float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    switch (type) {
        case 0: return x;
        case 1: return x * x * x;
        case 2: return 1.0f - std::pow(1.0f - x, 3.0f);
        case 3: return x < 0.5f ? 4.0f * x * x * x : 1.0f - std::pow(-2.0f * x + 2.0f, 3.0f) / 2.0f;
        case 4: return x < 1.0f ? 0.0f : 1.0f;
        default: return x;
    }
}

float App::evalUniform(const UniformState& s, int c) const {
    switch (s.drive) {
        case DriveTime: return c == 0 ? static_cast<float>(time_) : s.f[static_cast<size_t>(c)];
        case DriveLoop: {
            if (c != 0) return s.f[static_cast<size_t>(c)];
            const double d = std::max(0.01f, s.loopSeconds);
            return static_cast<float>(std::fmod(std::max(0.0, time_), d) / d);
        }
        case DriveVideo: return c == 0 ? textures_->firstVideoProgress() : s.f[static_cast<size_t>(c)];
        case DriveKeys: {
            if (s.keys.empty()) return s.f[static_cast<size_t>(c)];
            const float t = static_cast<float>(timelineTime());
            if (t <= s.keys.front().t) return s.keys.front().v[static_cast<size_t>(c)];
            if (t >= s.keys.back().t) return s.keys.back().v[static_cast<size_t>(c)];
            for (size_t k = 0; k + 1 < s.keys.size(); ++k) {
                const auto& a = s.keys[k];
                const auto& b = s.keys[k + 1];
                if (t >= a.t && t <= b.t) {
                    const float span = std::max(1e-5f, b.t - a.t);
                    const float e = ease(a.ease, (t - a.t) / span);
                    return a.v[static_cast<size_t>(c)] + (b.v[static_cast<size_t>(c)] - a.v[static_cast<size_t>(c)]) * e;
                }
            }
            return s.keys.back().v[static_cast<size_t>(c)];
        }
        default: return s.f[static_cast<size_t>(c)];
    }
}

void App::addKeyframe(const std::string& name, UniformState& s) {
    Keyframe k;
    k.t = static_cast<float>(timelineTime());
    for (int c = 0; c < 4; ++c) k.v[static_cast<size_t>(c)] = evalUniform(s, c);
    if (s.drive != DriveKeys) {
        s.keys.clear();
        s.drive = DriveKeys;
    }
    for (auto& existing : s.keys)
        if (std::fabs(existing.t - k.t) < 1.0f / 60.0f) {
            existing.v = k.v;
            return;
        }
    s.keys.push_back(k);
    std::sort(s.keys.begin(), s.keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.t < b.t; });
    selectedTrack_ = name;
    for (size_t i = 0; i < s.keys.size(); ++i)
        if (s.keys[i].t == k.t) selectedKey_ = static_cast<int>(i);
}

void App::drawTimeline() {
    if (!showTimeline_) return;
    if (!ImGui::Begin("Timeline", &showTimeline_)) {
        ImGui::End();
        return;
    }
    // ---- transport
    if (ImGui::Button("|<")) {
        time_ = 0.0;
        frame_ = 0;
        timeJumped_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(playing_ ? "Pause" : "Play ")) playing_ = !playing_;
    ImGui::SameLine();
    ImGui::PushFont(fonts_.mono, 0.0f);
    ImGui::Text("%6.2f / %.2f s", timelineTime(), duration_);
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    ImGui::DragFloat("Duration", &duration_, 0.1f, 0.5f, 600.0f, "%.1f s");
    ImGui::SameLine();
    ImGui::Checkbox("Loop", &loop_);
    ImGui::SameLine();
    ImGui::TextDisabled("· double-click a track to add a key, drag keys, right-click for easing");

    // ---- tracks = uniforms driven by keyframes
    std::vector<std::string> tracks;
    for (const auto* u : userUniforms()) {
        auto it = uniformValues_.find(u->name);
        if (it != uniformValues_.end() && it->second.drive == DriveKeys) tracks.push_back(u->name);
    }

    const float labelW = 150.0f, rulerH = 22.0f, rowH = 24.0f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(200.0f, ImGui::GetContentRegionAvail().x);
    const float height = rulerH + rowH * std::max<size_t>(1, tracks.size()) + 4.0f;
    ImGui::InvisibleButton("##timeline", ImVec2(width, height), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float x0 = origin.x + labelW, x1 = origin.x + width - 8.0f;
    auto toX = [&](float t) { return x0 + (x1 - x0) * (t / std::max(0.01f, duration_)); };
    auto toT = [&](float x) { return std::clamp((x - x0) / std::max(1.0f, x1 - x0), 0.0f, 1.0f) * duration_; };

    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), IM_COL32(14, 15, 18, 255), 4.0f);
    // Ruler ticks
    const float step = duration_ <= 5 ? 0.5f : (duration_ <= 20 ? 1.0f : (duration_ <= 120 ? 5.0f : 30.0f));
    for (float t = 0; t <= duration_ + 1e-3f; t += step) {
        const float x = toX(t);
        dl->AddLine(ImVec2(x, origin.y + rulerH - 6), ImVec2(x, origin.y + height), IM_COL32(35, 38, 45, 255));
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%gs", t);
        dl->AddText(ImVec2(x + 3, origin.y + 3), IM_COL32(138, 144, 156, 255), buf);
    }

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    static int dragKey = -1;
    static std::string dragTrack;
    static bool scrubbing = false;

    if (tracks.empty()) {
        dl->AddText(ImVec2(origin.x + 10, origin.y + rulerH + 4), IM_COL32(138, 144, 156, 255),
                    "No animated uniforms. Click [+key] next to a uniform in the Uniforms panel.");
    }
    for (size_t r = 0; r < tracks.size(); ++r) {
        const float y = origin.y + rulerH + rowH * static_cast<float>(r);
        auto& s = uniformValues_[tracks[r]];
        dl->AddText(ImVec2(origin.x + 8, y + 4), IM_COL32(215, 218, 224, 255), tracks[r].c_str());
        dl->AddLine(ImVec2(x0, y + rowH), ImVec2(x1, y + rowH), IM_COL32(28, 31, 37, 255));
        for (size_t k = 0; k < s.keys.size(); ++k) {
            const float x = toX(s.keys[k].t), cy = y + rowH * 0.5f, rad = 6.0f;
            const bool sel = selectedTrack_ == tracks[r] && selectedKey_ == static_cast<int>(k);
            const ImVec2 pts[4] = {{x, cy - rad}, {x + rad, cy}, {x, cy + rad}, {x - rad, cy}};
            dl->AddConvexPolyFilled(pts, 4, sel ? colors::Accent : IM_COL32(124, 199, 255, 255));
            if (k + 1 < s.keys.size())
                dl->AddLine(ImVec2(x + rad, cy), ImVec2(toX(s.keys[k + 1].t) - rad, cy), IM_COL32(60, 66, 78, 255), 1.0f);
            // Hit test
            if (hovered && std::fabs(mouse.x - x) <= rad + 2 && std::fabs(mouse.y - cy) <= rad + 2) {
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    selectedTrack_ = tracks[r];
                    selectedKey_ = static_cast<int>(k);
                    dragKey = static_cast<int>(k);
                    dragTrack = tracks[r];
                }
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    selectedTrack_ = tracks[r];
                    selectedKey_ = static_cast<int>(k);
                    ImGui::OpenPopup("##keymenu");
                }
                ImGui::SetTooltip("%s @ %.2fs\n%.3f %.3f %.3f %.3f", tracks[r].c_str(), s.keys[k].t, s.keys[k].v[0],
                                  s.keys[k].v[1], s.keys[k].v[2], s.keys[k].v[3]);
            }
        }
        // Double-click empty space: add a key there with the curve's current value.
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && mouse.y >= y && mouse.y < y + rowH &&
            mouse.x >= x0) {
            const double saved = time_;
            time_ = toT(mouse.x);
            addKeyframe(tracks[r], s);
            time_ = saved;
        }
    }

    // Scrub on the ruler, drag keys elsewhere.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && mouse.y < origin.y + rulerH && mouse.x >= x0)
        scrubbing = true;
    if (active && scrubbing) {
        time_ = toT(mouse.x);
        timeJumped_ = true;
    }
    if (active && dragKey >= 0 && !scrubbing && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
        auto& s = uniformValues_[dragTrack];
        if (dragKey < static_cast<int>(s.keys.size())) {
            s.keys[static_cast<size_t>(dragKey)].t = toT(mouse.x);
            const float t = s.keys[static_cast<size_t>(dragKey)].t;
            std::sort(s.keys.begin(), s.keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.t < b.t; });
            for (size_t i = 0; i < s.keys.size(); ++i)
                if (s.keys[i].t == t) dragKey = selectedKey_ = static_cast<int>(i);
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        dragKey = -1;
        scrubbing = false;
    }

    // Playhead
    const float px = toX(static_cast<float>(timelineTime()));
    dl->AddLine(ImVec2(px, origin.y), ImVec2(px, origin.y + height), colors::Accent, 2.0f);

    if (ImGui::BeginPopup("##keymenu")) {
        auto it = uniformValues_.find(selectedTrack_);
        if (it != uniformValues_.end() && selectedKey_ >= 0 && selectedKey_ < static_cast<int>(it->second.keys.size())) {
            auto& key = it->second.keys[static_cast<size_t>(selectedKey_)];
            for (int e = 0; e < 5; ++e)
                if (ImGui::MenuItem(kEaseNames[e], nullptr, key.ease == e)) key.ease = e;
            ImGui::Separator();
            if (ImGui::MenuItem("Delete key")) {
                it->second.keys.erase(it->second.keys.begin() + selectedKey_);
                selectedKey_ = -1;
            }
        }
        ImGui::EndPopup();
    }

    // ---- selected key editor
    auto it = uniformValues_.find(selectedTrack_);
    if (it != uniformValues_.end() && selectedKey_ >= 0 && selectedKey_ < static_cast<int>(it->second.keys.size())) {
        auto& s = it->second;
        auto& key = s.keys[static_cast<size_t>(selectedKey_)];
        ImGui::Separator();
        ImGui::Text("Key %d of %s", selectedKey_ + 1, selectedTrack_.c_str());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        if (ImGui::DragFloat("Time", &key.t, 0.01f, 0.0f, duration_, "%.2f s"))
            std::sort(s.keys.begin(), s.keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.t < b.t; });
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220);
        ImGui::DragFloat4("Value", key.v.data(), 0.01f);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        ImGui::Combo("Ease", &key.ease, kEaseNames, 5);
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete")) {
            s.keys.erase(s.keys.begin() + selectedKey_);
            selectedKey_ = -1;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop animating")) {
            s.drive = DriveManual;
            selectedKey_ = -1;
        }
    }
    ImGui::End();
}

void App::drawTransition() {
    if (!showTransition_) return;
    if (!ImGui::Begin("Transition", &showTransition_)) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("Blend From -> To with a 0..1 progress. Samplers named u_from / u_to (or gl-transitions "
                       "getFromColor/getToColor) use the slots below; other names use the Textures order.");
    auto& slots = textures_->slots();
    auto slotCombo = [&](const char* label, int& slot) {
        std::string preview = slot < static_cast<int>(slots.size()) ? std::to_string(slot) + ": " + slots[static_cast<size_t>(slot)].name
                                                                     : std::to_string(slot) + ": (empty)";
        ImGui::SetNextItemWidth(220);
        if (ImGui::BeginCombo(label, preview.c_str())) {
            for (int i = 0; i < static_cast<int>(slots.size()); ++i) {
                const std::string n = std::to_string(i) + ": " + slots[static_cast<size_t>(i)].name;
                if (ImGui::Selectable(n.c_str(), i == slot)) slot = i;
            }
            ImGui::EndCombo();
        }
    };
    slotCombo("From", transitionFrom_);
    slotCombo("To", transitionTo_);
    ImGui::SameLine();
    if (ImGui::SmallButton("Swap")) std::swap(transitionFrom_, transitionTo_);

    ImGui::Separator();
    auto it = uniformValues_.find("progress");
    if (it == uniformValues_.end())
        for (auto& [name, st] : uniformValues_)
            if (lower(name).find("progress") != std::string::npos) {
                it = uniformValues_.find(name);
                break;
            }
    if (it != uniformValues_.end()) {
        auto& s = it->second;
        float p = evalUniform(s, 0);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderFloat("##scrub", &p, 0.0f, 1.0f, (it->first + " %.3f").c_str())) {
            s.drive = DriveManual;
            s.f[0] = p;
        }
        if (ImGui::Button(s.drive == DriveLoop ? "Stop" : "Play loop")) s.drive = s.drive == DriveLoop ? DriveManual : DriveLoop;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::DragFloat("Transition length", &s.loopSeconds, 0.05f, 0.1f, 60.0f, "%.2f s");
        ImGui::SameLine();
        if (ImGui::Button("Keyframe 0 -> 1 over timeline")) {
            s.drive = DriveKeys;
            s.keys = {Keyframe{0.0f, {0, 0, 0, 0}, 3}, Keyframe{duration_, {1, 1, 1, 1}, 3}};
        }
    } else {
        ImGui::TextDisabled("No 'progress' uniform in the current shaders.");
        if (ImGui::Button("New from Transition template")) newProject(2);
    }
    ImGui::End();
}

void App::applyPreset(Preset p) {
    preset_ = p;
    const bool mobile = p != Preset::Desktop;
    mobileCheck_ = mobile;
    if (uvTopLeft_ != mobile) {
        uvTopLeft_ = mobile;
        textures_->setFlipAll(!mobile);
    }
    outputMode_ = mobile ? 3 : 0;  // 1080 x 1920 portrait
    if (p == Preset::iPhone) showCrossCompile_ = true;
    log(Level::Info, std::string("Preset: ") + (p == Preset::Desktop ? "Desktop" : p == Preset::Android ? "Android phone" : "iPhone") +
                         (mobile ? " · 1080x1920 · UV top-left · GLSL ES validation on" : ""));
    compileAll("preset");
}

}  // namespace app
