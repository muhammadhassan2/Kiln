#pragma once

#include "ai/Assistant.h"
#include "app/OutputWindow.h"
#include "app/Theme.h"
#include "media/Process.h"
#include "rhi/RHI.h"
#include "shader/Annotations.h"
#include "shader/CrossCompile.h"
#include "shader/Translate.h"
#include "textures/TextureLibrary.h"

#include <TextDiff.h>
#include <TextEditor.h>
#include <nlohmann/json_fwd.hpp>

#include <array>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct GLFWwindow;

namespace app {

namespace fs = std::filesystem;

// ------------------------------------------------------------------ project model

struct Snapshot {
    std::string time;
    std::string source;
};

// One render pass: Buffer A-D (feedback-capable, RGBA16F) or the final Image pass.
struct Pass {
    std::string name;  // "Buffer A".."Buffer D" or "Image"
    bool isImage = false;
    bool enabled = true;
    std::unique_ptr<TextEditor> editor;

    fs::path file;  // optional linked file (hot-reloaded)
    fs::file_time_type mtime{};
    size_t savedUndo = 0;
    bool externalWarned = false;

    std::shared_ptr<rhi::Program> program;  // last good program

    // Recently compiled programs, keyed by the final GLSL: switching back to a variant you already
    // compiled (e.g. EFFECT_ID 3 -> 7 -> 3) reuses it instead of compiling again.
    struct CachedProgram {
        std::string key;
        std::shared_ptr<rhi::Program> program;
        std::vector<rhi::Diagnostic> diagnostics;
    };
    std::vector<CachedProgram> cache;  // most recent last
    std::vector<std::pair<std::size_t, shader::EsCheck>> esCache;  // mobile check per source hash
    std::vector<shader::DefineInfo> defines;  // "#ifndef X / #define X v" pairs found in the source
    std::string lastCompiled;
    bool compileRequested = true;
    double compileMs = 0.0;
    shader::SourceKind kind = shader::SourceKind::Modern;
    std::string dialect;
    std::string translated;  // for the Metal panel
    std::vector<rhi::Diagnostic> diagnostics;
    std::vector<fs::path> includes;
    std::vector<fs::file_time_type> includeTimes;
    shader::AnnotationMap annotations;
    shader::EsCheck esCheck;

    // Ping-pong targets: `latest` holds the most recent frame; rendering writes the other one.
    std::unique_ptr<rhi::RenderTarget> targets[2];
    int latest = 0;

    // iChannel0..3 routing: -1 = texture slot N (default), 0..99 = texture slot, 100+k = Buffer k.
    std::array<int, 4> channels{-1, -1, -1, -1};

    std::vector<Snapshot> history;

    rhi::RenderTarget* output() const { return targets[latest].get(); }
    int bufferIndex() const { return isImage ? -1 : name.back() - 'A'; }
};

struct Keyframe {
    float t = 0.0f;
    std::array<float, 4> v{};
    int ease = 3;  // 0 Linear, 1 Ease in, 2 Ease out, 3 Ease in-out, 4 Step
};

enum Drive { DriveManual = 0, DriveTime = 1, DriveLoop = 2, DriveKeys = 3, DriveVideo = 4 };

struct UniformState {
    std::array<float, 4> f{};
    int i = 0;
    int drive = DriveManual;
    float loopSeconds = 5.0f;
    std::vector<Keyframe> keys;
};

enum class Preset { Desktop, Android, iPhone };

// ------------------------------------------------------------------ app

class App {
public:
    App(GLFWwindow* window, Fonts fonts);
    ~App();

    void update();   // after ImGui::NewFrame(): time, media, compile, render
    void drawUI();   // after update(): all panels
    void presentSecondary();  // after the main window swapped: second output window

    void onFilesDropped(int count, const char** paths);
    // Command line: shader/project/media paths, or --template <n>.
    void handleArgs(int argc, char** argv);
    bool quitRequested() const { return quit_; }
    void shutdown();  // autosave + stop media

private:
    enum class Level { Info, Ok, Warn, Error };
    struct LogEntry {
        std::string time;
        Level level;
        std::string text;
    };

    // ---- passes / compile (App.cpp)
    Pass& addPass(const std::string& name, const std::string& source);
    Pass* findPass(const std::string& name);
    Pass& imagePass();
    Pass& activePass();
    void removePass(size_t index);
    std::unique_ptr<TextEditor> makeEditor();
    void compilePass(Pass& p, const char* reason);
    void compileAll(const char* reason);
    void applyDiagnostics(Pass& p);
    void syncUniformDefaults(const Pass& p);
    std::vector<const rhi::UniformInfo*> userUniforms() const;
    const shader::Annotation* annotationFor(const std::string& name) const;
    void setDefine(const std::string& name, const std::string& value);  // override + compile now
    const std::string* defineOverride(const std::string& name) const;
    void applyCompileDelay();  // re-apply compileDelayMs_ to every editor

    // ---- rendering (App.cpp)
    void outputSize(int& w, int& h) const;
    void ensureTargets(int w, int h);
    void renderFrame();
    void collectBindings(const Pass& p, std::vector<rhi::UniformValue>& values,
                         std::vector<rhi::TextureBinding>& bindings);
    const rhi::Texture* textureForSlot(int slot) const;
    const rhi::Texture* textureForBuffer(int bufferIndex) const;

    // ---- time / animation (AppTimeline.cpp)
    double timelineTime() const;
    float evalUniform(const UniformState& s, int component) const;
    static float ease(int type, float x);
    void addKeyframe(const std::string& name, UniformState& s);
    void drawTimeline();
    void drawTransition();
    void applyPreset(Preset p);

    // ---- files / projects / settings (AppFiles.cpp)
    void newProject(int templateIndex);
    bool openProject(const fs::path& path);
    bool saveProject(const fs::path& path);
    nlohmann::json projectToJson(const fs::path& base) const;
    bool projectFromJson(const nlohmann::json& j, const fs::path& base);
    bool openShaderIntoPass(Pass& p, const fs::path& path);
    bool saveShaderFromPass(Pass& p, const fs::path& path);
    void pollFiles();
    void loadSettings();
    void saveSettings() const;
    void addRecent(const fs::path& p);
    void updateTitle();
    void runPendingDialog();
    void addTextureFile(const fs::path& p);
    fs::path configDir() const;

    // ---- UI (AppUI.cpp)
    void handleShortcuts();
    void drawMenuBar();
    void drawDockspace();
    void drawEditor();
    void drawConsole();
    void drawUniforms();
    void drawDefines();
    void drawOverlay();
    void setOverlay(bool on);
    void drawChannelPopup(Pass& p);
    void setupAutocomplete(TextEditor& e);

    // ---- preview / inspector (AppPreview.cpp)
    void drawPreview();
    void ensureViewProgram();

    // ---- textures (AppTextures.cpp)
    void drawTextures();

    // ---- tools (AppTools.cpp)
    void drawExport();
    void startExport();
    void stepExport();
    void finishExport(bool cancelled);
    void drawProfiler();
    void drawHistory();
    void drawCrossCompile();
    void drawLibrary();
    void refreshLibrary();
    void saveToLibrary(const std::string& name);
    void writeThumbnail(const fs::path& png);
    void drawAI();
    void drawOutputWindowMenu();

    void log(Level level, std::string text);

    // ------------------------------------------------------------------ state
    GLFWwindow* window_;
    Fonts fonts_;
    std::unique_ptr<rhi::Device> device_;
    std::unique_ptr<textures::TextureLibrary> textures_;

    std::vector<std::unique_ptr<Pass>> passes_;  // buffers first (A..D), Image last
    size_t activePass_ = 0;
    bool selectTab_ = true;  // ask the editor tab bar to select activePass_
    TextEditor::Palette editorPalette_;
    TextEditor::AutoCompleteConfig autocomplete_;

    std::unordered_map<std::string, UniformState> uniformValues_;
    std::vector<LogEntry> log_;
    bool scrollLog_ = false;

    // project
    fs::path projectPath_;
    fs::path sessionProject_;  // project path recorded inside the autosave
    std::vector<fs::path> recent_;
    bool uvTopLeft_ = false;
    Preset preset_ = Preset::Desktop;
    bool mobileCheck_ = false;
    int outputMode_ = 0;  // 0 = fit panel
    int desiredW_ = 1280, desiredH_ = 720;
    int transitionFrom_ = 0, transitionTo_ = 1;

    // playback / timeline
    bool playing_ = true;
    double time_ = 0.0;
    double timeDelta_ = 0.0;
    int frame_ = 0;
    double lastClock_ = 0.0;
    double pollTimer_ = 0.0;
    float duration_ = 10.0f;
    bool loop_ = true;
    bool timeJumped_ = true;
    std::array<float, 4> mouse_{};
    std::string selectedTrack_;
    int selectedKey_ = -1;

    // compile
    bool autoCompile_ = true;
    int compileDelayMs_ = 120;  // pause after typing before auto-compile
    std::vector<std::pair<std::string, std::string>> defineOverrides_;  // per project

    // preview / inspector
    int viewPass_ = -1;      // -1 = Image
    int channelMode_ = 0;    // 0 RGB, 1 R, 2 G, 3 B, 4 A, 5 Luma
    bool inspect_ = false;
    std::shared_ptr<rhi::Program> viewProgram_;
    std::unique_ptr<rhi::RenderTarget> viewTarget_;

    // view
    bool overlay_ = false;
    float overlayDim_ = 0.35f;
    bool layoutBuilt_ = false;
    bool resetLayout_ = false;
    int focusDefaults_ = 0;                 // frames left to force the tabs below selected
    std::vector<std::string> tabsToSelect_;  // window names whose dock tab should be selected
    bool quit_ = false;
    bool showTimeline_ = true, showTransition_ = true, showProfiler_ = true, showHistory_ = true;
    bool showLibrary_ = true, showAI_ = true, showCrossCompile_ = true, showExport_ = false;

    // profiler
    std::deque<float> cpuHistory_, gpuHistory_;

    // Mobile / Metal panel
    std::unique_ptr<TextEditor> mslView_;
    int mslIos_ = 1;  // 1 = iOS, 0 = macOS
    bool mslOk_ = false;
    std::string mslLog_;
    std::string mslPassName_;
    std::string metalCheckLog_;

    // history panel
    int historySelected_ = -1;
    int historyPass_ = -1;
    TextDiff diff_;
    bool diffDirty_ = true;

    // export
    struct ExportJob {
        bool active = false;
        int format = 2;  // 0 PNG frame, 1 PNG sequence, 2 MP4 (H.264), 3 MOV (ProRes 422 HQ)
        int width = 1080, height = 1920;
        float fps = 30.0f, start = 0.0f, length = 5.0f;
        bool withAudio = true;
        fs::path output;
        int frame = 0, total = 0;
        std::unique_ptr<media::Process> encoder;
        std::string status;
        double savedTime = 0.0;
        int savedFrame = 0;
        bool savedPlaying = true;
        std::vector<std::uint8_t> pixels;
    } export_;
    bool pendingExport_ = false;

    // library
    struct LibraryItem {
        std::string name;
        fs::path path;
        fs::path thumbPath;
        std::unique_ptr<rhi::Texture> thumb;
        bool thumbLoaded = false;
    };
    std::vector<LibraryItem> library_;
    bool libraryDirty_ = true;
    char libraryFilter_[128] = {};
    char libraryName_[128] = {};

    // AI
    ai::Assistant assistant_;
    char aiPrompt_[4096] = {};
    char aiKeyBuf_[256] = {};
    bool aiRememberKey_ = false;
    int aiMode_ = 0;  // 0 modify current pass, 1 new shader, 2 explain/fix errors

    // second output window
    OutputWindow outputWindow_;

    // misc / dialogs
    enum class Dialog {
        None, OpenProject, SaveProjectAs, OpenShader, SaveShaderAs, AddTextures, LinkPassFile, ExportPath,
    };
    Dialog pendingDialog_ = Dialog::None;
    fs::path lastDir_;
    std::string iniPath_;  // ImGui layout file, kept in the config folder
    char cameraDevice_[128] = {};
    char micDevice_[128] = {};
};

}  // namespace app
