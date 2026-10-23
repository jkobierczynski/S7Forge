// S7Forge - a simulator for Siemens S7-1200/S7-1500 programs
// Copyright (C) 2026 Jurgen Kobierczynski
// SPDX-License-Identifier: GPL-3.0-or-later
#include "json.hpp"

#include <cerrno>
#include <cstdlib>

namespace s7forge {
namespace {

class Reader {
public:
    explicit Reader(const std::string& t) : t_(t) {}

    Json document() {
        Json v = value(0);
        space();
        if (p_ != t_.size()) fail("text after the document");
        return v;
    }

private:
    const std::string& t_;
    size_t p_ = 0;

    [[noreturn]] void fail(const std::string& what) const {
        throw JsonError("JSON at offset " + std::to_string(p_) + ": " + what);
    }
    void space() {
        while (p_ < t_.size() && (t_[p_] == ' ' || t_[p_] == '\n' || t_[p_] == '\r' || t_[p_] == '\t')) ++p_;
    }
    bool take(const char* w) {
        size_t n = 0;
        while (w[n]) ++n;
        if (t_.compare(p_, n, w) != 0) return false;
        p_ += n;
        return true;
    }
    char peek() const { return p_ < t_.size() ? t_[p_] : '\0'; }

    Json value(int depth) {
        if (depth > 512) fail("nested too deeply");
        space();
        if (p_ >= t_.size()) fail("unexpected end");
        Json v;
        const char c = t_[p_];
        if (c == '{') {
            v.type = Json::Object;
            ++p_;
            space();
            if (peek() == '}') {
                ++p_;
                return v;
            }
            for (;;) {
                space();
                if (peek() != '"') fail("expected a key");
                std::string k = string();
                space();
                if (peek() != ':') fail("expected ':'");
                ++p_;
                v.members.emplace_back(std::move(k), value(depth + 1));
                space();
                if (peek() == ',') {
                    ++p_;
                    continue;
                }
                if (peek() == '}') {
                    ++p_;
                    return v;
                }
                fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v.type = Json::Array;
            ++p_;
            space();
            if (peek() == ']') {
                ++p_;
                return v;
            }
            for (;;) {
                v.items.push_back(value(depth + 1));
                space();
                if (peek() == ',') {
                    ++p_;
                    continue;
                }
                if (peek() == ']') {
                    ++p_;
                    return v;
                }
                fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            v.type = Json::String;
            v.s = string();
            return v;
        }
        if (take("true")) {
            v.type = Json::Bool;
            v.b = true;
            return v;
        }
        if (take("false")) {
            v.type = Json::Bool;
            return v;
        }
        if (take("null")) return v;
        if (c == '-' || (c >= '0' && c <= '9')) {
            // -?(0|[1-9][0-9]*)(.[0-9]+)?([eE][+-]?[0-9]+)?
            const size_t start = p_;
            auto digit = [&] { return p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9'; };
            auto digits = [&] {
                if (!digit()) fail("bad number");
                while (digit()) ++p_;
            };
            bool frac = false;
            if (peek() == '-') ++p_;
            if (p_ < t_.size() && t_[p_] == '0') {
                ++p_;
                if (digit()) fail("a number with a leading zero");
            } else {
                digits();
            }
            if (p_ < t_.size() && t_[p_] == '.') {
                ++p_;
                frac = true;
                digits();
            }
            if (p_ < t_.size() && (t_[p_] == 'e' || t_[p_] == 'E')) {
                ++p_;
                frac = true;
                if (p_ < t_.size() && (t_[p_] == '+' || t_[p_] == '-')) ++p_;
                digits();
            }
            const std::string num = t_.substr(start, p_ - start);
            v.type = Json::Number;
            v.number = std::strtod(num.c_str(), nullptr);
            v.integer = !frac;
            if (v.integer) {
                // up to 2^64 - 1 (ULInt, LWord) as the same 64 bits; beyond that a Real
                errno = 0;
                v.i = std::strtoll(num.c_str(), nullptr, 10);
                if (errno == ERANGE) {
                    errno = 0;
                    const unsigned long long u = num[0] == '-' ? 0 : std::strtoull(num.c_str(), nullptr, 10);
                    if (num[0] != '-' && errno != ERANGE) v.i = static_cast<int64_t>(u);
                    else v.integer = false;
                }
            }
            return v;
        }
        fail(std::string("unexpected '") + c + "'");
    }

    unsigned hex4() {
        if (p_ + 4 > t_.size()) fail("short \\u escape");
        unsigned v = 0;
        for (int k = 0; k < 4; ++k) {
            const char h = t_[p_++];
            v <<= 4;
            if (h >= '0' && h <= '9') v |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') v |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') v |= static_cast<unsigned>(h - 'A' + 10);
            else fail("bad \\u escape");
        }
        return v;
    }

    static void utf8(unsigned cp, std::string& out) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    std::string string() {
        ++p_;  // the quote
        std::string out;
        while (p_ < t_.size() && t_[p_] != '"') {
            const unsigned char c = static_cast<unsigned char>(t_[p_]);
            if (c < 0x20) fail("control character in a string");
            if (c != '\\') {
                out += static_cast<char>(c);
                ++p_;
                continue;
            }
            if (++p_ >= t_.size()) fail("unexpected end");
            const char e = t_[p_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = hex4();
                    if (cp >= 0xDC00 && cp < 0xE000) fail("a lone surrogate in \\u");
                    if (cp >= 0xD800 && cp < 0xDC00) {
                        if (!(p_ + 6 <= t_.size() && t_[p_] == '\\' && t_[p_ + 1] == 'u')) fail("a lone surrogate in \\u");
                        p_ += 2;
                        const unsigned lo = hex4();
                        if (lo < 0xDC00 || lo >= 0xE000) fail("a lone surrogate in \\u");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(cp, out);
                    break;
                }
                default: fail("bad escape");
            }
        }
        if (p_ >= t_.size()) fail("unterminated string");
        ++p_;
        return out;
    }
};

}  // namespace

Json parseJson(const std::string& text) { return Reader(text).document(); }

}  // namespace s7forge
