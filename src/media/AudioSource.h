#pragma once

#include "media/Process.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace media {

// Audio analysis texture (Shadertoy layout): 512 x 2, row 0 = FFT spectrum, row 1 = waveform.
// Files are decoded fully into memory (so analysis follows the timeline exactly, also during export);
// the microphone is captured live. Both go through the ffmpeg CLI.
class AudioSource {
public:
    static constexpr int kBins = 512;
    static constexpr int kSampleRate = 44100;

    AudioSource() = default;
    ~AudioSource();

    bool openFile(const std::filesystem::path& file, std::string& error);
    bool openMicrophone(const std::string& device, std::string& error);
    void close();

    // Recomputes spectrum/waveform for media time `seconds` (files) or the latest input (mic),
    // and writes the 512x2 RGBA8 texture data.
    void analyze(double seconds, std::vector<std::uint8_t>& rgba);

    bool ready() const { return loaded_.load(); }
    bool isMic() const { return mic_; }
    double duration() const;
    std::string lastError() const;

    // Band levels from the last analyze() call, 0..1.
    float level() const { return level_; }
    float bass() const { return bass_; }
    float mid() const { return mid_; }
    float high() const { return high_; }

    // Optional audible playback of files through ffplay (kept roughly in sync on play/seek).
    void setMonitor(bool on, double seconds, bool playing);
    bool monitoring() const { return monitor_; }

private:
    void decodeThread();
    void micThread();
    void fft(std::array<float, 1024>& re, std::array<float, 1024>& im) const;

    std::filesystem::path file_;
    std::string device_;
    bool mic_ = false;

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> loaded_{false};
    std::unique_ptr<Process> proc_;
    std::mutex procMutex_;

    mutable std::mutex mutex_;
    std::vector<float> samples_;  // whole file (mono) or mic ring buffer
    size_t ringWrite_ = 0;
    std::string error_;

    std::array<float, kBins> smoothed_{};
    float level_ = 0, bass_ = 0, mid_ = 0, high_ = 0;

    bool monitor_ = false;
    std::unique_ptr<Process> player_;
};

}  // namespace media
