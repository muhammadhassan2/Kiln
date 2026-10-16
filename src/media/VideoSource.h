#pragma once

#include "media/Process.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace media {

struct VideoInfo {
    bool ok = false;
    int width = 0, height = 0;
    double fps = 30.0;
    double duration = 0.0;
    bool hasAudio = false;
    std::string error;
};

// Requires ffprobe (ships with ffmpeg).
VideoInfo probeVideo(const std::filesystem::path& file);

// Default camera / microphone device strings for the current platform's ffmpeg input.
std::string defaultCameraDevice();
std::string defaultMicDevice();

// Decodes a video file or camera with the ffmpeg CLI into RGBA frames.
// Realtime mode: a background thread paces frames to the clock (pause/seek/loop supported).
// Stepped mode (export): frames are pulled synchronously at a fixed rate.
class VideoSource {
public:
    VideoSource() = default;
    ~VideoSource();

    bool openFile(const std::filesystem::path& file, bool flipY, std::string& error);
    bool openCamera(const std::string& device, int width, int height, bool flipY, std::string& error);
    void close();

    // UI thread: copies the newest decoded frame into `pixels` if one arrived since the last call.
    bool fetch(std::vector<std::uint8_t>& pixels);

    int width() const { return width_; }
    int height() const { return height_; }
    double duration() const { return duration_; }
    double fps() const { return fps_; }
    bool isCamera() const { return camera_; }
    double position() const { return position_.load(); }  // seconds of the last decoded frame
    std::string lastError() const;

    void setPaused(bool paused) { paused_ = paused; }
    void seek(double seconds);  // file only
    void setFlipY(bool flip);

    // Export support (file only; cameras keep running in realtime).
    bool beginStepped(double startSeconds, double fps);
    bool readStepped(std::vector<std::uint8_t>& pixels);
    void endStepped();

private:
    std::vector<std::string> buildArgs(double start, double steppedFps) const;
    void startThread(double startSeconds);
    void stopThread();
    void threadMain(double startSeconds);

    std::string ffmpeg_;
    std::filesystem::path file_;
    std::string device_;
    bool camera_ = false;
    bool flipY_ = true;
    int width_ = 0, height_ = 0;
    int srcW_ = 0, srcH_ = 0;
    double fps_ = 30.0, duration_ = 0.0;

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> paused_{false};
    std::atomic<double> position_{0.0};
    std::unique_ptr<Process> proc_;  // owned by the thread while running
    std::mutex procMutex_;

    mutable std::mutex frameMutex_;
    std::vector<std::uint8_t> frame_;
    bool fresh_ = false;
    std::string error_;
    std::string stderrPath_;

    std::unique_ptr<Process> stepped_;
    double steppedFps_ = 30.0, steppedStart_ = 0.0;
    bool inStepped_ = false;
    double resumeAt_ = 0.0;
};

}  // namespace media
