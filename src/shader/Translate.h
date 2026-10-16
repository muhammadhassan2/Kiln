#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace shader {

struct Varying {
    std::string type;  // float, vec2, vec3, vec4
    std::string name;
};

enum class SourceKind {
    Modern,      // #version 330+ desktop GLSL, run as written
    Es3,         // #version 300/310/320 es, re-targeted to 410 core
    Legacy,      // GLSL ES 1.00 / GLSL 1.10-1.50 (varying, texture2D, gl_FragColor)
    Shadertoy,   // mainImage(out vec4, in vec2) without main()
    Transition,  // gl-transitions style: vec4 transition(vec2 uv) without main()
};

struct TranslateOptions {
    // Directories searched for #include "file" (the shader's own folder first).
    std::vector<std::filesystem::path> includeDirs;
};

// A "#ifndef NAME / #define NAME value" pair: a default the user can override from the UI.
struct DefineInfo {
    std::string name;
    std::string value;  // default value as written
    int line = 0;       // 1-based line of the #define
};
std::vector<DefineInfo> findOverridableDefines(const std::string& source);
// Rewrites the detected "#define NAME value" lines with the values in `overrides` (same line count,
// so error line numbers are unchanged). Names not in `overrides` keep their default.
std::string applyDefineOverrides(const std::string& source, const std::vector<DefineInfo>& defines,
                                 const std::vector<std::pair<std::string, std::string>>& overrides);

struct TranslatedSource {
    std::string source;             // GLSL 410 core (or the user's desktop version) fragment source
    std::vector<Varying> varyings;  // fragment inputs the vertex stage must provide
    std::string dialect;            // human readable, e.g. "GLSL ES 1.00 (legacy, translated)"
    SourceKind kind = SourceKind::Modern;
    // Source-string index used in #line directives: 0 = the user's file, i = includes[i-1].
    std::vector<std::filesystem::path> includes;
    std::vector<std::string> includeErrors;  // "line N: cannot find "x.glsl""
};

// Makes common fragment-shader dialects compile on a desktop 4.1 core context while keeping
// line numbers identical to the user's source (errors in includes report their own file index):
//   * no #version / #version 100-150  -> legacy: varying, texture2D, gl_FragColor, ...
//   * #version 300 es / 310 es        -> re-targeted to 410 core
//   * Shadertoy (mainImage) and gl-transitions (transition()) shaders get a wrapper main()
//   * Android: samplerExternalOES and GL_OES_* extensions are mapped / disabled
//   * #include "file" is resolved against TranslateOptions::includeDirs
TranslatedSource translateFragment(const std::string& userSource, const TranslateOptions& options = {});

// Full-screen-triangle vertex shader that writes each varying from the 0..1 UV:
// vec2 = uv, vec3 = (uv, 0), vec4 = (uv, 0, 1), float = uv.x.
// uvTopLeft = false: origin bottom-left (GL). true: origin top-left (decoded video frames).
std::string buildVertexShader(const std::vector<Varying>& varyings, bool uvTopLeft);

}  // namespace shader
