// Projects (.ssproj), shader files, app settings, native dialogs, templates, hot reload.
#include "app/App.h"

#include "app/Common.h"
#include "app/Templates.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <nfd.h>
#include <nlohmann/json.hpp>

#include <cstdlib>

namespace app {

using json = nlohmann::json;

namespace {

std::string relOrAbs(const fs::path& p, const fs::path& base) {
    if (p.empty()) return {};
    if (!base.empty()) {
        std::error_code ec;
        const fs::path rel = fs::relative(p, base, ec);
        if (!ec && !rel.empty() && rel.generic_string().rfind("..", 0) != 0) return rel.generic_string();
    }
    return p.generic_string();
}

fs::path resolve(const std::string& s, const fs::path& base) {
    if (s.empty()) return {};
    fs::path p(s);
    if (p.is_relative() && !base.empty()) p = base / p;
    return p.lexically_normal();
}

fs::path homeDir() {
#if defined(_WIN32)
    if (const char* h = std::getenv("USERPROFILE")) return h;
#else
    if (const char* h = std::getenv("HOME")) return h;
#endif
    return ".";
}

}  // namespace

fs::path App::configDir() const {
#if defined(__APPLE__)
    return homeDir() / "Library" / "Application Support" / "ShaderStudio";
#elif defined(_WIN32)
    if (const char* a = std::getenv("APPDATA")) return fs::path(a) / "ShaderStudio";
    return homeDir() / "ShaderStudio";
#else
    if (const char* x = std::getenv("XDG_CONFIG_HOME")) return fs::path(x) / "ShaderStudio";
    return homeDir() / ".config" / "ShaderStudio";
#endif
}

// ====================================================================== settings

void App::loadSettings() {
    std::string text;
    if (!readFile(configDir() / "settings.json", text)) return;
    try {
        const json j = json::parse(text);
        for (const auto& r : j.value("recent", json::array())) recent_.emplace_back(r.get<std::string>());
        lastDir_ = j.value("lastDir", std::string());
        if (j.contains("apiKey")) {
            assistant_.setApiKey(j["apiKey"].get<std::string>());
            aiRememberKey_ = true;
        }
        assistant_.model = j.value("model", std::string());
        compileDelayMs_ = std::clamp(j.value("compileDelayMs", 120), 0, 2000);
        std::snprintf(cameraDevice_, sizeof(cameraDevice_), "%s", j.value("cameraDevice", std::string(cameraDevice_)).c_str());
        std::snprintf(micDevice_, sizeof(micDevice_), "%s", j.value("micDevice", std::string(micDevice_)).c_str());
    } catch (...) {
    }
}

void App::saveSettings() const {
    json j;
    j["recent"] = json::array();
    for (const auto& r : recent_) j["recent"].push_back(r.string());
    j["lastDir"] = lastDir_.string();
    if (aiRememberKey_ && assistant_.hasKey()) j["apiKey"] = assistant_.apiKey();
    j["model"] = assistant_.model;
    j["compileDelayMs"] = compileDelayMs_;
    j["cameraDevice"] = std::string(cameraDevice_);
    j["micDevice"] = std::string(micDevice_);
    std::error_code ec;
    fs::create_directories(configDir(), ec);
    const fs::path path = configDir() / "settings.json";
    writeFile(path, j.dump(2));
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
}

void App::addRecent(const fs::path& p) {
    recent_.erase(std::remove(recent_.begin(), recent_.end(), p), recent_.end());
    recent_.insert(recent_.begin(), p);
    if (recent_.size() > 10) recent_.resize(10);
    saveSettings();
}

// ====================================================================== projects

void App::newProject(int templateIndex) {
    const auto& all = templates();
    const Template& t = all[static_cast<size_t>(std::clamp(templateIndex, 0, static_cast<int>(all.size()) - 1))];
    passes_.clear();
    uniformValues_.clear();
    defineOverrides_.clear();
    for (const auto& [name, src] : t.passes) addPass(name, src);
    activePass_ = passes_.size() - 1;
    selectTab_ = true;
    projectPath_.clear();
    time_ = 0.0;
    frame_ = 0;
    timeJumped_ = true;
    compileAll("new");
    log(Level::Info, std::string("New project from template: ") + t.name);
    updateTitle();
}

json App::projectToJson(const fs::path& base) const {
    json j;
    j["app"] = "ShaderStudio";
    j["version"] = 1;
    j["session"] = projectPath_.string();
    j["settings"] = {{"uvTopLeft", uvTopLeft_},         {"preset", static_cast<int>(preset_)},
                     {"mobileCheck", mobileCheck_},     {"outputMode", outputMode_},
                     {"duration", duration_},           {"loop", loop_},
                     {"transitionFrom", transitionFrom_}, {"transitionTo", transitionTo_},
                     {"activePass", activePass_}};
    j["defines"] = json::object();
    for (const auto& [name, value] : defineOverrides_) j["defines"][name] = value;
    j["passes"] = json::array();
    for (const auto& p : passes_) {
        json jp = {{"name", p->name},
                   {"enabled", p->enabled},
                   {"source", p->editor->GetText()},
                   {"file", relOrAbs(p->file, base)},
                   {"channels", p->channels}};
        json hist = json::array();
        const size_t from = p->history.size() > 30 ? p->history.size() - 30 : 0;
        for (size_t i = from; i < p->history.size(); ++i)
            hist.push_back({{"time", p->history[i].time}, {"source", p->history[i].source}});
        jp["history"] = hist;
        j["passes"].push_back(jp);
    }
    j["textures"] = json::array();
    for (auto& s : textures_->slots()) {
        j["textures"].push_back({{"kind", static_cast<int>(s.kind)},
                                 {"name", s.name},
                                 {"path", relOrAbs(s.path, base)},
                                 {"device", s.device},
                                 {"flipY", s.flipY},
                                 {"monitorAudio", s.monitorAudio},
                                 {"filter", static_cast<int>(s.sampler.filter)},
                                 {"wrapU", static_cast<int>(s.sampler.wrapU)},
                                 {"wrapV", static_cast<int>(s.sampler.wrapV)}});
    }
    j["uniforms"] = json::object();
    for (const auto& [name, s] : uniformValues_) {
        json keys = json::array();
        for (const auto& k : s.keys) keys.push_back({{"t", k.t}, {"v", k.v}, {"ease", k.ease}});
        j["uniforms"][name] = {{"f", s.f}, {"i", s.i}, {"drive", s.drive}, {"loopSeconds", s.loopSeconds}, {"keys", keys}};
    }
    return j;
}

bool App::projectFromJson(const json& j, const fs::path& base) {
    if (j.value("app", "") != "ShaderStudio") return false;
    // An autosave remembers which project it came from; includes resolve relative to that project.
    sessionProject_ = resolve(j.value("session", ""), {});
    if (!sessionProject_.empty() && base == configDir()) projectPath_ = sessionProject_;
    const json st = j.value("settings", json::object());
    uvTopLeft_ = st.value("uvTopLeft", false);
    preset_ = static_cast<Preset>(st.value("preset", 0));
    mobileCheck_ = st.value("mobileCheck", false);
    outputMode_ = std::clamp(st.value("outputMode", 0), 0, kOutputCount - 1);
    duration_ = st.value("duration", 10.0f);
    loop_ = st.value("loop", true);
    transitionFrom_ = st.value("transitionFrom", 0);
    transitionTo_ = st.value("transitionTo", 1);
    defineOverrides_.clear();
    if (const json defs = j.value("defines", json::object()); defs.is_object())
        for (const auto& [name, value] : defs.items())
            if (value.is_string()) defineOverrides_.push_back({name, value.get<std::string>()});

    passes_.clear();
    for (const auto& jp : j.value("passes", json::array())) {
        Pass& p = addPass(jp.value("name", "Image"), jp.value("source", ""));
        p.enabled = jp.value("enabled", true);
        if (jp.contains("channels")) p.channels = jp["channels"].get<std::array<int, 4>>();
        p.file = resolve(jp.value("file", ""), base);
        std::error_code ec;
        if (!p.file.empty() && fs::exists(p.file, ec)) {
            std::string text;
            if (readFile(p.file, text) && text != p.editor->GetText()) p.editor->SetText(text);
            p.mtime = fs::last_write_time(p.file, ec);
        }
        p.savedUndo = p.editor->GetUndoIndex();
        for (const auto& h : jp.value("history", json::array()))
            p.history.push_back({h.value("time", ""), h.value("source", "")});
    }
    if (passes_.empty()) addPass("Image", templates()[0].passes[0].second);
    activePass_ = std::min<size_t>(st.value("activePass", passes_.size() - 1), passes_.size() - 1);
    selectTab_ = true;

    uniformValues_.clear();
    const json uniforms = j.value("uniforms", json::object());  // keep alive: items() is a view
    for (const auto& [name, ju] : uniforms.items()) {
        UniformState s;
        if (ju.contains("f")) s.f = ju["f"].get<std::array<float, 4>>();
        s.i = ju.value("i", 0);
        s.drive = ju.value("drive", 0);
        s.loopSeconds = ju.value("loopSeconds", 5.0f);
        for (const auto& k : ju.value("keys", json::array()))
            s.keys.push_back({k.value("t", 0.0f), k["v"].get<std::array<float, 4>>(), k.value("ease", 3)});
        uniformValues_[name] = s;
    }

    textures_->clear();
    textures_->setFlipAll(!uvTopLeft_);
    for (const auto& jt : j.value("textures", json::array())) {
        const auto kind = static_cast<textures::SourceKind>(jt.value("kind", 0));
        const fs::path path = resolve(jt.value("path", ""), base);
        const std::string device = jt.value("device", "");
        std::string err;
        switch (kind) {
            case textures::SourceKind::Image: err = textures_->addImage(path); break;
            case textures::SourceKind::Video: err = textures_->addVideo(path); break;
            case textures::SourceKind::AudioFile: err = textures_->addAudioFile(path); break;
            case textures::SourceKind::Camera: err = textures_->addCamera(device); break;
            case textures::SourceKind::Microphone: err = textures_->addMicrophone(device); break;
            case textures::SourceKind::Noise: textures_->addNoise(); break;
            case textures::SourceKind::Checker: textures_->addChecker(); break;
        }
        if (!err.empty()) {
            log(Level::Error, err);
            continue;
        }
        auto& s = textures_->slots().back();
        s.sampler.filter = static_cast<rhi::Filter>(jt.value("filter", static_cast<int>(s.sampler.filter)));
        s.sampler.wrapU = static_cast<rhi::Wrap>(jt.value("wrapU", static_cast<int>(s.sampler.wrapU)));
        s.sampler.wrapV = static_cast<rhi::Wrap>(jt.value("wrapV", static_cast<int>(s.sampler.wrapV)));
        textures_->applySampler(textures_->slots().size() - 1);
        const bool flip = jt.value("flipY", s.flipY);
        if (flip != s.flipY) {
            s.flipY = flip;
            textures_->reload(textures_->slots().size() - 1);
        }
        s.monitorAudio = jt.value("monitorAudio", false);
    }
    time_ = 0.0;
    frame_ = 0;
    timeJumped_ = true;
    compileAll("open");
    return true;
}

bool App::openProject(const fs::path& path) {
    std::string text;
    if (!readFile(path, text)) {
        log(Level::Error, "Could not open " + path.string());
        return false;
    }
    const fs::path previous = projectPath_;
    projectPath_ = path;  // set first: #include resolves relative to the project folder
    try {
        if (!projectFromJson(json::parse(text), path.parent_path())) {
            log(Level::Error, path.filename().string() + " is not a Shader Studio project");
            projectPath_ = previous;
            return false;
        }
    } catch (const std::exception& e) {
        log(Level::Error, "Could not read " + path.filename().string() + ": " + e.what());
        projectPath_ = previous;
        return false;
    }
    if (path.filename() != "autosave.ssproj") log(Level::Info, "Opened project " + path.string());
    updateTitle();
    return true;
}

bool App::saveProject(const fs::path& path) {
    // Linked pass files are written too, so external editors stay in sync.
    for (auto& p : passes_)
        if (!p->file.empty()) saveShaderFromPass(*p, p->file);
    const fs::path previous = projectPath_;
    if (path.filename() != "autosave.ssproj") projectPath_ = path;
    const bool ok = writeFile(path, projectToJson(path.parent_path()).dump(2));
    if (!ok) {
        projectPath_ = previous;
        log(Level::Error, "Could not write " + path.string());
        return false;
    }
    if (path.filename() != "autosave.ssproj") {
        log(Level::Ok, "Saved project " + path.string());
        addRecent(path);
        updateTitle();
    }
    return true;
}

// ====================================================================== shader files

bool App::openShaderIntoPass(Pass& p, const fs::path& path) {
    std::string text;
    if (!readFile(path, text)) {
        log(Level::Error, "Could not open " + path.string());
        return false;
    }
    p.editor->SetText(text);
    p.file = path;
    std::error_code ec;
    p.mtime = fs::last_write_time(path, ec);
    p.savedUndo = p.editor->GetUndoIndex();
    p.externalWarned = false;
    log(Level::Info, "Opened " + path.string() + " into " + p.name);
    compilePass(p, "open");
    updateTitle();
    return true;
}

bool App::saveShaderFromPass(Pass& p, const fs::path& path) {
    if (!writeFile(path, p.editor->GetText())) {
        log(Level::Error, "Could not write " + path.string());
        return false;
    }
    p.file = path;
    std::error_code ec;
    p.mtime = fs::last_write_time(path, ec);
    p.savedUndo = p.editor->GetUndoIndex();
    p.externalWarned = false;
    return true;
}

void App::pollFiles() {
    for (auto& pp : passes_) {
        Pass& p = *pp;
        bool recompile = false;
        if (!p.file.empty()) {
            std::error_code ec;
            const auto t = fs::last_write_time(p.file, ec);
            if (!ec && t != p.mtime) {
                if (p.editor->GetUndoIndex() != p.savedUndo) {
                    if (!p.externalWarned) {
                        log(Level::Warn, p.file.filename().string() + " changed on disk; " + p.name +
                                             " has unsaved edits, not reloading");
                        p.externalWarned = true;
                    }
                } else {
                    std::string text;
                    if (readFile(p.file, text)) {
                        p.editor->SetText(text);
                        p.savedUndo = p.editor->GetUndoIndex();
                        p.mtime = t;
                        recompile = true;
                    }
                }
            }
        }
        for (size_t i = 0; i < p.includes.size() && i < p.includeTimes.size(); ++i) {
            std::error_code ec;
            if (fs::last_write_time(p.includes[i], ec) != p.includeTimes[i] && !ec) recompile = true;
        }
        if (recompile) compilePass(p, "file changed on disk");
    }
    for (const auto& name : textures_->pollHotReload()) log(Level::Info, "Reloaded texture " + name);
}

void App::updateTitle() {
    const std::string name = projectPath_.empty() ? "untitled" : projectPath_.filename().string();
    glfwSetWindowTitle(window_, ("Shader Studio — " + name).c_str());
}

void App::addTextureFile(const fs::path& p) {
    const std::string err = textures_->addFile(p);
    if (!err.empty()) {
        log(Level::Error, err);
        return;
    }
    const auto& s = textures_->slots().back();
    log(Level::Ok, std::string("Added ") + textures::kindName(s.kind) + " " + p.filename().string() + " -> u_tex" +
                       std::to_string(textures_->slots().size() - 1));
    lastDir_ = p.parent_path();
}

void App::onFilesDropped(int count, const char** paths) {
    for (int i = 0; i < count; ++i) {
        const fs::path p = paths[i];
        if (lower(p.extension().string()) == ".ssproj") openProject(p);
        else if (isShaderFile(p)) openShaderIntoPass(activePass(), p);
        else addTextureFile(p);
    }
}

void App::handleArgs(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--template" && i + 1 < argc) {
            newProject(std::atoi(argv[++i]));
        } else if (a == "--preset" && i + 1 < argc) {
            const std::string p = lower(argv[++i]);
            applyPreset(p == "android" ? Preset::Android : p == "iphone" ? Preset::iPhone : Preset::Desktop);
        } else {
            const char* one[] = {argv[i]};
            onFilesDropped(1, one);
        }
    }
}

// ====================================================================== dialogs

void App::runPendingDialog() {
    const Dialog dialog = pendingDialog_;
    pendingDialog_ = Dialog::None;
    if (dialog == Dialog::None) return;

    static const nfdu8filteritem_t kProject[] = {{"Shader Studio project", "ssproj"}};
    static const nfdu8filteritem_t kShader[] = {{"GLSL shader", "frag,fs,glsl,txt"}};
    static const nfdu8filteritem_t kMedia[] = {
        {"Images, video, audio", "png,jpg,jpeg,bmp,tga,gif,psd,hdr,mp4,mov,m4v,mkv,webm,avi,mp3,wav,m4a,aac,flac,ogg"}};
    const std::string dirStr = lastDir_.string();
    const char* dir = dirStr.empty() ? nullptr : dirStr.c_str();

    auto single = [&](bool save, const nfdu8filteritem_t* f, const char* defName) -> fs::path {
        nfdu8char_t* out = nullptr;
        const nfdresult_t r = save ? NFD_SaveDialogU8(&out, f, 1, dir, defName) : NFD_OpenDialogU8(&out, f, 1, dir);
        if (r != NFD_OKAY) return {};
        fs::path p = out;
        NFD_FreePathU8(out);
        lastDir_ = p.parent_path();
        return p;
    };

    switch (dialog) {
        case Dialog::OpenProject:
            if (auto p = single(false, kProject, nullptr); !p.empty() && openProject(p)) addRecent(p);
            break;
        case Dialog::SaveProjectAs:
            if (auto p = single(true, kProject, "project.ssproj"); !p.empty()) {
                if (p.extension() != ".ssproj") p += ".ssproj";
                saveProject(p);
            }
            break;
        case Dialog::OpenShader:
            if (auto p = single(false, kShader, nullptr); !p.empty()) openShaderIntoPass(activePass(), p);
            break;
        case Dialog::SaveShaderAs: {
            Pass& ap = activePass();
            const std::string name = ap.file.empty() ? (ap.isImage ? "image.frag" : "buffer.frag") : ap.file.filename().string();
            if (auto p = single(true, kShader, name.c_str()); !p.empty() && saveShaderFromPass(ap, p))
                log(Level::Ok, "Saved " + p.string());
            break;
        }
        case Dialog::AddTextures: {
            const nfdpathset_t* set = nullptr;
            if (NFD_OpenDialogMultipleU8(&set, kMedia, 1, dir) == NFD_OKAY) {
                nfdpathsetsize_t count = 0;
                NFD_PathSet_GetCount(set, &count);
                for (nfdpathsetsize_t i = 0; i < count; ++i) {
                    nfdu8char_t* path = nullptr;
                    if (NFD_PathSet_GetPathU8(set, i, &path) != NFD_OKAY) continue;
                    const fs::path p = path;
                    NFD_PathSet_FreePathU8(path);
                    addTextureFile(p);
                }
                NFD_PathSet_Free(set);
            }
            break;
        }
        case Dialog::ExportPath: {
            if (export_.format == 1) {
                nfdu8char_t* out = nullptr;
                if (NFD_PickFolderU8(&out, dir) == NFD_OKAY) {
                    export_.output = out;
                    NFD_FreePathU8(out);
                }
            } else {
                static const nfdu8filteritem_t kPng[] = {{"PNG image", "png"}};
                static const nfdu8filteritem_t kMp4[] = {{"MP4 video", "mp4"}};
                static const nfdu8filteritem_t kMov[] = {{"QuickTime ProRes", "mov"}};
                static const nfdu8filteritem_t kGif[] = {{"Animated GIF", "gif"}};
                static const nfdu8filteritem_t kWebp[] = {{"Animated WebP", "webp"}};
                static const nfdu8filteritem_t* const kFilters[] = {kPng, kPng, kMp4, kMov, kGif, kWebp};
                static const char* const kDefaults[] = {"frame.png", "frame.png", "render.mp4", "render.mov",
                                                        "render.gif", "render.webp"};
                const nfdu8filteritem_t* f = kFilters[export_.format];
                const char* def = kDefaults[export_.format];
                if (auto p = single(true, f, def); !p.empty()) export_.output = p;
            }
            break;
        }
        default: break;
    }
    if (const char* err = NFD_GetError()) {
        log(Level::Error, std::string("File dialog: ") + err);
        NFD_ClearError();
    }
    // The modal dialog swallows the mouse-up event; release buttons so ImGui doesn't think they're held.
    ImGuiIO& io = ImGui::GetIO();
    for (int b = 0; b < ImGuiMouseButton_COUNT; ++b) io.AddMouseButtonEvent(b, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    io.AddKeyEvent(ImGuiMod_Super, false);
    io.AddKeyEvent(ImGuiMod_Shift, false);
    io.AddKeyEvent(ImGuiMod_Alt, false);
}

}  // namespace app
