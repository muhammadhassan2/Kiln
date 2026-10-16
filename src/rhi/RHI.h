#pragma once
// Thin render-hardware interface. The app talks only to these types, so a Metal backend
// is a new folder next to rhi/gl, not a refactor.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace shader {
struct TranslatedSource;
}

namespace rhi {

enum class Filter { Nearest, Linear, Trilinear };
enum class Wrap { Repeat, Clamp, Mirror };
enum class Format { RGBA8, RGBA16F, RGBA32F };

struct SamplerDesc {
    Filter filter = Filter::Trilinear;
    Wrap wrapU = Wrap::Repeat;
    Wrap wrapV = Wrap::Repeat;
};

class Texture {
public:
    virtual ~Texture() = default;
    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual void setSampler(const SamplerDesc& desc) = 0;
    // RGBA8 pixels, rows bottom-to-top. Reallocates when the size changes.
    virtual void upload(const void* rgba8, int w, int h) = 0;
    // GL texture name / MTLTexture* — used for ImGui display.
    virtual std::uint64_t nativeHandle() const = 0;
};

class RenderTarget {
public:
    virtual ~RenderTarget() = default;
    virtual void resize(int w, int h) = 0;
    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual Texture& color() = 0;
};

enum class UniformType { Float, Vec2, Vec3, Vec4, Int, Bool, Sampler2D, Other };

struct UniformInfo {
    std::string name;  // without "[0]"
    UniformType type = UniformType::Other;
    int location = -1;
    int arraySize = 1;
};

struct Diagnostic {
    int file = 0;  // source-string index: 0 = the user's file, >0 = an #include
    int line = 0;  // 1-based line in that file, 0 = unknown
    std::string message;
    bool isError = true;
};

class Program {
public:
    virtual ~Program() = default;
    virtual const std::vector<UniformInfo>& uniforms() const = 0;
};

struct CompileResult {
    std::shared_ptr<Program> program;  // null on failure
    std::string log;
    std::vector<Diagnostic> diagnostics;
    double milliseconds = 0.0;
    bool ok() const { return program != nullptr; }
};

struct UniformValue {
    int location = -1;
    UniformType type = UniformType::Float;
    float f[4]{};
    int i = 0;
};

struct TextureBinding {
    int location = -1;
    int unit = 0;
    const Texture* texture = nullptr;
};

class Device {
public:
    virtual ~Device() = default;
    virtual std::string name() const = 0;
    virtual int maxTextureUnits() const = 0;

    virtual std::unique_ptr<Texture> createTexture(int w, int h, Format format, const void* rgba8 = nullptr) = 0;
    virtual std::unique_ptr<RenderTarget> createRenderTarget(int w, int h, Format format) = 0;

    // Compiles an already-translated full-screen fragment shader (see shader::translateFragment).
    // uvTopLeft selects the origin of the generated varyings (false = GL bottom-left, true = video top-left).
    virtual CompileResult compileFragment(const shader::TranslatedSource& source, bool uvTopLeft) = 0;

    virtual void draw(const Program& program, RenderTarget& target,
                      std::span<const UniformValue> uniforms,
                      std::span<const TextureBinding> textures) = 0;

    virtual void clear(RenderTarget& target) = 0;

    // Read back pixels (origin bottom-left). Float RGBA for inspection, RGBA8 for export.
    virtual void readPixels(RenderTarget& target, int x, int y, int w, int h, std::vector<float>& rgba) = 0;
    virtual void readRGBA8(RenderTarget& target, std::vector<std::uint8_t>& rgba) = 0;

    // GPU timing (asynchronous; results lag a couple of frames). ids are small integers.
    virtual void beginTimer(int id) = 0;
    virtual void endTimer(int id) = 0;
    virtual double timerMs(int id) const = 0;
};

std::unique_ptr<Device> createGLDevice();

}  // namespace rhi
