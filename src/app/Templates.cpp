#include "app/Templates.h"

namespace app {

namespace {

const char* kBasic = R"(#version 410 core
// Kiln - live GLSL. Edits compile automatically (F5 forces it).
// Built-ins: u_time, u_timeDelta, u_frame, u_resolution, u_mouse, u_texCount, u_bass/u_mid/u_high
// Textures:  u_tex0 .. u_texN (or iChannel0..N) follow the Textures panel order.
// UI hints:  // @slider min..max default   // @color #RRGGBB   // @toggle   // @drag speed

uniform vec2      u_resolution;
uniform float     u_time;
uniform vec2      u_mouse;
uniform int       u_texCount; // number of textures in the Textures panel
uniform sampler2D u_tex0;     // add a texture in the Textures panel
uniform float     u_speed;    // @slider 0..3 1.0
uniform float     u_rings;    // @slider 1..20 6.0
uniform float     u_texMix;   // @slider 0..1 1.0
uniform vec3      u_tint;     // @color #C6F432

out vec4 fragColor;

void main() {
    vec2 uv = gl_FragCoord.xy / u_resolution;
    vec2 p  = (gl_FragCoord.xy - 0.5 * u_resolution) / u_resolution.y;
    vec2 m  = (u_mouse - 0.5 * u_resolution) / u_resolution.y;

    float t = u_time * u_speed;
    float d = length(p - m * 0.25);
    float a = atan(p.y, p.x);

    vec3 col = 0.5 + 0.5 * cos(t + d * u_rings - vec3(0.0, 2.1, 4.2) + a * 2.0);
    col *= smoothstep(1.1, 0.15, d);
    col  = mix(col, col * u_tint * 1.6, 0.45);

    vec2 warp = 0.015 * vec2(sin(t + uv.y * 12.0), cos(t + uv.x * 12.0));
    vec3 tex  = texture(u_tex0, uv + warp).rgb;
    // Show the first texture once one is added; the slider blends it with the pattern.
    col = mix(col, tex, u_texCount > 0 ? u_texMix : 0.0);

    fragColor = vec4(col, 1.0);
}
)";

const char* kVideoEffect = R"(precision highp float;
// Mobile-style video effect (GLSL ES 1.00): varying tc + frameTexture, like an app's effect pipeline.
// Tip: add a video in Textures, then Shader > UV origin top-left (video frames).
varying vec2 tc;
uniform sampler2D frameTexture;
uniform float uniform_progress;            // driven 0..1 (Loop) by default
uniform float u_strength;  // @slider 0..1 0.6
uniform float u_vignette;  // @slider 0..1.5 0.8

void main() {
    vec2 uv = tc;
    float wave = sin(uv.y * 40.0 + uniform_progress * 12.566) * 0.004 * u_strength;
    vec3 col;
    col.r = texture2D(frameTexture, uv + vec2(wave + 0.004 * u_strength, 0.0)).r;
    col.g = texture2D(frameTexture, uv + vec2(wave, 0.0)).g;
    col.b = texture2D(frameTexture, uv - vec2(wave + 0.004 * u_strength, 0.0)).b;
    float v = smoothstep(0.85, 0.2, length(uv - 0.5) * u_vignette * 1.4);
    gl_FragColor = vec4(col * v, 1.0);
}
)";

const char* kTransition = R"(// gl-transitions style: implement vec4 transition(vec2 uv).
// getFromColor / getToColor sample the From / To slots (Transition panel), progress runs 0..1.
uniform float softness;  // @slider 0..0.5 0.12
uniform vec2  direction; // @slider -1..1 1.0

vec4 transition(vec2 uv) {
    vec2 dir = normalize(direction + vec2(1e-4));
    float d = dot(uv - 0.5, dir) + 0.5;            // 0..1 along the wipe direction
    float edge = progress * (1.0 + 2.0 * softness) - softness;
    float m = smoothstep(edge - softness, edge + softness, d);
    return mix(getToColor(uv), getFromColor(uv), m);
}
)";

const char* kShadertoy = R"(// Shadertoy-compatible: mainImage, iTime, iResolution, iMouse, iChannel0..3 all work.
void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = fragCoord / iResolution.xy;
    vec3 col = 0.5 + 0.5 * cos(iTime + uv.xyx + vec3(0, 2, 4));
    fragColor = vec4(col, 1.0);
}
)";

const char* kFeedbackA = R"(#version 410 core
// Buffer A: paints a moving brush and keeps the previous frame (u_prev) for trails.
uniform vec2  u_resolution;
uniform float u_time;
uniform vec2  u_mouse;
uniform sampler2D u_prev;     // this buffer's previous frame
uniform float u_decay;        // @slider 0.8..1 0.97
uniform float u_brush;        // @slider 0.005..0.1 0.03
out vec4 fragColor;

void main() {
    vec2 uv = gl_FragCoord.xy / u_resolution;
    vec2 p  = (gl_FragCoord.xy - 0.5 * u_resolution) / u_resolution.y;
    vec2 c  = 0.35 * vec2(cos(u_time * 1.3), sin(u_time * 1.7));
    if (u_mouse.x > 0.0) c = (u_mouse - 0.5 * u_resolution) / u_resolution.y;
    vec3 prev = texture(u_prev, uv + vec2(0.0, 0.0015)).rgb * u_decay;
    float d = smoothstep(u_brush, 0.0, length(p - c));
    vec3 brush = (0.5 + 0.5 * cos(u_time + vec3(0, 2, 4))) * d;
    fragColor = vec4(max(prev, brush), 1.0);
}
)";

const char* kFeedbackImage = R"(#version 410 core
// Image: reads Buffer A and adds a glow.
uniform vec2 u_resolution;
uniform sampler2D u_bufferA;
uniform float u_glow;  // @slider 0..3 1.2
out vec4 fragColor;

void main() {
    vec2 uv = gl_FragCoord.xy / u_resolution;
    vec3 c = texture(u_bufferA, uv).rgb;
    vec3 blur = vec3(0.0);
    for (int i = -3; i <= 3; ++i)
        for (int j = -3; j <= 3; ++j)
            blur += texture(u_bufferA, uv + vec2(i, j) * 3.0 / u_resolution).rgb;
    fragColor = vec4(c + blur / 49.0 * u_glow, 1.0);
}
)";

const char* kAudio = R"(#version 410 core
// Audio reactive: add an audio file or the microphone in Textures (slot 0).
// Row 0 of the audio texture = spectrum, row 1 = waveform (Shadertoy layout).
uniform vec2  u_resolution;
uniform float u_time;
uniform sampler2D u_tex0;
uniform float u_bass;      // built-in band levels 0..1
uniform float u_high;
uniform vec3  u_color;     // @color #7CC7FF
out vec4 fragColor;

void main() {
    vec2 uv = gl_FragCoord.xy / u_resolution;
    float fft  = texture(u_tex0, vec2(uv.x * 0.5, 0.25)).r;      // low half of the spectrum
    float wave = texture(u_tex0, vec2(uv.x, 0.75)).r;
    vec3 col = vec3(0.02, 0.02, 0.03);
    col += u_color * step(uv.y, fft) * (0.4 + 0.6 * uv.y);
    col += vec3(1.0) * smoothstep(0.012, 0.0, abs(uv.y - wave));
    col *= 1.0 + u_bass * 0.8;
    col += vec3(0.6, 0.2, 1.0) * u_high * 0.3 * (1.0 - uv.y);
    fragColor = vec4(col, 1.0);
}
)";

}  // namespace

const std::vector<Template>& templates() {
    static const std::vector<Template> t = {
        {"Basic", "Animated pattern + texture mix", {{"Image", kBasic}}},
        {"Video effect (GLSL ES)", "Mobile-style effect: varying tc, frameTexture, uniform_progress", {{"Image", kVideoEffect}}},
        {"Transition", "gl-transitions style wipe between two inputs", {{"Image", kTransition}}},
        {"Shadertoy", "mainImage() with iTime / iResolution", {{"Image", kShadertoy}}},
        {"Multipass feedback", "Buffer A trails + Image glow", {{"Buffer A", kFeedbackA}, {"Image", kFeedbackImage}}},
        {"Audio reactive", "Spectrum + waveform from an audio texture", {{"Image", kAudio}}},
    };
    return t;
}

}  // namespace app
