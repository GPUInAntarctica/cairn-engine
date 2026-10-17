#version 450
// Cairn shadow depth stage: the cascade matrix arrives as a push constant (one
// push per cascade) and the per-object model matrix through the shared dynamic
// UBO, one element per instance. Depth-only.
layout(location = 0) in vec3 aPos;

layout(push_constant) uniform Push {
    mat4 lightViewProj;
} pc;

struct DrawUbo {
    mat4 model;
    vec4 color;
    vec4 emissive;
    vec4 tilingSpec;
    vec4 surf;
    vec4 flags;
    vec4 misc;
    // Padding so the std140 array stride is 256 bytes, matching the CPU
    // DrawUbo stride (and the 256-byte dynamic-offset window).
    vec4 pad[6];
};
layout(std140, set = 0, binding = 4) uniform DrawUBO {
    DrawUbo draws[256];   // one 64 KiB window: 256 x 256-byte stride
};

void main() {
    mat4 model = draws[gl_InstanceIndex].model;
    vec4 p = pc.lightViewProj * (model * vec4(aPos, 1.0));
    p.y = -p.y;
    p.z = (p.z + p.w) * 0.5;   // OpenGL [-1,1] -> Vulkan [0,1]
    gl_Position = p;
}
