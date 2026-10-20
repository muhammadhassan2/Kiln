#pragma once
// Helpers shared by the App*.cpp translation units.

#include "app/ExportPlan.h"  // isShaderFile
#include "rhi/RHI.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace app {

struct OutputSize {
    const char* label;
    int w, h;
};
inline constexpr OutputSize kOutputs[] = {
    {"Fit panel", 0, 0},         {"1280 x 720", 1280, 720},   {"1920 x 1080", 1920, 1080},
    {"1080 x 1920 (portrait)", 1080, 1920}, {"1080 x 1080", 1080, 1080}, {"2048 x 2048", 2048, 2048},
    {"3840 x 2160", 3840, 2160},
};
inline constexpr int kOutputCount = static_cast<int>(sizeof(kOutputs) / sizeof(kOutputs[0]));

inline const char* kFilterNames[] = {"Nearest", "Linear", "Trilinear"};
inline const char* kWrapNames[] = {"Repeat", "Clamp", "Mirror"};
inline const char* kEaseNames[] = {"Linear", "Ease in", "Ease out", "Ease in-out", "Step"};
inline const char* kDriveNames[] = {"Manual", "Time", "Loop 0..1", "Keyframes", "Video pos"};

inline std::string nowString() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

// App identity. Projects are saved as .kiln; older .ssproj files (from "Shader Studio") still open.
inline constexpr const char* kAppName = "Kiln";
inline constexpr const char* kProjectExt = ".kiln";
inline constexpr const char* kAutosaveName = "autosave.kiln";

inline bool isProjectFile(const std::filesystem::path& p) {
    std::string e = p.extension().string();
    for (auto& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e == ".kiln" || e == ".ssproj";
}
inline bool isAutosave(const std::filesystem::path& p) {
    return p.filename() == "autosave.kiln" || p.filename() == "autosave.ssproj";
}

// Moves a folder from the app's old name to its new one the first time (settings, library).
inline std::filesystem::path adoptLegacyFolder(const std::filesystem::path& dir, const std::filesystem::path& legacy) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(dir, ec) && fs::exists(legacy, ec)) {
        fs::create_directories(dir.parent_path(), ec);
        fs::rename(legacy, dir, ec);
        if (ec) {  // different volume or locked: copy instead and leave the old folder
            ec.clear();
            fs::copy(legacy, dir, fs::copy_options::recursive, ec);
        }
    }
    return dir;
}

inline std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

inline bool readFile(const std::filesystem::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

inline bool writeFile(const std::filesystem::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    return static_cast<bool>(out);
}

inline ImTextureID texId(const rhi::Texture& t) { return static_cast<ImTextureID>(t.nativeHandle()); }

inline ImVec4 col(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

inline void pushAccentButton() {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.776f, 0.957f, 0.196f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.886f, 1.0f, 0.478f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.70f, 0.88f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.055f, 0.059f, 0.071f, 1.0f));
}
inline void popAccentButton() { ImGui::PopStyleColor(4); }

// ---- built-in uniforms filled by the app
enum class Builtin { None, Time, TimeDelta, Frame, Resolution, Mouse, TexCount, Date, AudioLevel, Bass, Mid, High, Ratio };

inline Builtin builtinFor(const std::string& n) {
    if (n == "u_time" || n == "iTime") return Builtin::Time;
    if (n == "u_timeDelta" || n == "iTimeDelta") return Builtin::TimeDelta;
    if (n == "u_frame" || n == "iFrame") return Builtin::Frame;
    if (n == "u_resolution" || n == "iResolution") return Builtin::Resolution;
    if (n == "u_mouse" || n == "iMouse") return Builtin::Mouse;
    if (n == "u_texCount") return Builtin::TexCount;
    if (n == "iDate" || n == "u_date") return Builtin::Date;
    if (n == "u_audioLevel") return Builtin::AudioLevel;
    if (n == "u_bass") return Builtin::Bass;
    if (n == "u_mid") return Builtin::Mid;
    if (n == "u_high") return Builtin::High;
    if (n == "ratio" || n == "u_ratio") return Builtin::Ratio;
    return Builtin::None;
}

}  // namespace app
