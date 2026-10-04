// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The code pane: the vendored code widget, taught to speak Shadertoy.
//
// The widget itself (ImGuiColorTextEdit, MIT, under third/) does the editing -
// the buffer, the caret, the selection, undo/redo, the gutter, bracket matching,
// folding, word wrap and the minimap. What it does not have is anything to do
// with Shadertoy, and that is all this header adds:
//
//   * a language that knows Shadertoy's uniforms, which are declared by the
//     renderer rather than by the shader and so are the identifiers a reader
//     most wants to see light up;
//   * a mapping from this sample's colour schemes onto the widget's palette, so
//     picking a scheme re-themes the code pane along with the ImGui chrome.
//
// The word tables come from shadertoy_syntax.hpp rather than from the widget, for
// two reasons. The widget's own GLSL definition lumps every type in with every
// control-flow word, which loses the distinction the schemes colour differently;
// and the tables here already cover every version Shadertoy compiles (GLSL ES
// 1.00, 3.00 and 3.30 together), so a shader written for the site keeps colouring
// after the user switches version.
#ifndef MODULES_V4D_SAMPLES_SHADERTOY_CODE_HPP_
#define MODULES_V4D_SAMPLES_SHADERTOY_CODE_HPP_

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "TextEditor.h"

#include "shadertoy_syntax.hpp"
#include "shadertoy_theme.hpp"

namespace shadertoy {
namespace code {

/// Fills a word set from one of the space-separated tables in the tokenizer.
/// Anything already in `into` is kept, so several tables can share one set.
inline void split(const char *table, std::unordered_set<std::string> &into) {
  const char *p = table;
  while (*p != '\0') {
    while (*p == ' ')
      ++p;
    if (*p == '\0')
      break;
    const char *start = p;
    while (*p != ' ' && *p != '\0')
      ++p;
    into.emplace(start, std::size_t(p - start));
  }
}

/// The GLSL the editor colourises as "Shadertoy GLSL".
///
/// Built once from the widget's own GLSL definition - so the identifier, number
/// and punctuation tokenizers, the comment markers and the string handling stay
/// the ones the widget tests itself against - with the word sets replaced by the
/// tables in shadertoy_syntax.hpp.
inline const TextEditor::Language *language() {
  static const TextEditor::Language *theLanguage = [] {
    // Reading Glsl() is what initialises it, so the copy below sees a complete
    // definition rather than an empty one.
    static TextEditor::Language glsl;
    glsl = *TextEditor::Language::Glsl();
    glsl.name = "Shadertoy GLSL";

    glsl.keywords.clear();
    glsl.declarations.clear();
    glsl.identifiers.clear();

    // The widget has exactly three identifier classes - keyword, declaration and
    // knownIdentifier - where the tokenizer had four. Keywords and types map over
    // one to one, so the trade is between the built-ins and the uniforms: both
    // belong in knownIdentifier, but Shadertoy's own uniforms get the slot's
    // colour on their own because iResolution and iTime are what a shader is
    // mostly about, and a call like clamp() is readable from context anyway.
    split(syntax::kKeywords, glsl.keywords);
    split(syntax::kTypes, glsl.declarations);
    split(syntax::kBuiltins, glsl.identifiers);
    split(syntax::kUniforms, glsl.identifiers);

    return &glsl;
  }();
  return theLanguage;
}

// ---------------------------------------------------------------------------
// The palette
// ---------------------------------------------------------------------------

/// The widget's palette for one of this sample's schemes.
///
/// The widget asks for colours in its own vocabulary (five identifier classes, a
/// background, a cursor, three bracket-match levels), so the mapping is not a
/// renumbering. What decides it is which colour in the scheme carries the most
/// meaning, and that is Shadertoy's uniforms - the accent-adjacent orange the
/// schemes reserve for iResolution and friends.
inline TextEditor::Palette editorPalette(const Palette &palette) {
  using C = TextEditor::Color;
  TextEditor::Palette out{};
  const auto toU32 = [](const ImVec4 &c) {
    return ImGui::ColorConvertFloat4ToU32(c);
  };
  const auto set = [&](C color, const ImVec4 &value) {
    // The palette is a std::array indexed by the colour enum, and the enum is
    // scoped, so the cast is not optional.
    out[static_cast<size_t>(color)] = toU32(value);
  };

  set(C::text, palette.forToken(syntax::Token::Plain));
  set(C::keyword, palette.forToken(syntax::Token::Keyword));
  set(C::declaration, palette.forToken(syntax::Token::Type));
  set(C::number, palette.forToken(syntax::Token::Number));
  // GLSL has no string literals in a pass, but #define bodies and the preprocessor
  // block may carry quotes; giving them the preprocessor's colour keeps a macro
  // reading as one thing rather than splitting it.
  set(C::string, palette.forToken(syntax::Token::Preprocessor));
  set(C::punctuation, palette.forToken(syntax::Token::Bracket));
  set(C::preprocessor, palette.forToken(syntax::Token::Preprocessor));
  set(C::identifier, palette.forToken(syntax::Token::Identifier));
  set(C::knownIdentifier, palette.forToken(syntax::Token::Uniform));

  set(C::comment, palette.forToken(syntax::Token::Comment));
  set(C::background, palette.codeBg);
  set(C::cursor, palette.caret);
  set(C::selection, palette.selection);
  // Only ever seen when the whitespace indicator is switched on, where a faint
  // dot is the point - the full-strength text colour would read as content.
  set(C::whitespace, palette.gutterText);

  // The bracket the caret is in gets the accent, its partner is softer, and the
  // three nesting levels walk outwards from it. An unmatched one - which is
  // exactly what a half-typed expression looks like - goes red, so a missing
  // bracket shows up before the compiler is asked.
  set(C::matchingBracketBackground, palette.currentLine);
  set(C::matchingBracketActive, palette.matchHighlight);
  set(C::matchingBracketLevel1, palette.accentSoft);
  set(C::matchingBracketLevel2, palette.accentSoft);
  set(C::matchingBracketLevel3, palette.accentSoft);
  set(C::matchingBracketError, palette.errorLine);

  set(C::lineNumber, palette.gutterText);
  set(C::currentLineNumber, palette.gutterTextActive);
  return out;
}

/// The error-line highlight, in the two colours the widget wants: one for the
/// text area and one for the gutter swatch beside the line number.
///
/// Returns false when either is fully transparent, because the widget skips a
/// marker whose alpha is zero and a scheme that has no error colour should not
/// end up with a red gutter anyway.
inline bool errorMarker(ImU32 &textColor, ImU32 &lineNumberColor,
                        const Palette &palette) {
  if (palette.errorLine.w <= 0.0f && palette.errorGutter.w <= 0.0f)
    return false;
  textColor = ImGui::ColorConvertFloat4ToU32(palette.errorLine);
  lineNumberColor = ImGui::ColorConvertFloat4ToU32(palette.errorGutter);
  return true;
}

}  // namespace code
}  // namespace shadertoy

#endif  // MODULES_V4D_SAMPLES_SHADERTOY_CODE_HPP_