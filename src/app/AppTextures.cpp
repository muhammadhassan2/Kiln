// Textures panel: images, video files, camera, audio file / microphone FFT, procedural.
#include "app/App.h"

#include "app/Common.h"

#include <imgui.h>

namespace app {

void App::drawTextures() {
    if (ImGui::Begin("Textures")) {
        auto& slots = textures_->slots();
        ImGui::TextDisabled("%zu textures · up to %d units · slot i = u_tex<i> / iChannel<i>", slots.size(),
                            device_->maxTextureUnits());

        int removeIndex = -1, moveFrom = -1, moveTo = -1;
        for (size_t i = 0; i < slots.size(); ++i) {
            auto& s = slots[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::Separator();

            const float thumb = 56.0f;
            if (s.texture) {
                const bool bottomUp = s.kind == textures::SourceKind::Noise || s.kind == textures::SourceKind::Checker ||
                                      s.kind == textures::SourceKind::AudioFile || s.kind == textures::SourceKind::Microphone ||
                                      s.flipY;
                ImGui::Image(texId(*s.texture), ImVec2(thumb, thumb), bottomUp ? ImVec2(0, 1) : ImVec2(0, 0),
                             bottomUp ? ImVec2(1, 0) : ImVec2(1, 1));
            }
            ImGui::SameLine();
            ImGui::BeginGroup();
            ImGui::TextUnformatted(s.name.c_str());
            ImGui::SameLine();
            ImGui::PushFont(fonts_.mono, 0.0f);
            ImGui::TextColored(col(colors::Glsl), "u_tex%zu", i);
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::TextDisabled("%s", textures::kindName(s.kind));
            if (s.texture) {
                if (s.video && s.kind == textures::SourceKind::Video)
                    ImGui::TextDisabled("%dx%d · %.2f fps · %.1f / %.1f s", s.texture->width(), s.texture->height(),
                                        s.video->fps(), s.video->position(), s.video->duration());
                else if (s.audio)
                    ImGui::TextDisabled("512x2 FFT + waveform · level %.2f · bass %.2f", s.audio->level(), s.audio->bass());
                else
                    ImGui::TextDisabled("%dx%d · RGBA8", s.texture->width(), s.texture->height());
            }
            if (!s.status.empty()) ImGui::TextColored(col(colors::Error), "%s", s.status.c_str());

            bool samplerChanged = false;
            int filter = static_cast<int>(s.sampler.filter);
            int wu = static_cast<int>(s.sampler.wrapU);
            int wv = static_cast<int>(s.sampler.wrapV);
            ImGui::SetNextItemWidth(92.0f);
            samplerChanged |= ImGui::Combo("##filter", &filter, kFilterNames, 3);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            samplerChanged |= ImGui::Combo("##wu", &wu, kWrapNames, 3);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            samplerChanged |= ImGui::Combo("##wv", &wv, kWrapNames, 3);
            if (samplerChanged) {
                s.sampler.filter = static_cast<rhi::Filter>(filter);
                s.sampler.wrapU = static_cast<rhi::Wrap>(wu);
                s.sampler.wrapV = static_cast<rhi::Wrap>(wv);
                textures_->applySampler(i);
            }

            const bool flippable = s.kind == textures::SourceKind::Image || s.kind == textures::SourceKind::Video ||
                                   s.kind == textures::SourceKind::Camera;
            if (flippable) {
                if (ImGui::Checkbox("Flip Y", &s.flipY)) textures_->reload(i);
                ImGui::SameLine();
            }
            if (s.kind == textures::SourceKind::Image) {
                if (ImGui::SmallButton("Reload")) {
                    if (auto err = textures_->reload(i); !err.empty()) log(Level::Error, err);
                }
                ImGui::SameLine();
            }
            if (s.kind == textures::SourceKind::AudioFile && s.audio) {
                if (ImGui::Checkbox("Hear it", &s.monitorAudio)) s.audio->setMonitor(s.monitorAudio, timelineTime(), playing_);
                ImGui::SetItemTooltip("Play the audio through ffplay while previewing");
                ImGui::SameLine();
            }
            if (ImGui::SmallButton("Up") && i > 0) {
                moveFrom = static_cast<int>(i);
                moveTo = static_cast<int>(i) - 1;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Down") && i + 1 < slots.size()) {
                moveFrom = static_cast<int>(i);
                moveTo = static_cast<int>(i) + 1;
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, col(colors::Error));
            if (ImGui::SmallButton("Remove")) removeIndex = static_cast<int>(i);
            ImGui::PopStyleColor();
            ImGui::EndGroup();
            ImGui::PopID();
        }
        if (removeIndex >= 0) textures_->remove(static_cast<size_t>(removeIndex));
        if (moveFrom >= 0) textures_->move(static_cast<size_t>(moveFrom), static_cast<size_t>(moveTo));

        ImGui::Separator();
        const bool full = static_cast<int>(slots.size()) >= device_->maxTextureUnits();
        ImGui::BeginDisabled(full);
        pushAccentButton();
        if (ImGui::Button("+ Add files...")) pendingDialog_ = Dialog::AddTextures;
        popAccentButton();
        ImGui::SetItemTooltip("Images, videos (mp4/mov/...) or audio (mp3/wav/...); several at once");
        ImGui::SameLine();
        if (ImGui::Button("+ Noise")) textures_->addNoise();
        ImGui::SameLine();
        if (ImGui::Button("+ Checker")) textures_->addChecker();

        ImGui::SetNextItemWidth(130);
        ImGui::InputText("##cam", cameraDevice_, sizeof(cameraDevice_));
        ImGui::SameLine();
        if (ImGui::Button("+ Camera")) {
            if (auto err = textures_->addCamera(cameraDevice_); !err.empty()) log(Level::Error, err);
            else log(Level::Ok, std::string("Camera ") + cameraDevice_ + " -> u_tex" + std::to_string(slots.size() - 1));
        }
        ImGui::SetItemTooltip("ffmpeg device: macOS avfoundation index (0), Linux /dev/video0, Windows dshow name");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110);
        ImGui::InputText("##mic", micDevice_, sizeof(micDevice_));
        ImGui::SameLine();
        if (ImGui::Button("+ Microphone")) {
            if (auto err = textures_->addMicrophone(micDevice_); !err.empty()) log(Level::Error, err);
            else log(Level::Ok, std::string("Microphone ") + micDevice_ + " -> u_tex" + std::to_string(slots.size() - 1));
        }
        ImGui::SetItemTooltip("ffmpeg device: macOS avfoundation ':0', Linux pulse 'default', Windows dshow name");
        ImGui::EndDisabled();
        ImGui::TextDisabled("Drop files anywhere on the window. Video / camera / audio need ffmpeg.");
    }
    ImGui::End();
}

}  // namespace app
