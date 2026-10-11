#version 450
// Bloom composite: add the bilinearly upsampled half-res glow under `strength`
// to the scene. Matches ApplyBloom's final stage.
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D srcTex;    // scene
layout(set = 0, binding = 1) uniform sampler2D bloomTex;  // half-res glow
layout(push_constant) uniform Push {
    vec4 p0;   // x strength
    vec4 p1;   // xy half-res size
} pc;

void main() {
    vec3 scene = texture(srcTex, vUv).rgb;
    // The software pass samples the glow at (x+0.5)*0.5 - 0.5 in half-res
    // pixel space, i.e. exactly this uv with linear filtering.
    vec2 uvB = gl_FragCoord.xy * 0.5 / max(pc.p1.xy, vec2(1.0));
    vec3 glow = texture(bloomTex, uvB).rgb;
    outColor = vec4(clamp(scene + glow * pc.p0.x, 0.0, 1.0), 1.0);
}
