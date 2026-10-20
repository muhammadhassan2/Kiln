// Molten glass: domain-warped fbm, coloured like a kiln at full heat.
uniform float u_heat;   // @slider 0..2 1.2
uniform vec3  u_glow;   // @color #FF8A3D
uniform float u_warp;   // @slider 0..3 1.6

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1, 0)), u.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), u.x), u.y);
}
float fbm(vec2 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 5; i++) { v += a * noise(p); p = p * 2.03 + 17.0; a *= 0.5; }
    return v;
}
void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = (fragCoord - 0.5 * iResolution.xy) / iResolution.y;
    float t = iTime * 0.25;
    vec2 q = vec2(fbm(uv * 2.0 + t), fbm(uv * 2.0 - t + 4.0));
    vec2 r = vec2(fbm(uv * 2.0 + u_warp * q + vec2(1.7, 9.2) + t * 1.5), fbm(uv * 2.0 + u_warp * q + vec2(8.3, 2.8) - t));
    float f = fbm(uv * 2.0 + u_warp * r);
    vec3 col = mix(vec3(0.06, 0.02, 0.10), vec3(0.55, 0.08, 0.35), clamp(f * f * 2.5, 0.0, 1.0));
    col = mix(col, u_glow, clamp(length(q) * 0.9, 0.0, 1.0));
    col = mix(col, vec3(1.0, 0.92, 0.6), clamp(pow(r.x, 3.0) * u_heat, 0.0, 1.0));
    col *= 0.6 + 0.9 * f;
    col *= 1.0 - 0.35 * dot(uv, uv);
    fragColor = vec4(pow(col, vec3(2.2)) * 1.9, 1.0);
}
