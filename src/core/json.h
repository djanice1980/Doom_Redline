#pragma once
// A small JSON value with a parser and a writer: enough for the run records
// the game writes and the replies it reads from the online service. No
// dependencies; numbers are doubles; strings are UTF-8 passed through.
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace rl {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : type_(Type::Bool), bool_(b) {}
    Json(int v) : type_(Type::Number), num_(v) {}
    Json(int64_t v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    Json(double v) : type_(Type::Number), num_(v) {}
    Json(const char* s) : type_(Type::String), str_(s) {}
    Json(std::string s) : type_(Type::String), str_(std::move(s)) {}
    static Json array() { Json j; j.type_ = Type::Array; return j; }
    static Json object() { Json j; j.type_ = Type::Object; return j; }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isObject() const { return type_ == Type::Object; }
    bool isArray() const { return type_ == Type::Array; }
    bool isString() const { return type_ == Type::String; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isBool() const { return type_ == Type::Bool; }

    // Readers with defaults, so a missing or mistyped field never throws.
    const Json& operator[](const std::string& key) const;   // object member or a static null
    const Json& operator[](size_t index) const;             // array element or a static null
    const Json& get(const std::string& key) const { return (*this)[key]; }
    std::string asString(const std::string& def = "") const { return type_ == Type::String ? str_ : def; }
    double asNumber(double def = 0.0) const { return type_ == Type::Number ? num_ : def; }
    int asInt(int def = 0) const { return type_ == Type::Number ? static_cast<int>(num_) : def; }
    bool asBool(bool def = false) const { return type_ == Type::Bool ? bool_ : def; }
    size_t size() const { return type_ == Type::Array ? arr_.size() : type_ == Type::Object ? obj_.size() : 0; }
    const std::vector<Json>& items() const { return arr_; }
    const std::map<std::string, Json>& members() const { return obj_; }
    bool has(const std::string& key) const { return type_ == Type::Object && obj_.count(key) > 0; }

    // Writers.
    Json& set(const std::string& key, Json value);   // makes this an object if it is null
    Json& push(Json value);                          // makes this an array if it is null

    std::string dump(int indent = 0) const;   // indent 0 = compact
    // Parses `text`; on failure returns false and leaves `err` with a message.
    static bool parse(const std::string& text, Json& out, std::string* err = nullptr);

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    std::vector<Json> arr_;
    std::map<std::string, Json> obj_;
    void dumpTo(std::string& out, int indent, int depth) const;
};

std::string jsonEscape(const std::string& s);

}  // namespace rl
