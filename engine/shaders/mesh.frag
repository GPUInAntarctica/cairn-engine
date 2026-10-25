#version 450
#extension GL_EXT_nonuniform_qualifier : require
// Cairn mesh fragment stage: GGX/Cook-Torrance specular + Lambert diffuse
// (metallic workflow), cascaded directional shadows with 3x3 PCF, exponential
// height fog, then the shared tonemap + sRGB encode. This mirrors
// Renderer::ShadeAndStore on the software path formula for formula.
layout(location = 0) in vec3 vWorld;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vTangent;
layout(location = 3) in vec2 vUv;
layout(location = 4) flat in uint vInstance;
layout(location = 0) out vec4 outColor;

layout(std140, set = 0, binding = 0) uniform FrameUBO {
    mat4 viewProj;
    vec4 camPosOrtho;
    vec4 camForward;
    vec4 camRightTH;
    vec4 camUp;
    vec4 ambient;
    vec4 fogColorDensity;
    vec4 misc;       // x fogEnabled, y tonemap, z exposure
    vec4 skyTop;
    vec4 skyBottom;
    vec4 viewport;
    vec4 clearColor;
    vec4 timeParams;  // x animation clock for material shaders
} frame;

struct Light {
    vec4 posType;    // xyz world pos, w type (0 dir, 1 point, 2 spot)
    vec4 dirInten;   // xyz direction, w intensity
    vec4 colorRange; // rgb color (raw), a range
    vec4 cone;       // x cosOuter, y cosInner
};
layout(std140, set = 0, binding = 1) uniform LightsUBO {
    Light lights[16];
    int lightCount;
};

layout(std140, set = 0, binding = 2) uniform ShadowUBO {
    mat4 cascadeMatrix[4];
    vec4 cascadeSplits;   // far boundary of each cascade (view depth)
    vec4 cascadeRange;    // ortho world depth span per cascade
    vec4 cascadeTexel;    // world units per shadow texel
    vec4 shadowParams;    // x count, y strength, z map size, w unused
    vec4 shadowToLight;   // xyz towards the shadow-casting light
} shadow;

layout(set = 0, binding = 3) uniform sampler2DArray shadowMaps;

struct DrawUbo {
    mat4 model;
    vec4 color;        // sRGB albedo + alpha
    vec4 emissive;     // linear rgb (decoded on the CPU at fill time)
    vec4 tilingSpec;   // xy tiling, z specular, w shininess
    vec4 surf;         // x metallic, y roughness, z normalStrength, w unused
    vec4 flags;        // x unlit, y receiveShadows, z albedoSRGB, w hasNormalMap
    vec4 misc;
    // Bindless sampler slots (x albedo, y normal) into the texture arrays
    // below; they let one instanced draw span materials with different maps.
    vec4 texIndex;
    // Padding so the std140 array stride is 256 bytes, matching the CPU
    // DrawUbo stride (and the 256-byte dynamic-offset window).
    vec4 pad[5];
};
layout(std140, set = 0, binding = 4) uniform DrawUBO {
    DrawUbo draws[256];   // one 64 KiB window: 256 x 256-byte stride
};

// Material shader bytecode: four floats per instruction (opcode, a, b, c).
// DrawUbo::misc.y is the first instruction of this draw's program and misc.z
// its length; pad[0..3] are the program's p0..p3 parameters. The bytecode is
// the same one EvalShader runs on the software path.
struct ShaderInsn {
    vec4 data;
};
layout(std430, set = 0, binding = 5) readonly buffer ShaderCode {
    ShaderInsn code[];
} shaders;

// Opcodes, matching enum class ShaderOp in material_shader.h.
const int kOpConst = 0;
const int kOpU = 1;
const int kOpV = 2;
const int kOpTime = 3;
const int kOpNdotV = 4;
const int kOpNormalX = 5;
const int kOpNormalY = 6;
const int kOpNormalZ = 7;
const int kOpViewX = 8;
const int kOpViewY = 9;
const int kOpViewZ = 10;
const int kOpParam0 = 11;
const int kOpAdd = 15;
const int kOpSub = 16;
const int kOpMul = 17;
const int kOpDiv = 18;
const int kOpNeg = 19;
const int kOpSin = 20;
const int kOpCos = 21;
const int kOpAbs = 22;
const int kOpSqrt = 23;
const int kOpFract = 24;
const int kOpFloor = 25;
const int kOpPow = 26;
const int kOpMin = 27;
const int kOpMax = 28;
const int kOpClamp = 29;
const int kOpMix = 30;
const int kOpStep = 31;
const int kOpSmoothStep = 32;
const int kShaderStack = 16;

// One frame's worth of program straight out of the shared bytecode. Arithmetic
// is guarded the same way as the CPU evaluator: division by zero yields zero,
// pow clamps its base, sqrt clamps its argument, and an unknown opcode clears
// the stack instead of reading past it.
void evalMaterialShader(float first, float count, vec2 baseUv, vec3 geomN, vec3 V,
                        vec4 params, out vec2 uvOffset, out float emissive) {
    uvOffset = vec2(0.0);
    emissive = 0.0;
    int start = int(first);
    int n = int(count);
    if (n <= 0) return;

    float stack[kShaderStack];
    int sp = 0;
#define CAIRN_PUSH(v) do { if (sp < kShaderStack) stack[sp++] = (v); } while (false)
#define CAIRN_POP() (sp > 0 ? stack[--sp] : 0.0)

    for (int i = 0; i < n; ++i) {
        vec4 ins = shaders.code[start + i].data;
        int op = int(ins.x);
        float a = ins.y, b = ins.z, c = ins.w;
        if (op == kOpConst) {
            CAIRN_PUSH(a);
        } else if (op == kOpU) {
            CAIRN_PUSH(baseUv.x);
        } else if (op == kOpV) {
            CAIRN_PUSH(baseUv.y);
        } else if (op == kOpTime) {
            CAIRN_PUSH(frame.timeParams.x);
        } else if (op == kOpNdotV) {
            CAIRN_PUSH(dot(geomN, V));
        } else if (op == kOpNormalX) {
            CAIRN_PUSH(geomN.x);
        } else if (op == kOpNormalY) {
            CAIRN_PUSH(geomN.y);
        } else if (op == kOpNormalZ) {
            CAIRN_PUSH(geomN.z);
        } else if (op == kOpViewX) {
            CAIRN_PUSH(V.x);
        } else if (op == kOpViewY) {
            CAIRN_PUSH(V.y);
        } else if (op == kOpViewZ) {
            CAIRN_PUSH(V.z);
        } else if (op == kOpParam0) {
            CAIRN_PUSH(params.x);
        } else if (op == kOpParam0 + 1) {
            CAIRN_PUSH(params.y);
        } else if (op == kOpParam0 + 2) {
            CAIRN_PUSH(params.z);
        } else if (op == kOpParam0 + 3) {
            CAIRN_PUSH(params.w);
        } else if (op == kOpAdd) {
            float rhs = CAIRN_POP(); float lhs = CAIRN_POP(); CAIRN_PUSH(lhs + rhs);
        } else if (op == kOpSub) {
            float rhs = CAIRN_POP(); float lhs = CAIRN_POP(); CAIRN_PUSH(lhs - rhs);
        } else if (op == kOpMul) {
            float rhs = CAIRN_POP(); float lhs = CAIRN_POP(); CAIRN_PUSH(lhs * rhs);
        } else if (op == kOpDiv) {
            float rhs = CAIRN_POP(); float lhs = CAIRN_POP();
            CAIRN_PUSH(abs(rhs) < 1e-8 ? 0.0 : lhs / rhs);
        } else if (op == kOpNeg) {
            CAIRN_PUSH(-CAIRN_POP());
        } else if (op == kOpSin) {
            CAIRN_PUSH(sin(CAIRN_POP()));
        } else if (op == kOpCos) {
            CAIRN_PUSH(cos(CAIRN_POP()));
        } else if (op == kOpAbs) {
            CAIRN_PUSH(abs(CAIRN_POP()));
        } else if (op == kOpSqrt) {
            CAIRN_PUSH(sqrt(max(0.0, CAIRN_POP())));
        } else if (op == kOpFract) {
            float x = CAIRN_POP(); CAIRN_PUSH(x - floor(x));
        } else if (op == kOpFloor) {
            CAIRN_PUSH(floor(CAIRN_POP()));
        } else if (op == kOpPow) {
            float exponent = CAIRN_POP(); float base = CAIRN_POP();
            CAIRN_PUSH(pow(max(0.0, base), exponent));
        } else if (op == kOpMin) {
            float rhs = CAIRN_POP(); CAIRN_PUSH(min(CAIRN_POP(), rhs));
        } else if (op == kOpMax) {
            float rhs = CAIRN_POP(); CAIRN_PUSH(max(CAIRN_POP(), rhs));
        } else if (op == kOpClamp) {
            float hi = CAIRN_POP(); float lo = CAIRN_POP(); float x = CAIRN_POP();
            CAIRN_PUSH(clamp(x, lo, hi));
        } else if (op == kOpMix) {
            float t = CAIRN_POP(); float rhs = CAIRN_POP(); float lhs = CAIRN_POP();
            CAIRN_PUSH(lhs + (rhs - lhs) * t);
        } else if (op == kOpStep) {
            float x = CAIRN_POP(); float edge = CAIRN_POP();
            CAIRN_PUSH(x < edge ? 0.0 : 1.0);
        } else if (op == kOpSmoothStep) {
            float x = CAIRN_POP(); float e1 = CAIRN_POP(); float e0 = CAIRN_POP();
            float span = e1 - e0;
            if (abs(span) < 1e-8) {
                CAIRN_PUSH(x < e0 ? 0.0 : 1.0);
            } else {
                float t = clamp((x - e0) / span, 0.0, 1.0);
                CAIRN_PUSH(t * t * (3.0 - 2.0 * t));
            }
        } else {
            sp = 0;
        }
    }
    if (sp >= 3) {
        uvOffset = vec2(stack[0], stack[1]);
        emissive = stack[2];
    }
#undef CAIRN_PUSH
#undef CAIRN_POP
}

// Bindless texture arrays: every texture the frame samples is given a stable
// slot, so a batched run no longer has to share one albedo / normal pair.
// The size must match Impl::kBindlessSlots in backend_vulkan.cpp.
layout(set = 1, binding = 0) uniform sampler2D albedoMaps[512];
layout(set = 1, binding = 1) uniform sampler2D normalMaps[512];

const float kPi = 3.14159265;

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
    if (type < 0.5) {  // directional
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
    if (type > 1.5) {  // spot cone
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

    // World-unit bias (1.5 texels + slope term) converted with the cascade's
    // orthographic depth span - identical to SampleShadow on the CPU.
    float range = shadow.cascadeRange[cascade];
    float texelWorld = shadow.cascadeTexel[cascade];
    float worldBias = texelWorld * (1.5 + 2.0 * (1.0 - clamp(ndotl, 0.0, 1.0)));
    float bias = worldBias / max(1e-3, range);
    float refDepth = ndc.z * 0.5 + 0.5;
    float size = shadow.shadowParams.z;
    float fx = (ndc.x * 0.5 + 0.5) * size;
    // shadow.vert negates clip-space y so the depth pass rasterises into the
    // same top-down rows the software pass writes: with Vulkan's y-down NDC the
    // written row for this point is (1 - (ndc.y * 0.5 + 0.5)) * size. Reading
    // the unflipped row looks at the mirror image of the map and finds nothing.
    float fy = (1.0 - (ndc.y * 0.5 + 0.5)) * size;
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
    DrawUbo draw = draws[vInstance];
    vec3 P = vWorld;
    vec3 V = frame.camPosOrtho.xyz - P;
    float viewDist = length(V);
    if (viewDist > 1e-6) V /= viewDist;

    // Geometric normal, unit length: the shader reads the same one the shadow
    // bias does, and the software path feeds its interpolated normal here too.
    vec3 geomN = vNormal;
    if (dot(geomN, geomN) > 1e-9) geomN = normalize(geomN); else geomN = vec3(0, 1, 0);

    // A material program displaces the base UV before both texture lookups, so
    // albedo and normal map stay aligned. Its emissive output is applied after
    // the lighting branch below.
    vec2 uvOffset = vec2(0.0);
    float shaderEmissive = 0.0;
    if (draw.misc.z > 0.5) {
        evalMaterialShader(draw.misc.y, draw.misc.z, vUv, geomN, V, draw.pad[0],
                           uvOffset, shaderEmissive);
    }
    vec2 shaderUv = vUv + uvOffset;

    vec4 tex = texture(albedoMaps[nonuniformEXT(int(draw.texIndex.x))], shaderUv * draw.tilingSpec.xy);
    vec3 albedo = srgbToLinearFast3(draw.color.rgb);
    if (draw.flags.z > 0.5) albedo *= srgbToLinearFast3(tex.rgb);
    else albedo *= tex.rgb;

    // Shadow lookup with the geometric (pre-normal-map) normal.
    float shadowFactor = 1.0;
    if (shadow.shadowParams.x > 0.5 && draw.flags.y > 0.5) {
        float viewDepth = dot(P - frame.camPosOrtho.xyz, frame.camForward.xyz);
        float ndotl = max(dot(geomN, shadow.shadowToLight.xyz), 0.0);
        shadowFactor = sampleShadow(P, viewDepth, ndotl);
    }

    vec3 N = vNormal;
    if (dot(N, N) < 1e-9) N = vec3(0, 1, 0);
    N = normalize(N);
    if (draw.flags.w > 0.5) {  // tangent-space normal mapping
        float hand = vTangent.w;
        vec3 T = vTangent.xyz;
        if (abs(hand) > 1e-6 && dot(T, T) > 1e-12) {
            T = normalize(T - N * dot(N, T));
            vec3 B = cross(N, T) * (hand >= 0.0 ? 1.0 : -1.0);
            vec4 nm = texture(normalMaps[nonuniformEXT(int(draw.texIndex.y))], shaderUv * draw.tilingSpec.xy);
            vec3 nts = vec3((nm.r * 2.0 - 1.0) * draw.surf.z,
                            -(nm.g * 2.0 - 1.0) * draw.surf.z,
                            nm.b * 2.0 - 1.0);
            N = normalize(T * nts.x + B * nts.y + N * nts.z);
        }
    }
    if (dot(N, V) < 0.0) N = -N;  // two-sided

    float metallic = clamp(draw.surf.x, 0.0, 1.0);
    float roughness = draw.surf.y;
    if (roughness < 0.0) roughness = pow(2.0 / (max(draw.tilingSpec.w, 1.0) + 2.0), 0.25);
    roughness = clamp(roughness, 0.04, 1.0);
    float alphaR = roughness * roughness;
    float alpha2 = alphaR * alphaR;
    float specScale = 0.16 * clamp(draw.tilingSpec.z, 0.0, 1.0);
    vec3 F0 = albedo * metallic + vec3(specScale) * (1.0 - metallic);
    float NdotV = max(dot(N, V), 1e-4);
    bool wantsSpecular = metallic > 0.0 || draw.tilingSpec.z > 0.0;
    vec3 diffuseAlbedo = albedo * (1.0 - metallic);

    vec3 Lo;
    if (draw.flags.x > 0.5) {
        // Unlit materials hand their albedo straight to the shared tonemap:
        // no ambient, no lights, no emissive. This mirrors the software path's
        // unlit branch in Renderer::ShadeAndStore.
        Lo = albedo;
    } else {
        Lo = diffuseAlbedo * frame.ambient.rgb;
        for (int i = 0; i < lightCount; ++i) {
            vec3 L;
            float atten;
            if (!lightFalloff(i, P, L, atten)) continue;
            float NdotL = dot(N, L);
            if (NdotL <= 0.0) continue;
            float shadowTerm = lights[i].posType.w < 0.5 ? shadowFactor : 1.0;
            float s = lights[i].dirInten.w * atten * shadowTerm * NdotL;
            vec3 H = L + V;
            float hl = length(H);
            if (hl > 1e-5) H /= hl;
            float LdotH = clamp(dot(L, H), 0.0, 1.0);
            vec3 F = F0 + (vec3(1.0) - F0) * pow(1.0 - LdotH, 5.0);
            Lo += lights[i].colorRange.rgb * s * diffuseAlbedo * (1.0 - F);
            if (wantsSpecular) {
                float NdotH = clamp(dot(N, H), 0.0, 1.0);
                float dd = NdotH * NdotH * (alpha2 - 1.0) + 1.0;
                float D = alpha2 / (kPi * dd * dd);
                float gv = NdotL * sqrt(NdotV * NdotV * (1.0 - alpha2) + alpha2);
                float gl = NdotV * sqrt(NdotL * NdotL * (1.0 - alpha2) + alpha2);
                float Vis = 0.5 / max(gv + gl, 1e-7);
                float spec = D * Vis * kPi;
                Lo += lights[i].colorRange.rgb * s * F * spec;
            }
        }
        Lo += draw.emissive.rgb;
    }

    if (shaderEmissive != 0.0) {
        // The program's emissive output masks the material's emissive colour.
        // Applied outside the branch so unlit materials glow too.
        Lo += draw.emissive.rgb * shaderEmissive;
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
    outColor = vec4(enc, 1.0);
}
