#include "app/ExportPlan.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <regex>

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
#endif

namespace app {

namespace {

std::string lowerCopy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

}  // namespace

bool isShaderFile(const std::filesystem::path& p) {
    const auto e = lowerCopy(p.extension().string());
    return e == ".frag" || e == ".fs" || e == ".glsl" || e == ".txt";
}

namespace exportplan {

const char* extension(int format) {
    static const char* kExt[] = {".png", "", ".mp4", ".mov", ".gif", ".webp"};
    return kExt[std::clamp(format, 0, 5)];
}

fs::path defaultFolder(const fs::path& remembered, const fs::path& projectDir, const fs::path& documents) {
    std::error_code ec;
    for (const fs::path* p : {&remembered, &projectDir})
        if (!p->empty() && fs::is_directory(*p, ec)) return *p;
    return documents;
}

fs::path outputFor(const fs::path& folder, const std::string& stem, int format) {
    return folder / (stem + extension(format));
}

fs::path withFormat(const fs::path& current, int format) {
    // A PNG-sequence path is a folder name without extension; anything else has one.
    const std::string stem = current.has_extension() ? current.stem().string() : current.filename().string();
    return outputFor(current.parent_path(), stem, format);
}

fs::path keepBoth(const fs::path& target) {
    const std::string stem = target.has_extension() ? target.stem().string() : target.filename().string();
    const std::string ext = target.has_extension() ? target.extension().string() : "";
    std::error_code ec;
    for (int n = 2;; ++n) {
        const fs::path p = target.parent_path() / (stem + "_" + std::to_string(n) + ext);
        if (!fs::exists(p, ec)) return p;
    }
}

std::optional<Recommendation> recommendFor(int format, int width, int height, float fps) {
    if (format == 0 || width <= 0 || height <= 0) return std::nullopt;
    Recommendation r{30.0f, width, height};
    int maxWidth = 0;
    if (format == 4) {
        r.fps = 15.0f;
        maxWidth = 720;
    } else if (format == 5) {
        r.fps = 24.0f;
        maxWidth = 1080;
    }
    if (maxWidth > 0 && width != maxWidth && (format == 4 || width > maxWidth)) {
        r.width = maxWidth;
        r.height = static_cast<int>(std::lround(height * static_cast<double>(maxWidth) / width / 2.0)) * 2;
    }
    if (std::abs(r.fps - fps) < 0.01f && r.width == width && r.height == height) return std::nullopt;
    return r;
}

double gifMemoryGB(int width, int height, float fps, float length) {
    const double frames = std::max(1.0, std::round(static_cast<double>(fps) * length));
    return static_cast<double>(width) * height * 4.0 * frames / (1024.0 * 1024.0 * 1024.0);
}

bool gifTooHeavy(int width, int height, float fps, float length) {
    return gifMemoryGB(width, height, fps, length) > 1.0;
}

std::vector<BatchSource> scanShaderFolder(const fs::path& folder) {
    std::vector<BatchSource> out;
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && isShaderFile(it->path())) out.push_back({it->path(), it->path().stem().string()});
    std::sort(out.begin(), out.end(), [](const BatchSource& a, const BatchSource& b) {
        return lowerCopy(a.source.filename().string()) < lowerCopy(b.source.filename().string());
    });
    // wave.frag + wave.glsl would both become renders/wave.<ext>; keep the source extension for those.
    std::map<std::string, int> stems;
    for (const auto& s : out) ++stems[lowerCopy(s.outputStem)];
    for (auto& s : out)
        if (stems[lowerCopy(s.outputStem)] > 1) s.outputStem = s.source.filename().string();
    return out;
}

fs::path partPath(const fs::path& final) {
    fs::path p = final;
    p += ".part";
    return p;
}

bool isFrameOnlyFolder(const fs::path& dir) {
    static const std::regex frameRe(R"(frame_\d{5}\.png)");
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (!it->is_regular_file(ec) || !std::regex_match(it->path().filename().string(), frameRe)) return false;
    return !ec;
}

bool commitPart(const fs::path& part, const fs::path& final, std::string& error) {
    std::error_code ec;
    if (fs::is_directory(part, ec)) {
        if (fs::exists(final, ec)) {
            if (!fs::is_directory(final, ec) || !isFrameOnlyFolder(final)) {
                error = final.string() + " exists and holds other files";
                return false;
            }
            fs::remove_all(final, ec);
        }
        fs::rename(part, final, ec);
        if (ec) error = "Could not rename " + part.string() + ": " + ec.message();
        return !ec;
    }
#if defined(_WIN32)
    // Plain rename can't replace an existing file on Windows; MoveFileEx does, in one step.
    if (!MoveFileExW(part.wstring().c_str(), final.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "Could not move " + part.string() + " into place (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    return true;
#else
    if (std::rename(part.c_str(), final.c_str()) != 0) {
        error = "Could not move " + part.string() + " into place";
        return false;
    }
    return true;
#endif
}

void discardPart(const fs::path& part) {
    std::error_code ec;
    fs::remove_all(part, ec);
}

}  // namespace exportplan
}  // namespace app
