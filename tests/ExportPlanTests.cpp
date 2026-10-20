// Unit tests for app/ExportPlan (no GL, no UI). Run with: ctest --test-dir build -C Release

#include "app/ExportPlan.h"

#include <cstdio>
#include <fstream>

namespace fs = std::filesystem;
using namespace app::exportplan;

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                   \
        }                                                                 \
    } while (0)

static void touch(const fs::path& p, const char* text = "x") {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

static std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

int main() {
    const fs::path d = fs::temp_directory_path() / "kiln_exportplan_tests";
    fs::remove_all(d);
    fs::create_directories(d);

    // naming
    CHECK(std::string(extension(4)) == ".gif" && std::string(extension(5)) == ".webp" && std::string(extension(1)).empty());
    CHECK(outputFor(d, "BlackHole", 4) == d / "BlackHole.gif");
    CHECK(withFormat(d / "BlackHole.gif", 5) == d / "BlackHole.webp");
    CHECK(withFormat(d / "BlackHole.gif", 1) == d / "BlackHole");
    CHECK(withFormat(d / "BlackHole", 2) == d / "BlackHole.mp4");
    touch(d / "a.gif");
    touch(d / "a_2.gif");
    CHECK(keepBoth(d / "a.gif") == d / "a_3.gif");
    CHECK(defaultFolder(d / "missing", d, "docs") == d);
    CHECK(defaultFolder(d / "missing", d / "missing2", "docs") == fs::path("docs"));

    // recommendations
    auto gif = recommendFor(4, 1080, 1920, 30);
    CHECK(gif && gif->fps == 15.0f && gif->width == 720 && gif->height == 1280);
    auto webp = recommendFor(5, 1080, 1920, 30);
    CHECK(webp && webp->fps == 24.0f && webp->width == 1080 && webp->height == 1920);
    auto mp4 = recommendFor(2, 1080, 1920, 60);
    CHECK(mp4 && mp4->fps == 30.0f && mp4->width == 1080);
    CHECK(!recommendFor(2, 1080, 1920, 30));
    CHECK(!recommendFor(0, 1080, 1920, 30));
    CHECK(gifTooHeavy(1080, 1920, 30, 5) && !gifTooHeavy(540, 960, 15, 2));

    // batch scan: sorted, non-recursive, shader files only, clashing stems keep their extension
    const fs::path b = d / "batch";
    for (const char* f : {"b.frag", "wave.frag", "wave.glsl", "notes.md", "A.fs", "sub/c.frag"}) touch(b / f);
    const auto items = scanShaderFolder(b);
    CHECK(items.size() == 4);
    if (items.size() == 4) {
        CHECK(items[0].outputStem == "A" && items[1].outputStem == "b");
        CHECK(items[2].outputStem == "wave.frag" && items[3].outputStem == "wave.glsl");
    }
    CHECK(scanShaderFolder(d / "empty-missing").empty());

    // .part commits
    touch(d / "x.gif.part", "new");
    touch(d / "x.gif", "old");
    std::string err;
    CHECK(partPath(d / "x.gif") == d / "x.gif.part");
    CHECK(commitPart(d / "x.gif.part", d / "x.gif", err) && slurp(d / "x.gif") == "new" && !fs::exists(d / "x.gif.part"));
    touch(d / "seq.part/frame_00000.png", "new");
    touch(d / "seq/frame_00000.png", "old");
    CHECK(commitPart(d / "seq.part", d / "seq", err) && slurp(d / "seq/frame_00000.png") == "new");
    touch(d / "keep.part/frame_00000.png");
    touch(d / "keep/keep.txt");
    CHECK(!commitPart(d / "keep.part", d / "keep", err) && fs::exists(d / "keep/keep.txt"));
    discardPart(d / "keep.part");
    CHECK(!fs::exists(d / "keep.part"));

    fs::remove_all(d);
    if (failures == 0) std::printf("export_plan_tests: all passed\n");
    return failures == 0 ? 0 : 1;
}
