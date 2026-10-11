#version 450
// Cairn 2D fragment stage: alpha-blended sprites and bitmap text. Handles the
// unlit text path and the sprite lighting path (ambient + light falloff +
// directional shadows), then the shared fog / tonemap / encode. The rasterizer
// state blends in display space exactly like the software backend.
layout(location = 0) in vec3 vWorld;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUv;
layout(location = 0) out vec4 outColor;

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

struct Light {
    vec4 posType;
    vec4 dirInten;
    vec4 colorRange;
    vec4 cone;
};
layout(std140, set = 0, binding = 1) uniform LightsUBO {
    Light lights[16];
    int lightCount;
};

layout(std140, set = 0, binding = 2) uniform ShadowUBO {
    mat4 cascadeMatrix[4];
    vec4 cascadeSplits;
    vec4 cascadeRange;
    vec4 cascadeTexel;
    vec4 shadowParams;
    vec4 shadowToLight;
} shadow;

layout(set = 0, binding = 3) uniform sampler2DArray shadowMaps;

layout(std140, set = 0, binding = 4) uniform DrawUBO {
    mat4 model;        // unused (vertices are world-space)
    vec4 color;        // sRGB tint + alpha
    vec4 emissive;
    vec4 tilingSpec;   // xy tiling (1,1 for 2D), z specular, w shininess
    vec4 surf;
    vec4 flags;        // x unlit, y receiveShadows, z albedoSRGB, w hasNormalMap
    vec4 misc;         // x spriteLighting
} draw;

layout(set = 1, binding = 0) uniform sampler2D albedoMap;
layout(set = 1, binding = 1) uniform sampler2D normalMap;

float srgbToLinearFast(float c) {
    return c <= 0.04045 ? c / 12.92 : c * c * (c * 0.305306011 + 0.682171111) + 0.012522878;
}
vec3 srgbToLinearFast3(vec3 c) {
    return vec3(srgbToLinearFast(c.r), srgbToLinearFast(c.g), srgbToLinearFast(c.b));
}
float linearToSrgbFast(float c) {
    return c <= 0.0031308 ? c * 12.92 : c * (c * 0.4127714 + 0.5276643) + 0.0759182;
}
float toneMapChannel(float x, int mode) {
    if (mode == 1) return x / (1.0 + x);
    if (mode == 3) {
        x = max(x, 0.0);
        const float A = 0.15, B = 0.50, C = 0.10, D = 0.20, E = 0.02, F = 0.30;
        return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
    }
    if (mode == 2) {
        x = max(x, 0.0);
        const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
        return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
    }
    return x;
}

bool lightFalloff(int i, vec3 P, out vec3 L, out float atten) {
    float type = lights[i].posType.w;
    if (type < 0.5) {
        L = -lights[i].dirInten.xyz;
        atten = 1.0;
        return true;
    }
    vec3 ld = lights[i].posType.xyz - P;
    float dist = length(ld);
    float range = lights[i].colorRange.a;
    if (dist > range || dist < 1e-5) return false;
    L = ld / dist;
    atten = 1.0 - dist / range;
    atten *= atten;
    if (type > 1.5) {
        float cosAngle = dot(L, -lights[i].dirInten.xyz);
        if (cosAngle <= lights[i].cone.x) return false;
        if (lights[i].cone.y > lights[i].cone.x) {
            float t = clamp((cosAngle - lights[i].cone.x) / (lights[i].cone.y - lights[i].cone.x), 0.0, 1.0);
            atten *= t * t * (3.0 - 2.0 * t);
        } else {
            atten *= clamp((cosAngle - lights[i].cone.x) / 1e-4, 0.0, 1.0);
        }
    }
    return true;
}

float sampleShadow(vec3 P, float viewDepth, float ndotl) {
    int count = int(shadow.shadowParams.x);
    if (count <= 0) return 1.0;
    int cascade = count - 1;
    for (int i = 0; i < count; ++i) {
        if (viewDepth <= shadow.cascadeSplits[i]) { cascade = i; break; }
    }
    vec4 clip = shadow.cascadeMatrix[cascade] * vec4(P, 1.0);
    if (clip.w <= 1e-6) return 1.0;
    vec3 ndc = clip.xyz / clip.w;
    if (abs(ndc.x) > 1.0 || abs(ndc.y) > 1.0 || ndc.z > 1.0 || ndc.z < -1.0) return 1.0;
    float range = shadow.cascadeRange[cascade];
    float texelWorld = shadow.cascadeTexel[cascade];
    float worldBias = texelWorld * (1.5 + 2.0 * (1.0 - clamp(ndotl, 0.0, 1.0)));
    float bias = worldBias / max(1e-3, range);
    float refDepth = ndc.z * 0.5 + 0.5;
    float size = shadow.shadowParams.z;
    float fx = (ndc.x * 0.5 + 0.5) * size;
    // shadow.vert flips y before rasterising, so the lookup uses the raw
    // NDC row. Flipping here as well would mirror every shadow.
    float fy = (ndc.y * 0.5 + 0.5) * size;
    int ix = int(fx), iy = int(fy);
    float hits = 0.0, taps = 0.0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            vec2 uv = vec2((float(ix + dx) + 0.5) / size, (float(iy + dy) + 0.5) / size);
            // textureLod, not texture: the lookup runs inside non-uniform control flow,
            // where implicit derivatives are undefined.
            float d = textureLod(shadowMaps, vec3(uv, float(cascade)), 0.0).r;
            if (refDepth - bias <= d) hits += 1.0;
            taps += 1.0;
        }
    }
    float lit = hits / taps;
    return 1.0 - (1.0 - lit) * shadow.shadowParams.y;
}

void main() {
    vec3 P = vWorld;
    vec3 V = frame.camPosOrtho.xyz - P;
    float viewDist = length(V);
    if (viewDist > 1e-6) V /= viewDist;

    vec4 tex = texture(albedoMap, vUv * draw.tilingSpec.xy);
    vec3 albedo = srgbToLinearFast3(draw.color.rgb);
    if (draw.flags.z > 0.5) albedo *= srgbToLinearFast3(tex.rgb);
    else albedo *= tex.rgb;
    float alpha = tex.a;  // never color-space decoded

    float shadowFactor = 1.0;
    if (shadow.shadowParams.x > 0.5 && draw.flags.y > 0.5) {
        vec3 geomN = vNormal;
        if (dot(geomN, geomN) > 1e-9) geomN = normalize(geomN); else geomN = vec3(0, 0, -1);
        float viewDepth = dot(P - frame.camPosOrtho.xyz, frame.camForward.xyz);
        float ndotl = max(dot(geomN, shadow.shadowToLight.xyz), 0.0);
        shadowFactor = sampleShadow(P, viewDepth, ndotl);
    }

    vec3 Lo;
    if (draw.flags.x > 0.5) {          // unlit (text)
        Lo = albedo;
    } else if (draw.misc.x > 0.5) {    // sprite lighting
        vec3 N;
        if (draw.flags.w > 0.5) {      // 2D normal maps sample Clamp, untiled
            vec4 nm = texture(normalMap, vUv);
            N = vec3(nm.r * 2.0 - 1.0, -(nm.g * 2.0 - 1.0), nm.b * 2.0 - 1.0);
            N = normalize(N);
        } else {
            N = vNormal;
            if (dot(N, N) < 1e-9) N = vec3(0, 0, -1);
            N = normalize(N);
        }
        Lo = albedo * frame.ambient.rgb;
        for (int i = 0; i < lightCount; ++i) {
            vec3 L;
            float atten;
            if (!lightFalloff(i, P, L, atten)) continue;
            float d = dot(N, L);
            if (d <= 0.0) continue;
            float shadowTerm = lights[i].posType.w < 0.5 ? shadowFactor : 1.0;
            float s = lights[i].dirInten.w * atten * d * shadowTerm;
            Lo += albedo * lights[i].colorRange.rgb * s;
        }
    } else {
        Lo = albedo;
    }

    if (frame.misc.x > 0.5) {
        float f = 1.0 - exp(-frame.fogColorDensity.a * viewDist);
        Lo = Lo + (frame.fogColorDensity.rgb - Lo) * f;
    }

    Lo = max(Lo, vec3(0.0));
    int mode = int(frame.misc.y + 0.5);
    float ex = frame.misc.z;
    vec3 enc = clamp(vec3(linearToSrgbFast(toneMapChannel(Lo.r * ex, mode)),
                          linearToSrgbFast(toneMapChannel(Lo.g * ex, mode)),
                          linearToSrgbFast(toneMapChannel(Lo.b * ex, mode))), 0.0, 1.0);
    // Color channels blend with the shader alpha (SRC_ALPHA pipeline state);
    // the alpha channel keeps whatever the destination holds, which is always
    // 1 - matching the software backend's p[3] = 255.
    outColor = vec4(enc, alpha);
}
