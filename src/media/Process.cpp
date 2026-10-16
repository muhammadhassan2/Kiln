#include "media/Process.h"

#include <cstdio>
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <sstream>

#if defined(_WIN32)
    #include <io.h>
    #include <windows.h>
#else
    #include <csignal>
    #include <fcntl.h>
    #include <spawn.h>
    #include <sys/wait.h>
    #include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace media {

Process::~Process() {
    if (running()) {
        kill();
        wait();
    }
}

#if !defined(_WIN32)

bool Process::start(const std::vector<std::string>& argv, Pipe pipe, bool mergeStderr, const std::string& stderrPath) {
    if (argv.empty()) return false;
    // Pipes must be close-on-exec, otherwise later children inherit the write end of an encoder's
    // stdin and it never sees EOF. The lock closes the pipe()->fcntl() race between threads.
    static std::mutex spawnMutex;
    std::lock_guard<std::mutex> spawnLock(spawnMutex);
    int fds[2];
    if (::pipe(fds) != 0) return false;
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    const int devnull = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    if (pipe == Pipe::ReadStdout) {
        posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
        if (!stderrPath.empty() && !mergeStderr)
            posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, stderrPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        else
            posix_spawn_file_actions_adddup2(&actions, mergeStderr ? fds[1] : devnull, STDERR_FILENO);
        posix_spawn_file_actions_adddup2(&actions, devnull, STDIN_FILENO);
    } else {
        posix_spawn_file_actions_adddup2(&actions, fds[0], STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&actions, devnull, STDOUT_FILENO);
        if (!stderrPath.empty())
            posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, stderrPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        else
            posix_spawn_file_actions_adddup2(&actions, devnull, STDERR_FILENO);
    }
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);

    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);

    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (devnull >= 0) ::close(devnull);
    if (rc != 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return false;
    }
    pid_ = pid;
    if (pipe == Pipe::ReadStdout) {
        ::close(fds[1]);
        fd_ = fds[0];
    } else {
        ::close(fds[0]);
        fd_ = fds[1];
    }
    // Don't let a vanished reader kill us with SIGPIPE.
    std::signal(SIGPIPE, SIG_IGN);
    return true;
}

size_t Process::read(void* dst, size_t bytes) {
    if (fd_ < 0) return 0;
    const ssize_t n = ::read(fd_, dst, bytes);
    return n > 0 ? static_cast<size_t>(n) : 0;
}

bool Process::write(const void* src, size_t bytes) {
    const auto* p = static_cast<const char*>(src);
    while (bytes > 0) {
        const ssize_t n = ::write(fd_, p, bytes);
        if (n <= 0) return false;
        p += n;
        bytes -= static_cast<size_t>(n);
    }
    return true;
}

void Process::closePipe() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

int Process::wait() {
    closePipe();
    if (pid_ <= 0) return -1;
    int status = 0;
    ::waitpid(static_cast<pid_t>(pid_), &status, 0);
    pid_ = -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void Process::kill() {
    if (pid_ > 0) ::kill(static_cast<pid_t>(pid_), SIGKILL);
}

#else  // ---------------------------------------------------------------- Windows (popen based)

namespace {
std::string quote(const std::string& a) {
    if (a.find_first_of(" \t\"") == std::string::npos) return a;
    std::string q = "\"";
    for (char c : a) {
        if (c == '"') q += '\\';
        q += c;
    }
    return q + "\"";
}
}  // namespace

bool Process::start(const std::vector<std::string>& argv, Pipe pipe, bool mergeStderr, const std::string& stderrPath) {
    std::string cmd;
    for (const auto& a : argv) cmd += quote(a) + " ";
    if (mergeStderr) cmd += "2>&1";
    else if (!stderrPath.empty()) cmd += "2>" + quote(stderrPath);
    else cmd += "2>NUL";
    handle_ = _popen(("\"" + cmd + "\"").c_str(), pipe == Pipe::ReadStdout ? "rb" : "wb");
    return handle_ != nullptr;
}

size_t Process::read(void* dst, size_t bytes) {
    return handle_ ? std::fread(dst, 1, bytes, static_cast<FILE*>(handle_)) : 0;
}

bool Process::write(const void* src, size_t bytes) {
    return handle_ && std::fwrite(src, 1, bytes, static_cast<FILE*>(handle_)) == bytes;
}

void Process::closePipe() {}

int Process::wait() {
    if (!handle_) return -1;
    const int rc = _pclose(static_cast<FILE*>(handle_));
    handle_ = nullptr;
    return rc;
}

void Process::kill() {
    // _popen offers no kill; closing the pipe makes ffmpeg exit on the next read/write.
}

#endif

bool Process::readExact(void* dst, size_t bytes) {
    auto* p = static_cast<char*>(dst);
    while (bytes > 0) {
        const size_t n = read(p, bytes);
        if (n == 0) return false;
        p += n;
        bytes -= n;
    }
    return true;
}

int runCapture(const std::vector<std::string>& argv, std::string& output, bool mergeStderr) {
    Process p;
    output.clear();
    if (!p.start(argv, Process::Pipe::ReadStdout, mergeStderr)) return -1;
    char buf[4096];
    size_t n;
    while ((n = p.read(buf, sizeof(buf))) > 0) output.append(buf, n);
    return p.wait();
}

std::string findTool(const std::string& name) {
#if defined(_WIN32)
    const char sep = ';';
    const std::string exe = name + ".exe";
#else
    const char sep = ':';
    const std::string exe = name;
#endif
    std::vector<fs::path> dirs;
    if (const char* path = std::getenv("PATH")) {
        std::stringstream ss(path);
        std::string d;
        while (std::getline(ss, d, sep))
            if (!d.empty()) dirs.emplace_back(d);
    }
    for (const char* d : {"/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", "/opt/local/bin"}) dirs.emplace_back(d);
    for (const auto& d : dirs) {
        std::error_code ec;
        const fs::path p = d / exe;
        if (fs::exists(p, ec) && !fs::is_directory(p, ec)) return p.string();
    }
    return {};
}

fs::path tempDir() {
    std::error_code ec;
    fs::path t = fs::temp_directory_path(ec);
    if (ec) t = ".";
    t /= "ShaderStudio";
    fs::create_directories(t, ec);
    return t;
}

}  // namespace media
