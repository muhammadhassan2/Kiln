#pragma once

#include "rhi/RHI.h"

#include <string>
#include <vector>

namespace shader {

struct TranslatedSource;

void initCrossCompiler();
void shutdownCrossCompiler();

// Validates the user's source as a mobile GPU would see it (GLSL ES 1.00 or 3.x),
// e.g. missing default float precision, int/float mixing, desktop-only built-ins.
// Returns false if the shader is not an ES shader (desktop #version 330+), with no diagnostics.
struct EsCheck {
    bool applicable = false;
    bool ok = true;
    std::string profile;  // "GLSL ES 1.00" / "GLSL ES 3.00"
    std::vector<rhi::Diagnostic> diagnostics;
};
EsCheck validateEs(const std::string& userSource);

// GLSL (after translation) -> SPIR-V -> Metal Shading Language.
struct MslResult {
    bool ok = false;
    std::string msl;
    std::string log;
};
MslResult toMsl(const TranslatedSource& translated, bool ios);

// macOS only: compiles MSL with Apple's Metal compiler (xcrun metal). Returns false + log on error,
// or if the toolchain is unavailable (log explains).
bool validateMetal(const std::string& msl, bool ios, std::string& log);

}  // namespace shader
