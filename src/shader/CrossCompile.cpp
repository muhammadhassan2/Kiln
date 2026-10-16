#include "shader/CrossCompile.h"

#include "media/Process.h"
#include "shader/Diagnostics.h"
#include "shader/Translate.h"

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <spirv_msl.hpp>

#include <fstream>
#include <regex>
#include <sstream>

namespace shader {

void initCrossCompiler() { glslang::InitializeProcess(); }
void shutdownCrossCompiler() { glslang::FinalizeProcess(); }

EsCheck validateEs(const std::string& userSource) {
    EsCheck r;
    static const std::regex versionRe(R"(^\s*#\s*version\s+(\d+)(\s+es)?)");
    static const std::regex includeRe(R"(^\s*#\s*include\b)");
    int version = 100;
    bool hasVersion = false, es = false;
    std::string src;  // the source without #include lines (they're resolved only for the desktop compile)
    {
        std::istringstream in(userSource);
        std::string line;
        while (std::getline(in, line)) {
            std::smatch m;
            if (!hasVersion && line.find("version") != std::string::npos && std::regex_search(line, m, versionRe)) {
                hasVersion = true;
                version = std::stoi(m[1].str());
                es = m[2].matched;
            }
            const bool isInclude = line.find("include") != std::string::npos && std::regex_search(line, includeRe);
            src += isInclude ? std::string() : line;
            src += "\n";
        }
    }
    if (hasVersion) {
        if (!es && version >= 130) return r;  // desktop shader: not a mobile check
        if (!es) version = 100;               // 110/120 desktop legacy: check as ES 1.00
    }
    // Shadertoy / transition snippets aren't standalone ES shaders.
    static const std::regex mainImageRe(R"(\bmainImage\s*\()");
    static const std::regex mainRe(R"(\bvoid\s+main\s*\()");
    if (userSource.find("mainImage") != std::string::npos && std::regex_search(userSource, mainImageRe) &&
        !std::regex_search(userSource, mainRe))
        return r;

    r.applicable = true;
    r.profile = version >= 300 ? "GLSL ES " + std::to_string(version / 100) + "." + std::to_string(version % 100 / 10) + "0"
                               : "GLSL ES 1.00";

    const char* str = src.c_str();
    glslang::TShader sh(EShLangFragment);
    sh.setStrings(&str, 1);
    sh.setEnvInput(glslang::EShSourceGlsl, EShLangFragment, glslang::EShClientNone, 0);
    r.ok = sh.parse(GetDefaultResources(), version, EEsProfile, false, false, EShMsgDefault);
    for (auto& d : parseCompileLog(sh.getInfoLog())) {
        if (d.message.find("compilation terminated") != std::string::npos) continue;
        if (d.line == 0 && d.message.find("compilation errors") != std::string::npos) continue;
        r.diagnostics.push_back(std::move(d));
    }
    return r;
}

MslResult toMsl(const TranslatedSource& translated, bool ios) {
    MslResult r;
    const char* str = translated.source.c_str();
    glslang::TShader sh(EShLangFragment);
    sh.setStrings(&str, 1);
    sh.setEnvInput(glslang::EShSourceGlsl, EShLangFragment, glslang::EShClientVulkan, 100);
    sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    // Loose uniforms (OpenGL style) are gathered into one block named "Uniforms".
    sh.setEnvInputVulkanRulesRelaxed();
    sh.setAutoMapLocations(true);
    sh.setAutoMapBindings(true);
    sh.setGlobalUniformBlockName("Uniforms");
    sh.setGlobalUniformSet(0);
    sh.setGlobalUniformBinding(0);
    if (!sh.parse(GetDefaultResources(), 410, false, EShMsgDefault)) {
        r.log = sh.getInfoLog();
        return r;
    }
    glslang::TProgram program;
    program.addShader(&sh);
    if (!program.link(EShMsgDefault)) {
        r.log = program.getInfoLog();
        return r;
    }
    std::vector<uint32_t> spirv;
    glslang::GlslangToSpv(*program.getIntermediate(EShLangFragment), spirv);
    try {
        spirv_cross::CompilerMSL msl(std::move(spirv));
        auto opts = msl.get_msl_options();
        opts.platform = ios ? spirv_cross::CompilerMSL::Options::iOS : spirv_cross::CompilerMSL::Options::macOS;
        opts.set_msl_version(2, 4);
        msl.set_msl_options(opts);
        msl.rename_entry_point("main", "shader_fragment", spv::ExecutionModelFragment);
        r.msl = msl.compile();
        r.ok = true;
    } catch (const std::exception& e) {
        r.log = std::string("SPIRV-Cross: ") + e.what();
    }
    return r;
}

bool validateMetal(const std::string& msl, bool ios, std::string& log) {
#if defined(__APPLE__)
    const std::string xcrun = media::findTool("xcrun");
    if (xcrun.empty()) {
        log = "xcrun not found (install Xcode).";
        return false;
    }
    const auto dir = media::tempDir();
    const auto src = dir / "shader_check.metal";
    const auto air = dir / "shader_check.air";
    {
        std::ofstream out(src);
        out << msl;
    }
    const int rc = media::runCapture({xcrun, "-sdk", ios ? "iphoneos" : "macosx", "metal", "-c", src.string(), "-o",
                                      air.string()},
                                     log, true);
    if (rc != 0 && (log.find("unable to find utility") != std::string::npos ||
                    log.find("missing Metal Toolchain") != std::string::npos))
        log += "\nThe Metal compiler comes with full Xcode (not just the Command Line Tools). Newer Xcode versions "
               "download it separately: xcodebuild -downloadComponent MetalToolchain";
    return rc == 0;
#else
    (void)msl;
    (void)ios;
    log = "Metal compiler validation is only available on macOS (needs Xcode).";
    return false;
#endif
}

}  // namespace shader
