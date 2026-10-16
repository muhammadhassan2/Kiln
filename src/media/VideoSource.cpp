#include "media/VideoSource.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace media {

namespace {

constexpr int kMaxEdge = 2048;  // cap decode size to keep upload bandwidth sane

double parseRate(const std::string& r) {
    const auto slash = r.find('/');
    if (slash == std::string::npos) return std::atof(r.c_str());
    const double n = std::atof(r.substr(0, slash).c_str());
    const double d = std::atof(r.substr(slash + 1).c_str());
    return d > 0 ? n / d : 0.0;
}

std::string tail(const std::string& path, size_t maxBytes = 400) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    if (s.size() > maxBytes) s = s.substr(s.size() - maxBytes);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

}  // namespace

VideoInfo probeVideo(const fs::path& file) {
    VideoInfo info;
    const std::string ffprobe = findTool("ffprobe");
    if (ffprobe.empty()) {
        info.error = "ffprobe not found. Install ffmpeg (macOS: brew install ffmpeg).";
        return info;
    }
    std::string out;
    const int rc = runCapture({ffprobe, "-v", "error", "-show_entries",
                               "stream=codec_type,width,height,avg_frame_rate,r_frame_rate:format=duration",
                               "-of", "json", file.string()},
                              out, false);
    if (rc != 0) {
        info.error = "ffprobe could not read " + file.filename().string();
        return info;
    }
    try {
        const json j = json::parse(out);
        for (const auto& s : j.value("streams", json::array())) {
            const std::string type = s.value("codec_type", "");
            if (type == "audio") info.hasAudio = true;
            if (type == "video" && info.width == 0) {
                info.width = s.value("width", 0);
                info.height = s.value("height", 0);
                double fps = parseRate(s.value("avg_frame_rate", "0/0"));
                if (fps <= 0) fps = parseRate(s.value("r_frame_rate", "30/1"));
                info.fps = fps > 0 ? fps : 30.0;
            }
        }
        if (j.contains("format")) info.duration = std::atof(j["format"].value("duration", "0").c_str());
    } catch (const std::exception& e) {
        info.error = std::string("ffprobe output: ") + e.what();
        return info;
    }
    info.ok = info.width > 0 && info.height > 0;
    if (!info.ok) info.error = file.filename().string() + " has no video stream";
    return info;
}

std::string defaultCameraDevice() {
#if defined(__APPLE__)
    return "0";
#elif defined(_WIN32)
    return "Integrated Camera";
#else
    return "/dev/video0";
#endif
}

std::string defaultMicDevice() {
#if defined(__APPLE__)
    return ":0";
#elif defined(_WIN32)
    return "Microphone";
#else
    return "default";
#endif
}

VideoSource::~VideoSource() { close(); }

std::string VideoSource::lastError() const {
    std::lock_guard<std::mutex> lock(frameMutex_);
    return error_;
}

bool VideoSource::openFile(const fs::path& file, bool flipY, std::string& error) {
    close();
    ffmpeg_ = findTool("ffmpeg");
    if (ffmpeg_.empty()) {
        error = "ffmpeg not found. Install it (macOS: brew install ffmpeg) to use video textures.";
        return false;
    }
    const VideoInfo info = probeVideo(file);
    if (!info.ok) {
        error = info.error;
        return false;
    }
    file_ = file;
    camera_ = false;
    flipY_ = flipY;
    srcW_ = info.width;
    srcH_ = info.height;
    const double s = std::min(1.0, static_cast<double>(kMaxEdge) / std::max(srcW_, srcH_));
    width_ = std::max(2, static_cast<int>(std::lround(srcW_ * s)) & ~1);
    height_ = std::max(2, static_cast<int>(std::lround(srcH_ * s)) & ~1);
    fps_ = info.fps;
    duration_ = info.duration;
    stderrPath_ = (tempDir() / ("video_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".log")).string();
    startThread(0.0);
    return true;
}

bool VideoSource::openCamera(const std::string& device, int width, int height, bool flipY, std::string& error) {
    close();
    ffmpeg_ = findTool("ffmpeg");
    if (ffmpeg_.empty()) {
        error = "ffmpeg not found. Install it (macOS: brew install ffmpeg) to use the camera.";
        return false;
    }
    device_ = device.empty() ? defaultCameraDevice() : device;
    camera_ = true;
    flipY_ = flipY;
    width_ = width;
    height_ = height;
    fps_ = 30.0;
    duration_ = 0.0;
    stderrPath_ = (tempDir() / ("camera_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".log")).string();
    startThread(0.0);
    return true;
}

void VideoSource::close() {
    stopThread();
    if (stepped_) {
        stepped_->kill();
        stepped_->wait();
        stepped_.reset();
    }
    inStepped_ = false;
}

std::vector<std::string> VideoSource::buildArgs(double start, double steppedFps) const {
    std::vector<std::string> a = {ffmpeg_, "-hide_banner", "-loglevel", "error", "-nostdin"};
    if (camera_) {
#if defined(__APPLE__)
        a.insert(a.end(), {"-f", "avfoundation", "-framerate", "30", "-i", device_});
#elif defined(_WIN32)
        a.insert(a.end(), {"-f", "dshow", "-i", "video=" + device_});
#else
        a.insert(a.end(), {"-f", "v4l2", "-i", device_});
#endif
    } else {
        if (start > 0.0) a.insert(a.end(), {"-ss", std::to_string(start)});
        a.insert(a.end(), {"-i", file_.string(), "-an"});
    }
    std::string vf;
    if (steppedFps > 0.0) vf += "fps=" + std::to_string(steppedFps) + ",";
    vf += "scale=" + std::to_string(width_) + ":" + std::to_string(height_);
    // Raw frames arrive top row first; GL textures want bottom row first unless flipY is off.
    if (flipY_) vf += ",vflip";
    a.insert(a.end(), {"-vf", vf, "-f", "rawvideo", "-pix_fmt", "rgba", "-"});
    return a;
}

void VideoSource::startThread(double startSeconds) {
    stopThread();
    stop_ = false;
    thread_ = std::thread(&VideoSource::threadMain, this, startSeconds);
}

void VideoSource::stopThread() {
    stop_ = true;
    {
        std::lock_guard<std::mutex> lock(procMutex_);
        if (proc_) proc_->kill();
    }
    if (thread_.joinable()) thread_.join();
}

void VideoSource::threadMain(double startSeconds) {
    using clock = std::chrono::steady_clock;
    const size_t frameBytes = static_cast<size_t>(width_) * height_ * 4;
    std::vector<std::uint8_t> buf(frameBytes);
    double start = startSeconds;

    while (!stop_) {
        auto proc = std::make_unique<Process>();
        if (!proc->start(buildArgs(start, 0.0), Process::Pipe::ReadStdout, false, stderrPath_)) {
            std::lock_guard<std::mutex> lock(frameMutex_);
            error_ = "could not start ffmpeg";
            return;
        }
        Process* p = proc.get();
        {
            std::lock_guard<std::mutex> lock(procMutex_);
            proc_ = std::move(proc);
        }

        // Pacing: media time advances with the wall clock while not paused.
        double mediaTime = start;
        auto last = clock::now();
        int frames = 0;
        while (!stop_) {
            if (!p->readExact(buf.data(), frameBytes)) break;
            ++frames;
            {
                std::lock_guard<std::mutex> lock(frameMutex_);
                frame_.swap(buf);
                if (buf.size() != frameBytes) buf.resize(frameBytes);
                fresh_ = true;
                error_.clear();
            }
            const double frameTime = start + (frames - 1) / fps_;
            position_ = frameTime;
            if (camera_) continue;  // the device paces itself

            // Sleep until the next frame is due (holding while paused).
            const double next = start + frames / fps_;
            while (!stop_) {
                const auto now = clock::now();
                if (!paused_) mediaTime += std::chrono::duration<double>(now - last).count();
                last = now;
                if (mediaTime >= next) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(paused_ ? 10 : 2));
            }
        }

        {
            std::lock_guard<std::mutex> lock(procMutex_);
            if (proc_) {
                proc_->kill();
                proc_->wait();
                proc_.reset();
            }
        }
        if (stop_) break;
        if (camera_ || frames == 0) {
            std::lock_guard<std::mutex> lock(frameMutex_);
            error_ = camera_ ? "camera stopped: " + tail(stderrPath_) : "no frames decoded: " + tail(stderrPath_);
            if (camera_ || frames == 0) return;
        }
        start = 0.0;  // loop the file
    }
}

bool VideoSource::fetch(std::vector<std::uint8_t>& pixels) {
    std::lock_guard<std::mutex> lock(frameMutex_);
    if (!fresh_) return false;
    pixels = frame_;
    fresh_ = false;
    return true;
}

void VideoSource::seek(double seconds) {
    if (camera_ || file_.empty()) return;
    if (duration_ > 0) seconds = std::fmod(std::max(0.0, seconds), duration_);
    position_ = seconds;
    startThread(seconds);
}

void VideoSource::setFlipY(bool flip) {
    if (flip == flipY_) return;
    flipY_ = flip;
    if (!ffmpeg_.empty()) startThread(camera_ ? 0.0 : position_.load());
}

bool VideoSource::beginStepped(double startSeconds, double fps) {
    if (camera_ || file_.empty()) return false;
    resumeAt_ = position_.load();
    stopThread();
    steppedFps_ = fps;
    steppedStart_ = duration_ > 0 ? std::fmod(std::max(0.0, startSeconds), duration_) : startSeconds;
    stepped_ = std::make_unique<Process>();
    inStepped_ = stepped_->start(buildArgs(steppedStart_, fps), Process::Pipe::ReadStdout, false, stderrPath_);
    return inStepped_;
}

bool VideoSource::readStepped(std::vector<std::uint8_t>& pixels) {
    if (!inStepped_) return false;
    const size_t frameBytes = static_cast<size_t>(width_) * height_ * 4;
    pixels.resize(frameBytes);
    if (stepped_->readExact(pixels.data(), frameBytes)) return true;
    // End of file: loop from the start.
    stepped_->kill();
    stepped_->wait();
    stepped_ = std::make_unique<Process>();
    if (!stepped_->start(buildArgs(0.0, steppedFps_), Process::Pipe::ReadStdout, false, stderrPath_)) return false;
    return stepped_->readExact(pixels.data(), frameBytes);
}

void VideoSource::endStepped() {
    if (!inStepped_) return;
    inStepped_ = false;
    if (stepped_) {
        stepped_->kill();
        stepped_->wait();
        stepped_.reset();
    }
    if (!file_.empty()) startThread(resumeAt_);
}

}  // namespace media
