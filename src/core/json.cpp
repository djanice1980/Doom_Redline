#include "core/json.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace rl {

namespace {
const Json& nullJson() { static const Json n; return n; }

struct Parser {
    const std::string& s;
    size_t i = 0;
    std::string err;
    explicit Parser(const std::string& text) : s(text) {}

    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i; }
    bool fail(const char* m) { if (err.empty()) err = std::string(m) + " at offset " + std::to_string(i); return false; }

    bool value(Json& out, int depth) {
        if (depth > 200) return fail("nesting too deep");
        ws();
        if (i >= s.size()) return fail("unexpected end");
        const char c = s[i];
        if (c == '{') return object(out, depth);
        if (c == '[') return array(out, depth);
        if (c == '"') { std::string str; if (!string(str)) return false; out = Json(std::move(str)); return true; }
        if (c == 't' && s.compare(i, 4, "true") == 0) { i += 4; out = Json(true); return true; }
        if (c == 'f' && s.compare(i, 5, "false") == 0) { i += 5; out = Json(false); return true; }
        if (c == 'n' && s.compare(i, 4, "null") == 0) { i += 4; out = Json(); return true; }
        if (c == '-' || (c >= '0' && c <= '9')) return number(out);
        return fail("unexpected character");
    }
    bool number(Json& out) {
        const size_t start = i;
        if (s[i] == '-') ++i;
        while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' || s[i] == 'E' || s[i] == '+' || s[i] == '-')) ++i;
        const std::string t = s.substr(start, i - start);
        char* end = nullptr;
        const double v = std::strtod(t.c_str(), &end);
        if (!end || *end != '\0') return fail("bad number");
        out = Json(v);
        return true;
    }
    bool hex4(unsigned& cp) {
        if (i + 4 > s.size()) return fail("bad \\u escape");
        cp = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i++];
            cp <<= 4;
            if (c >= '0' && c <= '9') cp |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= static_cast<unsigned>(c - 'A' + 10);
            else return fail("bad \\u escape");
        }
        return true;
    }
    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out.push_back(static_cast<char>(cp));
        else if (cp < 0x800) { out.push_back(static_cast<char>(0xC0 | (cp >> 6))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) { out.push_back(static_cast<char>(0xE0 | (cp >> 12))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
        else { out.push_back(static_cast<char>(0xF0 | (cp >> 18))); out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    }
    bool string(std::string& out) {
        ++i;   // opening quote
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') return true;
            if (c == '\\') {
                if (i >= s.size()) return fail("bad escape");
                const char e = s[i++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        unsigned cp = 0;
                        if (!hex4(cp)) return false;
                        if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {   // surrogate pair
                            i += 2;
                            unsigned lo = 0;
                            if (!hex4(lo)) return false;
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        utf8(out, cp);
                        break;
                    }
                    default: return fail("bad escape");
                }
            } else out.push_back(c);
        }
        return fail("unterminated string");
    }
    bool array(Json& out, int depth) {
        ++i;
        out = Json::array();
        ws();
        if (i < s.size() && s[i] == ']') { ++i; return true; }
        while (true) {
            Json v;
            if (!value(v, depth + 1)) return false;
            out.push(std::move(v));
            ws();
            if (i >= s.size()) return fail("unterminated array");
            if (s[i] == ',') { ++i; continue; }
            if (s[i] == ']') { ++i; return true; }
            return fail("expected , or ]");
        }
    }
    bool object(Json& out, int depth) {
        ++i;
        out = Json::object();
        ws();
        if (i < s.size() && s[i] == '}') { ++i; return true; }
        while (true) {
            ws();
            if (i >= s.size() || s[i] != '"') return fail("expected key");
            std::string key;
            if (!string(key)) return false;
            ws();
            if (i >= s.size() || s[i] != ':') return fail("expected :");
            ++i;
            Json v;
            if (!value(v, depth + 1)) return false;
            out.set(key, std::move(v));
            ws();
            if (i >= s.size()) return fail("unterminated object");
            if (s[i] == ',') { ++i; continue; }
            if (s[i] == '}') { ++i; return true; }
            return fail("expected , or }");
        }
    }
};
}  // namespace

const Json& Json::operator[](const std::string& key) const {
    if (type_ != Type::Object) return nullJson();
    auto it = obj_.find(key);
    return it == obj_.end() ? nullJson() : it->second;
}

const Json& Json::operator[](size_t index) const {
    if (type_ != Type::Array || index >= arr_.size()) return nullJson();
    return arr_[index];
}

Json& Json::set(const std::string& key, Json value) {
    if (type_ != Type::Object) { *this = Json::object(); }
    obj_[key] = std::move(value);
    return *this;
}

Json& Json::push(Json value) {
    if (type_ != Type::Array) { *this = Json::array(); }
    arr_.push_back(std::move(value));
    return *this;
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
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
                if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
                else out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

void Json::dumpTo(std::string& out, int indent, int depth) const {
    auto nl = [&](int d) { if (indent > 0) { out.push_back('\n'); out.append(static_cast<size_t>(indent * d), ' '); } };
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: {
            if (std::isfinite(num_) && num_ == std::floor(num_) && std::fabs(num_) < 1e15) { char buf[32]; std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(num_)); out += buf; }
            else if (std::isfinite(num_)) { char buf[32]; std::snprintf(buf, sizeof buf, "%.6g", num_); out += buf; }
            else out += "null";
            break;
        }
        case Type::String: out.push_back('"'); out += jsonEscape(str_); out.push_back('"'); break;
        case Type::Array:
            out.push_back('[');
            for (size_t k = 0; k < arr_.size(); ++k) { if (k) out.push_back(','); nl(depth + 1); arr_[k].dumpTo(out, indent, depth + 1); }
            if (!arr_.empty()) nl(depth);
            out.push_back(']');
            break;
        case Type::Object: {
            out.push_back('{');
            size_t k = 0;
            for (const auto& [key, v] : obj_) {
                if (k++) out.push_back(',');
                nl(depth + 1);
                out.push_back('"'); out += jsonEscape(key); out += indent > 0 ? "\": " : "\":";
                v.dumpTo(out, indent, depth + 1);
            }
            if (!obj_.empty()) nl(depth);
            out.push_back('}');
            break;
        }
    }
}

std::string Json::dump(int indent) const {
    std::string out;
    dumpTo(out, indent, 0);
    return out;
}

bool Json::parse(const std::string& text, Json& out, std::string* err) {
    Parser p(text);
    if (!p.value(out, 0)) { if (err) *err = p.err; return false; }
    p.ws();
    if (p.i != text.size()) { if (err) *err = "trailing characters at offset " + std::to_string(p.i); return false; }
    return true;
}

}  // namespace rl
