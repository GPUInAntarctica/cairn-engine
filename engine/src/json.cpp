#include "cairn/json.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace Cairn {

namespace {

void EscapeString(const std::string& s, std::string& out) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    out += '"';
}

void WriteNumber(double v, std::string& out) {
    if (std::isnan(v) || std::isinf(v)) {
        out += "null";
        return;
    }
    char buf[32];
    if (v == (double)(int64_t)v && std::fabs(v) < 1e15) {
        std::snprintf(buf, sizeof(buf), "%lld", (long long)v);
    } else {
        std::snprintf(buf, sizeof(buf), "%.10g", v);
    }
    out += buf;
}

struct Parser {
    const char* p = nullptr;
    const char* end = nullptr;
    std::string error;

    bool Fail(const std::string& msg) {
        if (error.empty()) error = msg;
        return false;
    }
    void SkipWs() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }
    bool ParseValue(Json* out, int depth) {
        if (depth > 64) return Fail("nesting too deep");
        SkipWs();
        if (p >= end) return Fail("unexpected end of input");
        char c = *p;
        if (c == '{') return ParseObject(out, depth);
        if (c == '[') return ParseArray(out, depth);
        if (c == '"') return ParseStringRaw(out);
        if (c == 't') return ParseLit(out, "true", Json(true));
        if (c == 'f') return ParseLit(out, "false", Json(false));
        if (c == 'n') return ParseLit(out, "null", Json());
        return ParseNumber(out);
    }
    bool ParseLit(Json* out, const char* lit, Json value) {
        size_t len = std::strlen(lit);
        if ((size_t)(end - p) < len || std::strncmp(p, lit, len) != 0)
            return Fail(std::string("invalid literal, expected '") + lit + "'");
        p += len;
        *out = std::move(value);
        return true;
    }
    bool ParseNumber(Json* out) {
        const char* start = p;
        if (p < end && (*p == '-' || *p == '+')) ++p;
        while (p < end && (std::isdigit((unsigned char)*p) || *p == '.' || *p == 'e' || *p == 'E' || *p == '-' || *p == '+')) ++p;
        if (p == start) return Fail("invalid number");
        char buf[64];
        size_t len = std::min((size_t)(p - start), sizeof(buf) - 1);
        std::memcpy(buf, start, len);
        buf[len] = '\0';
        char* endptr = nullptr;
        double v = std::strtod(buf, &endptr);
        if (endptr != buf + len) return Fail("invalid number");
        *out = Json(v);
        return true;
    }
    bool ParseStringRaw(Json* out) {
        if (*p != '"') return Fail("expected string");
        ++p;
        std::string s;
        while (p < end) {
            char c = *p;
            if (c == '"') {
                ++p;
                *out = Json(std::move(s));
                return true;
            }
            if (c == '\\') {
                ++p;
                if (p >= end) break;
                char e = *p++;
                switch (e) {
                    case '"': s += '"'; break;
                    case '\\': s += '\\'; break;
                    case '/': s += '/'; break;
                    case 'n': s += '\n'; break;
                    case 'r': s += '\r'; break;
                    case 't': s += '\t'; break;
                    case 'b': s += '\b'; break;
                    case 'f': s += '\f'; break;
                    case 'u': {
                        if (end - p < 4) return Fail("bad \\u escape");
                        unsigned cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = *p++;
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                            else return Fail("bad \\u escape");
                        }
                        // Surrogate pair handling.
                        if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                            unsigned lo = 0;
                            bool ok = true;
                            for (int i = 0; i < 4; ++i) {
                                char h = p[2 + i];
                                lo <<= 4;
                                if (h >= '0' && h <= '9') lo |= (unsigned)(h - '0');
                                else if (h >= 'a' && h <= 'f') lo |= (unsigned)(h - 'a' + 10);
                                else if (h >= 'A' && h <= 'F') lo |= (unsigned)(h - 'A' + 10);
                                else { ok = false; break; }
                            }
                            if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                                p += 6;
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            }
                        }
                        // UTF-8 encode.
                        if (cp < 0x80) s += (char)cp;
                        else if (cp < 0x800) {
                            s += (char)(0xC0 | (cp >> 6));
                            s += (char)(0x80 | (cp & 0x3F));
                        } else if (cp < 0x10000) {
                            s += (char)(0xE0 | (cp >> 12));
                            s += (char)(0x80 | ((cp >> 6) & 0x3F));
                            s += (char)(0x80 | (cp & 0x3F));
                        } else {
                            s += (char)(0xF0 | (cp >> 18));
                            s += (char)(0x80 | ((cp >> 12) & 0x3F));
                            s += (char)(0x80 | ((cp >> 6) & 0x3F));
                            s += (char)(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default: return Fail("bad escape character");
                }
            } else {
                s += c;
                ++p;
            }
        }
        return Fail("unterminated string");
    }
    bool ParseArray(Json* out, int depth) {
        ++p;  // [
        Json arr = Json::MakeArray();
        SkipWs();
        if (p < end && *p == ']') { ++p; *out = std::move(arr); return true; }
        while (true) {
            Json v;
            if (!ParseValue(&v, depth + 1)) return false;
            arr.push(std::move(v));
            SkipWs();
            if (p >= end) return Fail("unterminated array");
            if (*p == ',') { ++p; continue; }
            if (*p == ']') { ++p; *out = std::move(arr); return true; }
            return Fail("expected ',' or ']' in array");
        }
    }
    bool ParseObject(Json* out, int depth) {
        ++p;  // {
        Json obj = Json::MakeObject();
        SkipWs();
        if (p < end && *p == '}') { ++p; *out = std::move(obj); return true; }
        while (true) {
            SkipWs();
            Json key;
            if (p >= end || *p != '"') return Fail("expected object key");
            if (!ParseStringRaw(&key)) return false;
            SkipWs();
            if (p >= end || *p != ':') return Fail("expected ':' after key");
            ++p;
            Json v;
            if (!ParseValue(&v, depth + 1)) return false;
            obj.set(key.asString(), std::move(v));
            SkipWs();
            if (p >= end) return Fail("unterminated object");
            if (*p == ',') { ++p; continue; }
            if (*p == '}') { ++p; *out = std::move(obj); return true; }
            return Fail("expected ',' or '}' in object");
        }
    }
};

}  // namespace

bool Json::has(const std::string& key) const { return find(key) != nullptr; }

const Json* Json::find(const std::string& key) const {
    if (type_ != Type::Object || !obj_) return nullptr;
    for (const auto& kv : *obj_)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

Json& Json::operator[](const std::string& key) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        if (!obj_) obj_ = std::make_shared<Object>();
        else obj_->clear();
    }
    for (auto& kv : *obj_)
        if (kv.first == key) return kv.second;
    obj_->emplace_back(key, Json());
    return obj_->back().second;
}

const Json& Json::at(const std::string& key) const {
    static const Json kNull;
    const Json* v = find(key);
    return v ? *v : kNull;
}

void Json::set(const std::string& key, Json v) { (*this)[key] = std::move(v); }

void Json::remove(const std::string& key) {
    if (type_ != Type::Object || !obj_) return;
    for (auto it = obj_->begin(); it != obj_->end(); ++it) {
        if (it->first == key) {
            obj_->erase(it);
            return;
        }
    }
}

void Json::push(Json v) {
    if (type_ != Type::Array) {
        type_ = Type::Array;
        if (!arr_) arr_ = std::make_shared<Array>();
        else arr_->clear();
    }
    arr_->push_back(std::move(v));
}

size_t Json::size() const {
    if (type_ == Type::Array && arr_) return arr_->size();
    if (type_ == Type::Object && obj_) return obj_->size();
    return 0;
}

Json& Json::operator[](size_t i) {
    static Json kNull;
    if (type_ != Type::Array || !arr_ || i >= arr_->size()) return kNull;
    return (*arr_)[i];
}

const Json& Json::operator[](size_t i) const {
    static const Json kNull;
    if (type_ != Type::Array || !arr_ || i >= arr_->size()) return kNull;
    return (*arr_)[i];
}

const Json::Array& Json::arrayItems() const {
    static const Array kEmpty;
    return (type_ == Type::Array && arr_) ? *arr_ : kEmpty;
}

const Json::Object& Json::objectItems() const {
    static const Object kEmpty;
    return (type_ == Type::Object && obj_) ? *obj_ : kEmpty;
}

void Json::Write(std::string& out, int indent, int depth) const {
    auto nl = [&](int d) {
        if (indent >= 0) {
            out += '\n';
            out.append((size_t)(indent * d), ' ');
        }
    };
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: WriteNumber(number_, out); break;
        case Type::String: EscapeString(str_, out); break;
        case Type::Array: {
            out += '[';
            bool first = true;
            for (const auto& v : *arr_) {
                if (!first) out += ',';
                first = false;
                nl(depth + 1);
                v.Write(out, indent, depth + 1);
            }
            if (!arr_->empty()) nl(depth);
            out += ']';
            break;
        }
        case Type::Object: {
            out += '{';
            bool first = true;
            for (const auto& kv : *obj_) {
                if (!first) out += ',';
                first = false;
                nl(depth + 1);
                EscapeString(kv.first, out);
                out += indent >= 0 ? ": " : ":";
                kv.second.Write(out, indent, depth + 1);
            }
            if (!obj_->empty()) nl(depth);
            out += '}';
            break;
        }
    }
}

std::string Json::Dump(int indent) const {
    std::string out;
    out.reserve(256);
    Write(out, indent, 0);
    return out;
}

bool Json::Parse(const std::string& text, Json* out, std::string* error) {
    Parser parser;
    parser.p = text.data();
    parser.end = text.data() + text.size();
    Json v;
    if (!parser.ParseValue(&v, 0)) {
        if (error) *error = parser.error.empty() ? "parse error" : parser.error;
        return false;
    }
    parser.SkipWs();
    if (parser.p != parser.end) {
        if (error) *error = "trailing characters after JSON value";
        return false;
    }
    *out = std::move(v);
    return true;
}

Json Json::ParseOr(const std::string& text, Json fallback) {
    Json v;
    if (Parse(text, &v)) return v;
    return fallback;
}

}  // namespace Cairn
