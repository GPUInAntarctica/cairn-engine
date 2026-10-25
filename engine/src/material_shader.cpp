#include "material_shader.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace Cairn {

namespace {

// ---------------------------------------------------------------------------
// Expression parser -> stack bytecode.
//
// The language is deliberately scalar and side-effect free: numbers, the
// variables from ShaderInputs, the GLSL maths functions with fixed arity, and
// the four arithmetic operators. No assignment, no branching, no user
// functions - a fragment shader that can loop is a fragment shader that can
// hang, and none of the effects this exists for need either.
// ---------------------------------------------------------------------------

int Arity(const std::string& name) {
    if (name == "sin" || name == "cos" || name == "abs" || name == "sqrt" ||
        name == "fract" || name == "floor")
        return 1;
    if (name == "pow" || name == "min" || name == "max" || name == "step") return 2;
    if (name == "clamp" || name == "mix" || name == "smoothstep") return 3;
    return -1;
}

ShaderOp UnaryOp(const std::string& name) {
    if (name == "sin") return ShaderOp::Sin;
    if (name == "cos") return ShaderOp::Cos;
    if (name == "abs") return ShaderOp::Abs;
    if (name == "sqrt") return ShaderOp::Sqrt;
    if (name == "fract") return ShaderOp::Fract;
    return ShaderOp::Floor;
}

ShaderOp BinaryFuncOp(const std::string& name) {
    if (name == "pow") return ShaderOp::Pow;
    if (name == "min") return ShaderOp::Min;
    if (name == "max") return ShaderOp::Max;
    return ShaderOp::Step;
}

ShaderOp TernaryFuncOp(const std::string& name) {
    if (name == "clamp") return ShaderOp::Clamp;
    if (name == "mix") return ShaderOp::Mix;
    return ShaderOp::SmoothStep;
}

class Parser {
public:
    Parser(const std::string& text, const char* output, std::vector<ShaderInsn>* code,
           int* depth, int* high, std::string* error)
        : s_(text), output_(output), code_(code), depth_(depth), high_(high), error_(error) {}

    bool Run() {
        SkipSpace();
        if (Pos() >= s_.size()) {
            Emit(ShaderOp::Const);  // empty expression = 0
            return true;
        }
        if (!Expression()) return false;
        SkipSpace();
        if (Pos() < s_.size()) {
            Fail("unexpected '" + s_.substr(Pos(), 1) + "'");
            return false;
        }
        return true;
    }

private:
    size_t Pos() const { return pos_; }

    void SkipSpace() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' ||
                                    s_[pos_] == '\r'))
            ++pos_;
    }

    bool Fail(const std::string& what) {
        if (error_ && error_->empty())
            *error_ = std::string(output_) + ": " + what + " at " + std::to_string(pos_);
        return false;
    }

    void Emit(ShaderOp op, float a = 0, float b = 0, float c = 0) {
        if (!error_ || !error_->empty()) return;
        if ((int)code_->size() >= kMaxShaderInsns) {
            *error_ = std::string(output_) + ": program exceeds " +
                      std::to_string(kMaxShaderInsns) + " instructions";
            return;
        }
        ShaderInsn ins;
        ins.op = (int)op;
        ins.a = a;
        ins.b = b;
        ins.c = c;
        code_->push_back(ins);
        // Track the operand stack so a program that could underflow is
        // rejected at compile time instead of at a pixel.
        int delta = 0;
        switch (op) {
            case ShaderOp::Const: case ShaderOp::U: case ShaderOp::V: case ShaderOp::Time:
            case ShaderOp::NdotV: case ShaderOp::NormalX: case ShaderOp::NormalY:
            case ShaderOp::NormalZ: case ShaderOp::ViewX: case ShaderOp::ViewY:
            case ShaderOp::ViewZ: case ShaderOp::Param0: case ShaderOp::Param1:
            case ShaderOp::Param2: case ShaderOp::Param3:
                delta = 1;
                break;
            case ShaderOp::Add: case ShaderOp::Sub: case ShaderOp::Mul: case ShaderOp::Div:
            case ShaderOp::Pow: case ShaderOp::Min: case ShaderOp::Max:
            case ShaderOp::Step:
                delta = -1;
                break;
            case ShaderOp::Clamp: case ShaderOp::Mix: case ShaderOp::SmoothStep:
                delta = -2;
                break;
            case ShaderOp::Neg: case ShaderOp::Sin: case ShaderOp::Cos:
            case ShaderOp::Abs: case ShaderOp::Sqrt: case ShaderOp::Fract:
            case ShaderOp::Floor:
                delta = 0;
                break;
            default:
                break;
        }
        *depth_ += delta;
        *high_ = std::max(*high_, *depth_);
        if (*depth_ > kMaxShaderStack) {
            *error_ = std::string(output_) + ": expression nests deeper than " +
                      std::to_string(kMaxShaderStack) + " values";
        } else if (*depth_ < 0) {
            *error_ = std::string(output_) + ": malformed expression";
        }
    }

    bool Expression() {
        if (!Term()) return false;
        for (;;) {
            SkipSpace();
            if (Pos() >= s_.size() || (s_[Pos()] != '+' && s_[Pos()] != '-')) return true;
            const char op = s_[Pos()];
            ++pos_;
            if (!Term()) return false;
            Emit(op == '+' ? ShaderOp::Add : ShaderOp::Sub);
        }
    }

    bool Term() {
        if (!Unary()) return false;
        for (;;) {
            SkipSpace();
            if (Pos() >= s_.size() || (s_[pos_] != '*' && s_[pos_] != '/')) return true;
            const char op = s_[pos_];
            ++pos_;
            if (!Unary()) return false;
            Emit(op == '*' ? ShaderOp::Mul : ShaderOp::Div);
        }
    }

    bool Unary() {
        SkipSpace();
        if (Pos() < s_.size() && s_[pos_] == '-') {
            ++pos_;
            if (!Unary()) return false;
            Emit(ShaderOp::Neg);
            return true;
        }
        if (Pos() < s_.size() && s_[pos_] == '+') {
            ++pos_;
            return Unary();
        }
        return Primary();
    }

    bool Number() {
        const char* begin = s_.c_str() + pos_;
        char* end = nullptr;
        const double v = std::strtod(begin, &end);
        if (end == begin) return Fail("expected a number");
        pos_ += (size_t)(end - begin);
        Emit(ShaderOp::Const, (float)v);
        return true;
    }

    bool Name() {
        const size_t start = pos_;
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '_';
            if (!ok) break;
            ++pos_;
        }
        if (start == pos_) return Fail("expected a name");
        return true;
    }

    static bool IsVariable(const std::string& name, ShaderOp* op) {
        struct Var { const char* name; ShaderOp op; };
        static const Var kVars[] = {
            {"u", ShaderOp::U}, {"v", ShaderOp::V}, {"time", ShaderOp::Time},
            {"ndotv", ShaderOp::NdotV},
            {"nx", ShaderOp::NormalX}, {"ny", ShaderOp::NormalY}, {"nz", ShaderOp::NormalZ},
            {"vx", ShaderOp::ViewX}, {"vy", ShaderOp::ViewY}, {"vz", ShaderOp::ViewZ},
            {"p0", ShaderOp::Param0}, {"p1", ShaderOp::Param1}, {"p2", ShaderOp::Param2},
            {"p3", ShaderOp::Param3},
        };
        for (const Var& v : kVars) {
            if (name == v.name) { *op = v.op; return true; }
        }
        return false;
    }

    bool Primary() {
        SkipSpace();
        if (Pos() >= s_.size()) return Fail("expected a value");
        if (s_[pos_] == '(') {
            ++pos_;
            if (!Expression()) return false;
            SkipSpace();
            if (Pos() >= s_.size() || s_[pos_] != ')') return Fail("missing ')'");
            ++pos_;
            return true;
        }
        const char c = s_[pos_];
        if ((c >= '0' && c <= '9') || c == '.') return Number();
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            const size_t start = pos_;
            if (!Name()) return false;
            const std::string name = s_.substr(start, pos_ - start);
            SkipSpace();
            const int arity = Arity(name);
            if (arity >= 0) {
                if (Pos() >= s_.size() || s_[pos_] != '(') return Fail("expected '(' after " + name);
                ++pos_;
                for (int i = 0; i < arity; ++i) {
                    if (i > 0) {
                        SkipSpace();
                        if (Pos() >= s_.size() || s_[pos_] != ',')
                            return Fail("expected ',' in " + name);
                        ++pos_;
                    }
                    if (!Expression()) return false;
                }
                SkipSpace();
                if (Pos() >= s_.size() || s_[pos_] != ')') return Fail("missing ')' after " + name);
                ++pos_;
                if (arity == 1) Emit(UnaryOp(name));
                else if (arity == 2) Emit(BinaryFuncOp(name));
                else Emit(TernaryFuncOp(name));
                return true;
            }
            ShaderOp var;
            if (!IsVariable(name, &var)) return Fail("unknown name '" + name + "'");
            Emit(var);
            return true;
        }
        return Fail(std::string("unexpected '") + c + "'");
    }

    const std::string& s_;
    const char* output_;
    std::vector<ShaderInsn>* code_;
    int* depth_;
    int* high_;
    std::string* error_;
    size_t pos_ = 0;
};

}  // namespace

bool CompileShaderSource(const std::string& du, const std::string& dv,
                         const std::string& emissive, MaterialShader* out,
                         std::string* error) {
    if (!out) return false;
    out->code.clear();
    out->stackDepth = 0;
    if (error) error->clear();
    int depth = 0;
    int high = 0;

    const std::string* expressions[3] = {&du, &dv, &emissive};
    const char* names[3] = {"du", "dv", "emissive"};
    for (int i = 0; i < 3; ++i) {
        Parser parser(*expressions[i], names[i], &out->code, &depth, &high, error);
        if (!parser.Run()) return false;
        // Every expression leaves exactly one value; depth is cumulative, so a
        // program that gets here always ends with exactly three.
    }
    if (depth != 3) {
        if (error && error->empty()) *error = "program does not produce three outputs";
        return false;
    }
    out->stackDepth = high;
    out->source = "du: " + du + "\n dv: " + dv + "\n emissive: " + emissive;
    return true;
}

bool CompileShaderDocument(const Json& doc, MaterialShader* out, std::string* error) {
    if (!doc.isObject()) {
        if (error) *error = "shader document is not an object";
        return false;
    }
    const std::string format = doc["format"].asString("cairn-shader");
    if (format != "cairn-shader") {
        if (error) *error = "not a cairn shader (format '" + format + "')";
        return false;
    }
    const std::string du = doc["du"].isString() ? doc["du"].asString() : "";
    const std::string dv = doc["dv"].isString() ? doc["dv"].asString() : "";
    const std::string emissive = doc["emissive"].isString() ? doc["emissive"].asString() : "";
    if (du.empty() && dv.empty() && emissive.empty()) {
        if (error) *error = "shader defines no outputs (du, dv, emissive)";
        return false;
    }
    return CompileShaderSource(du, dv, emissive, out, error);
}

namespace {

float Pop(float* stack, int* sp) {
    if (*sp <= 0) return 0.0f;
    return stack[--(*sp)];
}

void Push(float* stack, int* sp, float v) {
    if (*sp < kMaxShaderStack) stack[(*sp)++] = v;
}

float SafeDiv(float a, float b) { return std::fabs(b) < 1e-8f ? 0.0f : a / b; }

float Clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

float Smooth(float e0, float e1, float x) {
    const float span = e1 - e0;
    if (std::fabs(span) < 1e-8f) return x < e0 ? 0.0f : 1.0f;
    const float t = Clamp01((x - e0) / span);
    return t * t * (3.0f - 2.0f * t);
}

}  // namespace

ShaderOutputs EvalShader(const MaterialShader& shader, const ShaderInputs& in) {
    ShaderOutputs out;
    float stack[kMaxShaderStack];
    int sp = 0;
    for (const ShaderInsn& ins : shader.code) {
        switch ((ShaderOp)ins.op) {
            case ShaderOp::Const: Push(stack, &sp, ins.a); break;
            case ShaderOp::U: Push(stack, &sp, in.u); break;
            case ShaderOp::V: Push(stack, &sp, in.v); break;
            case ShaderOp::Time: Push(stack, &sp, in.time); break;
            case ShaderOp::NdotV: Push(stack, &sp, in.ndotv); break;
            case ShaderOp::NormalX: Push(stack, &sp, in.nx); break;
            case ShaderOp::NormalY: Push(stack, &sp, in.ny); break;
            case ShaderOp::NormalZ: Push(stack, &sp, in.nz); break;
            case ShaderOp::ViewX: Push(stack, &sp, in.vx); break;
            case ShaderOp::ViewY: Push(stack, &sp, in.vy); break;
            case ShaderOp::ViewZ: Push(stack, &sp, in.vz); break;
            case ShaderOp::Param0: Push(stack, &sp, in.p[0]); break;
            case ShaderOp::Param1: Push(stack, &sp, in.p[1]); break;
            case ShaderOp::Param2: Push(stack, &sp, in.p[2]); break;
            case ShaderOp::Param3: Push(stack, &sp, in.p[3]); break;
            case ShaderOp::Add: { float b = Pop(stack, &sp), a = Pop(stack, &sp); Push(stack, &sp, a + b); break; }
            case ShaderOp::Sub: { float b = Pop(stack, &sp), a = Pop(stack, &sp); Push(stack, &sp, a - b); break; }
            case ShaderOp::Mul: { float b = Pop(stack, &sp), a = Pop(stack, &sp); Push(stack, &sp, a * b); break; }
            case ShaderOp::Div: { float b = Pop(stack, &sp), a = Pop(stack, &sp); Push(stack, &sp, SafeDiv(a, b)); break; }
            case ShaderOp::Neg: { Push(stack, &sp, -Pop(stack, &sp)); break; }
            case ShaderOp::Sin: { Push(stack, &sp, std::sin(Pop(stack, &sp))); break; }
            case ShaderOp::Cos: { Push(stack, &sp, std::cos(Pop(stack, &sp))); break; }
            case ShaderOp::Abs: { Push(stack, &sp, std::fabs(Pop(stack, &sp))); break; }
            case ShaderOp::Sqrt: { Push(stack, &sp, std::sqrt(std::max(0.0f, Pop(stack, &sp)))); break; }
            case ShaderOp::Fract: { float x = Pop(stack, &sp); Push(stack, &sp, x - std::floor(x)); break; }
            case ShaderOp::Floor: { Push(stack, &sp, std::floor(Pop(stack, &sp))); break; }
            case ShaderOp::Pow: {
                float b = Pop(stack, &sp), a = Pop(stack, &sp);
                Push(stack, &sp, std::pow(std::max(0.0f, a), b));
                break;
            }
            case ShaderOp::Min: { float b = Pop(stack, &sp), a = Pop(stack, &sp); Push(stack, &sp, std::min(a, b)); break; }
            case ShaderOp::Max: { float b = Pop(stack, &sp), a = Pop(stack, &sp); Push(stack, &sp, std::max(a, b)); break; }
            case ShaderOp::Step: { float x = Pop(stack, &sp), edge = Pop(stack, &sp); Push(stack, &sp, x < edge ? 0.0f : 1.0f); break; }
            case ShaderOp::Clamp: {
                float hi = Pop(stack, &sp), lo = Pop(stack, &sp), x = Pop(stack, &sp);
                Push(stack, &sp, std::clamp(x, lo, hi));
                break;
            }
            case ShaderOp::Mix: {
                float t = Pop(stack, &sp), b = Pop(stack, &sp), a = Pop(stack, &sp);
                Push(stack, &sp, a + (b - a) * t);
                break;
            }
            case ShaderOp::SmoothStep: {
                float x = Pop(stack, &sp), e1 = Pop(stack, &sp), e0 = Pop(stack, &sp);
                Push(stack, &sp, Smooth(e0, e1, x));
                break;
            }
            default:
                // Unknown opcode: stop rather than read past the stack. The
                // outputs below are then zero, the "no effect" value.
                sp = 0;
                break;
        }
    }
    if (sp >= 3) {
        out.du = stack[0];
        out.dv = stack[1];
        out.emissive = stack[2];
    }
    return out;
}

}  // namespace Cairn
