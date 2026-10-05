#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

/// Strict, bounded JSON reader shared by the config loader and offline tools: duplicate keys, trailing data,
/// non-finite numbers, deep nesting and surrogate escapes are rejected.
namespace harbinger::json {

struct Json;
using Object = std::map<std::string, Json>;
using Array = std::vector<Json>;
struct Json {
    std::variant<std::nullptr_t, bool, int64_t, double, std::string, Array, Object> value{nullptr};
    const Json* get(const std::string& key) const {
        const auto* object = std::get_if<Object>(&value);
        if (!object) return nullptr;
        const auto found = object->find(key);
        return found == object->end() ? nullptr : &found->second;
    }
    const std::string* string() const { return std::get_if<std::string>(&value); }
    std::optional<double> number() const {
        if (const auto* i = std::get_if<int64_t>(&value)) return static_cast<double>(*i);
        if (const auto* d = std::get_if<double>(&value)) return *d;
        return std::nullopt;
    }
    std::optional<int64_t> integer() const {
        if (const auto* i = std::get_if<int64_t>(&value)) return *i;
        return std::nullopt;
    }
};

/// Parse one complete JSON document; throws std::runtime_error on any violation.
class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}
    Json parse() {
        Json out = value();
        skip();
        if (pos_ != text_.size()) fail("trailing characters");
        return out;
    }

private:
    [[noreturn]] void fail(const char* why) const {
        throw std::runtime_error(std::string("invalid JSON: ") + why);
    }
    void skip() { while (pos_ < text_.size() && std::string_view(" \t\r\n").find(text_[pos_]) != std::string_view::npos) ++pos_; }
    char peek() { skip(); if (pos_ >= text_.size()) fail("unexpected end"); return text_[pos_]; }
    void expect(char c) { if (peek() != c) fail("unexpected token"); ++pos_; }
    bool literal(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }
    Json value(int depth = 0) {
        if (depth > 32) fail("nesting too deep");
        switch (peek()) {
            case '{': return object(depth);
            case '[': return array(depth);
            case '"': return Json{str()};
            case 't': if (literal("true")) return Json{true}; fail("bad literal");
            case 'f': if (literal("false")) return Json{false}; fail("bad literal");
            case 'n': if (literal("null")) return Json{nullptr}; fail("bad literal");
            default: return number();
        }
    }
    Json object(int depth) {
        Object out;
        expect('{');
        if (peek() == '}') { ++pos_; return Json{std::move(out)}; }
        for (;;) {
            std::string key = str();
            expect(':');
            if (!out.emplace(std::move(key), value(depth + 1)).second) fail("duplicate key");
            if (peek() == ',') { ++pos_; continue; }
            expect('}');
            return Json{std::move(out)};
        }
    }
    Json array(int depth) {
        Array out;
        expect('[');
        if (peek() == ']') { ++pos_; return Json{std::move(out)}; }
        for (;;) {
            out.push_back(value(depth + 1));
            if (peek() == ',') { ++pos_; continue; }
            expect(']');
            return Json{std::move(out)};
        }
    }
    std::string str() {
        expect('"');
        std::string out;
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) fail("control character in string");
            if (c != '\\') { out.push_back(c); continue; }
            if (pos_ >= text_.size()) break;
            const char e = text_[pos_++];
            switch (e) {
                case '"': case '\\': case '/': out.push_back(e); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (pos_ + 4 > text_.size()) fail("short unicode escape");
                    unsigned code = 0;
                    const auto parsed = std::from_chars(text_.data() + pos_, text_.data() + pos_ + 4, code, 16);
                    if (parsed.ptr != text_.data() + pos_ + 4) fail("bad unicode escape");
                    pos_ += 4;
                    if (code >= 0xD800 && code <= 0xDFFF) fail("surrogate escapes unsupported");
                    if (code < 0x80) out.push_back(static_cast<char>(code));
                    else if (code < 0x800) { out.push_back(static_cast<char>(0xC0 | code >> 6)); out.push_back(static_cast<char>(0x80 | (code & 0x3F))); }
                    else { out.push_back(static_cast<char>(0xE0 | code >> 12)); out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (code & 0x3F))); }
                    break;
                }
                default: fail("bad escape");
            }
        }
        fail("unterminated string");
    }
    Json number() {
        const std::size_t start = pos_;
        bool floating = false;
        if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
        while (pos_ < text_.size() && std::string_view("0123456789.eE+-").find(text_[pos_]) != std::string_view::npos) {
            floating |= std::string_view(".eE").find(text_[pos_]) != std::string_view::npos;
            ++pos_;
        }
        const char* begin = text_.data() + start;
        const char* end = text_.data() + pos_;
        if (begin == end) fail("expected value");
        if (!floating) {
            int64_t integer = 0;
            const auto parsed = std::from_chars(begin, end, integer);
            if (parsed.ec == std::errc{} && parsed.ptr == end) return Json{integer};
        }
        double real = 0;
        const auto parsed = std::from_chars(begin, end, real);
        if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(real)) fail("bad number");
        return Json{real};
    }

    std::string_view text_;
    std::size_t pos_{0};
};

} // namespace harbinger::json
