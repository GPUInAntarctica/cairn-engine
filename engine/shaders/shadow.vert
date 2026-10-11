#version 450
// Cairn shadow depth stage: cascade matrices arrive as a push constant, the
// object model matrix through the shared dynamic UBO. Depth-only.
layout(location = 0) in vec3 aPos;

layout(push_constant) uniform Push {
    mat4 lightViewProj;
} pc;

layout(std140, set = 0, binding = 4) uniform DrawUBO {
    mat4 model;
    vec4 color;
    vec4 emissive;
    vec4 tilingSpec;
    vec4 surf;
    vec4 flags;
    vec4 misc;
} draw;

void main() {
    vec4 p = pc.lightViewProj * (draw.model * vec4(aPos, 1.0));
    p.y = -p.y;
    p.z = (p.z + p.w) * 0.5;   // OpenGL [-1,1] -> Vulkan [0,1]
    gl_Position = p;
}
