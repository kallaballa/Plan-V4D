// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// Unit tests for the editor's plain-C++ halves: the GLSL tokenizer, the text
// buffer with its undo history, and the palette table.
//
// Deliberately not an OpenCV sample. These three headers are where the fiddly
// logic lives - offsets, line caches, state carried across line breaks - and
// reaching for a two-minute rebuild of libopencv to check an off-by-one is a
// bad way to spend an afternoon. This file builds in a second with a plain g++
// (see shadertoy-text-test.sh) and needs nothing but the OpenCV headers, which
// the theme header pulls in for its asset lookup.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "shadertoy-editor/shadertoy_syntax.hpp"
#include "shadertoy_text.hpp"
#include "shadertoy_theme.hpp"

namespace {

int gFailures = 0;
int gChecks = 0;

void check(bool ok, const std::string &what, const char *file, int line) {
  ++gChecks;
  if (ok)
    return;
  ++gFailures;
  std::printf("FAIL %s:%d: %s\n", file, line, what.c_str());
}

template <typename A, typename B>
void checkEq(const A &got, const B &want, const std::string &what,
             const char *file, int line) {
  ++gChecks;
  if (got == want)
    return;
  ++gFailures;
  std::printf("FAIL %s:%d: %s\n", file, line, what.c_str());
}

#define CHECK(cond) check((cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(got, want) checkEq((got), (want), #got " == " #want, __FILE__, __LINE__)

using namespace shadertoy;          // NOLINT: the tests are all about it
using namespace shadertoy::syntax;  // NOLINT

/// The token of the span that starts exactly at `offset`.
Token tokenAt(const std::vector<Span> &spans, std::size_t offset) {
  for (const Span &span : spans) {
    if (span.length > 0 && span.begin <= offset && offset < span.begin + span.length)
      return span.token;
  }
  return Token::Plain;
}

bool hasSpan(const std::vector<Span> &spans, std::size_t begin, std::size_t length,
             Token token) {
  for (const Span &span : spans)
    if (span.begin == begin && span.length == length && span.token == token)
      return true;
  return false;
}

/// Reassembles a line from its spans. Anything the lexer drops or overlaps shows
/// up here as a mismatch, which is the one invariant that must never break: what
/// the highlighter paints has to add up to the text it was given.
std::string joined(const std::vector<Span> &spans, std::string_view line) {
  std::string out;
  std::size_t at = 0;
  for (const Span &span : spans) {
    if (span.length == 0 || span.begin != at)
      continue;
    out.append(line.substr(span.begin, span.length));
    at = span.begin + span.length;
  }
  if (at < line.size())
    out.append(line.substr(at));
  return out;
}

// ---------------------------------------------------------------------------
// The tokenizer
// ---------------------------------------------------------------------------

void testWordClasses() {
  State state;
  const std::string line = "void main() { gl_FragColor = vec4(1.0); }";
  const auto spans = tokenizeLine(line, state);
  CHECK_EQ(joined(spans, line), line);
  CHECK(tokenAt(spans, 0) == Token::Type);        // void
  CHECK(tokenAt(spans, 5) == Token::Function);    // main(
  CHECK(tokenAt(spans, 13) == Token::Bracket);    // (
  CHECK(tokenAt(spans, 26) == Token::Uniform);    // gl_FragColor
  CHECK(tokenAt(spans, 40) == Token::Type);       // vec4
  CHECK(tokenAt(spans, 45) == Token::Number);     // 1.0
  CHECK(!state.inBlockComment);
}

void testBuiltinsAreNotUserFunctions() {
  State state;
  const std::string line = "float f(vec3 p) { return clamp(p, 0.0, 1.0); }";
  const auto spans = tokenizeLine(line, state);
  // clamp is a built-in, so it reads as one; f is the author's own.
  CHECK(tokenAt(spans, line.find("clamp")) == Token::Builtin);
  CHECK(tokenAt(spans, 6) == Token::Function);
  CHECK(tokenAt(spans, line.find("0.0")) == Token::Number);
}

void testComments() {
  {
    State state;
    const std::string line = "int a; // gone; int b;";
    const auto spans = tokenizeLine(line, state);
    CHECK(hasSpan(spans, 8, line.size() - 8, Token::Comment));
    CHECK_EQ(joined(spans, line), line);
  }
  {  // a block comment that spans three lines, and the code after it
    const auto lines = tokenize("/* one\ntwo\nthree */ int x;");
    CHECK_EQ(lines.size(), 3u);
    CHECK(!lines[0].empty() && lines[0][0].token == Token::Comment);
    CHECK(!lines[1].empty() && lines[1][0].token == Token::Comment);
    CHECK_EQ(joined(lines[1], "two"), "two");
    CHECK(hasSpan(lines[2], 0, 10, Token::Comment));
    CHECK(tokenAt(lines[2], 11) == Token::Type);
  }
  {  // a comment closed on the same line it opened
    State state;
    const std::string line = "a /* b */ c";
    const auto spans = tokenizeLine(line, state);
    CHECK(hasSpan(spans, 2, 7, Token::Comment));
    CHECK(!state.inBlockComment);
  }
  {  // "//" inside a block comment is not a line comment
    const auto lines = tokenize("/* //\n*/ int");
    CHECK(tokenAt(lines[1], 3) == Token::Type);
  }
}

void testPreprocessor() {
  {  // a directive is one colour, whatever it contains
    State state;
    const std::string line = "#define K 2.0 // times";
    const auto spans = tokenizeLine(line, state);
    CHECK(hasSpan(spans, 0, 8, Token::Preprocessor));
    CHECK_EQ(joined(spans, line), line);
    CHECK(!state.continuedPreprocessor);
  }
  {  // and it is the continuation that spills over, not the newline
    const auto lines = tokenize("#define K \\\n    2.0\nint x;");
    CHECK_EQ(lines.size(), 3u);
    CHECK(hasSpan(lines[0], 0, 12, Token::Preprocessor));
    CHECK(!lines[1].empty());
    CHECK_EQ(lines[1][0].token, Token::Preprocessor);
    CHECK_EQ(joined(lines[1], "    2.0"), "    2.0");
    CHECK(tokenAt(lines[2], 0) == Token::Type);
  }
  {  // two backslashes continue it again
    const auto lines = tokenize("#define K \\\n1 \\\n2\nint");
    CHECK_EQ(lines[2][0].token, Token::Preprocessor);
    CHECK(tokenAt(lines[3], 0) == Token::Type);
  }
  {  // a directive that does not continue stops colouring at the newline
    const auto lines = tokenize("#version 300 es\nint");
    CHECK(hasSpan(lines[0], 0, 15, Token::Preprocessor));
    CHECK(!lines[1].empty());
    CHECK(lines[1][0].token != Token::Preprocessor);
  }
  {  // a '#' in the middle of an expression is an operator, not a directive
    State state;
    const std::string line = "a # b";
    const auto spans = tokenizeLine(line, state);
    CHECK(tokenAt(spans, 2) == Token::Operator);
  }
}

void testNumbers() {
  const struct {
    const char *text;
    std::size_t offset;
  } cases[] = {{"1.0", 0},   {"0x1f", 0},  {"2e-3", 0},   {"1.", 0},
               {".5", 0},    {"3u", 0},    {"1.0f", 0},   {"7.0LF", 0},
               {"vec2(1.)", 5}};
  for (const auto &c : cases) {
    State state;
    const std::string line = c.text;
    const auto spans = tokenizeLine(line, state);
    CHECK(hasSpan(spans, c.offset, line.size() - c.offset, Token::Number));
    CHECK_EQ(joined(spans, line), line);
  }
  {  // the '.' of a member access is not the start of a number
    State state;
    const std::string line = "a.b";
    CHECK(tokenAt(tokenizeLine(line, state), 1) == Token::Operator);
  }
}

void testBrackets() {
  State state;
  const auto spans = tokenizeLine("if (a[0]) {", state);
  const char *expected = "()[]{}";
  std::size_t seen = 0;
  for (const Span &span : spans) {
    if (span.token != Token::Bracket)
      continue;
    CHECK(span.length == 1);
    CHECK(span.bracket == expected[seen % 6]);
    ++seen;
  }
  CHECK_EQ(seen, 6u);
}

void testEveryLineRoundTrips() {
  const std::string source =
      "#version 300 es\n"
      "precision highp float;\n"
      "uniform vec3 iResolution;\n"
      "in vec2 vUv;\n"
      "out vec4 fragColor;\n"
      "// a comment\n"
      "/* block */\n"
      "void main() {\n"
      "  vec3 c = texture(tex, vUv).rgb;\n"
      "  fragColor = vec4(c, 1.0);\n"
      "}\n";
  const auto lines = tokenize(source);
  std::size_t from = 0;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const std::size_t nl = source.find('\n', from);
    const std::size_t end = (nl == std::string::npos) ? source.size() : nl;
    const std::string line = source.substr(from, end - from);
    CHECK_EQ(joined(lines[i], line), line);
    from = end + 1;
  }
}

// ---------------------------------------------------------------------------
// The text buffer
// ---------------------------------------------------------------------------

using text::Buffer;
using text::Edit;
using text::Position;

Position pos(std::size_t offset, int line = 0, int column = 0) {
  Position p;
  p.offset = offset;
  p.line = line;
  p.column = column;
  return p;
}

void testLines() {
  Buffer buffer("a\nbb\nccc");
  CHECK_EQ(buffer.lineCount(), 3);
  CHECK_EQ(buffer.line(0), std::string("a"));
  CHECK_EQ(buffer.line(1), std::string("bb"));
  CHECK_EQ(buffer.line(2), std::string("ccc"));
  CHECK_EQ(buffer.lineRange(1).first, 2u);
  CHECK_EQ(buffer.lineRange(1).second, 4u);
  // A trailing newline means an empty last line, which is where the caret sits
  // after the user presses return at the end of the file.
  Buffer trailing("a\n");
  CHECK_EQ(trailing.lineCount(), 2);
  CHECK_EQ(trailing.line(1), std::string(""));
  CHECK_EQ(trailing.text(), std::string("a\n"));
  // An empty buffer still has one (empty) line to draw a caret on.
  Buffer empty("");
  CHECK_EQ(empty.lineCount(), 1);
  CHECK_EQ(empty.line(0), std::string(""));
}

void testPositions() {
  Buffer buffer("ab\ncd\nef");
  const Position p = buffer.offsetToPosition(4);
  CHECK_EQ(p.line, 1);
  CHECK_EQ(p.column, 1);
  CHECK_EQ(p.offset, 4u);
  CHECK_EQ(buffer.positionAt(2, 1).offset, 8u);
  // Out of range asks are clamped rather than trusted: the editor hands these
  // whatever the mouse did.
  CHECK_EQ(buffer.offsetToPosition(9999).line, 2);
  CHECK_EQ(buffer.positionAt(99, 99).offset, buffer.text().size());
  CHECK_EQ(buffer.positionAt(-3, 0).line, 0);
}

void testSelection() {
  Buffer buffer("hello world");
  CHECK(!buffer.hasSelection());
  CHECK_EQ(buffer.selectedText(), std::string(""));
  buffer.setSelection(6, 0);  // backwards, the way a drag from right to left goes
  CHECK(buffer.hasSelection());
  CHECK_EQ(buffer.selectedText(), std::string("hello "));
  CHECK_EQ(buffer.selection().first, 0u);
  CHECK_EQ(buffer.selection().second, 6u);
  buffer.setCaret(3);
  CHECK(!buffer.hasSelection());
}

void testWordAt() {
  Buffer buffer("vec3(1.0) + name");
  CHECK_EQ(buffer.selectedText(), std::string(""));
  const auto word = buffer.wordAt(1);
  CHECK_EQ(buffer.text().substr(word.first, word.second - word.first),
           std::string("vec3"));
  // On the bracket, both halves come along.
  const auto pair = buffer.wordAt(4);
  CHECK_EQ(buffer.text().substr(pair.first, pair.second - pair.first),
           std::string("(1.0)"));
  // On the closing bracket, the same pair, found backwards.
  const auto same = buffer.wordAt(8);
  CHECK_EQ(same.first, pair.first);
  CHECK_EQ(same.second, pair.second);
  // Past the end is clamped instead of reading past it.
  const auto tail = buffer.wordAt(9999);
  CHECK(tail.first <= tail.second);
  CHECK(tail.second <= buffer.text().size());
  // Nested brackets find their own pair.
  Buffer nested("f(g(1), 2)");
  const auto inner = nested.wordAt(4);
  CHECK_EQ(nested.text().substr(inner.first, inner.second - inner.first),
           std::string("(1)"));
  // An unbalanced bracket selects itself rather than running away.
  Buffer lonely("f(");
  const auto one = lonely.wordAt(1);
  CHECK_EQ(one.first, 1u);
  CHECK_EQ(one.second, 2u);
}

void testUndoRedo() {
  Buffer buffer("start");
  CHECK(!buffer.canUndo());  // a buffer nobody has touched has nothing to undo
  CHECK(!buffer.canRedo());
  CHECK_EQ(buffer.undoDepth(), 0u);

  buffer.commit(Edit{"one", pos(3), pos(3)}, /*breakGroup=*/true);
  CHECK(buffer.canUndo());
  CHECK_EQ(buffer.undoDepth(), 1u);
  CHECK_EQ(buffer.generation(), 1u);

  // Three keystrokes without a pause are one gesture, so one undo step.
  buffer.commit(Edit{"onet", pos(4), pos(4)});
  buffer.commit(Edit{"onetw", pos(5), pos(5)});
  buffer.commit(Edit{"onetwo", pos(6), pos(6)});
  CHECK_EQ(buffer.undoDepth(), 1u);

  CHECK_EQ(buffer.undo(), std::string("start"));
  CHECK_EQ(buffer.text(), std::string("start"));
  CHECK(!buffer.canUndo());
  CHECK(buffer.canRedo());
  CHECK_EQ(buffer.generation(), 3u);

  CHECK_EQ(buffer.redo(), std::string("onetwo"));
  CHECK(!buffer.canRedo());
  CHECK(buffer.canUndo());

  // And back again, to make sure the round trip does not drift.
  CHECK_EQ(buffer.undo(), std::string("start"));
  CHECK_EQ(buffer.redo(), std::string("onetwo"));

  // A commit that changed nothing is not an undo step.
  const unsigned long generation = buffer.generation();
  buffer.commit(Edit{"onetwo", pos(6), pos(6)}, /*breakGroup=*/true);
  CHECK_EQ(buffer.generation(), generation);
  CHECK_EQ(buffer.undoDepth(), 1u);

  // A new edit clears the redo branch, which is what every editor does and what
  // stops redo from resurrecting text the user has since replaced.
  buffer.undo();
  CHECK(buffer.canRedo());
  buffer.commit(Edit{"other", pos(5), pos(5)}, /*breakGroup=*/true);
  CHECK(!buffer.canRedo());
}

void testUndoRestoresCaret() {
  Buffer buffer("abc\ndef");
  buffer.setCaret(5);  // the 'f'
  buffer.commit(Edit{"abc\ndef!", pos(8), pos(8)}, /*breakGroup=*/true);
  CHECK_EQ(buffer.head().offset, 8u);
  buffer.undo();
  CHECK_EQ(buffer.head().offset, 5u);
  CHECK_EQ(buffer.head().line, 1);
  CHECK_EQ(buffer.head().column, 2);
}

void testResetDropsHistory() {
  Buffer buffer("old");
  buffer.commit(Edit{"older", pos(5), pos(5)}, /*breakGroup=*/true);
  CHECK(buffer.canUndo());
  buffer.reset("a fresh file");
  CHECK_EQ(buffer.text(), std::string("a fresh file"));
  CHECK(!buffer.canUndo());
  CHECK(!buffer.canRedo());
  CHECK_EQ(buffer.head().offset, 0u);
  CHECK(buffer.canUndo() == false);
  // The line cache follows the new text rather than the old one.
  CHECK_EQ(buffer.lineCount(), 1);
  buffer.reset("one\ntwo\nthree");
  CHECK_EQ(buffer.lineCount(), 3);
  CHECK_EQ(buffer.line(2), std::string("three"));
}

void testLineCacheSurvivesEdits() {
  Buffer buffer("one\ntwo");
  CHECK_EQ(buffer.line(1), std::string("two"));
  // Editing in the middle must not leave a stale index behind - this is the bug
  // a line cache exists to have.
  buffer.commit(Edit{"one\n\ntwo", pos(5), pos(5)}, /*breakGroup=*/true);
  CHECK_EQ(buffer.lineCount(), 3);
  CHECK_EQ(buffer.line(1), std::string(""));
  CHECK_EQ(buffer.line(2), std::string("two"));
  buffer.commit(Edit{"x", pos(1), pos(1)}, /*breakGroup=*/true);
  CHECK_EQ(buffer.lineCount(), 1);
  CHECK_EQ(buffer.line(0), std::string("x"));
  CHECK_EQ(buffer.offsetToPosition(0).line, 0);
  CHECK_EQ(buffer.offsetToPosition(1).line, 0);
  buffer.undo();
  CHECK_EQ(buffer.lineCount(), 3);
}

// ---------------------------------------------------------------------------
// The palettes
// ---------------------------------------------------------------------------

void testPalettes() {
  CHECK(palettes().size() >= 5u);
  bool sawDark = false;
  bool sawLight = false;
  for (const Palette &palette : palettes()) {
    CHECK(palette.name != nullptr && palette.name[0] != '\0');
    CHECK(paletteIndexByName(palette.name) >= 0);
    sawDark = sawDark || palette.dark;
    sawLight = sawLight || !palette.dark;
    // A token colour must be opaque, or the code pane fades out.
    for (const ImVec4 &color : palette.token)
      if (color.w >= 0.0f)
        CHECK(color.w >= 0.999f);
    CHECK(palette.forToken(Token::Keyword).w > 0.5f);
    CHECK(palette.forToken(Token::Uniform).w > 0.5f);
  }
  CHECK(sawDark);
  CHECK(sawLight);
  CHECK_EQ(paletteIndexByName("Midnight"), 0);
  CHECK(palettes()[paletteIndexByName("OLED")].windowBg.x == 0.0f);
  // An unknown name falls back to the first scheme rather than reading off the
  // end of the table.
  CHECK_EQ(paletteIndexByName("no such scheme"), 0);
}

}  // namespace

int main() {
  testWordClasses();
  testBuiltinsAreNotUserFunctions();
  testComments();
  testPreprocessor();
  testNumbers();
  testBrackets();
  testEveryLineRoundTrips();

  testLines();
  testPositions();
  testSelection();
  testWordAt();
  testUndoRedo();
  testUndoRestoresCaret();
  testResetDropsHistory();
  testLineCacheSurvivesEdits();

  testPalettes();

  if (gFailures == 0) {
    std::printf("ok: %d check(s) passed\n", gChecks);
    return 0;
  }
  std::printf("FAILED: %d of %d check(s)\n", gFailures, gChecks);
  return 1;
}