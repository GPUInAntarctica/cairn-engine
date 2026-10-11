#version 450
// Radial darkening: strength, radius where it starts, softness as the falloff
// width past the radius. Distance is in aspect-corrected half-height units so
// the falloff stays circular at any resolution.
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D srcTex;
layout(push_constant) uniform Push {
    vec4 p0;   // x strength, y radius, z softness
    vec4 p1;   // x aspect, y width, z height
} pc;

void main() {
    float aspect = pc.p1.x;
    float dx = (gl_FragCoord.x / pc.p1.y * 2.0 - 1.0) * aspect;
    float dy = gl_FragCoord.y / pc.p1.z * 2.0 - 1.0;
    float d = sqrt(dx * dx + dy * dy);
    float t = clamp((d - pc.p0.y) / max(1e-3, pc.p0.z), 0.0, 1.0);
    t = t * t * (3.0 - 2.0 * t);
    float f = 1.0 - pc.p0.x * t;
    outColor = vec4(clamp(texture(srcTex, vUv).rgb * f, 0.0, 1.0), 1.0);
}
