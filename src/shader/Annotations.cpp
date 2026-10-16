#include "shader/Annotations.h"

#include <cctype>
#include <cstdlib>
#include <regex>
#include <sstream>

namespace shader {

namespace {

std::optional<std::array<float, 4>> parseHexColor(const std::string& s) {
    std::string h = s;
    if (!h.empty() && h[0] == '#') h.erase(0, 1);
    if (h.size() != 6 && h.size() != 8) return std::nullopt;
    for (char c : h)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return std::nullopt;
    const unsigned long v = std::strtoul(h.c_str(), nullptr, 16);
    std::array<float, 4> c{};
    if (h.size() == 6) {
        c = {((v >> 16) & 0xFF) / 255.0f, ((v >> 8) & 0xFF) / 255.0f, (v & 0xFF) / 255.0f, 1.0f};
    } else {
        c = {((v >> 24) & 0xFF) / 255.0f, ((v >> 16) & 0xFF) / 255.0f, ((v >> 8) & 0xFF) / 255.0f,
             (v & 0xFF) / 255.0f};
    }
    return c;
}

}  // namespace

AnnotationMap parseAnnotations(const std::string& source) {
    static const std::regex decl(
        R"(^\s*uniform\s+\w+\s+(\w+)\s*(?:\[\s*\d+\s*\])?\s*;\s*//\s*@(\w+)\s*(.*)$)");
    static const std::regex range(R"(^\s*(-?[\d.]+)\s*\.\.\s*(-?[\d.]+)\s*(-?[\d.]+)?)");

    // gl-transitions style default: "uniform float amount; // = 0.5" or "// = vec2(0.5, 0.2)"
    static const std::regex glDefault(R"(^\s*uniform\s+\w+\s+(\w+)\s*;\s*//\s*=\s*(.*)$)");
    static const std::regex number(R"(-?\d*\.?\d+(?:[eE][-+]?\d+)?)");

    AnnotationMap out;
    std::istringstream in(source);
    std::string line;
    while (std::getline(in, line)) {
        // Fast path: only "uniform ... // ..." lines can carry an annotation.
        const size_t u = line.find("uniform");
        if (u == std::string::npos || line.find("//", u) == std::string::npos) continue;
        std::smatch m;
        if (std::regex_match(line, m, glDefault)) {
            Annotation a;
            std::array<float, 4> v{};
            const std::string rest = m[2].str();
            int n = 0;
            for (auto it = std::sregex_iterator(rest.begin(), rest.end(), number); it != std::sregex_iterator() && n < 4; ++it)
                v[static_cast<size_t>(n++)] = std::strtof(it->str().c_str(), nullptr);
            if (n == 1) v = {v[0], v[0], v[0], v[0]};
            if (n > 0) a.defaultValue = v;
            out[m[1].str()] = a;
            continue;
        }
        if (!std::regex_match(line, m, decl)) continue;
        const std::string name = m[1].str();
        const std::string tag = m[2].str();
        const std::string args = m[3].str();

        Annotation a;
        if (tag == "slider") {
            a.kind = Annotation::Kind::Slider;
            std::smatch r;
            if (std::regex_search(args, r, range)) {
                a.min = std::strtof(r[1].str().c_str(), nullptr);
                a.max = std::strtof(r[2].str().c_str(), nullptr);
                if (r[3].matched) {
                    const float d = std::strtof(r[3].str().c_str(), nullptr);
                    a.defaultValue = std::array<float, 4>{d, d, d, d};
                }
            }
        } else if (tag == "color") {
            a.kind = Annotation::Kind::Color;
            std::istringstream as(args);
            std::string hex;
            as >> hex;
            a.defaultValue = parseHexColor(hex);
        } else if (tag == "toggle") {
            a.kind = Annotation::Kind::Toggle;
        } else if (tag == "drag") {
            a.kind = Annotation::Kind::Drag;
            const float s = std::strtof(args.c_str(), nullptr);
            if (s > 0.0f) a.speed = s;
        } else {
            continue;
        }
        out[name] = a;
    }
    return out;
}

}  // namespace shader
