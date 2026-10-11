#version 450
// Cairn debug-line stage (editor grid, selection outline, Debug::DrawLine).
// Depth-tested against geometry with the same small slack the software
// rasterizer grants, encoded as an NDC z offset.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;

layout(location = 0) out vec4 vColor;

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
    vColor = aColor;
    vec4 p = frame.viewProj * vec4(aPos, 1.0);
    p.y = -p.y;
    p.z = (p.z + p.w) * 0.5 - 0.0002 * p.w;
    gl_Position = p;
}
