#include "textures/TextureLibrary.h"

#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <random>

namespace fs = std::filesystem;

namespace textures {

namespace {

std::string ext(const fs::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
    return e;
}

}  // namespace

const char* kindName(SourceKind k) {
    switch (k) {
        case SourceKind::Image: return "Image";
        case SourceKind::Noise: return "Noise";
        case SourceKind::Checker: return "Checker";
        case SourceKind::Video: return "Video";
        case SourceKind::Camera: return "Camera";
        case SourceKind::AudioFile: return "Audio";
        case SourceKind::Microphone: return "Microphone";
    }
    return "?";
}

TextureLibrary::TextureLibrary(rhi::Device& device) : device_(device) {
    const std::uint8_t black[4] = {0, 0, 0, 255};
    fallback_ = device_.createTexture(1, 1, rhi::Format::RGBA8, black);
}

TextureLibrary::~TextureLibrary() = default;

bool TextureLibrary::isImageFile(const fs::path& p) {
    const auto e = ext(p);
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".bmp" || e == ".tga" || e == ".gif" ||
           e == ".psd" || e == ".hdr";
}

bool TextureLibrary::isVideoFile(const fs::path& p) {
    const auto e = ext(p);
    return e == ".mp4" || e == ".mov" || e == ".m4v" || e == ".mkv" || e == ".webm" || e == ".avi";
}

bool TextureLibrary::isAudioFile(const fs::path& p) {
    const auto e = ext(p);
    return e == ".mp3" || e == ".wav" || e == ".m4a" || e == ".aac" || e == ".flac" || e == ".ogg";
}

std::string TextureLibrary::addFile(const fs::path& path) {
    if (isImageFile(path)) return addImage(path);
    if (isVideoFile(path)) return addVideo(path);
    if (isAudioFile(path)) return addAudioFile(path);
    return "Unsupported file type: " + path.filename().string();
}

std::string TextureLibrary::loadImage(TextureSlot& slot) {
    stbi_set_flip_vertically_on_load(slot.flipY ? 1 : 0);
    int w = 0, h = 0, n = 0;
    stbi_uc* pixels = stbi_load(slot.path.string().c_str(), &w, &h, &n, 4);
    if (!pixels) return std::string("Could not load ") + slot.path.string() + ": " + stbi_failure_reason();

    if (slot.texture) slot.texture->upload(pixels, w, h);
    else slot.texture = device_.createTexture(w, h, rhi::Format::RGBA8, pixels);
    stbi_image_free(pixels);

    slot.texture->setSampler(slot.sampler);
    std::error_code ec;
    slot.mtime = fs::last_write_time(slot.path, ec);
    return {};
}

std::string TextureLibrary::addImage(const fs::path& path) {
    TextureSlot slot;
    slot.kind = SourceKind::Image;
    slot.flipY = defaultFlipY_;
    slot.path = path;
    slot.name = path.filename().string();
    if (auto err = loadImage(slot); !err.empty()) return err;
    slots_.push_back(std::move(slot));
    return {};
}

std::string TextureLibrary::addVideo(const fs::path& path) {
    TextureSlot slot;
    slot.kind = SourceKind::Video;
    slot.flipY = defaultFlipY_;
    slot.path = path;
    slot.name = path.filename().string();
    slot.sampler.filter = rhi::Filter::Linear;
    slot.sampler.wrapU = slot.sampler.wrapV = rhi::Wrap::Clamp;
    slot.video = std::make_unique<media::VideoSource>();
    std::string err;
    if (!slot.video->openFile(path, slot.flipY, err)) return err;
    const std::vector<std::uint8_t> black(static_cast<size_t>(slot.video->width() * slot.video->height() * 4), 0);
    slot.texture = device_.createTexture(slot.video->width(), slot.video->height(), rhi::Format::RGBA8, black.data());
    slot.texture->setSampler(slot.sampler);
    slots_.push_back(std::move(slot));
    return {};
}

std::string TextureLibrary::addCamera(const std::string& device) {
    TextureSlot slot;
    slot.kind = SourceKind::Camera;
    slot.flipY = defaultFlipY_;
    slot.device = device.empty() ? media::defaultCameraDevice() : device;
    slot.name = "Camera " + slot.device;
    slot.sampler.filter = rhi::Filter::Linear;
    slot.sampler.wrapU = slot.sampler.wrapV = rhi::Wrap::Clamp;
    slot.video = std::make_unique<media::VideoSource>();
    std::string err;
    if (!slot.video->openCamera(slot.device, 1280, 720, slot.flipY, err)) return err;
    const std::vector<std::uint8_t> black(1280 * 720 * 4, 0);
    slot.texture = device_.createTexture(1280, 720, rhi::Format::RGBA8, black.data());
    slot.texture->setSampler(slot.sampler);
    slots_.push_back(std::move(slot));
    return {};
}

std::string TextureLibrary::addAudioFile(const fs::path& path) {
    TextureSlot slot;
    slot.kind = SourceKind::AudioFile;
    slot.path = path;
    slot.name = path.filename().string();
    slot.sampler.filter = rhi::Filter::Linear;
    slot.sampler.wrapU = slot.sampler.wrapV = rhi::Wrap::Clamp;
    slot.audio = std::make_unique<media::AudioSource>();
    std::string err;
    if (!slot.audio->openFile(path, err)) return err;
    const std::vector<std::uint8_t> black(media::AudioSource::kBins * 2 * 4, 0);
    slot.texture = device_.createTexture(media::AudioSource::kBins, 2, rhi::Format::RGBA8, black.data());
    slot.texture->setSampler(slot.sampler);
    slots_.push_back(std::move(slot));
    return {};
}

std::string TextureLibrary::addMicrophone(const std::string& device) {
    TextureSlot slot;
    slot.kind = SourceKind::Microphone;
    slot.device = device.empty() ? media::defaultMicDevice() : device;
    slot.name = "Microphone " + slot.device;
    slot.sampler.filter = rhi::Filter::Linear;
    slot.sampler.wrapU = slot.sampler.wrapV = rhi::Wrap::Clamp;
    slot.audio = std::make_unique<media::AudioSource>();
    std::string err;
    if (!slot.audio->openMicrophone(slot.device, err)) return err;
    const std::vector<std::uint8_t> black(media::AudioSource::kBins * 2 * 4, 0);
    slot.texture = device_.createTexture(media::AudioSource::kBins, 2, rhi::Format::RGBA8, black.data());
    slot.texture->setSampler(slot.sampler);
    slots_.push_back(std::move(slot));
    return {};
}

void TextureLibrary::addNoise(int size) {
    std::vector<std::uint8_t> px(static_cast<size_t>(size * size * 4));
    std::mt19937 rng(1337u + static_cast<unsigned>(slots_.size()));
    std::uniform_int_distribution<int> dist(0, 255);
    for (auto& v : px) v = static_cast<std::uint8_t>(dist(rng));
    TextureSlot slot;
    slot.kind = SourceKind::Noise;
    slot.name = "noise_rgba " + std::to_string(size);
    slot.texture = device_.createTexture(size, size, rhi::Format::RGBA8, px.data());
    slot.texture->setSampler(slot.sampler);
    slots_.push_back(std::move(slot));
}

void TextureLibrary::addChecker(int size) {
    std::vector<std::uint8_t> px(static_cast<size_t>(size * size * 4));
    const int cell = std::max(1, size / 8);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const bool on = ((x / cell) + (y / cell)) % 2 == 0;
            auto* p = &px[static_cast<size_t>((y * size + x) * 4)];
            p[0] = p[1] = p[2] = on ? 230 : 40;
            p[3] = 255;
        }
    TextureSlot slot;
    slot.kind = SourceKind::Checker;
    slot.name = "checker " + std::to_string(size);
    slot.sampler.filter = rhi::Filter::Nearest;
    slot.texture = device_.createTexture(size, size, rhi::Format::RGBA8, px.data());
    slot.texture->setSampler(slot.sampler);
    slots_.push_back(std::move(slot));
}

void TextureLibrary::remove(size_t index) {
    if (index < slots_.size()) slots_.erase(slots_.begin() + static_cast<std::ptrdiff_t>(index));
}

void TextureLibrary::clear() { slots_.clear(); }

void TextureLibrary::move(size_t from, size_t to) {
    if (from >= slots_.size() || to >= slots_.size() || from == to) return;
    std::swap(slots_[from], slots_[to]);
}

std::string TextureLibrary::reload(size_t index) {
    if (index >= slots_.size()) return {};
    auto& s = slots_[index];
    switch (s.kind) {
        case SourceKind::Image: return loadImage(s);
        case SourceKind::Video:
        case SourceKind::Camera:
            if (s.video) s.video->setFlipY(s.flipY);
            return {};
        default: return {};
    }
}

void TextureLibrary::applySampler(size_t index) {
    if (index < slots_.size() && slots_[index].texture) slots_[index].texture->setSampler(slots_[index].sampler);
}

void TextureLibrary::setFlipAll(bool flipY) {
    defaultFlipY_ = flipY;
    for (size_t i = 0; i < slots_.size(); ++i) {
        auto& s = slots_[i];
        const bool flippable = s.kind == SourceKind::Image || s.kind == SourceKind::Video || s.kind == SourceKind::Camera;
        if (!flippable || s.flipY == flipY) continue;
        s.flipY = flipY;
        reload(i);
    }
}

void TextureLibrary::uploadAudio(TextureSlot& s, double t) {
    if (!s.audio || !s.audio->ready()) return;
    s.audio->analyze(t, s.scratch);
    if (!s.scratch.empty()) s.texture->upload(s.scratch.data(), media::AudioSource::kBins, 2);
}

void TextureLibrary::update(double mediaTime, bool playing, bool jumped) {
    for (auto& s : slots_) {
        if (s.video) {
            s.video->setPaused(!playing);
            if (s.kind == SourceKind::Video && s.video->duration() > 0.0) {
                // Keep the clip on the timeline: re-seek after scrubs/rewinds or large drift.
                const double expected = std::fmod(std::max(0.0, mediaTime), s.video->duration());
                const double drift = std::fabs(s.video->position() - expected);
                if (jumped || (drift > 0.75 && drift < s.video->duration() - 0.75)) s.video->seek(expected);
            }
            if (s.video->fetch(s.scratch)) {
                s.texture->upload(s.scratch.data(), s.video->width(), s.video->height());
                s.status.clear();
            } else if (auto e = s.video->lastError(); !e.empty()) {
                s.status = e;
            }
        }
        if (s.audio) {
            if (s.kind == SourceKind::AudioFile && s.monitorAudio && (playing != wasPlaying_ || jumped))
                s.audio->setMonitor(true, mediaTime, playing);
            uploadAudio(s, mediaTime);
            if (auto e = s.audio->lastError(); !e.empty()) s.status = e;
            else if (!s.audio->ready()) s.status = "decoding...";
            else s.status.clear();
        }
    }
    wasPlaying_ = playing;
}

void TextureLibrary::beginExport(double startSeconds, double fps) {
    for (auto& s : slots_) {
        if (s.kind == SourceKind::Video && s.video) s.video->beginStepped(startSeconds, fps);
        if (s.audio) s.audio->setMonitor(false, 0, false);
    }
}

void TextureLibrary::exportFrame(double mediaTime) {
    for (auto& s : slots_) {
        if (s.kind == SourceKind::Video && s.video && s.video->readStepped(s.scratch))
            s.texture->upload(s.scratch.data(), s.video->width(), s.video->height());
        if (s.kind == SourceKind::Camera && s.video && s.video->fetch(s.scratch))
            s.texture->upload(s.scratch.data(), s.video->width(), s.video->height());
        if (s.audio) uploadAudio(s, mediaTime);
    }
}

void TextureLibrary::endExport() {
    for (auto& s : slots_)
        if (s.kind == SourceKind::Video && s.video) s.video->endStepped();
    wasPlaying_ = !wasPlaying_;  // force monitors to resync on the next update()
}

std::vector<std::string> TextureLibrary::pollHotReload() {
    std::vector<std::string> changed;
    for (auto& s : slots_) {
        if (s.kind != SourceKind::Image) continue;
        std::error_code ec;
        const auto t = fs::last_write_time(s.path, ec);
        if (!ec && t != s.mtime && loadImage(s).empty()) changed.push_back(s.name);
    }
    return changed;
}

void TextureLibrary::audioLevels(float& level, float& bass, float& mid, float& high) const {
    level = bass = mid = high = 0.0f;
    for (const auto& s : slots_)
        if (s.audio) {
            level = s.audio->level();
            bass = s.audio->bass();
            mid = s.audio->mid();
            high = s.audio->high();
            return;
        }
}

float TextureLibrary::firstVideoProgress() const {
    for (const auto& s : slots_)
        if (s.kind == SourceKind::Video && s.video && s.video->duration() > 0)
            return static_cast<float>(std::clamp(s.video->position() / s.video->duration(), 0.0, 1.0));
    return 0.0f;
}

}  // namespace textures
