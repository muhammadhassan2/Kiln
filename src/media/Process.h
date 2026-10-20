#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace media {

// Minimal child-process wrapper with one pipe: read the child's stdout, or write its stdin.
// Used to drive the ffmpeg / ffprobe / curl / xcrun command-line tools.
class Process {
public:
    enum class Pipe { ReadStdout, WriteStdin };

    Process() = default;
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // stderrPath: optional file that receives the child's stderr (for error reporting).
    bool start(const std::vector<std::string>& argv, Pipe pipe, bool mergeStderr = false,
               const std::string& stderrPath = {});
    bool running() const { return pid_ > 0 || handle_ != nullptr; }

    // Blocking. Returns bytes read; 0 = EOF / error.
    size_t read(void* dst, size_t bytes);
    bool readExact(void* dst, size_t bytes);
    bool write(const void* src, size_t bytes);

    void closePipe();   // signals EOF to a stdin-writing child
    int wait();         // waits for exit, returns exit code (-1 on failure)
    void kill();        // terminates immediately

private:
    int fd_ = -1;
    long pid_ = -1;
    void* handle_ = nullptr;  // FILE* on Windows
};

// Runs a command to completion and captures stdout (+stderr if merge). Returns exit code.
int runCapture(const std::vector<std::string>& argv, std::string& output, bool mergeStderr = true);

// Locates a command-line tool: PATH first, then common install folders
// (/opt/homebrew/bin, /usr/local/bin — GUI apps on macOS often don't inherit the shell PATH).
std::string findTool(const std::string& name);
// Every copy of a tool that can be found, PATH order first (Windows also checks winget, Scoop and
// Chocolatey folders, so a tool installed while the app runs is found without a restart).
std::vector<std::string> findToolCandidates(const std::string& name);
// Makes findTool(name) return `path` (e.g. the one ffmpeg that has libwebp). Empty clears it.
void setToolPath(const std::string& name, const std::string& path);

std::filesystem::path tempDir();

}  // namespace media
