// Export, performance, history, mobile/Metal cross-compile, library, AI assistant, output window.
#include "app/App.h"

#include "app/Common.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <stb_image.h>
#include <stb_image_write.h>

#include <chrono>
#include <future>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace app {

using json = nlohmann::json;

namespace {

fs::path documentsDir() {
#if defined(_WIN32)
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    return fs::path(home ? home : ".") / "Documents";
}

fs::path libraryDir() { return documentsDir() / "ShaderStudio" / "Library"; }

std::string sanitize(std::string s) {
    for (auto& c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == ' ')) c = '_';
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s.empty() ? "untitled" : s;
}

}  // namespace

// ====================================================================== export

void App::drawExport() {
    if (!showExport_) return;
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Export", &showExport_, ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }
    auto& e = export_;
    if (batch_.active || !batch_.items.empty()) {
        drawBatch();
        ImGui::End();
        return;
    }
    if (e.active) {
        ImGui::Text("Rendering frame %d / %d", e.frame, e.total);
        ImGui::ProgressBar(e.total ? static_cast<float>(e.frame) / e.total : 0.0f, ImVec2(-1, 0));
        if (ImGui::Button("Cancel")) finishExport(true);
        ImGui::End();
        return;
    }
    if (e.finishing.valid()) {
        ImGui::Text("Finishing... ffmpeg is writing %s", e.output.filename().string().c_str());
        ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-1, 0), "");
        ImGui::End();
        return;
    }
    ensureExportOutput();

    static const char* kFormats[] = {"PNG (current frame)", "PNG sequence", "MP4 (H.264)", "MOV (ProRes 422 HQ)",
                                     "GIF (animated)", "WebP (animated)"};
    ImGui::SetNextItemWidth(220);
    if (ImGui::Combo("Format", &e.format, kFormats, 6)) e.output = exportplan::withFormat(e.output, e.format);

    static const char* kSizes[] = {"Custom", "Current output", "1080 x 1920", "1920 x 1080", "1080 x 1080", "3840 x 2160"};
    ImGui::SetNextItemWidth(220);
    if (ImGui::Combo("Size", &e.sizePreset, kSizes, 6)) {
        int w = e.width, h = e.height;
        switch (e.sizePreset) {
            case 1: outputSize(w, h); break;
            case 2: w = 1080; h = 1920; break;
            case 3: w = 1920; h = 1080; break;
            case 4: w = 1080; h = 1080; break;
            case 5: w = 3840; h = 2160; break;
            default: break;
        }
        e.width = w;
        e.height = h;
    }
    ImGui::SetNextItemWidth(220);
    if (ImGui::InputInt2("Width x Height", &e.width)) e.sizePreset = 0;
    e.width = std::clamp(e.width, 16, 8192);
    e.height = std::clamp(e.height, 16, 8192);
    if (e.format != 0) {
        ImGui::SetNextItemWidth(220);
        ImGui::DragFloat("FPS", &e.fps, 0.1f, 1.0f, 240.0f, "%.2f");
        ImGui::SetNextItemWidth(220);
        ImGui::DragFloat("Length", &e.length, 0.05f, 0.04f, 3600.0f, "%.2f s");
    }
    ImGui::SetNextItemWidth(220);
    ImGui::DragFloat(e.format == 0 ? "At time" : "Start", &e.start, 0.05f, 0.0f, 3600.0f, "%.2f s");
    if (e.format == 2 || e.format == 3) ImGui::Checkbox("Include audio (first audio texture)", &e.withAudio);
    if (e.format == 5) {
        ImGui::SetNextItemWidth(220);
        ImGui::SliderInt("Quality", &e.webpQuality, 0, 100);
        ImGui::SameLine();
        ImGui::Checkbox("Lossless", &e.webpLossless);
    }

    // Suggestions only: the user's numbers are never changed without the button.
    if (auto r = exportplan::recommendFor(e.format, e.width, e.height, e.fps)) {
        ImGui::TextDisabled("Recommended for %s: %g fps, %d x %d", kFormats[e.format], r->fps, r->width, r->height);
        ImGui::SameLine();
        if (ImGui::SmallButton("Use recommended")) {
            e.fps = r->fps;
            e.width = r->width;
            e.height = r->height;
            e.sizePreset = 0;
        }
    }
    if (e.format == 4 && exportplan::gifTooHeavy(e.width, e.height, e.fps, e.length))
        ImGui::TextColored(col(colors::Metal), "This GIF needs about %.1f GB of memory while encoding; lower the size, fps or length.",
                           exportplan::gifMemoryGB(e.width, e.height, e.fps, e.length));

    ImGui::Separator();
    ImGui::TextWrapped("%s", e.output.string().c_str());
    if (ImGui::Button(e.format == 1 ? "Choose folder..." : "Choose file...")) pendingDialog_ = Dialog::ExportPath;
    ImGui::SameLine();
    pushAccentButton();
    if (ImGui::Button("Render")) requestRender();
    popAccentButton();
    ImGui::SameLine();
    if (ImGui::Button("Batch...")) pendingDialog_ = Dialog::BatchFolder;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Render every shader file in a folder into its renders/ subfolder");
    drawFfmpegSetup(e.format);
    if (!e.status.empty()) ImGui::TextWrapped("%s", e.status.c_str());

    if (askOverwrite_) {
        ImGui::OpenPopup("Already exists");
        askOverwrite_ = false;
    }
    if (ImGui::BeginPopupModal("Already exists", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s already exists.", e.output.filename().string().c_str());
        if (ImGui::Button("Overwrite")) {
            ImGui::CloseCurrentPopup();
            if (startExport()) saveSettings();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep both")) {
            ImGui::CloseCurrentPopup();
            e.output = exportplan::keepBoth(e.output);
            if (startExport()) saveSettings();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::End();
}

std::string App::exportStem() const {
    if (!projectPath_.empty()) return projectPath_.stem().string();
    for (const auto& p : passes_)
        if (p->isImage && !p->file.empty()) return p->file.stem().string();
    return "untitled";
}

void App::ensureExportOutput() {
    if (!export_.output.empty()) return;
    const fs::path projectDir = projectPath_.empty() ? fs::path() : projectPath_.parent_path();
    export_.output = exportplan::outputFor(exportplan::defaultFolder(exportFolder_, projectDir, documentsDir()),
                                           exportStem(), export_.format);
}

void App::requestRender() {
    exportFolder_ = export_.output.parent_path();
    std::error_code ec;
    if (fs::exists(export_.output, ec)) {
        askOverwrite_ = true;
        return;
    }
    if (startExport()) saveSettings();
}

bool App::startExport() {
    auto& e = export_;
    e.status.clear();
    e.result = ExportResult::None;
    const auto fail = [&](std::string why) {
        e.status = std::move(why);
        e.result = ExportResult::Failed;
        log(Level::Error, "Export: " + e.status);
        return false;
    };
    if (e.format == 2) {  // H.264 needs even dimensions
        e.width &= ~1;
        e.height &= ~1;
    }
    e.total = e.format == 0 ? 1 : std::max(1, static_cast<int>(std::lround(e.length * e.fps)));
    e.frame = 0;
    // Render into "<output>.part" and rename only when complete, so a cancel or crash never leaves a
    // broken file under the real name or destroys an older good render.
    e.part = exportplan::partPath(e.output);
    exportplan::discardPart(e.part);  // leftover from an earlier crash
    std::error_code ec;
    fs::create_directories(e.output.parent_path(), ec);
    if (e.format == 1) fs::create_directories(e.part, ec);
    if (e.format >= 2) {
        const std::string ffmpeg = media::findTool("ffmpeg");
        if (ffmpeg.empty()) return fail("ffmpeg not found.");
        if (e.format == 5 && webpEncoder(ffmpeg).empty())
            return fail("This ffmpeg has no WebP encoder; use Install ffmpeg in the Export window.");
        char size[32], rate[32];
        std::snprintf(size, sizeof(size), "%dx%d", e.width, e.height);
        std::snprintf(rate, sizeof(rate), "%.4f", e.fps);
        std::vector<std::string> args = {ffmpeg, "-y", "-hide_banner", "-loglevel", "error", "-f", "rawvideo",
                                         "-pix_fmt", "rgba", "-s", size, "-r", rate, "-i", "-"};
        fs::path audio;
        if (e.withAudio && (e.format == 2 || e.format == 3))
            for (auto& s : textures_->slots())
                if (s.kind == textures::SourceKind::AudioFile) {
                    audio = s.path;
                    break;
                }
        if (!audio.empty())
            args.insert(args.end(), {"-ss", std::to_string(e.start), "-t", std::to_string(e.length), "-i", audio.string(),
                                     "-map", "0:v", "-map", "1:a"});
        // GL readback is bottom-up. GIF: build one 256-colour palette from the whole clip, then map every
        // frame to it (far better than ffmpeg's default fixed palette).
        args.insert(args.end(), {"-vf", e.format == 4 ? "vflip,split[a][b];[a]palettegen[p];[b][p]paletteuse" : "vflip"});
        if (e.format == 2)
            args.insert(args.end(), {"-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "18", "-preset", "medium",
                                     "-movflags", "+faststart"});
        else if (e.format == 3)
            args.insert(args.end(), {"-c:v", "prores_ks", "-profile:v", "3", "-pix_fmt", "yuv422p10le"});
        else if (e.format == 4)
            args.insert(args.end(), {"-c:v", "gif", "-loop", "0"});
        else  // the WebP muxer plays once by default; -loop 0 repeats forever like GIF
            args.insert(args.end(), {"-c:v", webpEncoder(ffmpeg), "-pix_fmt", e.webpLossless ? "bgra" : "yuva420p",
                                     "-lossless", e.webpLossless ? "1" : "0", "-quality", std::to_string(e.webpQuality),
                                     "-compression_level", "4", "-loop", "0"});
        if (!audio.empty()) args.insert(args.end(), {"-c:a", e.format == 2 ? "aac" : "pcm_s16le"});
        // Cap the output length instead of -shortest: ffmpeg must keep reading stdin until we close it.
        // The muxer is named explicitly because the ".part" name hides the extension.
        static const char* kMuxers[] = {"", "", "mp4", "mov", "gif", "webp"};
        args.insert(args.end(), {"-t", std::to_string(e.length), "-f", kMuxers[e.format], e.part.string()});
        e.encoder = std::make_unique<media::Process>();
        if (!e.encoder->start(args, media::Process::Pipe::WriteStdin, false,
                              (media::tempDir() / "export.log").string())) {
            e.encoder.reset();
            return fail("Could not start ffmpeg.");
        }
    }
    e.savedTime = time_;
    e.savedFrame = frame_;
    e.savedPlaying = playing_;
    playing_ = false;
    e.active = true;
    // Deterministic start: clear all buffers at the export size.
    ensureTargets(e.width, e.height);
    for (auto& p : passes_)
        for (auto& t : p->targets) device_->clear(*t);
    textures_->beginExport(e.start, e.fps);
    log(Level::Info, "Export started: " + e.output.string());
    return true;
}

void App::stepExport() {
    auto& e = export_;
    const auto stop = [&](std::string why) {
        e.status = std::move(why);
        e.result = ExportResult::Failed;
        finishExport(true);
    };
    const auto begin = std::chrono::steady_clock::now();
    while (e.active && e.frame < e.total) {
        time_ = e.start + e.frame / static_cast<double>(e.fps);
        timeDelta_ = 1.0 / e.fps;
        frame_ = e.frame;
        textures_->exportFrame(timelineTime());
        renderFrame();
        Pass& img = imagePass();
        if (!img.program) return stop("The Image pass has no compiled shader.");
        device_->readRGBA8(*img.targets[img.latest], e.pixels);
        const int w = img.targets[img.latest]->width(), h = img.targets[img.latest]->height();
        if (e.format <= 1) {
            fs::path out = e.part;
            if (e.format == 1) {
                char name[32];
                std::snprintf(name, sizeof(name), "frame_%05d.png", e.frame);
                out = e.part / name;
            }
            stbi_flip_vertically_on_write(1);
            if (!stbi_write_png(out.string().c_str(), w, h, 4, e.pixels.data(), w * 4))
                return stop("Could not write " + out.string());
        } else if (!e.encoder->write(e.pixels.data(), e.pixels.size())) {
            std::string tail;
            readFile(media::tempDir() / "export.log", tail);
            return stop("ffmpeg stopped: " + tail.substr(tail.size() > 400 ? tail.size() - 400 : 0));
        }
        ++e.frame;
        if (std::chrono::steady_clock::now() - begin > std::chrono::milliseconds(30)) break;
    }
    if (e.active && e.frame >= e.total) finishExport(false);
}

void App::finishExport(bool cancelled) {
    auto& e = export_;
    textures_->endExport();
    time_ = e.savedTime;
    frame_ = e.savedFrame;
    playing_ = e.savedPlaying;
    timeJumped_ = true;
    e.active = false;
    if (e.encoder) {
        // ffmpeg may need seconds after the last frame (GIF palette, MP4 index); wait off the UI thread.
        e.encoder->closePipe();
        e.finishingCancelled = cancelled;
        e.finishing = std::async(std::launch::async, [p = e.encoder.get()] { return p->wait(); });
        return;
    }
    completeExport(cancelled, 0);
}

void App::pollExportFinish() {
    auto& e = export_;
    if (!e.finishing.valid() || e.finishing.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    const int rc = e.finishing.get();
    e.encoder.reset();
    completeExport(e.finishingCancelled, rc);
}

void App::completeExport(bool cancelled, int rc) {
    auto& e = export_;
    std::string error;
    if (cancelled) {
        exportplan::discardPart(e.part);
        if (e.status.empty()) e.status = "Cancelled.";
        if (e.result == ExportResult::None) e.result = ExportResult::Cancelled;
        log(Level::Warn, "Export stopped: " + e.status);
    } else if (rc != 0) {
        exportplan::discardPart(e.part);
        std::string tail;
        readFile(media::tempDir() / "export.log", tail);
        e.status = "ffmpeg failed: " + tail.substr(tail.size() > 400 ? tail.size() - 400 : 0);
        e.result = ExportResult::Failed;
        log(Level::Error, e.status);
    } else if (!exportplan::commitPart(e.part, e.output, error)) {
        exportplan::discardPart(e.part);
        e.status = error;
        e.result = ExportResult::Failed;
        log(Level::Error, "Export: " + error);
    } else {
        e.status = "Done: " + e.output.string();
        e.result = ExportResult::Ok;
        log(Level::Ok, "Exported " + std::to_string(e.total) + " frame(s) to " + e.output.string());
    }
}

// ====================================================================== performance

void App::drawProfiler() {
    if (!showProfiler_) return;
    if (!ImGui::Begin("Performance", &showProfiler_)) {
        ImGui::End();
        return;
    }
    std::vector<float> cpu(cpuHistory_.begin(), cpuHistory_.end()), gpu(gpuHistory_.begin(), gpuHistory_.end());
    const float lastGpu = gpu.empty() ? 0.0f : gpu.back();
    char overlay[64];
    std::snprintf(overlay, sizeof(overlay), "frame %.2f ms (%.0f fps)", cpu.empty() ? 0.0f : cpu.back(), ImGui::GetIO().Framerate);
    ImGui::PlotLines("##cpu", cpu.data(), static_cast<int>(cpu.size()), 0, overlay, 0.0f, 40.0f, ImVec2(-1, 50));
    std::snprintf(overlay, sizeof(overlay), "GPU %.3f ms (all passes)", lastGpu);
    ImGui::PlotLines("##gpu", gpu.data(), static_cast<int>(gpu.size()), 0, overlay, 0.0f, std::max(2.0f, lastGpu * 3.0f), ImVec2(-1, 50));

    if (ImGui::BeginTable("##passes", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Pass");
        ImGui::TableSetupColumn("GPU ms");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("Format");
        ImGui::TableSetupColumn("Compile");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < passes_.size(); ++i) {
            const Pass& p = *passes_[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(p.name.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", device_->timerMs(static_cast<int>(i)));
            ImGui::TableNextColumn();
            if (p.targets[0]) ImGui::Text("%dx%d", p.targets[0]->width(), p.targets[0]->height());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(p.isImage ? "RGBA8" : "RGBA16F x2");
            ImGui::TableNextColumn();
            ImGui::Text("%.1f ms", p.compileMs);
        }
        ImGui::EndTable();
    }
    int w, h;
    outputSize(w, h);
    const double mpix = w * static_cast<double>(h) / 1.0e6;
    ImGui::TextDisabled("%.2f Mpixel per pass · GPU time per Mpixel %.3f ms", mpix, mpix > 0 ? lastGpu / mpix : 0.0);
    ImGui::TextWrapped("Phone GPUs are much slower than desktop ones: keep GPU time a small fraction of the "
                       "16.6 ms frame budget here, then confirm on a real device.");
    ImGui::End();
}

// ====================================================================== history

void App::drawHistory() {
    if (!showHistory_) return;
    if (!ImGui::Begin("History", &showHistory_)) {
        ImGui::End();
        return;
    }
    static bool diffInit = false;
    if (!diffInit) {
        diff_.SetLanguage(TextEditor::Language::Glsl());
        diff_.SetSideBySideMode(true);
        diff_.SetShowWhitespacesEnabled(false);
        diffInit = true;
    }
    if (historyPass_ < 0 || historyPass_ >= static_cast<int>(passes_.size())) historyPass_ = static_cast<int>(activePass_);
    Pass& p = *passes_[static_cast<size_t>(historyPass_)];
    ImGui::SetNextItemWidth(120);
    if (ImGui::BeginCombo("##histpass", p.name.c_str())) {
        for (size_t i = 0; i < passes_.size(); ++i)
            if (ImGui::Selectable(passes_[i]->name.c_str(), static_cast<int>(i) == historyPass_)) {
                historyPass_ = static_cast<int>(i);
                historySelected_ = -1;
                diffDirty_ = true;
            }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu snapshots (one per successful compile)", p.history.size());

    ImGui::BeginChild("##snaps", ImVec2(200, 0), ImGuiChildFlags_Borders);
    for (int i = static_cast<int>(p.history.size()) - 1; i >= 0; --i) {
        const auto& snap = p.history[static_cast<size_t>(i)];
        const long lines = std::count(snap.source.begin(), snap.source.end(), '\n');
        char label[64];
        std::snprintf(label, sizeof(label), "#%d  %s  (%ld lines)", i + 1, snap.time.c_str(), lines);
        if (ImGui::Selectable(label, historySelected_ == i)) {
            historySelected_ = i;
            diffDirty_ = true;
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginGroup();
    if (historySelected_ >= 0 && historySelected_ < static_cast<int>(p.history.size())) {
        const auto& snap = p.history[static_cast<size_t>(historySelected_)];
        if (ImGui::Button("Restore this version")) {
            p.editor->SelectAll();
            p.editor->ReplaceTextInCurrentCursor(snap.source);  // undoable
            compilePass(p, "restored from history");
            diffDirty_ = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("left: snapshot #%d · right: current", historySelected_ + 1);
        if (diffDirty_) {
            diff_.SetText(snap.source, p.editor->GetText());
            diffDirty_ = false;
        }
        ImGui::PushFont(fonts_.mono, 0.0f);
        diff_.Render("##diff");
        ImGui::PopFont();
    } else {
        ImGui::TextDisabled("Select a snapshot to compare it with the current code.");
    }
    ImGui::EndGroup();
    ImGui::End();
}

// ====================================================================== mobile / metal

void App::drawCrossCompile() {
    if (!showCrossCompile_) return;
    if (!ImGui::Begin("Mobile / Metal", &showCrossCompile_)) {
        ImGui::End();
        return;
    }
    Pass& p = activePass();
    ImGui::Text("Pass: %s", p.name.c_str());
    ImGui::SeparatorText("Mobile GPU check (GLSL ES)");
    if (ImGui::Checkbox("Validate as GLSL ES on every compile", &mobileCheck_)) compileAll("mobile check toggled");
    if (mobileCheck_) {
        if (!p.esCheck.applicable) {
            ImGui::TextDisabled("Not an ES shader (desktop #version 330+ or Shadertoy) - nothing to check.");
        } else if (p.esCheck.diagnostics.empty()) {
            ImGui::TextColored(col(colors::Accent), "Passes %s validation.", p.esCheck.profile.c_str());
        } else {
            for (const auto& d : p.esCheck.diagnostics)
                ImGui::TextColored(col(d.isError ? colors::Error : colors::Metal), "line %d: %s", d.line, d.message.c_str());
        }
    }

    ImGui::SeparatorText("Metal Shading Language (iOS / macOS)");
    ImGui::RadioButton("iOS", &mslIos_, 1);
    ImGui::SameLine();
    ImGui::RadioButton("macOS", &mslIos_, 0);
    ImGui::SameLine();
    pushAccentButton();
    const bool generate = ImGui::Button("Convert to MSL");
    popAccentButton();
    if (generate) {
        shader::TranslatedSource tr;
        tr.source = p.translated;
        const auto r = shader::toMsl(tr, mslIos_ != 0);
        mslOk_ = r.ok;
        mslLog_ = r.log;
        mslPassName_ = p.name;
        metalCheckLog_.clear();
        if (!mslView_) {
            mslView_ = std::make_unique<TextEditor>();
            mslView_->SetLanguage(TextEditor::Language::Cpp());
            mslView_->SetReadOnlyEnabled(true);
            mslView_->SetShowWhitespacesEnabled(false);
        }
        mslView_->SetText(r.ok ? r.msl : std::string("// conversion failed\n"));
        log(r.ok ? Level::Ok : Level::Error, p.name + (r.ok ? " converted to MSL" : " MSL conversion failed: " + r.log));
    }
    if (mslView_) {
        ImGui::SameLine();
        if (ImGui::Button("Copy")) ImGui::SetClipboardText(mslView_->GetText().c_str());
        ImGui::SameLine();
#if defined(__APPLE__)
        if (ImGui::Button("Validate with Xcode")) {
            const bool ok = shader::validateMetal(mslView_->GetText(), mslIos_ != 0, metalCheckLog_);
            if (ok && metalCheckLog_.empty()) metalCheckLog_ = "Metal compiler: OK";
            log(ok ? Level::Ok : Level::Error, "xcrun metal: " + (ok ? std::string("OK") : metalCheckLog_));
        }
#else
        ImGui::BeginDisabled();
        ImGui::Button("Validate with Xcode");
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Needs macOS with Xcode");
#endif
        ImGui::TextDisabled("Generated from %s via glslang -> SPIR-V -> SPIRV-Cross", mslPassName_.c_str());
        if (!mslOk_) ImGui::TextColored(col(colors::Error), "%s", mslLog_.c_str());
        if (!metalCheckLog_.empty()) ImGui::TextWrapped("%s", metalCheckLog_.c_str());
        ImGui::PushFont(fonts_.mono, 0.0f);
        mslView_->Render("##msl");
        ImGui::PopFont();
    } else {
        ImGui::TextWrapped("Converts the current pass to Metal so you can check the iOS version of the effect: "
                           "uniforms become a 'Uniforms' struct at [[buffer(0)]], textures map to [[texture(i)]].");
    }
    ImGui::End();
}

// ====================================================================== library

void App::refreshLibrary() {
    library_.clear();
    const fs::path dir = libraryDir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        const auto ext = lower(entry.path().extension().string());
        if (ext != ".ssproj" && ext != ".frag" && ext != ".glsl") continue;
        LibraryItem item;
        item.name = entry.path().stem().string();
        item.path = entry.path();
        item.thumbPath = entry.path().parent_path() / (item.name + ".png");
        library_.push_back(std::move(item));
    }
    std::sort(library_.begin(), library_.end(), [](const auto& a, const auto& b) { return lower(a.name) < lower(b.name); });
    libraryDirty_ = false;
}

void App::writeThumbnail(const fs::path& png) {
    Pass& img = imagePass();
    if (!img.targets[img.latest]) return;
    auto& rt = *img.targets[img.latest];
    std::vector<std::uint8_t> px;
    device_->readRGBA8(rt, px);
    const int w = rt.width(), h = rt.height();
    const int tw = 256, th = std::max(1, static_cast<int>(256.0 * h / std::max(1, w)));
    std::vector<std::uint8_t> small(static_cast<size_t>(tw * th * 4));
    for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x) {
            const int sx = x * w / tw, sy = (th - 1 - y) * h / th;  // flip to top-down
            std::copy_n(&px[static_cast<size_t>((sy * w + sx) * 4)], 4, &small[static_cast<size_t>((y * tw + x) * 4)]);
        }
    stbi_flip_vertically_on_write(0);
    stbi_write_png(png.string().c_str(), tw, th, 4, small.data(), tw * 4);
}

void App::saveToLibrary(const std::string& rawName) {
    const std::string name = sanitize(rawName);
    const fs::path dir = libraryDir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    json j = projectToJson(dir);
    j["session"] = "";
    if (!writeFile(dir / (name + ".ssproj"), j.dump(2))) {
        log(Level::Error, "Could not save to library: " + dir.string());
        return;
    }
    writeThumbnail(dir / (name + ".png"));
    libraryDirty_ = true;
    log(Level::Ok, "Saved \"" + name + "\" to the library (" + dir.string() + ")");
}

void App::drawLibrary() {
    if (!showLibrary_) return;
    if (!ImGui::Begin("Library", &showLibrary_)) {
        ImGui::End();
        return;
    }
    if (libraryDirty_) refreshLibrary();
    ImGui::SetNextItemWidth(160);
    ImGui::InputTextWithHint("##libname", "name", libraryName_, sizeof(libraryName_));
    ImGui::SameLine();
    pushAccentButton();
    if (ImGui::Button("Save current")) saveToLibrary(libraryName_[0] ? libraryName_ : "untitled");
    popAccentButton();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::InputTextWithHint("##libfilter", "search", libraryFilter_, sizeof(libraryFilter_));
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) libraryDirty_ = true;
    ImGui::TextDisabled("%s", libraryDir().string().c_str());

    const float cell = 132.0f;
    const int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (cell + 8)));
    int n = 0;
    std::string toDelete;
    const std::string filter = lower(libraryFilter_);
    for (auto& item : library_) {
        if (!filter.empty() && lower(item.name).find(filter) == std::string::npos) continue;
        if (!item.thumbLoaded) {
            item.thumbLoaded = true;
            int w, h, c;
            stbi_set_flip_vertically_on_load(0);
            if (stbi_uc* px = stbi_load(item.thumbPath.string().c_str(), &w, &h, &c, 4)) {
                item.thumb = device_->createTexture(w, h, rhi::Format::RGBA8, px);
                item.thumb->setSampler({rhi::Filter::Linear, rhi::Wrap::Clamp, rhi::Wrap::Clamp});
                stbi_image_free(px);
            }
        }
        if (n % columns != 0) ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushID(item.name.c_str());
        bool clicked;
        if (item.thumb) clicked = ImGui::ImageButton("##t", texId(*item.thumb), ImVec2(cell, cell * 0.75f));
        else clicked = ImGui::Button(item.name.c_str(), ImVec2(cell + 8, cell * 0.75f + 8));
        if (clicked) {
            if (item.path.extension() == ".ssproj") {
                openProject(item.path);
                projectPath_.clear();  // edit a copy; Save As to keep changes
                updateTitle();
            } else {
                newProject(0);
                std::string text;
                readFile(item.path, text);
                imagePass().editor->SetText(text);
                compileAll("library");
            }
        }
        if (ImGui::BeginPopupContextItem("##ctx")) {
            if (ImGui::MenuItem("Delete")) toDelete = item.name;
            ImGui::EndPopup();
        }
        ImGui::TextUnformatted(item.name.c_str());
        ImGui::PopID();
        ImGui::EndGroup();
        ++n;
    }
    if (library_.empty()) ImGui::TextDisabled("Empty. Save the current project with a name above.");
    if (!toDelete.empty()) {
        std::error_code ec;
        for (const char* ext : {".ssproj", ".frag", ".glsl", ".png"}) fs::remove(libraryDir() / (toDelete + ext), ec);
        libraryDirty_ = true;
    }
    ImGui::End();
}

// ====================================================================== AI

void App::drawAI() {
    if (!showAI_) return;
    if (!ImGui::Begin("AI Assistant", &showAI_)) {
        ImGui::End();
        return;
    }
    if (!assistant_.hasKey()) {
        ImGui::TextWrapped("Uses the Claude API. Set ANTHROPIC_API_KEY before launching, or paste a key:");
        ImGui::SetNextItemWidth(260);
        ImGui::InputText("##key", aiKeyBuf_, sizeof(aiKeyBuf_), ImGuiInputTextFlags_Password);
        ImGui::SameLine();
        if (ImGui::Button("Use key") && aiKeyBuf_[0]) {
            assistant_.setApiKey(aiKeyBuf_);
            std::fill(std::begin(aiKeyBuf_), std::end(aiKeyBuf_), '\0');
            saveSettings();
            assistant_.refreshModels();
        }
        ImGui::Checkbox("Remember on this computer (stored in plain text in the settings file)", &aiRememberKey_);
        ImGui::End();
        return;
    }

    // Model picker (listed from the API so it never goes stale).
    ImGui::SetNextItemWidth(240);
    if (ImGui::BeginCombo("Model", assistant_.model.empty() ? "(refresh to load)" : assistant_.model.c_str())) {
        for (const auto& m : assistant_.models)
            if (ImGui::Selectable(m.c_str(), m == assistant_.model)) {
                assistant_.model = m;
                saveSettings();
            }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) assistant_.refreshModels();
    ImGui::SameLine();
    if (ImGui::SmallButton("Forget key")) {
        assistant_.setApiKey("");
        aiRememberKey_ = false;
        saveSettings();
    }

    ImGui::RadioButton("Modify current pass", &aiMode_, 0);
    ImGui::SameLine();
    ImGui::RadioButton("New shader", &aiMode_, 1);
    ImGui::SameLine();
    ImGui::RadioButton("Explain / fix errors", &aiMode_, 2);
    ImGui::InputTextMultiline("##prompt", aiPrompt_, sizeof(aiPrompt_), ImVec2(-1, 70));

    ImGui::BeginDisabled(assistant_.busy());
    pushAccentButton();
    if (ImGui::Button(assistant_.busy() ? "Thinking..." : "Send")) {
        Pass& p = activePass();
        std::string errors;
        for (const auto& d : p.diagnostics) errors += "line " + std::to_string(d.line) + ": " + d.message + "\n";
        std::string msg;
        if (aiMode_ == 1) {
            msg = std::string("Write a new shader for the ") + p.name + " pass: " + aiPrompt_;
        } else {
            msg = "Current " + p.name + " pass (" + p.dialect + "):\n```glsl\n" + p.editor->GetText() + "\n```\n";
            if (!errors.empty()) msg += "Compiler output:\n" + errors;
            msg += aiMode_ == 2 ? std::string("Explain the problem briefly and return a fixed version. ") + aiPrompt_
                                : std::string("Change request: ") + aiPrompt_;
        }
        assistant_.send(ai::systemPrompt(), msg);
    }
    popAccentButton();
    ImGui::EndDisabled();

    const std::string code = ai::extractCode(assistant_.reply);
    if (!code.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Apply code to current pass")) {
            Pass& p = activePass();
            p.editor->SelectAll();
            p.editor->ReplaceTextInCurrentCursor(code);  // undoable; History keeps the old version too
            compilePass(p, "AI");
        }
    }
    if (!assistant_.error.empty()) ImGui::TextColored(col(colors::Error), "%s", assistant_.error.c_str());
    ImGui::BeginChild("##reply", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::TextWrapped("%s", assistant_.reply.c_str());
    ImGui::EndChild();
    ImGui::End();
}

// ====================================================================== output window

void App::drawOutputWindowMenu() {
    if (ImGui::BeginMenu("Output window")) {
        if (ImGui::MenuItem("Windowed", nullptr, outputWindow_.isOpen() && outputWindow_.monitor() < 0))
            if (!outputWindow_.open(window_, -1)) log(Level::Error, "Could not open the output window");
        const auto monitors = OutputWindow::monitorNames();
        for (size_t i = 0; i < monitors.size(); ++i) {
            const std::string label = "Fullscreen on " + monitors[i];
            if (ImGui::MenuItem(label.c_str(), nullptr, outputWindow_.isOpen() && outputWindow_.monitor() == static_cast<int>(i)))
                if (!outputWindow_.open(window_, static_cast<int>(i))) log(Level::Error, "Could not open the output window");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Close", nullptr, false, outputWindow_.isOpen())) outputWindow_.close();
        ImGui::EndMenu();
    }
}

}  // namespace app
