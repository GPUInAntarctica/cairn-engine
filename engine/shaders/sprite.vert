#version 450
// Cairn 2D vertex stage. Sprites and text arrive as pre-transformed world
// quads (built by the shared draw-list code), so there is no model matrix.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec4 aColor;  // reserved (tint lives in DrawUBO)

layout(location = 0) out vec3 vWorld;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUv;

layout(std140, set = 0, binding = 0) uniform FrameUBO {
    mat4 viewProj;
    vec4 camPosOrtho;
    vec4 camForward;
    vec4 camRightTH;
    vec4 camUp;
    vec4 ambient;
    vec4 fogColorDensity;
    vec4 misc;
    vec4 skyTop;
    vec4 skyBottom;
    vec4 viewport;         // x width, y height, z aspect, w orthoSize
    vec4 clearColor;       // rgb, used when the sky is disabled
} frame;

void main() {
    vWorld = aPos;
    vNormal = aNormal;
    vUv = aUv;
    vec4 p = frame.viewProj * vec4(aPos, 1.0);
    p.y = -p.y;
    p.z = (p.z + p.w) * 0.5;
    gl_Position = p;
}
