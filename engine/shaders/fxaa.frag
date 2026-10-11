#version 450
// Console-style FXAA over the final LDR image: luma-edge detection on the 3x3
// neighborhood, then two bilinear taps along the edge-perpendicular direction.
// A faithful port of ApplyFxaa.
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D srcTex;
layout(push_constant) uniform Push {
    vec4 p0;   // xy size
} pc;

const float kEdgeMin = 0.0312;       // absolute luma range floor
const float kEdgeThreshold = 0.063;  // relative to the local max
const float kReduceMin = 1.0 / 128.0;
const float kReduceMul = 1.0 / 8.0;
const float kSpanMax = 8.0;

float lumaAt(ivec2 size, ivec2 p) {
    p = clamp(p, ivec2(0), size - 1);
    vec3 c = texelFetch(srcTex, p, 0).rgb;
    return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
}

void main() {
    ivec2 size = ivec2(pc.p0.xy);
    ivec2 ip = ivec2(gl_FragCoord.xy);
    float lM = lumaAt(size, ip);
    float lNW = lumaAt(size, ip + ivec2(-1, -1));
    float lNE = lumaAt(size, ip + ivec2(1, -1));
    float lSW = lumaAt(size, ip + ivec2(-1, 1));
    float lSE = lumaAt(size, ip + ivec2(1, 1));
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    vec3 here = texelFetch(srcTex, ip, 0).rgb;
    if (lMax - lMin < max(kEdgeMin, lMax * kEdgeThreshold)) {
        outColor = vec4(here, 1.0);  // flat area: passthrough
        return;
    }
    float dirX = -((lNW + lNE) - (lSW + lSE));
    float dirY = ((lNW + lSW) - (lNE + lSE));
    float dirReduce = max((lNW + lNE + lSW + lSE) * 0.25 * kReduceMul, kReduceMin);
    float rcpDirMin = 1.0 / (min(abs(dirX), abs(dirY)) + dirReduce);
    dirX = clamp(dirX * rcpDirMin, -kSpanMax, kSpanMax);
    dirY = clamp(dirY * rcpDirMin, -kSpanMax, kSpanMax);
    // SampleBilinearRGB(p) == texture() at p/size with linear + edge clamp.
    vec2 base = gl_FragCoord.xy;
    vec2 uvA = (base + vec2(dirX, dirY) * (1.0 / 3.0 - 0.5)) / vec2(size);
    vec2 uvB = (base + vec2(dirX, dirY) * (2.0 / 3.0 - 0.5)) / vec2(size);
    vec3 a = texture(srcTex, uvA).rgb;
    vec3 b = texture(srcTex, uvB).rgb;
    outColor = vec4(clamp((a + b) * 0.5, 0.0, 1.0), 1.0);
}
