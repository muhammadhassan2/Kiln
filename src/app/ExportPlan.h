#pragma once

// Export decisions that need no GL or UI: output naming, per-format recommendations, the batch
// folder scan and crash-safe ".part" commits. Kept dependency-free so it can be unit tested.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace app {

bool isShaderFile(const std::filesystem::path& p);

namespace exportplan {

namespace fs = std::filesystem;

// Export format indices: 0 PNG frame, 1 PNG sequence, 2 MP4, 3 MOV, 4 GIF, 5 WebP.
const char* extension(int format);  // "" for the PNG sequence (a folder)

// First of the three that exists as a directory (remembered folder, project folder, Documents).
fs::path defaultFolder(const fs::path& remembered, const fs::path& projectDir, const fs::path& documents);
fs::path outputFor(const fs::path& folder, const std::string& stem, int format);
fs::path withFormat(const fs::path& current, int format);  // keeps folder + stem
fs::path keepBoth(const fs::path& target);                   // name_2, name_3, ... (first free)

struct Recommendation {
    float fps;
    int width, height;
};
// Suggested values for the format, or nothing when there is no advice or the values already match.
std::optional<Recommendation> recommendFor(int format, int width, int height, float fps);
// GIF palette generation keeps every frame in memory; true when that exceeds 1 GiB.
bool gifTooHeavy(int width, int height, float fps, float length);
double gifMemoryGB(int width, int height, float fps, float length);

struct BatchSource {
    fs::path source;
    std::string outputStem;  // file stem, or the full file name when two files share a stem
};
std::vector<BatchSource> scanShaderFolder(const fs::path& folder);  // non-recursive, sorted by name

fs::path partPath(const fs::path& final);  // "<final>.part"
// Renames a finished .part into place. A folder target is only replaced if it holds nothing but
// frame_NNNNN.png files; anything else is left alone and the commit fails.
bool commitPart(const fs::path& part, const fs::path& final, std::string& error);
void discardPart(const fs::path& part);
bool isFrameOnlyFolder(const fs::path& dir);

}  // namespace exportplan
}  // namespace app
