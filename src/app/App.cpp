// App core: passes, compilation, uniforms/bindings, per-frame update and rendering.
#include "app/App.h"

#include "app/Common.h"

#include <GLFW/glfw3.h>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <cmath>
#include <cstdio>
#include <regex>

namespace app {

// ====================================================================== lifecycle

App::App(GLFWwindow* window, Fonts fonts) : window_(window), fonts_(fonts) {
    device_ = rhi::createGLDevice();
    textures_ = std::make_unique<textures::TextureLibrary>(*device_);
    shader::initCrossCompiler();

    // Palette used for every editor (overlay mode swaps the background to transparent).
    TextEditor probe;
    editorPalette_ = probe.GetPalette();

    autocomplete_.triggerOnTyping = true;
    autocomplete_.callback = [this](TextEditor::AutoCompleteState& st) {
        st.suggestions.clear();
        if (st.inComment || st.inString || st.searchTerm.size() < 2) return;
        const std::string term = lower(st.searchTerm);
        std::vector<std::string> pool;
        if (st.language) {
            pool.insert(pool.end(), st.language->keywords.begin(), st.language->keywords.end());
            pool.insert(pool.end(), st.language->declarations.begin(), st.language->declarations.end());
            pool.insert(pool.end(), st.language->identifiers.begin(), st.language->identifiers.end());
        }
        for (const char* b : {"u_time", "u_timeDelta", "u_frame", "u_resolution", "u_mouse", "u_texCount",
                              "u_audioLevel", "u_bass", "u_mid", "u_high", "u_bufferA", "u_bufferB", "u_bufferC",
                              "u_bufferD", "u_prev", "u_from", "u_to", "progress", "ratio", "fragColor",
                              "gl_FragCoord", "texture", "textureLod", "texelFetch", "smoothstep", "fract", "mix"})
            pool.emplace_back(b);
        for (const auto* u : userUniforms()) pool.push_back(u->name);
        activePass().editor->IterateIdentifiers([&](const std::string& id) { pool.push_back(id); });
        std::sort(pool.begin(), pool.end());
        pool.erase(std::unique(pool.begin(), pool.end()), pool.end());
        for (const auto& w : pool)
            if (w.size() > term.size() && lower(w).rfind(term, 0) == 0) st.suggestions.push_back(w);
        if (st.suggestions.size() > 40) st.suggestions.resize(40);
    };

    std::snprintf(cameraDevice_, sizeof(cameraDevice_), "%s", media::defaultCameraDevice().c_str());
    std::snprintf(micDevice_, sizeof(micDevice_), "%s", media::defaultMicDevice().c_str());

    lastClock_ = glfwGetTime();
    log(Level::Info, device_->name() + " · max " + std::to_string(device_->maxTextureUnits()) + " texture units");
    chooseFfmpeg();

    // Window layout lives with the other settings, not in the working directory.
    {
        std::error_code ec;
        fs::create_directories(configDir(), ec);
        iniPath_ = (configDir() / "layout.ini").string();
        ImGui::GetIO().IniFilename = iniPath_.c_str();
    }
    loadSettings();
    if (const char* key = std::getenv("ANTHROPIC_API_KEY"); key && *key && !assistant_.hasKey())
        assistant_.setApiKey(key);

    // Restore the last session (autosave) or start from the default template.
    const fs::path autosave = configDir() / "autosave.ssproj";
    std::error_code ec;
    if (fs::exists(autosave, ec) && openProject(autosave)) {
        projectPath_ = sessionProject_;  // the autosave remembers which project it was
        log(Level::Info, "Restored last session");
    } else {
        newProject(0);
    }
    updateTitle();

    // Headless smoke-test hook (CI): SHADER_STUDIO_EXPORT=/path/out.{png,mp4,mov,gif,webp} renders 2 s at 540x960.
    if (const char* out = std::getenv("SHADER_STUDIO_EXPORT"); out && *out) {
        export_.output = out;
        const std::string ext = lower(export_.output.extension().string());
        export_.format = ext == ".png"  ? 0
                       : ext == ".mov"  ? 3
                       : ext == ".gif"  ? 4
                       : ext == ".webp" ? 5
                       : ext.empty()    ? 1
                                        : 2;
        export_.width = 540;
        export_.height = 960;
        export_.length = 2.0f;
        export_.fps = 30.0f;
        pendingExport_ = true;
    }
    // Headless batch (CI): SHADER_STUDIO_BATCH=<folder> renders every shader there into renders/ as
    // 540x960, 15 fps, 2 s (SHADER_STUDIO_BATCH_FORMAT = gif|webp|mp4|mov|png), then quits;
    // exit code 1 if any file failed. Leaves settings.json and the autosave untouched.
    if (const char* dir = std::getenv("SHADER_STUDIO_BATCH"); dir && *dir) {
        const char* f = std::getenv("SHADER_STUDIO_BATCH_FORMAT");
        const std::string fmt = lower(f ? f : "gif");
        export_.format = fmt == "webp" ? 5 : fmt == "mp4" ? 2 : fmt == "mov" ? 3 : fmt == "png" ? 1 : 4;
        export_.width = 540;
        export_.height = 960;
        export_.fps = 15.0f;
        export_.start = 0.0f;
        export_.length = 2.0f;
        persistSettings_ = false;
        pendingBatch_ = dir;
        batch_.quitWhenDone = true;
    }
}

App::~App() { shutdown(); }

void App::shutdown() {
    static bool done = false;
    if (done) return;
    done = true;
    abortBatchNow();  // restore the user's project before it is autosaved
    if (ffmpegInstall_.valid()) ffmpegInstall_.wait();  // don't abandon winget mid-install
    if (export_.finishing.valid()) export_.finishing.wait();  // let ffmpeg finish the last export
    pollExportFinish();
    std::error_code ec;
    if (persistSettings_) {
        fs::create_directories(configDir(), ec);
        saveProject(configDir() / "autosave.ssproj");
    }
    saveSettings();
    outputWindow_.close();
    textures_->clear();
    shader::shutdownCrossCompiler();
}

void App::log(Level level, std::string text) {
    // Mirror to stderr so the IDE's run console shows it too.
    static const char* kTags[] = {"INFO", "OK", "WARN", "ERR"};
    std::fprintf(stderr, "[%s] %s\n", kTags[static_cast<int>(level)], text.c_str());
    log_.push_back({nowString(), level, std::move(text)});
    if (log_.size() > 1500) log_.erase(log_.begin(), log_.begin() + 300);
    scrollLog_ = true;
}

// ====================================================================== passes

std::unique_ptr<TextEditor> App::makeEditor() {
    auto e = std::make_unique<TextEditor>();
    e->SetLanguage(TextEditor::Language::Glsl());
    e->SetTabSize(4);
    e->SetInsertSpacesOnTabs(true);
    e->SetShowMatchingBrackets(true);
    e->SetCompletePairedGlyphs(true);
    e->SetShowWhitespacesEnabled(false);
    e->SetPalette(overlay_ ? editorPalette_ : editorPalette_);
    setupAutocomplete(*e);
    return e;
}

Pass& App::addPass(const std::string& name, const std::string& source) {
    auto p = std::make_unique<Pass>();
    p->name = name;
    p->isImage = name == "Image";
    p->editor = makeEditor();
    p->editor->SetText(source);
    p->savedUndo = p->editor->GetUndoIndex();
    Pass* raw = p.get();
    raw->editor->SetChangeCallback([this, raw] {
        if (autoCompile_) raw->compileRequested = true;
    }, compileDelayMs_);

    // Keep order: Buffer A..D then Image.
    auto rank = [](const Pass& x) { return x.isImage ? 100 : x.name.back() - 'A'; };
    auto it = std::find_if(passes_.begin(), passes_.end(), [&](const auto& q) { return rank(*q) > rank(*raw); });
    it = passes_.insert(it, std::move(p));
    activePass_ = static_cast<size_t>(it - passes_.begin());
    selectTab_ = true;
    return *raw;
}

Pass* App::findPass(const std::string& name) {
    for (auto& p : passes_)
        if (p->name == name) return p.get();
    return nullptr;
}

Pass& App::imagePass() {
    for (auto& p : passes_)
        if (p->isImage) return *p;
    return addPass("Image", "");
}

Pass& App::activePass() {
    if (activePass_ >= passes_.size()) activePass_ = passes_.size() - 1;
    return *passes_[activePass_];
}

void App::removePass(size_t index) {
    if (index >= passes_.size() || passes_[index]->isImage) return;
    log(Level::Info, "Removed " + passes_[index]->name);
    passes_.erase(passes_.begin() + static_cast<std::ptrdiff_t>(index));
    if (activePass_ >= passes_.size()) activePass_ = passes_.size() - 1;
    selectTab_ = true;
}

// ====================================================================== compile

void App::compileAll(const char* reason) {
    for (auto& p : passes_) compilePass(*p, reason);
}

void App::applyCompileDelay() {
    for (auto& p : passes_) {
        Pass* raw = p.get();
        raw->editor->SetChangeCallback([this, raw] {
            if (autoCompile_) raw->compileRequested = true;
        }, compileDelayMs_);
    }
}

const std::string* App::defineOverride(const std::string& name) const {
    for (const auto& [n, v] : defineOverrides_)
        if (n == name) return &v;
    return nullptr;
}

void App::setDefine(const std::string& name, const std::string& value) {
    bool found = false;
    for (auto& [n, v] : defineOverrides_)
        if (n == name) {
            v = value;
            found = true;
        }
    if (!found) defineOverrides_.push_back({name, value});
    for (auto& p : passes_)
        for (const auto& d : p->defines)
            if (d.name == name) {
                compilePass(*p, "define");
                break;
            }
}

void App::compilePass(Pass& p, const char* reason) {
    const std::string text = p.editor->GetText();
    p.lastCompiled = text;
    p.compileRequested = false;

    // "#ifndef X / #define X v" defaults. If the code's own default changed since last time, the
    // user is editing it by hand: drop the UI override so the code wins.
    auto defines = shader::findOverridableDefines(text);
    for (const auto& d : defines)
        for (const auto& old : p.defines)
            if (old.name == d.name && old.value != d.value)
                std::erase_if(defineOverrides_, [&](const auto& o) { return o.first == d.name; });
    p.defines = std::move(defines);
    const std::string src = shader::applyDefineOverrides(text, p.defines, defineOverrides_);

    shader::TranslateOptions opts;
    if (!p.file.empty()) opts.includeDirs.push_back(p.file.parent_path());
    if (!projectPath_.empty()) opts.includeDirs.push_back(projectPath_.parent_path());
    opts.includeDirs.push_back(configDir() / "includes");

    const auto t0 = std::chrono::steady_clock::now();
    const shader::TranslatedSource tr = shader::translateFragment(src, opts);

    // Reuse an identical program compiled earlier (same final GLSL and UV origin).
    const std::string key = (uvTopLeft_ ? "T" : "B") + tr.source;
    rhi::CompileResult result;
    bool cached = false;
    for (auto it = p.cache.begin(); it != p.cache.end(); ++it)
        if (it->key == key) {
            Pass::CachedProgram hit = std::move(*it);
            p.cache.erase(it);
            result.program = hit.program;
            result.diagnostics = hit.diagnostics;
            p.cache.push_back(std::move(hit));
            cached = true;
            break;
        }
    if (!cached) {
        result = device_->compileFragment(tr, uvTopLeft_);
        if (result.ok() && tr.includeErrors.empty()) {
            p.cache.push_back({key, result.program, result.diagnostics});
            if (p.cache.size() > 48) p.cache.erase(p.cache.begin());
        }
    }
    result.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    p.compileMs = result.milliseconds;
    p.kind = tr.kind;
    p.dialect = tr.dialect;
    p.translated = tr.source;
    p.diagnostics = result.diagnostics;
    for (const auto& e : tr.includeErrors) {
        rhi::Diagnostic d;
        std::smatch m;
        static const std::regex lineRe(R"(^line (\d+): (.*)$)");
        if (std::regex_match(e, m, lineRe)) {
            d.line = std::stoi(m[1].str());
            d.message = m[2].str();
        } else {
            d.message = e;
        }
        p.diagnostics.push_back(d);
    }
    p.includes = tr.includes;
    p.includeTimes.clear();
    for (const auto& inc : p.includes) {
        std::error_code ec;
        p.includeTimes.push_back(fs::last_write_time(inc, ec));
    }

    char ms[32];
    std::snprintf(ms, sizeof(ms), cached ? "%.1f ms, cached" : "%.1f ms", result.milliseconds);
    const std::string label = p.name + (p.file.empty() ? "" : " (" + p.file.filename().string() + ")");

    if (result.ok() && tr.includeErrors.empty()) {
        p.program = result.program;
        p.annotations = shader::parseAnnotations(src);
        for (const auto& inc : p.includes) {
            std::string text;
            if (readFile(inc, text))
                for (auto& [k, v] : shader::parseAnnotations(text)) p.annotations.emplace(k, v);
        }
        syncUniformDefaults(p);
        if (p.history.empty() || p.history.back().source != text) {
            p.history.push_back({nowString(), text});
            if (p.history.size() > 100) p.history.erase(p.history.begin());
            diffDirty_ = true;
        }
        log(Level::Ok, label + " compiled · " + tr.dialect + " · " + ms + " (" + reason + ")");
        for (const auto& d : p.diagnostics)
            if (!d.isError) log(Level::Warn, p.name + " line " + std::to_string(d.line) + ": " + d.message);
    } else {
        int errors = 0;
        for (const auto& d : p.diagnostics) {
            if (d.isError) ++errors;
            std::string where;
            if (d.file > 0 && static_cast<size_t>(d.file) <= tr.includes.size())
                where = tr.includes[static_cast<size_t>(d.file - 1)].filename().string() + " ";
            if (d.line > 0) where += "line " + std::to_string(d.line) + ": ";
            log(d.isError ? Level::Error : Level::Warn, p.name + " " + where + d.message);
        }
        log(Level::Error, label + " failed (" + std::to_string(std::max(errors, 1)) + " error(s)) · keeping last good shader");
    }

    // Mobile (GLSL ES) validation, as a phone's compiler would see the original source.
    p.esCheck = {};
    if (mobileCheck_) {
        const std::size_t h = std::hash<std::string>{}(src) ^ src.size();
        auto hit = std::find_if(p.esCache.begin(), p.esCache.end(), [&](const auto& e) { return e.first == h; });
        if (hit != p.esCache.end()) {
            p.esCheck = hit->second;
        } else {
            p.esCheck = shader::validateEs(src);
            p.esCache.push_back({h, p.esCheck});
            if (p.esCache.size() > 48) p.esCache.erase(p.esCache.begin());
        }
        if (p.esCheck.applicable) {
            if (p.esCheck.ok && p.esCheck.diagnostics.empty())
                log(Level::Ok, p.name + " passes " + p.esCheck.profile + " (mobile) validation");
            for (const auto& d : p.esCheck.diagnostics)
                log(d.isError ? Level::Error : Level::Warn,
                    "[mobile " + p.esCheck.profile + "] " + p.name + " line " + std::to_string(d.line) + ": " + d.message);
        }
    }
    applyDiagnostics(p);
}

void App::applyDiagnostics(Pass& p) {
    p.editor->ClearMarkers();
    for (const auto& d : p.diagnostics) {
        if (d.line <= 0 || d.file != 0) continue;
        const ImU32 numberColor = d.isError ? colors::Error : IM_COL32(255, 200, 90, 255);
        const ImU32 lineColor = d.isError ? IM_COL32(255, 80, 80, 45) : IM_COL32(255, 200, 90, 35);
        p.editor->AddMarker(static_cast<size_t>(d.line - 1), numberColor, lineColor, d.message, d.message);
    }
    for (const auto& d : p.esCheck.diagnostics) {
        if (d.line <= 0) continue;
        const std::string msg = "[mobile] " + d.message;
        p.editor->AddMarker(static_cast<size_t>(d.line - 1), colors::Metal, IM_COL32(255, 159, 67, 40), msg, msg);
    }
}

void App::syncUniformDefaults(const Pass& p) {
    if (!p.program) return;
    for (const auto& u : p.program->uniforms()) {
        if (u.type == rhi::UniformType::Sampler2D || builtinFor(u.name) != Builtin::None) continue;
        if (uniformValues_.count(u.name)) continue;
        UniformState s;
        const auto a = p.annotations.find(u.name);
        const bool annotated = a != p.annotations.end();
        if (annotated && a->second.defaultValue) s.f = *a->second.defaultValue;
        else if (annotated && a->second.kind == shader::Annotation::Kind::Color) s.f = {1, 1, 1, 1};
        else if (annotated && a->second.kind == shader::Annotation::Kind::Slider) s.f.fill(a->second.min);
        s.i = static_cast<int>(s.f[0]);
        // Effect / transition shaders usually take a 0..1 progress; animate it out of the box.
        const std::string ln = lower(u.name);
        if (u.type == rhi::UniformType::Float && !(annotated && a->second.defaultValue) &&
            ln.find("progress") != std::string::npos)
            s.drive = DriveLoop;
        uniformValues_[u.name] = s;
    }
}

std::vector<const rhi::UniformInfo*> App::userUniforms() const {
    std::vector<const rhi::UniformInfo*> out;
    for (const auto& p : passes_) {
        if (!p->program) continue;
        for (const auto& u : p->program->uniforms()) {
            if (u.type == rhi::UniformType::Sampler2D || builtinFor(u.name) != Builtin::None) continue;
            if (std::none_of(out.begin(), out.end(), [&](const auto* x) { return x->name == u.name; }))
                out.push_back(&u);
        }
    }
    return out;
}

const shader::Annotation* App::annotationFor(const std::string& name) const {
    for (const auto& p : passes_) {
        auto it = p->annotations.find(name);
        if (it != p->annotations.end()) return &it->second;
    }
    return nullptr;
}

// ====================================================================== per frame

void App::update() {
    runPendingDialog();
    if (pendingExport_ && frame_ > 5) {  // let shaders compile and media start first
        pendingExport_ = false;
        startExport();
    }
    if (!pendingBatch_.empty() && frame_ > 5) {
        if (loadBatchFolder(pendingBatch_)) startBatch(false);
        if (!batch_.active) quit_ = true;  // nothing to render
        pendingBatch_.clear();
    }
    pollExportFinish();
    pollFfmpegInstall();
    if (batch_.active) stepBatch();

    const double now = glfwGetTime();
    timeDelta_ = now - lastClock_;
    lastClock_ = now;

    if (export_.active) {
        stepExport();
    } else if (!batch_.active) {  // between batch jobs: no hot reload, edits or preview of the user's project
        if (playing_) {
            time_ += timeDelta_;
            ++frame_;
            if (!loop_ && time_ >= duration_) {
                time_ = duration_;
                playing_ = false;
            }
        }
        pollTimer_ += timeDelta_;
        if (pollTimer_ > 0.5) {
            pollTimer_ = 0.0;
            pollFiles();
        }
        for (auto& p : passes_)
            if (p->compileRequested && p->editor->GetText() != p->lastCompiled) compilePass(*p, "edit");
            else p->compileRequested = false;

        // Loop wrap counts as a jump so file videos restart with the timeline.
        static double lastTimeline = 0.0;
        const double tl = timelineTime();
        if (tl + 1e-3 < lastTimeline) timeJumped_ = true;
        lastTimeline = tl;
        textures_->update(tl, playing_, timeJumped_);
        timeJumped_ = false;
        renderFrame();
    }

    if (assistant_.poll()) {
        if (!assistant_.error.empty()) log(Level::Error, "AI: " + assistant_.error);
        else if (!assistant_.reply.empty()) log(Level::Ok, "AI reply received");
    }

    // Profiler history.
    cpuHistory_.push_back(static_cast<float>(timeDelta_ * 1000.0));
    float gpu = 0.0f;
    for (size_t i = 0; i < passes_.size(); ++i) gpu += static_cast<float>(device_->timerMs(static_cast<int>(i)));
    gpuHistory_.push_back(gpu);
    while (cpuHistory_.size() > 240) cpuHistory_.pop_front();
    while (gpuHistory_.size() > 240) gpuHistory_.pop_front();

    handleShortcuts();
}

void App::outputSize(int& w, int& h) const {
    if (export_.active) {
        w = export_.width;
        h = export_.height;
    } else if (overlay_ || outputMode_ == 0) {
        w = desiredW_;
        h = desiredH_;
    } else {
        w = kOutputs[outputMode_].w;
        h = kOutputs[outputMode_].h;
    }
    w = std::clamp(w, 1, 8192);
    h = std::clamp(h, 1, 8192);
}

void App::ensureTargets(int w, int h) {
    for (auto& p : passes_) {
        const rhi::Format fmt = p->isImage ? rhi::Format::RGBA8 : rhi::Format::RGBA16F;
        for (auto& t : p->targets) {
            if (!t) {
                t = device_->createRenderTarget(w, h, fmt);
                device_->clear(*t);
            } else if (t->width() != w || t->height() != h) {
                t->resize(w, h);
                device_->clear(*t);
            }
        }
    }
}

void App::renderFrame() {
    int w, h;
    outputSize(w, h);
    ensureTargets(w, h);
    std::vector<rhi::UniformValue> values;
    std::vector<rhi::TextureBinding> bindings;
    for (size_t i = 0; i < passes_.size(); ++i) {
        Pass& p = *passes_[i];
        if (!p.enabled || !p.program) continue;
        values.clear();
        bindings.clear();
        collectBindings(p, values, bindings);
        const int write = 1 - p.latest;
        device_->beginTimer(static_cast<int>(i));
        device_->draw(*p.program, *p.targets[write], values, bindings);
        device_->endTimer(static_cast<int>(i));
        p.latest = write;
    }
}

const rhi::Texture* App::textureForSlot(int slot) const {
    auto& slots = textures_->slots();
    if (slot >= 0 && static_cast<size_t>(slot) < slots.size() && slots[static_cast<size_t>(slot)].texture)
        return slots[static_cast<size_t>(slot)].texture.get();
    return &textures_->fallback();
}

const rhi::Texture* App::textureForBuffer(int bufferIndex) const {
    for (const auto& p : passes_)
        if (!p->isImage && p->bufferIndex() == bufferIndex && p->targets[p->latest])
            return &p->targets[p->latest]->color();
    return &textures_->fallback();
}

void App::collectBindings(const Pass& p, std::vector<rhi::UniformValue>& values,
                          std::vector<rhi::TextureBinding>& bindings) {
    static const std::regex texRe(R"(^u_tex(\d+)$)");
    static const std::regex channelRe(R"(^iChannel(\d+)$)");
    static const std::regex bufferRe(R"(^(?:u_|i)?[bB]uffer([A-Da-d])$)");
    static const std::regex prevRe(R"(^(?:u_prev|u_previous|u_feedback|u_backbuffer|backbuffer|iPrev)$)");

    int nextUnit = 0;
    int autoSlot = 0;
    const int maxUnits = device_->maxTextureUnits();
    const auto& out = *p.targets[p.latest];
    const float rw = static_cast<float>(out.width());
    const float rh = static_cast<float>(out.height());

    for (const auto& u : p.program->uniforms()) {
        if (u.type == rhi::UniformType::Sampler2D) {
            if (nextUnit >= maxUnits) continue;
            const rhi::Texture* tex = nullptr;
            std::smatch m;
            if (std::regex_match(u.name, m, channelRe)) {
                const int n = std::stoi(m[1].str());
                const int route = n < 4 ? p.channels[static_cast<size_t>(n)] : -1;
                if (route < 0) tex = textureForSlot(n);
                else if (route < 100) tex = textureForSlot(route);
                else tex = textureForBuffer(route - 100);
            } else if (std::regex_match(u.name, m, texRe)) {
                tex = textureForSlot(std::stoi(m[1].str()));
            } else if (std::regex_match(u.name, m, bufferRe)) {
                tex = textureForBuffer(std::toupper(static_cast<unsigned char>(m[1].str()[0])) - 'A');
            } else if (std::regex_match(u.name, prevRe)) {
                tex = &p.targets[p.latest]->color();  // this pass's previous frame
            } else if (u.name == "u_from" || u.name == "from") {
                tex = textureForSlot(transitionFrom_);
            } else if (u.name == "u_to" || u.name == "to") {
                tex = textureForSlot(transitionTo_);
            } else {
                tex = textureForSlot(autoSlot++);
            }
            bindings.push_back({u.location, nextUnit++, tex});
            continue;
        }

        rhi::UniformValue v;
        v.location = u.location;
        v.type = u.type;
        float lvl, bass, mid, high;
        switch (builtinFor(u.name)) {
            case Builtin::Time: v.f[0] = static_cast<float>(time_); break;
            case Builtin::TimeDelta: v.f[0] = static_cast<float>(timeDelta_); break;
            case Builtin::Frame: v.i = frame_; v.f[0] = static_cast<float>(frame_); break;
            case Builtin::Resolution: v.f[0] = rw; v.f[1] = rh; v.f[2] = 1.0f; break;
            case Builtin::Mouse: std::copy(mouse_.begin(), mouse_.end(), v.f); break;
            case Builtin::TexCount:
                v.i = static_cast<int>(textures_->slots().size());
                v.f[0] = static_cast<float>(v.i);
                break;
            case Builtin::Date: {
                const std::time_t t = std::time(nullptr);
                std::tm tm{};
#if defined(_WIN32)
                localtime_s(&tm, &t);
#else
                localtime_r(&t, &tm);
#endif
                v.f[0] = static_cast<float>(tm.tm_year + 1900);
                v.f[1] = static_cast<float>(tm.tm_mon);
                v.f[2] = static_cast<float>(tm.tm_mday);
                v.f[3] = static_cast<float>(tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec);
                break;
            }
            case Builtin::AudioLevel: textures_->audioLevels(lvl, bass, mid, high); v.f[0] = lvl; break;
            case Builtin::Bass: textures_->audioLevels(lvl, bass, mid, high); v.f[0] = bass; break;
            case Builtin::Mid: textures_->audioLevels(lvl, bass, mid, high); v.f[0] = mid; break;
            case Builtin::High: textures_->audioLevels(lvl, bass, mid, high); v.f[0] = high; break;
            case Builtin::Ratio: v.f[0] = rh > 0 ? rw / rh : 1.0f; break;
            case Builtin::None: {
                auto it = uniformValues_.find(u.name);
                if (it == uniformValues_.end()) break;
                const UniformState& s = it->second;
                for (int c = 0; c < 4; ++c) v.f[c] = evalUniform(s, c);
                v.i = (s.drive == DriveKeys) ? static_cast<int>(std::lround(v.f[0])) : s.i;
                break;
            }
        }
        if (v.type == rhi::UniformType::Float && builtinFor(u.name) == Builtin::Frame) v.f[0] = static_cast<float>(frame_);
        values.push_back(v);
    }
}

void App::presentSecondary() {
    if (!outputWindow_.isOpen()) return;
    const Pass& img = imagePass();
    if (!img.targets[img.latest]) return;
    auto* rt = img.targets[img.latest].get();
    outputWindow_.present(rt->color().nativeHandle(), rt->width(), rt->height(), window_);
}

}  // namespace app
