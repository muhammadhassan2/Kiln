namespace app {

extern const char* const kDefaultShader = R"(#version 410 core
// Shader Studio - live GLSL. Edits compile automatically (F5 forces it).
// Built-ins: u_time, u_timeDelta, u_frame, u_resolution, u_mouse, u_texCount
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

}  // namespace app
