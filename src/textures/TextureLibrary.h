#pragma once

#include "media/AudioSource.h"
#include "media/VideoSource.h"
#include "rhi/RHI.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace textures {

enum class SourceKind { Image, Noise, Checker, Video, Camera, AudioFile, Microphone };

const char* kindName(SourceKind k);

struct TextureSlot {
    std::string name;
    SourceKind kind = SourceKind::Image;
    std::filesystem::path path;      // Image / Video / AudioFile
    std::string device;              // Camera / Microphone
    std::filesystem::file_time_type mtime{};
    bool flipY = true;
    bool monitorAudio = false;       // AudioFile: play it through ffplay
    rhi::SamplerDesc sampler;
    std::unique_ptr<rhi::Texture> texture;
    std::unique_ptr<media::VideoSource> video;
    std::unique_ptr<media::AudioSource> audio;
    std::string status;              // last error / state shown in the UI
    std::vector<std::uint8_t> scratch;
};

// Owns the ordered list of textures. Slot i is bound to u_tex<i> / iChannel<i> (GLSL)
// or [[texture(i)]] (Metal). There is no fixed count; the device limit applies.
class TextureLibrary {
public:
    explicit TextureLibrary(rhi::Device& device);
    ~TextureLibrary();

    // Each returns an error message on failure, empty on success.
    std::string addImage(const std::filesystem::path& path);
    std::string addVideo(const std::filesystem::path& path);
    std::string addCamera(const std::string& device);
    std::string addAudioFile(const std::filesystem::path& path);
    std::string addMicrophone(const std::string& device);
    void addNoise(int size = 256);
    void addChecker(int size = 256);
    // Adds by file type (image / video / audio). Returns error or empty.
    std::string addFile(const std::filesystem::path& path);

    void remove(size_t index);
    void move(size_t from, size_t to);
    std::string reload(size_t index);
    void applySampler(size_t index);
    void clear();

    // Sets flipY on every image/video slot (and the default for new ones) and reloads them.
    void setFlipAll(bool flipY);
    bool defaultFlipY() const { return defaultFlipY_; }

    // Per UI frame: pull new video/camera frames, recompute audio analysis at `mediaTime`.
    // `jumped` = the timeline was scrubbed/rewound, so file videos re-seek.
    void update(double mediaTime, bool playing, bool jumped);

    // Export: deterministic frame stepping for file videos and audio files.
    void beginExport(double startSeconds, double fps);
    void exportFrame(double mediaTime);
    void endExport();

    // Poll file timestamps and reload changed images. Returns names that reloaded.
    std::vector<std::string> pollHotReload();

    // First audio slot's band levels (0..1), for the u_audioLevel / u_bass / u_mid / u_high built-ins.
    void audioLevels(float& level, float& bass, float& mid, float& high) const;
    // Normalised 0..1 playback position of the first file video (for "Video position" driven uniforms).
    float firstVideoProgress() const;

    std::vector<TextureSlot>& slots() { return slots_; }
    const rhi::Texture& fallback() const { return *fallback_; }

    static bool isImageFile(const std::filesystem::path& p);
    static bool isVideoFile(const std::filesystem::path& p);
    static bool isAudioFile(const std::filesystem::path& p);

private:
    std::string loadImage(TextureSlot& slot);
    void uploadAudio(TextureSlot& slot, double t);

    rhi::Device& device_;
    bool defaultFlipY_ = true;
    std::vector<TextureSlot> slots_;
    std::unique_ptr<rhi::Texture> fallback_;  // 1x1 black for unbound samplers
    bool wasPlaying_ = true;
};

}  // namespace textures
