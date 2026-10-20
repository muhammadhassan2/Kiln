// Runtime tool setup: pick the best ffmpeg on this machine and, on request, install one.
#include "app/App.h"

#include "app/Common.h"

#include <imgui.h>

#include <map>

namespace app {

const std::string& webpEncoder(const std::string& ffmpeg) {
    // Asking ffmpeg costs a process launch, so remember the answer per executable.
    static std::map<std::string, std::string> cache;
    auto it = cache.find(ffmpeg);
    if (it != cache.end()) return it->second;
    std::string encoder, out;
    if (!ffmpeg.empty() && media::runCapture({ffmpeg, "-hide_banner", "-encoders"}, out) == 0) {
        if (out.find(" libwebp_anim ") != std::string::npos) encoder = "libwebp_anim";
        else if (out.find(" libwebp ") != std::string::npos) encoder = "libwebp";
    }
    return cache.emplace(ffmpeg, encoder).first->second;
}

namespace {

// The first package manager found that can install an ffmpeg with WebP, as an argv. Windows and macOS
// managers install per user without a password; on Linux pkexec shows the system password prompt.
std::vector<std::string> ffmpegInstallCommand() {
    const auto has = [](const char* tool) { return media::findTool(tool); };
#if defined(_WIN32)
    if (auto w = has("winget"); !w.empty())
        return {w, "install", "--id", "Gyan.FFmpeg", "-e", "--silent", "--accept-source-agreements",
                "--accept-package-agreements", "--disable-interactivity"};
    if (auto s = has("scoop"); !s.empty()) return {s, "install", "ffmpeg"};
    if (auto c = has("choco"); !c.empty()) return {c, "install", "ffmpeg-full", "-y"};
#elif defined(__APPLE__)
    if (auto b = has("brew"); !b.empty()) return {b, "install", "ffmpeg"};
    if (auto p = has("port"); !p.empty())
        return {"/usr/bin/osascript", "-e",
                "do shell script \"" + p + " -N install ffmpeg +nonfree\" with administrator privileges"};
#else
    const std::string pk = has("pkexec");
    if (!pk.empty()) {
        if (auto a = has("apt-get"); !a.empty()) return {pk, a, "install", "-y", "ffmpeg"};
        if (auto d = has("dnf"); !d.empty()) return {pk, d, "install", "-y", "ffmpeg"};
        if (auto m = has("pacman"); !m.empty()) return {pk, m, "-S", "--noconfirm", "ffmpeg"};
        if (auto z = has("zypper"); !z.empty()) return {pk, z, "--non-interactive", "install", "ffmpeg"};
    }
#endif
    return {};
}

}  // namespace

void App::chooseFfmpeg() {
    for (const char* t : {"ffmpeg", "ffprobe", "ffplay"}) media::setToolPath(t, {});
    // Several ffmpegs are common (Anaconda, Python packages, a full build...). The first one on PATH
    // is often a minimal build, so prefer any copy that can also write WebP.
    const auto all = media::findToolCandidates("ffmpeg");
    std::string pick = all.empty() ? std::string() : all.front();
    for (const auto& c : all)
        if (!webpEncoder(c).empty()) {
            pick = c;
            break;
        }
    ffmpegPath_ = pick;
    ffmpegWebp_ = !pick.empty() && !webpEncoder(pick).empty();

    installCmd_ = ffmpegInstallCommand();
#if defined(_WIN32)
    const char* exe = ".exe";
#else
    const char* exe = "";
#endif

    if (pick.empty()) {
        log(Level::Warn, "ffmpeg not found: video, camera, audio and MP4/GIF/WebP export need it "
                         "(Export window -> Install ffmpeg)");
        return;
    }
    // Use ffprobe / ffplay from the same build so they agree with ffmpeg.
    media::setToolPath("ffmpeg", pick);
    const fs::path dir = fs::path(pick).parent_path();
    for (const char* t : {"ffprobe", "ffplay"}) {
        std::error_code ec;
        const fs::path p = dir / (std::string(t) + exe);
        if (fs::exists(p, ec)) media::setToolPath(t, p.string());
    }
    log(ffmpegWebp_ ? Level::Info : Level::Warn,
        "Using ffmpeg " + pick + (ffmpegWebp_ ? "" : " (no WebP encoder; Export window -> Install ffmpeg)"));
}

void App::startFfmpegInstall() {
    if (installCmd_.empty() || ffmpegInstall_.valid()) return;
    log(Level::Info, "Installing ffmpeg: this can take a few minutes...");
    ffmpegInstall_ = std::async(std::launch::async, [cmd = installCmd_] {
        std::string out;
        const int rc = media::runCapture(cmd, out);
        return std::make_pair(rc, out);
    });
}

void App::pollFfmpegInstall() {
    if (!ffmpegInstall_.valid() || ffmpegInstall_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    const auto [rc, out] = ffmpegInstall_.get();
    chooseFfmpeg();  // winget's folders are searched directly, so no restart or PATH change is needed
    if (ffmpegWebp_) {
        log(Level::Ok, "ffmpeg is ready: " + ffmpegPath_);
        return;
    }
    std::string last = out;
    while (!last.empty() && (last.back() == '\n' || last.back() == '\r' || last.back() == ' ')) last.pop_back();
    if (const auto nl = last.find_last_of('\n'); nl != std::string::npos) last = last.substr(nl + 1);
    log(Level::Error, "ffmpeg install did not finish (code " + std::to_string(rc) + "): " + last);
}

void App::drawFfmpegSetup(int format) {
    const bool missing = format >= 2 && ffmpegPath_.empty();
    const bool noWebp = format == 5 && !ffmpegPath_.empty() && !ffmpegWebp_;
    if (!missing && !noWebp && !ffmpegInstall_.valid()) return;
    if (ffmpegInstall_.valid()) {
        ImGui::TextColored(col(colors::Accent), "Installing ffmpeg... this can take a few minutes");
        ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-1, 0), "");
        return;
    }
    ImGui::TextColored(col(colors::Error), missing ? "ffmpeg not found" : "This ffmpeg has no WebP encoder");
    if (!installCmd_.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Install ffmpeg")) startFfmpegInstall();
        if (ImGui::IsItemHovered()) {
            std::string cmd;
            for (const auto& arg : installCmd_)
                cmd += (cmd.empty() ? fs::path(arg).filename().string() : arg) + " ";
            ImGui::SetTooltip("Runs: %s", cmd.c_str());
        }
    } else {
        // No package manager we can drive: point at a manual install, then "Search again" finds it.
#if defined(_WIN32)
        ImGui::TextDisabled("Download a full build from gyan.dev/ffmpeg and unzip it so ffmpeg.exe is in C:/ffmpeg/bin.");
#elif defined(__APPLE__)
        ImGui::TextDisabled("Install Homebrew (brew.sh), then: brew install ffmpeg");
#else
        ImGui::TextDisabled("Install ffmpeg with your package manager (e.g. sudo apt install ffmpeg).");
#endif
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Search again")) chooseFfmpeg();
}

}  // namespace app
