#version 450
// Cairn mesh vertex stage. The engine's matrices follow the OpenGL
// convention (NDC z in [-1,1], +y up); Vulkan wants z in [0,1] and +y down,
// so the clip position is remapped here to keep both backends pixel-aligned.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec4 aTangent;  // xyz tangent, w handedness

layout(location = 0) out vec3 vWorld;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec4 vTangent;
layout(location = 3) out vec2 vUv;
// Which element of the draw array this vertex belongs to. Flat (no
// interpolation) so every fragment of the instance reads the same material.
layout(location = 4) out flat uint vInstance;

layout(std140, set = 0, binding = 0) uniform FrameUBO {
    mat4 viewProj;
    vec4 camPosOrtho;      // xyz camPos, w ortho flag
    vec4 camForward;       // xyz
    vec4 camRightTH;       // xyz camRight, w tanHalfFov
    vec4 camUp;            // xyz
    vec4 ambient;          // rgb (sRGB-authored; lighting uses it raw)
    vec4 fogColorDensity;  // rgb linear fog color, a density
    vec4 misc;             // x fogEnabled, y tonemap, z exposure, w skyEnabled
    vec4 skyTop;
    vec4 skyBottom;
    vec4 viewport;         // x width, y height, z aspect, w orthoSize
    vec4 clearColor;       // rgb, used when the sky is disabled
    vec4 timeParams;       // x animation clock for material shaders
} frame;

struct DrawUbo {
    mat4 model;
    vec4 color;
    vec4 emissive;
    vec4 tilingSpec;
    vec4 surf;
    vec4 flags;
    vec4 misc;
    // Bindless sampler slots (x albedo, y normal) into the fragment stage's
    // texture arrays. Occupies the first 16 bytes of the old padding.
    vec4 texIndex;
    // Padding so the std140 array stride is 256 bytes, matching the CPU
    // DrawUbo stride (and the 256-byte dynamic-offset window).
    vec4 pad[5];
};
// One instanced draw covers a run of identical meshes/materials; the dynamic
// offset points at the run's first element and gl_InstanceIndex picks the rest.
layout(std140, set = 0, binding = 4) uniform DrawUBO {
    DrawUbo draws[256];   // one 64 KiB window: 256 x 256-byte stride
};

void main() {
    uint inst = uint(gl_InstanceIndex);
    vInstance = inst;
    mat4 model = draws[inst].model;
    vec4 w = model * vec4(aPos, 1.0);
    vWorld = w.xyz;
    vNormal = mat3(model) * aNormal;
    vTangent = vec4(mat3(model) * aTangent.xyz, aTangent.w);
    vUv = aUv;
    vec4 p = frame.viewProj * w;
    p.y = -p.y;
    p.z = (p.z + p.w) * 0.5;
    gl_Position = p;
}
