// Batch render: every shader file in a folder, one at a time, into <folder>/renders/.
// The user's passes, uniforms and timeline are set aside for the batch and put back afterwards
// (editors are moved, not rebuilt, so undo history survives).
#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
    #include <shellapi.h>
#endif

#include "app/App.h"

#include "app/Common.h"

#include <imgui.h>

#include <cstdlib>

namespace app {

namespace {

const char* statusName(App::BatchStatus s) {
    switch (s) {
        case App::BatchStatus::Queued: return "Queued";
        case App::BatchStatus::Compiling: return "Compiling";
        case App::BatchStatus::Rendering: return "Rendering";
        case App::BatchStatus::Finishing: return "Finishing";
        case App::BatchStatus::Done: return "Done";
        case App::BatchStatus::Failed: return "Failed";
        case App::BatchStatus::Skipped: return "Skipped";
        case App::BatchStatus::Exists: return "Exists";
        case App::BatchStatus::Cancelled: return "Cancelled";
    }
    return "";
}

bool isFinished(App::BatchStatus s) {
    return s == App::BatchStatus::Done || s == App::BatchStatus::Failed || s == App::BatchStatus::Skipped ||
           s == App::BatchStatus::Exists || s == App::BatchStatus::Cancelled;
}

void openFolder(const fs::path& dir) {
#if defined(_WIN32)
    ShellExecuteW(nullptr, L"open", dir.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    std::system(("open \"" + dir.string() + "\" &").c_str());
#else
    std::system(("xdg-open \"" + dir.string() + "\" &").c_str());
#endif
}

}  // namespace

bool App::loadBatchFolder(const fs::path& folder) {
    const bool quitWhenDone = batch_.quitWhenDone;
    batch_ = {};
    batch_.folder = folder;
    batch_.quitWhenDone = quitWhenDone;
    for (auto& s : exportplan::scanShaderFolder(folder)) batch_.items.push_back({s.source, s.outputStem});
    if (batch_.items.empty()) {
        log(Level::Warn, "No shader files (.frag .fs .glsl .txt) in " + folder.string());
        return false;
    }
    log(Level::Info, "Batch: " + std::to_string(batch_.items.size()) + " shader file(s) in " + folder.string());
    return true;
}

void App::startBatch(bool onlyFailed) {
    auto& b = batch_;
    if (b.active || exportBusy()) return;
    std::error_code ec;
    fs::create_directories(b.folder / "renders", ec);
    if (ec) {
        log(Level::Error, "Batch: could not create " + (b.folder / "renders").string() + ": " + ec.message());
        return;
    }
    bool any = false;
    for (auto& it : b.items) {
        if (onlyFailed) {
            if (it.status != BatchStatus::Failed) continue;
        } else if (!it.enabled) {
            it.status = BatchStatus::Skipped;
            it.message.clear();
            continue;
        }
        it.status = BatchStatus::Queued;
        it.message.clear();
        any = true;
    }
    if (!any) return;

    // Set the user's work aside; every job gets a fresh, lone Image pass.
    b.passes = std::move(passes_);
    b.uniforms = std::move(uniformValues_);
    b.defines = std::move(defineOverrides_);
    b.activePass = activePass_;
    b.duration = duration_;
    b.loop = loop_;
    b.output = export_.output;
    b.withAudio = export_.withAudio;
    passes_.clear();
    uniformValues_.clear();
    defineOverrides_.clear();
    duration_ = export_.start + export_.length;
    loop_ = false;
    b.current = -1;
    b.cancelRequested = false;
    b.active = true;
    log(Level::Info, "Batch started: " + b.folder.string());
}

void App::stepBatch() {
    auto& b = batch_;
    BatchItem* cur = b.current >= 0 ? &b.items[static_cast<size_t>(b.current)] : nullptr;
    if (exportBusy()) {
        if (cur) cur->status = export_.active ? BatchStatus::Rendering : BatchStatus::Finishing;
        return;
    }
    if (cur && (cur->status == BatchStatus::Rendering || cur->status == BatchStatus::Finishing)) {
        switch (export_.result) {
            case ExportResult::Ok: cur->status = BatchStatus::Done; break;
            case ExportResult::Cancelled: cur->status = BatchStatus::Cancelled; break;
            default:
                cur->status = BatchStatus::Failed;
                cur->message = export_.status;
                break;
        }
    }
    if (b.cancelRequested) {
        for (auto& it : b.items)
            if (it.status == BatchStatus::Queued) it.status = BatchStatus::Cancelled;
        endBatch();
        return;
    }

    // One job per call: the next queued file.
    int next = -1;
    for (size_t i = 0; i < b.items.size(); ++i)
        if (b.items[i].status == BatchStatus::Queued) {
            next = static_cast<int>(i);
            break;
        }
    if (next < 0) {
        endBatch();
        return;
    }
    b.current = next;
    BatchItem& it = b.items[static_cast<size_t>(next)];
    const fs::path out = exportplan::outputFor(b.folder / "renders", it.outputStem, export_.format);
    std::error_code ec;
    if (!b.overwrite && fs::exists(out, ec)) {
        it.status = BatchStatus::Exists;
        return;
    }
    std::string text;
    if (!readFile(it.source, text)) {
        it.status = BatchStatus::Failed;
        it.message = "Could not read the file";
        return;
    }

    it.status = BatchStatus::Compiling;
    passes_.clear();
    uniformValues_.clear();
    Pass& p = addPass("Image", text);
    p.file = it.source;  // #include resolves next to the shader
    selectTab_ = true;
    compilePass(p, "batch");
    if (!p.program) {
        it.status = BatchStatus::Failed;
        it.message = "Does not compile";
        for (const auto& d : p.diagnostics)
            if (d.isError) {
                it.message = (d.line > 0 ? "line " + std::to_string(d.line) + ": " : std::string()) + d.message;
                break;
            }
        return;
    }
    // Un-annotated "progress" uniforms loop 0..1 by default; make one loop span the whole render so
    // a transition plays exactly once.
    for (auto& [name, s] : uniformValues_)
        if (s.drive == DriveLoop) s.loopSeconds = export_.length;

    export_.output = out;
    export_.withAudio = false;  // don't mux the project's soundtrack into every file
    it.status = BatchStatus::Rendering;
    if (!startExport()) {
        it.status = BatchStatus::Failed;
        it.message = export_.status;
    }
}

void App::cancelBatch() {
    if (!batch_.active) return;
    batch_.cancelRequested = true;
    if (export_.active) finishExport(true);
}

void App::abortBatchNow() {
    if (!batch_.active) return;
    cancelBatch();
    if (export_.finishing.valid()) {
        export_.finishing.wait();
        pollExportFinish();
    }
    stepBatch();  // records the cancel and restores the user's project
}

void App::endBatch() {
    auto& b = batch_;
    passes_ = std::move(b.passes);
    uniformValues_ = std::move(b.uniforms);
    defineOverrides_ = std::move(b.defines);
    activePass_ = b.activePass;
    duration_ = b.duration;
    loop_ = b.loop;
    export_.output = b.output;
    export_.withAudio = b.withAudio;
    b.passes.clear();
    b.active = false;
    b.current = -1;
    timeJumped_ = true;
    selectTab_ = true;

    int done = 0, failed = 0, other = 0;
    for (const auto& it : b.items)
        if (it.status == BatchStatus::Done) ++done;
        else if (it.status == BatchStatus::Failed) ++failed;
        else ++other;
    log(failed ? Level::Warn : Level::Ok, "Batch finished: " + std::to_string(done) + " done, " + std::to_string(failed) +
                                              " failed, " + std::to_string(other) + " skipped/existing/cancelled");
    for (const auto& it : b.items)
        if (it.status == BatchStatus::Failed)
            log(Level::Error, "Batch: " + it.source.filename().string() + " failed: " + it.message);
    if (b.quitWhenDone) {
        exitCode_ = failed ? 1 : 0;
        quit_ = true;
    }
}

void App::drawBatch() {
    auto& b = batch_;
    int done = 0, failed = 0, finished = 0, todo = 0;
    for (const auto& it : b.items) {
        if (it.status == BatchStatus::Done) ++done;
        if (it.status == BatchStatus::Failed) ++failed;
        if (isFinished(it.status)) ++finished;
        if (it.enabled) ++todo;
    }
    static const char* kFormats[] = {"PNG frame", "PNG sequence", "MP4", "MOV", "GIF", "WebP"};
    ImGui::TextWrapped("Batch: %s", b.folder.string().c_str());
    ImGui::TextDisabled("Into renders/ as %s, %d x %d, %g fps, %.2f s (from the export settings)",
                        kFormats[export_.format], export_.width, export_.height, export_.fps, export_.length);

    if (b.active) {
        const float overall = b.items.empty() ? 0.0f : static_cast<float>(finished) / static_cast<float>(b.items.size());
        char label[64];
        std::snprintf(label, sizeof(label), "%d / %d", finished, static_cast<int>(b.items.size()));
        ImGui::ProgressBar(overall, ImVec2(-1, 0), label);
        if (b.current >= 0) {
            const auto& cur = b.items[static_cast<size_t>(b.current)];
            ImGui::Text("%s  -  %s", cur.source.filename().string().c_str(), statusName(cur.status));
            if (export_.active)
                ImGui::ProgressBar(export_.total ? static_cast<float>(export_.frame) / export_.total : 0.0f, ImVec2(-1, 0));
            else
                ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-1, 0), "");
        }
        ImGui::BeginDisabled(b.cancelRequested);
        if (ImGui::Button("Cancel batch")) cancelBatch();
        ImGui::EndDisabled();
    } else {
        ImGui::Checkbox("Overwrite existing renders", &b.overwrite);
        ImGui::BeginDisabled(todo == 0);
        pushAccentButton();
        if (ImGui::Button("Render all")) startBatch(false);
        popAccentButton();
        ImGui::EndDisabled();
        if (failed > 0) {
            ImGui::SameLine();
            if (ImGui::Button("Retry failed")) startBatch(true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Open renders folder")) openFolder(b.folder / "renders");
        ImGui::SameLine();
        if (ImGui::Button("Back")) b.items.clear();
        if (finished > 0)
            ImGui::Text("%d done, %d failed, %d skipped / existing / cancelled", done, failed, finished - done - failed);
    }

    if (ImGui::BeginTable("batch", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0, 260))) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24);
        ImGui::TableSetupColumn("File");
        ImGui::TableSetupColumn("Status");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < b.items.size(); ++i) {
            auto& it = b.items[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(b.active);
            ImGui::Checkbox("##on", &it.enabled);
            ImGui::EndDisabled();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(it.source.filename().string().c_str());
            ImGui::TableNextColumn();
            const ImU32 c = it.status == BatchStatus::Done ? colors::Accent
                          : it.status == BatchStatus::Failed ? colors::Error
                                                              : colors::Muted;
            ImGui::TextColored(col(c), "%s", statusName(it.status));
            if (!it.message.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", it.message.c_str());
            if (!it.message.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", it.message.c_str());
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

}  // namespace app
