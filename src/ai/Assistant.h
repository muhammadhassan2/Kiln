#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ai {

// Small async client for the Claude Messages API, via the curl command-line tool
// (no TLS library to build). The API key comes from ANTHROPIC_API_KEY or the AI panel.
class Assistant {
public:
    ~Assistant();

    void setApiKey(const std::string& key) { apiKey_ = key; }
    const std::string& apiKey() const { return apiKey_; }
    bool hasKey() const { return !apiKey_.empty(); }

    void refreshModels();                                         // async GET /v1/models
    void send(const std::string& system, const std::string& user);  // async POST /v1/messages

    bool busy() const { return busy_.load(); }
    // UI thread: returns true once when a request finished since the last call.
    bool poll();

    std::string model;                // selected model id
    std::vector<std::string> models;  // ids from /v1/models
    std::string reply;                // last text reply
    std::string error;                // last error

private:
    void run(std::vector<std::string> args, std::string body, bool isModels);

    std::string apiKey_;
    std::thread thread_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> done_{false};
    std::mutex mutex_;
    std::string rawResult_;
    bool lastWasModels_ = false;
    int exitCode_ = 0;
};

// Returns the first fenced code block (```glsl ... ``` or ``` ... ```) in a reply, or empty.
std::string extractCode(const std::string& reply);

// The system prompt that explains Shader Studio's conventions to the model.
std::string systemPrompt();

}  // namespace ai
