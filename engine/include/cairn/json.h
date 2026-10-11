// Cairn Engine - minimal JSON value type with parser and writer.
// Used for scene serialization, project files and editor communication.
// Objects preserve insertion order (serialized documents stay diff-friendly).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Cairn {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<Json>;
    using Object = std::vector<std::pair<std::string, Json>>;

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string str_;
    std::shared_ptr<Array> arr_;
    std::shared_ptr<Object> obj_;

public:
    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : type_(Type::Bool), bool_(b) {}
    Json(int v) : type_(Type::Number), number_((double)v) {}
    Json(int64_t v) : type_(Type::Number), number_((double)v) {}
    Json(uint32_t v) : type_(Type::Number), number_((double)v) {}
    Json(float v) : type_(Type::Number), number_((double)v) {}
    Json(double v) : type_(Type::Number), number_(v) {}
    Json(const char* s) : type_(Type::String), str_(s) {}
    Json(std::string s) : type_(Type::String), str_(std::move(s)) {}
    Json(Array a) : type_(Type::Array), arr_(std::make_shared<Array>(std::move(a))) {}
    Json(Object o) : type_(Type::Object), obj_(std::make_shared<Object>(std::move(o))) {}

    static Json MakeArray(Array a = Array{}) { return Json(std::move(a)); }
    static Json MakeObject(Object o = Object{}) { return Json(std::move(o)); }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool asBool(bool def = false) const { return isBool() ? bool_ : def; }
    double asNumber(double def = 0.0) const { return isNumber() ? number_ : def; }
    float asFloat(float def = 0.0f) const { return (float)asNumber(def); }
    int asInt(int def = 0) const { return (int)asNumber(def); }
    uint32_t asUint(uint32_t def = 0) const { return (uint32_t)asNumber(def); }
    const std::string& asString() const { static const std::string kEmpty; return isString() ? str_ : kEmpty; }
    std::string asString(const char* def) const { return isString() ? str_ : std::string(def); }

    // Object access ------------------------------------------------------
    bool has(const std::string& key) const;
    const Json* find(const std::string& key) const;
    Json& operator[](const std::string& key);          // creates missing keys
    const Json& operator[](const std::string& key) const { return at(key); }
    // Without this pair, indexing a mutable Json with a string literal leaves
    // MSVC choosing between the string and size_t overloads (C2666). The
    // const char* form is an exact match, so the ambiguity disappears.
    Json& operator[](const char* key) { return (*this)[std::string(key)]; }
    const Json& operator[](const char* key) const { return at(key); }
    const Json& at(const std::string& key) const;      // null if missing
    void set(const std::string& key, Json v);
    const Object& objectItems() const;
    void remove(const std::string& key);

    // Array access -------------------------------------------------------
    void push(Json v);
    size_t size() const;
    Json& operator[](size_t i);
    const Json& operator[](size_t i) const;
    const Json& operator[](int i) const { return (*this)[(size_t)i]; }
    const Array& arrayItems() const;

    // Serializes to compact or indented text.
    std::string Dump(int indent = -1) const;
    // Parses text; returns false and fills *error on failure.
    static bool Parse(const std::string& text, Json* out, std::string* error = nullptr);
    static Json ParseOr(const std::string& text, Json fallback);

    void Write(std::string& out, int indent, int depth) const;
};

}  // namespace Cairn
