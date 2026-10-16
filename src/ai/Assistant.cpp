#include "ai/Assistant.h"

#include "media/Process.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <random>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace ai {

namespace {

fs::path privateTempFile(const std::string& stem) {
    std::random_device rd;
    const fs::path p = media::tempDir() / (stem + "_" + std::to_string(rd()) + ".tmp");
    { std::ofstream touch(p); }
    std::error_code ec;
    fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
    return p;
}

}  // namespace

Assistant::~Assistant() {
    if (thread_.joinable()) thread_.join();
}

void Assistant::run(std::vector<std::string> args, std::string body, bool isModels) {
    if (busy_) return;
    if (thread_.joinable()) thread_.join();
    const std::string curl = media::findTool("curl");
    if (curl.empty()) {
        error = "curl not found on this system.";
        done_ = true;
        return;
    }
    if (apiKey_.empty()) {
        error = "No API key. Set ANTHROPIC_API_KEY or paste a key in the AI panel.";
        done_ = true;
        return;
    }
    busy_ = true;
    lastWasModels_ = isModels;
    error.clear();

    // Key and body go through private temp files so they never appear on a command line.
    const fs::path headers = privateTempFile("hdr");
    {
        std::ofstream h(headers);
        h << "x-api-key: " << apiKey_ << "\nanthropic-version: 2023-06-01\ncontent-type: application/json\n";
    }
    fs::path bodyFile;
    if (!body.empty()) {
        bodyFile = privateTempFile("body");
        std::ofstream b(bodyFile, std::ios::binary);
        b << body;
    }
    args.insert(args.begin(), curl);
    args.insert(args.end(), {"-sS", "--max-time", "180", "-H", "@" + headers.string()});
    if (!bodyFile.empty()) args.insert(args.end(), {"--data-binary", "@" + bodyFile.string()});

    thread_ = std::thread([this, args, headers, bodyFile] {
        std::string out;
        const int rc = media::runCapture(args, out, true);
        std::error_code ec;
        fs::remove(headers, ec);
        if (!bodyFile.empty()) fs::remove(bodyFile, ec);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            rawResult_ = std::move(out);
            exitCode_ = rc;
        }
        busy_ = false;
        done_ = true;
    });
}

void Assistant::refreshModels() { run({"https://api.anthropic.com/v1/models?limit=100"}, {}, true); }

void Assistant::send(const std::string& system, const std::string& user) {
    if (model.empty()) {
        error = "Pick a model first (Refresh loads the list).";
        done_ = true;
        return;
    }
    json body = {{"model", model},
                 {"max_tokens", 8000},
                 {"system", system},
                 {"messages", json::array({{{"role", "user"}, {"content", user}}})}};
    reply.clear();
    run({"https://api.anthropic.com/v1/messages"}, body.dump(), false);
}

bool Assistant::poll() {
    if (!done_.exchange(false)) return false;
    std::string raw;
    int rc = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        raw.swap(rawResult_);
        rc = exitCode_;
    }
    if (raw.empty()) return true;  // error already set synchronously
    if (rc != 0) {
        error = "curl failed (" + std::to_string(rc) + "): " + raw.substr(0, 300);
        return true;
    }
    try {
        const json j = json::parse(raw);
        if (j.value("type", "") == "error" || j.contains("error")) {
            error = j["error"].value("message", raw.substr(0, 300));
            return true;
        }
        if (lastWasModels_) {
            models.clear();
            for (const auto& m : j.value("data", json::array())) models.push_back(m.value("id", ""));
            if (model.empty() && !models.empty()) model = models.front();
        } else {
            std::string text;
            for (const auto& c : j.value("content", json::array()))
                if (c.value("type", "") == "text") text += c.value("text", "");
            reply = text;
        }
    } catch (const std::exception& e) {
        error = std::string("Unexpected response: ") + e.what();
    }
    return true;
}

std::string extractCode(const std::string& reply) {
    const auto start = reply.find("```");
    if (start == std::string::npos) return {};
    const auto lineEnd = reply.find('\n', start);
    if (lineEnd == std::string::npos) return {};
    const auto end = reply.find("```", lineEnd);
    if (end == std::string::npos) return {};
    return reply.substr(lineEnd + 1, end - lineEnd - 1);
}

std::string systemPrompt() {
    return R"(You write fragment shaders for Shader Studio, a live GLSL editor (desktop OpenGL 4.1 core).
Rules for code you return:
- Return the complete shader in ONE fenced ```glsl code block, then at most 3 short sentences.
- Prefer "#version 410 core" with "out vec4 fragColor;". Legacy GLSL ES 1.00 (varying, texture2D,
  gl_FragColor, precision) and Shadertoy mainImage() also work if the user's code uses them; keep their style.
- Built-in uniforms: u_time (s), u_timeDelta, u_frame (int), u_resolution (vec2 px), u_mouse (vec2 px),
  u_texCount (int), u_audioLevel/u_bass/u_mid/u_high (floats 0..1). Textures: sampler2D u_tex0..u_texN.
  Multipass: sampler2D u_bufferA..u_bufferD read other passes, u_prev reads this pass's previous frame.
  Transitions: u_from, u_to samplers and float progress (0..1).
- Expose tweakables as uniforms with UI hints in a trailing comment:
  // @slider min..max default    // @color #RRGGBB    // @toggle    // @drag speed
- Code must compile on OpenGL 4.1 and be fast enough for mobile GPUs (avoid huge loops).)";
}

}  // namespace ai
