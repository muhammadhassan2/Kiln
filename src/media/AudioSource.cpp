#include "media/AudioSource.h"

#include "media/VideoSource.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace fs = std::filesystem;

namespace media {

namespace {
constexpr int kFft = 1024;
constexpr size_t kMicRing = AudioSource::kSampleRate * 2;  // 2 s ring buffer
constexpr float kPi = 3.14159265358979f;
}  // namespace

AudioSource::~AudioSource() { close(); }

std::string AudioSource::lastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

double AudioSource::duration() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mic_ ? 0.0 : static_cast<double>(samples_.size()) / kSampleRate;
}

bool AudioSource::openFile(const fs::path& file, std::string& error) {
    close();
    if (findTool("ffmpeg").empty()) {
        error = "ffmpeg not found. Install it (macOS: brew install ffmpeg) to use audio textures.";
        return false;
    }
    file_ = file;
    mic_ = false;
    stop_ = false;
    thread_ = std::thread(&AudioSource::decodeThread, this);
    return true;
}

bool AudioSource::openMicrophone(const std::string& device, std::string& error) {
    close();
    if (findTool("ffmpeg").empty()) {
        error = "ffmpeg not found. Install it (macOS: brew install ffmpeg) to use the microphone.";
        return false;
    }
    device_ = device.empty() ? defaultMicDevice() : device;
    mic_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        samples_.assign(kMicRing, 0.0f);
        ringWrite_ = 0;
    }
    stop_ = false;
    thread_ = std::thread(&AudioSource::micThread, this);
    return true;
}

void AudioSource::close() {
    stop_ = true;
    {
        std::lock_guard<std::mutex> lock(procMutex_);
        if (proc_) proc_->kill();
    }
    if (thread_.joinable()) thread_.join();
    setMonitor(false, 0, false);
    loaded_ = false;
}

void AudioSource::decodeThread() {
    auto proc = std::make_unique<Process>();
    const std::vector<std::string> args = {findTool("ffmpeg"), "-hide_banner", "-loglevel", "error", "-nostdin",
                                           "-i", file_.string(), "-vn", "-ac", "1", "-ar",
                                           std::to_string(kSampleRate), "-f", "f32le", "-"};
    if (!proc->start(args, Process::Pipe::ReadStdout)) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = "could not start ffmpeg";
        return;
    }
    Process* p = proc.get();
    {
        std::lock_guard<std::mutex> lock(procMutex_);
        proc_ = std::move(proc);
    }
    std::vector<float> all;
    std::vector<float> chunk(8192);
    while (!stop_) {
        const size_t n = p->read(chunk.data(), chunk.size() * sizeof(float));
        if (n == 0) break;
        all.insert(all.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n / sizeof(float)));
    }
    {
        std::lock_guard<std::mutex> lock(procMutex_);
        proc_->wait();
        proc_.reset();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (all.empty()) {
        error_ = "no audio decoded from " + file_.filename().string();
        return;
    }
    samples_ = std::move(all);
    loaded_ = true;
}

void AudioSource::micThread() {
    std::vector<std::string> args = {findTool("ffmpeg"), "-hide_banner", "-loglevel", "error", "-nostdin"};
#if defined(__APPLE__)
    args.insert(args.end(), {"-f", "avfoundation", "-i", device_});
#elif defined(_WIN32)
    args.insert(args.end(), {"-f", "dshow", "-i", "audio=" + device_});
#else
    args.insert(args.end(), {"-f", "pulse", "-i", device_});
#endif
    args.insert(args.end(), {"-ac", "1", "-ar", std::to_string(kSampleRate), "-f", "f32le", "-"});
    auto proc = std::make_unique<Process>();
    if (!proc->start(args, Process::Pipe::ReadStdout)) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = "could not start ffmpeg";
        return;
    }
    Process* p = proc.get();
    {
        std::lock_guard<std::mutex> lock(procMutex_);
        proc_ = std::move(proc);
    }
    loaded_ = true;
    std::vector<float> chunk(512);
    while (!stop_) {
        if (!p->readExact(chunk.data(), chunk.size() * sizeof(float))) break;
        std::lock_guard<std::mutex> lock(mutex_);
        for (float s : chunk) {
            samples_[ringWrite_] = s;
            ringWrite_ = (ringWrite_ + 1) % samples_.size();
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
    if (!stop_) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = "microphone input stopped (check the device name and microphone permission)";
    }
}

void AudioSource::fft(std::array<float, 1024>& re, std::array<float, 1024>& im) const {
    // In-place iterative radix-2 Cooley-Tukey.
    const int n = kFft;
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const float ang = -2.0f * kPi / static_cast<float>(len);
        const float wr = std::cos(ang), wi = std::sin(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int k = 0; k < len / 2; ++k) {
                const int a = i + k, b = i + k + len / 2;
                const float tr = re[b] * cr - im[b] * ci;
                const float ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
                const float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

void AudioSource::analyze(double seconds, std::vector<std::uint8_t>& rgba) {
    std::array<float, kFft> window{};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (samples_.empty()) return;
        const size_t n = samples_.size();
        if (mic_) {
            for (int i = 0; i < kFft; ++i) window[static_cast<size_t>(i)] = samples_[(ringWrite_ + n - kFft + static_cast<size_t>(i)) % n];
        } else {
            const double dur = static_cast<double>(n) / kSampleRate;
            double t = dur > 0 ? std::fmod(std::max(0.0, seconds), dur) : 0.0;
            const long center = static_cast<long>(t * kSampleRate);
            for (int i = 0; i < kFft; ++i) {
                const long idx = center - kFft + i;
                window[static_cast<size_t>(i)] = idx >= 0 && static_cast<size_t>(idx) < n ? samples_[static_cast<size_t>(idx)] : 0.0f;
            }
        }
    }

    // Waveform: the last 512 samples. Spectrum: Hann-windowed 1024-point FFT -> 512 bins.
    std::array<float, kFft> re{}, im{};
    double rms = 0.0;
    for (int i = 0; i < kFft; ++i) {
        const float w = 0.5f - 0.5f * std::cos(2.0f * kPi * static_cast<float>(i) / (kFft - 1));
        re[static_cast<size_t>(i)] = window[static_cast<size_t>(i)] * w;
        rms += window[static_cast<size_t>(i)] * window[static_cast<size_t>(i)];
    }
    fft(re, im);

    rgba.assign(static_cast<size_t>(kBins * 2 * 4), 255);
    float bands[3] = {0, 0, 0};
    int bandCount[3] = {0, 0, 0};
    for (int b = 0; b < kBins; ++b) {
        const float mag = std::sqrt(re[static_cast<size_t>(b)] * re[static_cast<size_t>(b)] + im[static_cast<size_t>(b)] * im[static_cast<size_t>(b)]) * 2.0f / kFft;
        const float db = 20.0f * std::log10(std::max(mag, 1e-9f));
        const float v = std::clamp((db + 100.0f) / 70.0f, 0.0f, 1.0f);  // WebAudio-style -100..-30 dB
        smoothed_[static_cast<size_t>(b)] = smoothed_[static_cast<size_t>(b)] * 0.8f + v * 0.2f;
        const auto byte = static_cast<std::uint8_t>(smoothed_[static_cast<size_t>(b)] * 255.0f);
        auto* px = &rgba[static_cast<size_t>(b * 4)];
        px[0] = px[1] = px[2] = byte;
        const float hz = static_cast<float>(b) * kSampleRate / kFft;
        const int band = hz < 250.0f ? 0 : (hz < 4000.0f ? 1 : 2);
        bands[band] += smoothed_[static_cast<size_t>(b)];
        ++bandCount[band];
    }
    for (int i = 0; i < kBins; ++i) {
        const float s = window[static_cast<size_t>(kFft - kBins + i)];
        const auto byte = static_cast<std::uint8_t>(std::clamp(s * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f);
        auto* px = &rgba[static_cast<size_t>((kBins + i) * 4)];
        px[0] = px[1] = px[2] = byte;
    }
    level_ = std::clamp(static_cast<float>(std::sqrt(rms / kFft)) * 3.0f, 0.0f, 1.0f);
    bass_ = bandCount[0] ? bands[0] / bandCount[0] : 0;
    mid_ = bandCount[1] ? bands[1] / bandCount[1] : 0;
    high_ = bandCount[2] ? bands[2] / bandCount[2] : 0;
}

void AudioSource::setMonitor(bool on, double seconds, bool playing) {
    if (player_) {
        player_->kill();
        player_->wait();
        player_.reset();
    }
    monitor_ = on;
    if (!on || !playing || mic_ || file_.empty()) return;
    const std::string ffplay = findTool("ffplay");
    if (ffplay.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = "ffplay not found (it ships with ffmpeg) - audio monitoring unavailable";
        return;
    }
    player_ = std::make_unique<Process>();
    player_->start({ffplay, "-nodisp", "-autoexit", "-loglevel", "quiet", "-loop", "0", "-ss",
                    std::to_string(std::max(0.0, seconds)), file_.string()},
                   Process::Pipe::ReadStdout);
}

}  // namespace media
