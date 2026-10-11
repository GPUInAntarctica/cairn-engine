#version 450
// Separable 9-tap Gaussian blur (horizontal or vertical, driven by the push
// constant direction). Weights mirror ApplyBloom's kWeights.
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D srcTex;
layout(push_constant) uniform Push {
    vec4 p0;   // xy: texel-space step direction (1/w,0) or (0,1/h)
} pc;

const float kWeights[5] = float[](0.227027, 0.194594, 0.121621, 0.054054, 0.016216);

void main() {
    vec3 acc = vec3(0.0);
    for (int t = -4; t <= 4; ++t) {
        acc += texture(srcTex, vUv + pc.p0.xy * float(t)).rgb * kWeights[abs(t)];
    }
    outColor = vec4(clamp(acc, 0.0, 1.0), 1.0);
}
