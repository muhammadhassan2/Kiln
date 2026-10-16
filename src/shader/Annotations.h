#pragma once

#include <array>
#include <optional>
#include <string>
#include <unordered_map>

namespace shader {

// Comment hints after a uniform declaration drive the auto-generated UI:
//   uniform float u_warp;   // @slider 0..2 0.85
//   uniform vec3  u_tint;   // @color #FFB27A
//   uniform bool  u_invert; // @toggle
//   uniform vec2  u_offset; // @drag 0.01
struct Annotation {
    enum class Kind { None, Slider, Color, Toggle, Drag };
    Kind kind = Kind::None;
    float min = 0.0f;
    float max = 1.0f;
    float speed = 0.01f;
    std::optional<std::array<float, 4>> defaultValue;
};

using AnnotationMap = std::unordered_map<std::string, Annotation>;

AnnotationMap parseAnnotations(const std::string& source);

}  // namespace shader
