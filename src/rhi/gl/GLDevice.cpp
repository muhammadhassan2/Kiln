#include "rhi/RHI.h"
#include "rhi/gl/GLHeaders.h"
#include "shader/Diagnostics.h"
#include "shader/Translate.h"

#include <array>
#include <chrono>
#include <cstring>
#include <unordered_map>

namespace rhi {

namespace {

GLenum toGL(Wrap w) {
    switch (w) {
        case Wrap::Repeat: return GL_REPEAT;
        case Wrap::Clamp: return GL_CLAMP_TO_EDGE;
        case Wrap::Mirror: return GL_MIRRORED_REPEAT;
    }
    return GL_REPEAT;
}

void formatToGL(Format f, GLint& internal, GLenum& type) {
    switch (f) {
        case Format::RGBA8: internal = GL_RGBA8; type = GL_UNSIGNED_BYTE; return;
        case Format::RGBA16F: internal = GL_RGBA16F; type = GL_HALF_FLOAT; return;
        case Format::RGBA32F: internal = GL_RGBA32F; type = GL_FLOAT; return;
    }
}

UniformType fromGL(GLenum t) {
    switch (t) {
        case GL_FLOAT: return UniformType::Float;
        case GL_FLOAT_VEC2: return UniformType::Vec2;
        case GL_FLOAT_VEC3: return UniformType::Vec3;
        case GL_FLOAT_VEC4: return UniformType::Vec4;
        case GL_INT: return UniformType::Int;
        case GL_BOOL: return UniformType::Bool;
        case GL_SAMPLER_2D: return UniformType::Sampler2D;
        default: return UniformType::Other;
    }
}

std::string shaderLog(GLuint s) {
    GLint len = 0;
    glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
    std::string log(static_cast<size_t>(len > 1 ? len : 1), '\0');
    glGetShaderInfoLog(s, len, nullptr, log.data());
    log.resize(std::strlen(log.c_str()));
    return log;
}

std::string programLog(GLuint p) {
    GLint len = 0;
    glGetProgramiv(p, GL_INFO_LOG_LENGTH, &len);
    std::string log(static_cast<size_t>(len > 1 ? len : 1), '\0');
    glGetProgramInfoLog(p, len, nullptr, log.data());
    log.resize(std::strlen(log.c_str()));
    return log;
}

// ------------------------------------------------------------------ Texture
class GLTexture final : public Texture {
public:
    GLTexture(int w, int h, Format format, const void* pixels) : format_(format) {
        glGenTextures(1, &id_);
        allocate(w, h, pixels);
        setSampler(sampler_);
    }
    ~GLTexture() override { glDeleteTextures(1, &id_); }

    int width() const override { return w_; }
    int height() const override { return h_; }
    std::uint64_t nativeHandle() const override { return id_; }

    void setSampler(const SamplerDesc& d) override {
        sampler_ = d;
        glBindTexture(GL_TEXTURE_2D, id_);
        GLint minF = GL_LINEAR, magF = GL_LINEAR;
        if (d.filter == Filter::Nearest) { minF = magF = GL_NEAREST; }
        if (d.filter == Filter::Trilinear) {
            minF = GL_LINEAR_MIPMAP_LINEAR;
            if (!hasMips_) { glGenerateMipmap(GL_TEXTURE_2D); hasMips_ = true; }
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minF);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magF);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(toGL(d.wrapU)));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(toGL(d.wrapV)));
    }

    void upload(const void* rgba8, int w, int h) override {
        if (w != w_ || h != h_ || format_ != Format::RGBA8) {
            format_ = Format::RGBA8;
            allocate(w, h, rgba8);
        } else {
            glBindTexture(GL_TEXTURE_2D, id_);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba8);
        }
        hasMips_ = false;
        setSampler(sampler_);
    }

    GLuint id() const { return id_; }

    void allocate(int w, int h, const void* pixels) {
        w_ = w; h_ = h;
        GLint internal; GLenum type;
        formatToGL(format_, internal, type);
        glBindTexture(GL_TEXTURE_2D, id_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        // Pixel data is always RGBA8 from the CPU side.
        glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, GL_RGBA,
                     pixels ? GL_UNSIGNED_BYTE : type, pixels);
        hasMips_ = false;
    }

private:
    GLuint id_ = 0;
    int w_ = 0, h_ = 0;
    Format format_;
    SamplerDesc sampler_{Filter::Linear, Wrap::Clamp, Wrap::Clamp};
    bool hasMips_ = false;
};

// ------------------------------------------------------------------ RenderTarget
class GLRenderTarget final : public RenderTarget {
public:
    GLRenderTarget(int w, int h, Format f) : color_(w, h, f, nullptr) {
        glGenFramebuffers(1, &fbo_);
        attach();
    }
    ~GLRenderTarget() override { glDeleteFramebuffers(1, &fbo_); }

    void resize(int w, int h) override {
        if (w == color_.width() && h == color_.height()) return;
        color_.allocate(w, h, nullptr);
        attach();
    }
    int width() const override { return color_.width(); }
    int height() const override { return color_.height(); }
    Texture& color() override { return color_; }
    GLuint fbo() const { return fbo_; }

private:
    void attach() {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_.id(), 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    GLTexture color_;
    GLuint fbo_ = 0;
};

// ------------------------------------------------------------------ Program
class GLProgram final : public Program {
public:
    explicit GLProgram(GLuint id) : id_(id) { reflect(); }
    ~GLProgram() override { glDeleteProgram(id_); }
    const std::vector<UniformInfo>& uniforms() const override { return uniforms_; }
    GLuint id() const { return id_; }

private:
    void reflect() {
        GLint count = 0;
        glGetProgramiv(id_, GL_ACTIVE_UNIFORMS, &count);
        for (GLint i = 0; i < count; ++i) {
            char name[256];
            GLsizei len = 0;
            GLint size = 0;
            GLenum type = 0;
            glGetActiveUniform(id_, static_cast<GLuint>(i), sizeof(name), &len, &size, &type, name);
            UniformInfo info;
            info.name.assign(name, static_cast<size_t>(len));
            if (auto p = info.name.find('['); p != std::string::npos) info.name.resize(p);
            info.type = fromGL(type);
            info.arraySize = size;
            info.location = glGetUniformLocation(id_, name);
            if (info.location >= 0) uniforms_.push_back(std::move(info));
        }
    }

    GLuint id_;
    std::vector<UniformInfo> uniforms_;
};

// ------------------------------------------------------------------ Device
class GLDevice final : public Device {
public:
    GLDevice() {
        glGenVertexArrays(1, &vao_);
        glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &maxUnits_);
    }
    ~GLDevice() override {
        for (auto& [id, t] : timers_)
            if (t.queries[0]) glDeleteQueries(static_cast<GLsizei>(t.queries.size()), t.queries.data());
        glDeleteVertexArrays(1, &vao_);
    }

    std::string name() const override {
        const auto* r = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        return std::string("OpenGL · ") + (r ? r : "unknown");
    }
    int maxTextureUnits() const override { return maxUnits_; }

    std::unique_ptr<Texture> createTexture(int w, int h, Format f, const void* rgba8) override {
        return std::make_unique<GLTexture>(w, h, f, rgba8);
    }
    std::unique_ptr<RenderTarget> createRenderTarget(int w, int h, Format f) override {
        return std::make_unique<GLRenderTarget>(w, h, f);
    }

    CompileResult compileFragment(const shader::TranslatedSource& translated, bool uvTopLeft) override {
        const auto t0 = std::chrono::steady_clock::now();
        CompileResult result;
        const char* csrc = translated.source.c_str();
        GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fs, 1, &csrc, nullptr);
        glCompileShader(fs);
        GLint ok = 0;
        glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
        result.log = shaderLog(fs);

        if (ok) {
            const std::string vsSource = shader::buildVertexShader(translated.varyings, uvTopLeft);
            const char* cvs = vsSource.c_str();
            GLuint vs = glCreateShader(GL_VERTEX_SHADER);
            glShaderSource(vs, 1, &cvs, nullptr);
            glCompileShader(vs);

            GLuint prog = glCreateProgram();
            glAttachShader(prog, vs);
            glAttachShader(prog, fs);
            glLinkProgram(prog);
            glDetachShader(prog, vs);
            glDetachShader(prog, fs);
            glDeleteShader(vs);
            glGetProgramiv(prog, GL_LINK_STATUS, &ok);
            const std::string link = programLog(prog);
            if (!link.empty()) result.log += (result.log.empty() ? "" : "\n") + link;
            if (ok) result.program = std::make_unique<GLProgram>(prog);
            else glDeleteProgram(prog);
        }
        glDeleteShader(fs);

        result.diagnostics = shader::parseCompileLog(result.log);
        result.milliseconds =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return result;
    }

    void draw(const Program& program, RenderTarget& target,
              std::span<const UniformValue> uniforms,
              std::span<const TextureBinding> textures) override {
        const auto& p = static_cast<const GLProgram&>(program);
        auto& rt = static_cast<GLRenderTarget&>(target);

        glBindFramebuffer(GL_FRAMEBUFFER, rt.fbo());
        glViewport(0, 0, rt.width(), rt.height());
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_CULL_FACE);
        glUseProgram(p.id());

        for (const auto& u : uniforms) {
            switch (u.type) {
                case UniformType::Float: glUniform1f(u.location, u.f[0]); break;
                case UniformType::Vec2: glUniform2fv(u.location, 1, u.f); break;
                case UniformType::Vec3: glUniform3fv(u.location, 1, u.f); break;
                case UniformType::Vec4: glUniform4fv(u.location, 1, u.f); break;
                case UniformType::Int:
                case UniformType::Bool: glUniform1i(u.location, u.i); break;
                default: break;
            }
        }
        for (const auto& t : textures) {
            glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(t.unit));
            glBindTexture(GL_TEXTURE_2D, t.texture ? static_cast<GLuint>(t.texture->nativeHandle()) : 0);
            glUniform1i(t.location, t.unit);
        }

        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindVertexArray(0);
        glActiveTexture(GL_TEXTURE0);
        glUseProgram(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void clear(RenderTarget& target) override {
        auto& rt = static_cast<GLRenderTarget&>(target);
        glBindFramebuffer(GL_FRAMEBUFFER, rt.fbo());
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void readPixels(RenderTarget& target, int x, int y, int w, int h, std::vector<float>& rgba) override {
        auto& rt = static_cast<GLRenderTarget&>(target);
        rgba.assign(static_cast<size_t>(w * h * 4), 0.0f);
        glBindFramebuffer(GL_FRAMEBUFFER, rt.fbo());
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(x, y, w, h, GL_RGBA, GL_FLOAT, rgba.data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void readRGBA8(RenderTarget& target, std::vector<std::uint8_t>& rgba) override {
        auto& rt = static_cast<GLRenderTarget&>(target);
        rgba.resize(static_cast<size_t>(rt.width() * rt.height() * 4));
        glBindFramebuffer(GL_FRAMEBUFFER, rt.fbo());
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, rt.width(), rt.height(), GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // Timer queries: a small ring per id so reading results never stalls the pipeline.
    void beginTimer(int id) override {
        auto& t = timers_[id];
        if (t.queries[0] == 0) glGenQueries(static_cast<GLsizei>(t.queries.size()), t.queries.data());
        // Harvest the oldest finished query before reusing its slot.
        const GLuint q = t.queries[t.next];
        if (t.used[t.next]) {
            GLint available = 0;
            glGetQueryObjectiv(q, GL_QUERY_RESULT_AVAILABLE, &available);
            if (available) {
                GLuint64 ns = 0;
                glGetQueryObjectui64v(q, GL_QUERY_RESULT, &ns);
                t.lastMs = static_cast<double>(ns) / 1.0e6;
            }
        }
        glBeginQuery(GL_TIME_ELAPSED, q);
        t.used[t.next] = true;
    }
    void endTimer(int id) override {
        auto& t = timers_[id];
        glEndQuery(GL_TIME_ELAPSED);
        t.next = (t.next + 1) % t.queries.size();
    }
    double timerMs(int id) const override {
        auto it = timers_.find(id);
        return it == timers_.end() ? 0.0 : it->second.lastMs;
    }

private:
    struct Timer {
        std::array<GLuint, 4> queries{};
        std::array<bool, 4> used{};
        size_t next = 0;
        double lastMs = 0.0;
    };
    std::unordered_map<int, Timer> timers_;
    GLuint vao_ = 0;
    GLint maxUnits_ = 16;
};

}  // namespace

std::unique_ptr<Device> createGLDevice() { return std::make_unique<GLDevice>(); }

}  // namespace rhi
