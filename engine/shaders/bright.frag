#version 450
// Bloom bright pass: 2x2 box downsample into the half-res target, keeping
// pixels above the display-luma threshold with a smooth knee. Matches
// ApplyBloom's first stage on the software path.
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D srcTex;
layout(push_constant) uniform Push {
    vec4 p0;   // x threshold
    vec4 p1;   // xy full-res size
} pc;

void main() {
    ivec2 hp = ivec2(gl_FragCoord.xy);
    ivec2 fs = ivec2(pc.p1.xy);
    vec3 sum = vec3(0.0);
    for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
            ivec2 s = min(hp * 2 + ivec2(dx, dy), fs - 1);
            sum += texelFetch(srcTex, s, 0).rgb;
        }
    }
    vec3 c = sum * 0.25;
    float luma = 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
    float threshold = pc.p0.x;
    float k = clamp((luma - threshold) / max(1e-3, 1.0 - threshold), 0.0, 1.0);
    k = k * k * (3.0 - 2.0 * k);
    outColor = vec4(clamp(c * k, 0.0, 1.0), 1.0);
}
