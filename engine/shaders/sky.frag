#version 450
// Sky gradient by view-ray elevation, per row like the software clear pass.
// The sky bypasses tonemapping on both backends (raw authored colors); with
// the sky disabled the clear color is written straight out.
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(std140, set = 0, binding = 0) uniform FrameUBO {
    mat4 viewProj;
    vec4 camPosOrtho;      // w: ortho flag
    vec4 camForward;
    vec4 camRightTH;       // w: tanHalfFov
    vec4 camUp;
    vec4 ambient;
    vec4 fogColorDensity;
    vec4 misc;             // w: skyEnabled
    vec4 skyTop;
    vec4 skyBottom;
    vec4 viewport;         // x width, y height, z aspect, w orthoSize
    vec4 clearColor;       // rgb, used when the sky is disabled
} frame;

void main() {
    if (frame.misc.w < 0.5) {
        outColor = vec4(clamp(frame.clearColor.rgb, 0.0, 1.0), 1.0);
        return;
    }
    float h = max(frame.viewport.y, 1.0);
    float ndcY = 1.0 - 2.0 * (gl_FragCoord.y / h);
    float tY;
    if (frame.camPosOrtho.w > 0.5) {
        tY = frame.camUp.y * ndcY + frame.camForward.y;
    } else {
        vec3 ray = frame.camForward.xyz + frame.camUp.xyz * (ndcY * frame.camRightTH.w);
        tY = ray.y;
    }
    float t = clamp((tY + 0.35) * 1.5, 0.0, 1.0);
    t = pow(t, 0.85);
    vec3 c = mix(frame.skyBottom.rgb, frame.skyTop.rgb, t);
    outColor = vec4(clamp(c, 0.0, 1.0), 1.0);
}
