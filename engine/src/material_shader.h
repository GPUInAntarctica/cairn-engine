// Cairn Engine - material shader programs (internal).
//
// A material shader is a small, deterministic bytecode program evaluated per
// fragment by BOTH backends: the software rasterizer runs it in C++, the Vulkan
// fragment stage runs the same instructions in GLSL. The program is compiled
// from three scalar expressions in a .cshader document:
//
//   du        - offset added to the albedo/normal UV, x
//   dv        - the same, y
//   emissive  - how much of the material's emissive colour to add on top
//
// Scalar-only by design: a stack machine over floats is small enough to keep
// one implementation per backend in step, and it is what lets a picture that
// uses a custom shader still have a reference implementation to compare against.
#pragma once

#include "cairn/json.h"

#include <string>
#include <vector>

namespace Cairn {

enum class ShaderOp : int {
    Const = 0,  // y = value
    U,          // base u
    V,          // base v
    Time,
    NdotV,
    NormalX, NormalY, NormalZ,
    ViewX, ViewY, ViewZ,
    Param0, Param1, Param2, Param3,
    // Binary arithmetic pops two values and pushes one.
    Add, Sub, Mul, Div,
    Neg,
    // Unary functions.
    Sin, Cos, Abs, Sqrt, Fract, Floor,
    // The rest take their GLSL argument counts.
    Pow,     // (base, exponent)
    Min, Max,
    Clamp,   // (x, lo, hi)
    Mix,     // (a, b, t)
    Step,    // (edge, x)
    SmoothStep,  // (edge0, edge1, x)
    Count
};

struct ShaderInsn {
    int op = 0;
    float a = 0, b = 0, c = 0;
};

struct MaterialShader {
    std::string name;    // file path or @preset, for diagnostics
    std::string source;  // the three expressions, for diagnostics
    std::vector<ShaderInsn> code;
    int stackDepth = 0;  // high-water mark of the operand stack
};

struct ShaderInputs {
    float u = 0, v = 0;
    float time = 0;
    float ndotv = 0;
    float nx = 0, ny = 1, nz = 0;  // geometric normal, unit length
    float vx = 0, vy = 0, vz = -1; // unit vector from surface to camera
    float p[4] = {1, 1, 1, 1};     // material shaderParams
};

struct ShaderOutputs {
    float du = 0, dv = 0, emissive = 0;
};

constexpr int kMaxShaderInsns = 64;
constexpr int kMaxShaderStack = 16;

// Compiles the three expressions. Empty strings compile to 0 (for du/dv) or 0
// (emissive), which is the "this output does nothing" value.
bool CompileShaderSource(const std::string& du, const std::string& dv,
                         const std::string& emissive, MaterialShader* out,
                         std::string* error);

// Compiles a .cshader document: an object whose optional "du", "dv" and
// "emissive" keys are expression strings. Unknown keys are ignored so a
// document can carry editor metadata without breaking older engines.
bool CompileShaderDocument(const Json& doc, MaterialShader* out, std::string* error);

// Evaluates a compiled program. Never runs past a missing operand: an
// malformed program yields the origin, which is what "no effect" looks like.
ShaderOutputs EvalShader(const MaterialShader& shader, const ShaderInputs& in);

}  // namespace Cairn
