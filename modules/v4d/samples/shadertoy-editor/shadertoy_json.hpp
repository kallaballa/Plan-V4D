// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// A minimal JSON reader for the Shadertoy samples.
//
// shadertoy.com answers with plain JSON and pulling in a JSON library for a
// sample is not worth the build-system churn, so this is a small recursive
// descent parser. It is deliberately separate from shadertoy_project.hpp: the
// offline editor (shadertoy-editor.cpp) reads and writes its project files with
// it, and the two can be used on their own.
#ifndef MODULES_V4D_SAMPLES_SHADERTOY_JSON_HPP_
#define MODULES_V4D_SAMPLES_SHADERTOY_JSON_HPP_

#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace shadertoy {

// ---------------------------------------------------------------------------
// A very small JSON value
// ---------------------------------------------------------------------------

struct Json {
  enum class Kind { Null, Bool, Number, String, Array, Object };

  Kind kind = Kind::Null;
  bool boolean = false;
  double number = 0.0;
  std::string text;
  std::vector<Json> items;                            // Array
  std::vector<std::pair<std::string, Json>> members;  // Object, in input order

  bool isNull() const { return kind == Kind::Null; }
  bool isObject() const { return kind == Kind::Object; }
  bool isArray() const { return kind == Kind::Array; }
  bool isString() const { return kind == Kind::String; }
  bool isNumber() const { return kind == Kind::Number; }

  /// Object member lookup; nullptr when absent or when this is not an object.
  const Json *get(const std::string &key) const {
    for (const auto &kv : members)
      if (kv.first == key)
        return &kv.second;
    return nullptr;
  }

  /// Follows a chain of object keys, e.g. child({"Shader"}, {"info"}).
  const Json *path(const std::vector<std::string> &keys) const {
    const Json *node = this;
    for (const auto &key : keys) {
      if (node == nullptr)
        return nullptr;
      node = node->get(key);
    }
    return node;
  }

  std::string str(const std::string &key,
                  const std::string &fallback = std::string()) const {
    const Json *v = get(key);
    return (v != nullptr && v->isString()) ? v->text : fallback;
  }

  double num(const std::string &key, double fallback = 0.0) const {
    const Json *v = get(key);
    return (v != nullptr && v->isNumber()) ? v->number : fallback;
  }

  long long integer(const std::string &key, long long fallback = 0) const {
    const Json *v = get(key);
    return (v != nullptr && v->isNumber()) ? (long long)v->number : fallback;
  }
};

/// Reads a JSON document out of `in`. Returns false and fills `error` on
/// malformed input; `out` is left unspecified then.
inline bool jsonParse(const std::string &in, Json &out, std::string &error) {
  size_t pos = 0;

  auto fail = [&](const char *what) {
    error = std::string("JSON: ") + what + " at offset " + std::to_string(pos);
    return false;
  };

  auto skipSpace = [&]() {
    while (pos < in.size() &&
           (in[pos] == ' ' || in[pos] == '\t' || in[pos] == '\n' ||
            in[pos] == '\r'))
      ++pos;
  };

  std::function<bool(Json &)> readValue, readString, readNumber;

  // Appends the UTF-8 encoding of a code point to `out`.
  auto appendUtf8 = [&](std::string &outStr, unsigned cp) {
    if (cp < 0x80u) {
      outStr += char(cp);
    } else if (cp < 0x800u) {
      outStr += char(0xC0u | (cp >> 6));
      outStr += char(0x80u | (cp & 0x3Fu));
    } else if (cp < 0x10000u) {
      outStr += char(0xE0u | (cp >> 12));
      outStr += char(0x80u | ((cp >> 6) & 0x3Fu));
      outStr += char(0x80u | (cp & 0x3Fu));
    } else {
      outStr += char(0xF0u | (cp >> 18));
      outStr += char(0x80u | ((cp >> 12) & 0x3Fu));
      outStr += char(0x80u | ((cp >> 6) & 0x3Fu));
      outStr += char(0x80u | (cp & 0x3Fu));
    }
  };

  readString = [&](Json &dst) -> bool {
    if (pos >= in.size() || in[pos] != '"')
      return fail("expected '\"'");
    ++pos;
    std::string acc;
    while (true) {
      if (pos >= in.size())
        return fail("unterminated string");
      const char c = in[pos++];
      if (c == '"')
        break;
      if (c != '\\') {
        acc += c;
        continue;
      }
      if (pos >= in.size())
        return fail("unterminated escape");
      const char esc = in[pos++];
      switch (esc) {
        case '"': acc += '"'; break;
        case '\\': acc += '\\'; break;
        case '/': acc += '/'; break;
        case 'b': acc += '\b'; break;
        case 'f': acc += '\f'; break;
        case 'n': acc += '\n'; break;
        case 'r': acc += '\r'; break;
        case 't': acc += '\t'; break;
        case 'u': {
          if (pos + 4 > in.size())
            return fail("truncated \\u escape");
          auto hex4 = [&](size_t at) {
            unsigned v = 0;
            for (size_t k = 0; k < 4; ++k) {
              const char h = in[at + k];
              v <<= 4;
              if (h >= '0' && h <= '9')
                v |= unsigned(h - '0');
              else if (h >= 'a' && h <= 'f')
                v |= unsigned(h - 'a' + 10);
              else if (h >= 'A' && h <= 'F')
                v |= unsigned(h - 'A' + 10);
              else
                v = 0xFFFFFFFFu;
            }
            return v;
          };
          unsigned cp = hex4(pos);
          pos += 4;
          if (cp >= 0xD800u && cp <= 0xDBFFu && pos + 6 <= in.size() &&
              in[pos] == '\\' && in[pos + 1] == 'u') {
            const unsigned low = hex4(pos + 2);
            if (low >= 0xDC00u && low <= 0xDFFFu) {
              cp = 0x10000u + ((cp - 0xD800u) << 10) + (low - 0xDC00u);
              pos += 6;
            }
          }
          appendUtf8(acc, cp);
          break;
        }
        default: return fail("unknown escape");
      }
    }
    dst.kind = Json::Kind::String;
    dst.text = std::move(acc);
    return true;
  };

  readNumber = [&](Json &dst) -> bool {
    const char *begin = in.c_str() + pos;
    char *stop = nullptr;
    const double v = std::strtod(begin, &stop);
    if (stop == begin)
      return fail("expected a number");
    pos += size_t(stop - begin);
    dst.kind = Json::Kind::Number;
    dst.number = v;
    return true;
  };

  std::function<bool(Json &)> readArray, readObject;

  readArray = [&](Json &dst) -> bool {
    ++pos;  // '['
    dst.kind = Json::Kind::Array;
    skipSpace();
    if (pos < in.size() && in[pos] == ']') {
      ++pos;
      return true;
    }
    while (true) {
      Json element;
      skipSpace();
      if (!readValue(element))
        return false;
      dst.items.push_back(std::move(element));
      skipSpace();
      if (pos >= in.size())
        return fail("unterminated array");
      if (in[pos] == ',') {
        ++pos;
        continue;
      }
      if (in[pos] == ']') {
        ++pos;
        return true;
      }
      return fail("expected ',' or ']'");
    }
  };

  readObject = [&](Json &dst) -> bool {
    ++pos;  // '{'
    dst.kind = Json::Kind::Object;
    skipSpace();
    if (pos < in.size() && in[pos] == '}') {
      ++pos;
      return true;
    }
    while (true) {
      skipSpace();
      Json key;
      if (!readString(key))
        return false;
      skipSpace();
      if (pos >= in.size() || in[pos] != ':')
        return fail("expected ':'");
      ++pos;
      skipSpace();
      Json value;
      if (!readValue(value))
        return false;
      dst.members.emplace_back(std::move(key.text), std::move(value));
      skipSpace();
      if (pos >= in.size())
        return fail("unterminated object");
      if (in[pos] == ',') {
        ++pos;
        continue;
      }
      if (in[pos] == '}') {
        ++pos;
        return true;
      }
      return fail("expected ',' or '}'");
    }
  };

  readValue = [&](Json &dst) -> bool {
    skipSpace();
    if (pos >= in.size())
      return fail("unexpected end of input");
    const char c = in[pos];
    if (c == '{')
      return readObject(dst);
    if (c == '[')
      return readArray(dst);
    if (c == '"')
      return readString(dst);
    if (c == '-' || (c >= '0' && c <= '9'))
      return readNumber(dst);
    if (in.compare(pos, 4, "true") == 0) {
      pos += 4;
      dst.kind = Json::Kind::Bool;
      dst.boolean = true;
      return true;
    }
    if (in.compare(pos, 5, "false") == 0) {
      pos += 5;
      dst.kind = Json::Kind::Bool;
      dst.boolean = false;
      return true;
    }
    if (in.compare(pos, 4, "null") == 0) {
      pos += 4;
      dst.kind = Json::Kind::Null;
      return true;
    }
    return fail("unexpected character");
  };

  skipSpace();
  if (!readValue(out))
    return false;
  skipSpace();
  if (pos != in.size())
    return fail("trailing characters");
  return true;
}

}  // namespace shadertoy

#endif  // MODULES_V4D_SAMPLES_SHADERTOY_JSON_HPP_
