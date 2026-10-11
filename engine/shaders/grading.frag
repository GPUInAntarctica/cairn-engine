#version 450
// Color grading: contrast around the 0.5 display pivot, then saturation
// around Rec.709 luma. Intermediate values stay unclamped like the CPU pass.
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D srcTex;
layout(push_constant) uniform Push {
    vec4 p0;   // x contrast, y saturation
} pc;

void main() {
    vec3 c = texture(srcTex, vUv).rgb;
    c = (c - 0.5) * pc.p0.x + 0.5;
    float luma = 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
    c = luma + (c - luma) * pc.p0.y;
    outColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
