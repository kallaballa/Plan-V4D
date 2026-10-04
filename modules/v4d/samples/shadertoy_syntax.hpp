// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// A GLSL tokenizer, for colouring the editor's code pane.
//
// Deliberately a lexer and nothing else: it splits a line into classified spans
// and carries only the two things a line break cannot decide - whether a block
// comment is still open, and whether the previous line ended with a backslash.
// It does not parse, does not know about types, and cannot tell you that a
// semicolon is missing - that is the driver's job, and the editor shows what
// the driver said. What it does know is everything needed to make GLSL read
// well:
//
//   * the reserved words of every version Shadertoy compiles (GLSL ES 1.00,
//     3.00 and 3.30 are all in the one table, because a shader written for the
//     site has to keep colouring after the user switches the version);
//   * the built-in functions, which read as calls and not as identifiers;
//   * Shadertoy's own uniforms (iResolution, iChannel3, ...), so that the line
//     a shader is mostly about lights up;
//   * preprocessor directives, where everything up to the continuation is one
//     colour, `#define`s included;
//   * line and block comments.
//
// The tokenizer is a pure function of (line, carried state), which is what lets
// the editor cache: the spans of a line only have to be recomputed when the line
// or the version changes.
#ifndef MODULES_V4D_SAMPLES_SHADERTOY_SYNTAX_HPP_
#define MODULES_V4D_SAMPLES_SHADERTOY_SYNTAX_HPP_

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace shadertoy {
namespace syntax {

/// What a span of a line is, for colouring purposes.
enum class Token {
  Plain,        // anything that needs no colour of its own
  Whitespace,
  Comment,      // // and /* ... */
  Preprocessor, // #version, #define, #ifdef ...
  Keyword,      // if, return, struct, discard ...
  Type,         // void, vec3, mat4, sampler2D ...
  Builtin,      // texture2D, clamp, mix, length ...
  Uniform,      // iResolution, iTime, iChannel0 ...
  Number,       // 1.0, 0x1f, 2e-3
  Operator,     // + - * / = < > & | ^ ~ ! ? : , ; . ( ) [ ] { }
  Bracket,      // the three bracket kinds on their own, for auto-pairing
  Function,     // an identifier followed by '(' - the user's own function
  Identifier,
};

/// One coloured run of a line. `length` is 0 for a zero-width marker (the caret
/// on a bracket), which the editor uses to decide whether to auto-pair.
struct Span {
  std::size_t begin = 0;
  std::size_t length = 0;
  Token token = Token::Plain;
  /// The character this span would be paired with, for Token::Bracket.
  char bracket = 0;
};

/// What the tokenizer carries from one line to the next.
struct State {
  /// Inside a /* ... */ comment.
  bool inBlockComment = false;
  /// The previous line was a preprocessor directive ending in a backslash, so
  /// this line is still part of it and stays one colour.
  bool continuedPreprocessor = false;
};

// ---------------------------------------------------------------------------
// Word lists
// ---------------------------------------------------------------------------

/// Case-sensitive lookup in a space-separated list. GLSL has no reserved words
/// in lower case that differ from the upper case spelling, so one table serves
/// both the type and the keyword classes.
inline bool isIn(const char *list, std::string_view word) {
  const std::size_t len = word.size();
  const char *p = list;
  while (*p != '\0') {
    const char *start = p;
    while (*p != ' ' && *p != '\0')
      ++p;
    if (std::size_t(p - start) == len && std::memcmp(start, word.data(), len) == 0)
      return true;
    if (*p == '\0')
      break;
    ++p;
  }
  return false;
}

// Keywords of GLSL ES 1.00/3.00 and GLSL 3.30. `invariant` and `precise` are
// desktop-only, `common` and `partition` are not reserved words at all - they
// are kept out on purpose, because colouring something the compiler does not
// treat as special is misleading.
inline constexpr const char *kKeywords =
    "attribute const uniform varying buffer shared coherent volatile restrict "
    "readonly writeonly atomic_uint layout centroid flat smooth noperspective "
    "patch sample break continue do for while switch case default if else "
    "subroutine in out inout true false invariant discard return struct "
    "precision highp mediump lowp";

inline constexpr const char *kTypes =
    "void bool int uint float double vec2 vec3 vec4 bvec2 bvec3 bvec4 ivec2 "
    "ivec3 ivec4 uvec2 uvec3 uvec4 dvec2 dvec3 dvec4 mat2 mat3 mat4 mat2x2 "
    "mat2x3 mat2x4 mat3x2 mat3x3 mat3x4 mat4x2 mat4x3 mat4x4 sampler2D "
    "sampler3D samplerCube sampler2DArray sampler2DShadow "
    "samplerCubeShadow sampler2DArrayShadow isampler2D isampler3D "
    "isamplerCube isampler2DArray usampler2D usampler3D usamplerCube "
    "usampler2DArray struct";

inline constexpr const char *kBuiltins =
    "radians degrees sin cos tan asin acos atan sinh cosh tanh asinh acosh "
    "atanh pow exp log exp2 log2 sqrt inversesqrt abs sign floor trunc round "
    "roundEven ceil fract mod modf min max clamp mix step smoothstep isnan "
    "isinf floatBitsToInt floatBitsToUint intBitsToFloat uintBitsToFloat "
    "packSnorm2x16 unpackSnorm2x16 packUnorm2x16 unpackUnorm2x16 "
    "packHalf2x16 unpackHalf2x16 length distance dot cross normalize faceforward "
    "reflect refract matrixCompMult outerProduct transpose determinant inverse "
    "lessThan lessThanEqual greaterThan greaterThanEqual equal notEqual any all "
    "not texture textureProj textureLod textureOffset texelFetch texelFetchOffset "
    "textureSize textureQueryLod textureGrad dFdx dFdy fwidth "
    "interpolateAtCentroid interpolateAtSample interpolateAtOffset noise1 "
    "noise2 noise3 noise4 texture2D texture2DProj texture2DLod textureCube "
    "textureCubeLod dFdxFINE dFdyFINE fwidthFINE dFdxCoarse dFdyCoarse "
    "fwidthCoarse shadow2D";

/// The uniforms Shadertoy hands to every pass. They are declared by the
/// renderer, not by the shader, and a reader wants to see them as such.
inline constexpr const char *kUniforms =
    "iResolution iTime iTimeDelta iFrame iFrameRate iMouse iDate iSampleRate "
    "iChannelTime iChannelResolution iChannel0 iChannel1 iChannel2 iChannel3 "
    "iChannelKeyboard iCursor fragColor gl_FragCoord gl_FrontFacing gl_PointSize "
    "gl_FragDepth outColor";

inline bool isKeyword(std::string_view word) {
  return isIn(kKeywords, word);
}
inline bool isType(std::string_view word) { return isIn(kTypes, word); }
inline bool isBuiltin(std::string_view word) { return isIn(kBuiltins, word); }
inline bool isUniform(std::string_view word) { return isIn(kUniforms, word); }

/// True for the characters that make an identifier.
inline bool isIdentStart(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
inline bool isIdentChar(char c) { return isIdentStart(c) || (c >= '0' && c <= '9'); }
inline bool isDigit(char c) { return c >= '0' && c <= '9'; }
inline bool isSpace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
         c == '\v';
}
/// The GLSL operator characters, single ones only: a run of them is one span.
inline bool isOperatorChar(char c) {
  return c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
         c == '<' || c == '>' || c == '=' || c == '!' || c == '&' ||
         c == '|' || c == '^' || c == '~' || c == '?' || c == ':' || c == '.';
}
inline char closingOf(char c) {
  switch (c) {
    case '(': return ')';
    case '[': return ']';
    case '{': return '}';
    case ')': return '(';
    case ']': return '[';
    case '}': return '{';
    default: return 0;
  }
}

/// True when `line` ends with a backslash, which is how a preprocessor directive
/// spills onto the next physical line. Trailing blanks do not count: `foo \` is
/// continued, `foo \ ` is not.
inline bool endsWithContinuation(std::string_view line) {
  while (!line.empty() && isSpace(line.back()))
    line.remove_suffix(1);
  return !line.empty() && line.back() == '\\';
}

/// True when the '#' at `i` starts a preprocessor directive rather than being
/// the '#' of some operator. GLSL only recognises a directive as the first
/// token of the logical line, so only blanks may come before it: without this
/// check `a # b` lexes as a directive, and because a directive is consumed to
/// the end of the line that also throws away the rest of the expression.
inline bool isDirectiveStart(std::string_view line, std::size_t i) {
  for (std::size_t k = 0; k < i; ++k)
    if (!isSpace(line[k]))
      return false;
  return true;
}

// ---------------------------------------------------------------------------
// The tokenizer
// ---------------------------------------------------------------------------

/// Classifies one line. `state` is updated to what the next line has to start
/// with, so the caller threads it through the buffer in order.
inline std::vector<Span> tokenizeLine(std::string_view line, State &state) {
  std::vector<Span> spans;
  const std::size_t n = line.size();
  std::size_t i = 0;

  auto emit = [&](std::size_t begin, std::size_t length, Token token,
                  char bracket = 0) {
    if (length == 0 && bracket == 0)
      return;
    // Merge with the previous span when it is the same class: fewer draw calls,
    // and it keeps a long run of spaces from becoming hundreds of spans.
    if (!spans.empty() && spans.back().token == token &&
        spans.back().begin + spans.back().length == begin && bracket == 0) {
      spans.back().length += length;
      return;
    }
    Span span;
    span.begin = begin;
    span.length = length;
    span.token = token;
    span.bracket = bracket;
    spans.push_back(span);
  };

  // The continuation flag is consumed here: whatever this line turns out to be,
  // it can only continue a directive once.
  const bool continued = state.continuedPreprocessor;
  state.continuedPreprocessor = false;

  // -- a block comment that started on an earlier line ----------------------
  if (state.inBlockComment) {
    const std::size_t end = line.find("*/");
    if (end == std::string_view::npos) {
      emit(0, n, Token::Comment);
      return spans;
    }
    emit(0, end + 2, Token::Comment);
    i = end + 2;
    state.inBlockComment = false;
  }

  // -- the rest of a directive that spilled over from the previous line ------
  // None of this line is code, whatever it happens to contain, so the whole of
  // it is the directive's colour and lexing it further would be wrong.
  if (continued) {
    emit(0, n, Token::Preprocessor);
    state.continuedPreprocessor = endsWithContinuation(line);
    return spans;
  }

  while (i < n) {
    const char c = line[i];

    // -- whitespace --------------------------------------------------------
    if (isSpace(c)) {
      std::size_t j = i;
      while (j < n && isSpace(line[j]))
        ++j;
      emit(i, j - i, Token::Whitespace);
      i = j;
      continue;
    }

    // -- a line comment runs to the end of the line, whatever it contains ---
    if (c == '/' && i + 1 < n && line[i + 1] == '/') {
      emit(i, n - i, Token::Comment);
      break;
    }

    // -- a block comment may or may not end on this line ---------------------
    if (c == '/' && i + 1 < n && line[i + 1] == '*') {
      const std::size_t end = line.find("*/", i + 2);
      if (end == std::string_view::npos) {
        emit(i, n - i, Token::Comment);
        state.inBlockComment = true;
        break;
      }
      emit(i, end + 2 - i, Token::Comment);
      i = end + 2;
      continue;
    }

    // -- preprocessor: the whole rest of the logical line --------------------
    // A backslash at the end continues it, which is the only thing that makes
    // these worth treating as one run.
    //
    // A '#' only opens a directive when nothing but blanks precede it on the
    // line. Testing only the character in front of it would turn `a # b` into a
    // directive - and since a directive swallows the rest of the line as one
    // colour, that mistake also hides the code after it.
    if (c == '#' && isDirectiveStart(line, i)) {
      std::size_t j = i;
      while (j < n) {
        if (line[j] == '\\' && j + 1 < n && isSpace(line[j + 1])) {
          j += 2;
          continue;
        }
        if (line[j] == '/' && j + 1 < n && line[j + 1] == '/')
          break;  // a trailing comment is not part of the directive
        ++j;
      }
      emit(i, j - i, Token::Preprocessor);
      state.continuedPreprocessor = endsWithContinuation(line);
      i = j;
      continue;
    }

    // -- brackets, paired markers -------------------------------------------
    if (c == '(' || c == '[' || c == '{' || c == ')' || c == ']' || c == '}') {
      Span span;
      span.begin = i;
      span.length = 1;
      span.token = Token::Bracket;
      span.bracket = closingOf(c);
      spans.push_back(span);
      ++i;
      continue;
    }

    // -- separators ---------------------------------------------------------
    if (c == ',' || c == ';') {
      emit(i, 1, Token::Operator);
      ++i;
      continue;
    }

    // -- numbers -------------------------------------------------------------
    if (isDigit(c) || (c == '.' && i + 1 < n && isDigit(line[i + 1]))) {
      std::size_t j = i;
      if (c == '0' && i + 1 < n && (line[i + 1] == 'x' || line[i + 1] == 'X')) {
        j = i + 2;
        while (j < n && std::isxdigit(static_cast<unsigned char>(line[j])))
          ++j;
      } else {
        while (j < n && isDigit(line[j]))
          ++j;
        if (j < n && line[j] == '.') {  // a trailing '.' is part of the number
          ++j;
          while (j < n && isDigit(line[j]))
            ++j;
        }
        if (j < n && (line[j] == 'e' || line[j] == 'E')) {
          std::size_t k = j + 1;
          if (k < n && (line[k] == '+' || line[k] == '-'))
            ++k;
          if (k < n && isDigit(line[k])) {
            j = k;
            while (j < n && isDigit(line[j]))
              ++j;
          }
        }
      }
      // A float suffix (1.0f, 2u, 3lf, 7.0LF) belongs to the number.
      while (j < n && (line[j] == 'f' || line[j] == 'F' || line[j] == 'u' ||
                       line[j] == 'U' || line[j] == 'l' || line[j] == 'L'))
        ++j;
      emit(i, j - i, Token::Number);
      i = j;
      continue;
    }

    // -- identifiers, and everything a name can be --------------------------
    if (isIdentStart(c)) {
      const std::size_t begin = i;
      while (i < n && isIdentChar(line[i]))
        ++i;
      const std::string_view word = line.substr(begin, i - begin);
      // A '(' after the name is what makes a call; whether the name is a known
      // builtin or the author's own decides the colour.
      std::size_t k = i;
      while (k < n && isSpace(line[k]))
        ++k;
      const bool call = k < n && line[k] == '(';
      if (isUniform(word))
        emit(begin, i - begin, Token::Uniform);
      else if (isType(word))
        emit(begin, i - begin, Token::Type);
      else if (isKeyword(word))
        emit(begin, i - begin, Token::Keyword);
      else if (isBuiltin(word))
        emit(begin, i - begin, Token::Builtin);
      else if (call)
        emit(begin, i - begin, Token::Function);
      else
        emit(begin, i - begin, Token::Identifier);
      continue;
    }

    // -- operator runs --------------------------------------------------------
    if (isOperatorChar(c)) {
      std::size_t j = i;
      while (j < n && isOperatorChar(line[j]))
        ++j;
      emit(i, j - i, Token::Operator);
      i = j;
      continue;
    }

    // -- anything else (unicode in a comment-free identifier, say) ------------
    emit(i, 1, Token::Plain);
    ++i;
  }
  return spans;
}

/// Tokenizes a whole buffer, threading the block-comment state. Convenience
/// wrapper; the editor calls tokenizeLine() directly because it caches.
inline std::vector<std::vector<Span>> tokenize(const std::string &text) {
  State state;
  std::vector<std::vector<Span>> out;
  std::size_t from = 0;
  while (true) {
    const std::size_t nl = text.find('\n', from);
    const std::size_t end = (nl == std::string::npos) ? text.size() : nl;
    out.push_back(tokenizeLine(std::string_view(text).substr(from, end - from),
                               state));
    if (nl == std::string::npos)
      break;
    from = nl + 1;
  }
  return out;
}

}  // namespace syntax
}  // namespace shadertoy

#endif  // MODULES_V4D_SAMPLES_SHADERTOY_SYNTAX_HPP_
