// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// How the editor looks: the colour schemes, the fonts, and the ImGui style they
// imply.
//
// Kept apart from the panels so that a theme is a value that can be listed,
// picked and applied, rather than a hundred PushStyleColor calls sprinkled
// through the drawing code. The ImGui chrome and the code pane are coloured from
// the same struct, so a scheme stays coherent across both.
//
// The fonts are looked up at runtime rather than baked in: a packaged build may
// or may not have shipped the .ttf files, and a system fallback keeps the editor
// usable either way. It is better to be plainly monospaced-from-the-system than
// to be nothing at all.
#ifndef MODULES_V4D_SAMPLES_SHADERTOY_THEME_HPP_
#define MODULES_V4D_SAMPLES_SHADERTOY_THEME_HPP_

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <string>
#include <vector>

#include <opencv2/core/utility.hpp>

#include "imgui.h"

#include "shadertoy_syntax.hpp"

namespace shadertoy {

/// The colours one scheme is made of.
struct Palette {
  const char *name = "";
  bool dark = true;

  // ImGui chrome
  ImVec4 windowBg{0.075f, 0.078f, 0.090f, 1.0f};
  ImVec4 childBg{0.098f, 0.102f, 0.118f, 1.0f};
  ImVec4 panelBg{0.063f, 0.066f, 0.078f, 1.0f};
  ImVec4 titleBg{0.055f, 0.058f, 0.070f, 1.0f};
  ImVec4 frameBg{0.145f, 0.152f, 0.180f, 1.0f};
  ImVec4 frameBgHovered{0.196f, 0.207f, 0.250f, 1.0f};
  ImVec4 frameBgActive{0.145f, 0.301f, 0.360f, 1.0f};
  ImVec4 header{0.110f, 0.180f, 0.230f, 1.0f};
  ImVec4 headerHovered{0.145f, 0.250f, 0.320f, 1.0f};
  ImVec4 headerActive{0.145f, 0.301f, 0.360f, 1.0f};
  ImVec4 text{0.870f, 0.890f, 0.910f, 1.0f};
  ImVec4 textDisabled{0.450f, 0.470f, 0.510f, 1.0f};
  ImVec4 border{0.180f, 0.190f, 0.220f, 1.0f};
  ImVec4 accent{0.301f, 0.627f, 0.909f, 1.0f};
  ImVec4 accentSoft{0.301f, 0.627f, 0.909f, 0.32f};

  // Status
  ImVec4 ok{0.400f, 0.780f, 0.500f, 1.0f};
  ImVec4 warn{0.960f, 0.730f, 0.300f, 1.0f};
  ImVec4 error{0.940f, 0.400f, 0.400f, 1.0f};
  ImVec4 info{0.550f, 0.700f, 0.950f, 1.0f};

  // Code pane
  ImVec4 codeBg{0.055f, 0.058f, 0.070f, 1.0f};
  ImVec4 gutterBg{0.071f, 0.075f, 0.090f, 1.0f};
  ImVec4 gutterText{0.360f, 0.380f, 0.430f, 1.0f};
  ImVec4 gutterTextActive{0.870f, 0.890f, 0.910f, 1.0f};
  ImVec4 currentLine{1.0f, 1.0f, 1.0f, 0.055f};
  ImVec4 selection{0.180f, 0.330f, 0.520f, 0.550f};
  ImVec4 caret{0.980f, 0.850f, 0.400f, 1.0f};
  ImVec4 indentGuide{1.0f, 1.0f, 1.0f, 0.055f};
  ImVec4 matchHighlight{1.0f, 0.850f, 0.250f, 0.420f};
  ImVec4 errorLine{0.940f, 0.400f, 0.400f, 0.130f};
  ImVec4 errorGutter{0.940f, 0.400f, 0.400f, 1.0f};

  // GLSL token colours, indexed by syntax::Token. The size is derived from the
  // enum rather than written down, so adding a Token cannot silently shift every
  // colour after it -- which is exactly the bug this used to have, when the array
  // still carried a /*Bracket(paired)*/ entry for a Token that no longer exists
  // and so gave Function and Identifier each the other's colour.
  static constexpr std::size_t kTokenCount =
      static_cast<std::size_t>(syntax::Token::Identifier) + 1;
  std::array<ImVec4, kTokenCount> token{{
      /*Plain*/ ImVec4{0.870f, 0.890f, 0.910f, 1.0f},
      /*Whitespace*/ ImVec4{0.870f, 0.890f, 0.910f, 1.0f},
      /*Comment*/ ImVec4{0.470f, 0.570f, 0.500f, 1.0f},
      /*Preprocessor*/ ImVec4{0.780f, 0.560f, 0.900f, 1.0f},
      /*Keyword*/ ImVec4{0.960f, 0.560f, 0.700f, 1.0f},
      /*Type*/ ImVec4{0.400f, 0.780f, 0.780f, 1.0f},
      /*Builtin*/ ImVec4{0.600f, 0.700f, 1.000f, 1.0f},
      /*Uniform*/ ImVec4{0.980f, 0.780f, 0.400f, 1.0f},
      /*Number*/ ImVec4{0.700f, 0.880f, 0.560f, 1.0f},
      /*Operator*/ ImVec4{0.800f, 0.820f, 0.860f, 1.0f},
      /*Bracket*/ ImVec4{0.800f, 0.820f, 0.860f, 1.0f},
      /*Function*/ ImVec4{0.950f, 0.850f, 0.550f, 1.0f},
      /*Identifier*/ ImVec4{0.870f, 0.890f, 0.910f, 1.0f},
  }};

  ImVec4 forToken(syntax::Token t) const {
    const std::size_t i = static_cast<std::size_t>(t);
    return i < token.size() ? token[i] : text;
  }
};

/// The schemes the editor ships with. A dark one is the default because the
/// canvas next to it is nearly always a dark image, and the eye adapts to the
/// darker half of a split screen.
inline const std::vector<Palette> &palettes() {
  static const std::vector<Palette> all = [] {
    std::vector<Palette> p;

    {  // Midnight - the default
      Palette s;
      s.name = "Midnight";
      p.push_back(s);
    }
    {  // Graphite - neutral, for people who dislike a blue accent
      Palette s;
      s.name = "Graphite";
      s.windowBg = ImVec4(0.110f, 0.112f, 0.118f, 1.0f);
      s.childBg = ImVec4(0.135f, 0.137f, 0.145f, 1.0f);
      s.panelBg = ImVec4(0.098f, 0.100f, 0.106f, 1.0f);
      s.titleBg = ImVec4(0.086f, 0.088f, 0.094f, 1.0f);
      s.codeBg = ImVec4(0.086f, 0.088f, 0.094f, 1.0f);
      s.gutterBg = ImVec4(0.106f, 0.108f, 0.115f, 1.0f);
      s.frameBg = ImVec4(0.180f, 0.182f, 0.192f, 1.0f);
      s.frameBgHovered = ImVec4(0.230f, 0.234f, 0.246f, 1.0f);
      s.frameBgActive = ImVec4(0.290f, 0.294f, 0.310f, 1.0f);
      s.header = ImVec4(0.150f, 0.152f, 0.162f, 1.0f);
      s.headerHovered = ImVec4(0.200f, 0.203f, 0.215f, 1.0f);
      s.headerActive = ImVec4(0.250f, 0.254f, 0.268f, 1.0f);
      s.border = ImVec4(0.220f, 0.222f, 0.235f, 1.0f);
      s.accent = ImVec4(0.780f, 0.780f, 0.800f, 1.0f);
      s.accentSoft = ImVec4(0.780f, 0.780f, 0.800f, 0.28f);
      s.token[static_cast<std::size_t>(syntax::Token::Keyword)] =
          ImVec4(0.900f, 0.500f, 0.640f, 1.0f);
      s.token[static_cast<std::size_t>(syntax::Token::Type)] =
          ImVec4(0.400f, 0.800f, 0.740f, 1.0f);
      s.token[static_cast<std::size_t>(syntax::Token::Builtin)] =
          ImVec4(0.560f, 0.680f, 1.000f, 1.0f);
      s.token[static_cast<std::size_t>(syntax::Token::Uniform)] =
          ImVec4(0.950f, 0.760f, 0.420f, 1.0f);
      s.token[static_cast<std::size_t>(syntax::Token::Number)] =
          ImVec4(0.680f, 0.860f, 0.560f, 1.0f);
      s.token[static_cast<std::size_t>(syntax::Token::Comment)] =
          ImVec4(0.480f, 0.490f, 0.500f, 1.0f);
      s.token[static_cast<std::size_t>(syntax::Token::Preprocessor)] =
          ImVec4(0.760f, 0.600f, 0.880f, 1.0f);
      s.caret = ImVec4(0.980f, 0.980f, 0.980f, 1.0f);
      p.push_back(s);
    }
    {  // Solarized Dark - the classic low-contrast palette
      Palette s;
      s.name = "Solarized Dark";
      s.windowBg = ImVec4(0.043f, 0.059f, 0.086f, 1.0f);
      s.childBg = ImVec4(0.055f, 0.071f, 0.102f, 1.0f);
      s.panelBg = ImVec4(0.035f, 0.047f, 0.071f, 1.0f);
      s.titleBg = ImVec4(0.031f, 0.043f, 0.063f, 1.0f);
      s.codeBg = ImVec4(0.039f, 0.051f, 0.075f, 1.0f);
      s.gutterBg = ImVec4(0.043f, 0.059f, 0.086f, 1.0f);
      s.gutterText = ImVec4(0.337f, 0.424f, 0.482f, 1.0f);
      s.gutterTextActive = ImVec4(0.671f, 0.741f, 0.769f, 1.0f);
      s.frameBg = ImVec4(0.102f, 0.129f, 0.169f, 1.0f);
      s.frameBgHovered = ImVec4(0.137f, 0.173f, 0.224f, 1.0f);
      s.frameBgActive = ImVec4(0.180f, 0.290f, 0.239f, 1.0f);
      s.header = ImVec4(0.114f, 0.145f, 0.192f, 1.0f);
      s.headerHovered = ImVec4(0.157f, 0.196f, 0.251f, 1.0f);
      s.headerActive = ImVec4(0.188f, 0.294f, 0.239f, 1.0f);
      s.text = ImVec4(0.831f, 0.843f, 0.812f, 1.0f);
      s.textDisabled = ImVec4(0.463f, 0.514f, 0.561f, 1.0f);
      s.border = ImVec4(0.129f, 0.161f, 0.212f, 1.0f);
      s.accent = ImVec4(0.267f, 0.620f, 0.702f, 1.0f);
      s.accentSoft = ImVec4(0.267f, 0.620f, 0.702f, 0.32f);
      s.ok = ImVec4(0.596f, 0.741f, 0.404f, 1.0f);
      s.warn = ImVec4(0.855f, 0.663f, 0.267f, 1.0f);
      s.error = ImVec4(0.898f, 0.322f, 0.282f, 1.0f);
      s.selection = ImVec4(0.180f, 0.451f, 0.475f, 0.550f);
      s.caret = ImVec4(0.933f, 0.596f, 0.153f, 1.0f);
      s.token[static_cast<std::size_t>(syntax::Token::Comment)] =
          ImVec4(0.580f, 0.647f, 0.545f, 1.0f);
      s.token[static_cast<std::size_t>(syntax::Token::Keyword)] =
          ImVec4(0.898f, 0.322f, 0.282f, 1.0f);
      s.token[static_cast<std::size_t>(syntax::Token::Type)] =
          ImVec4(0.580f, 0.647f, 0.545f, 1.0f);
      s.token[static_cast<std::size_t>(syntax::Token::Builtin)] =
          ImVec4(0.827f, 0.596f, 0.263f, 1.0f);
      s.token[static_cast<std::size_t>(syntax::Token::Uniform)] =
          ImVec4(0.859f, 0.631f, 0.416f, 1.0f);
      s.token[static_cast<std::size_t>(syntax::Token::Number)] =
          ImVec4(0.859f, 0.631f, 0.416f, 1.0f);
      s.token[static_cast<std::size_t>(syntax::Token::Function)] =
          ImVec4(0.267f, 0.620f, 0.702f, 1.0f);
      s.token[static_cast<std::size_t>(syntax::Token::Preprocessor)] =
          ImVec4(0.549f, 0.463f, 0.671f, 1.0f);
      p.push_back(s);
    }
    {  // Solarized Light - for a bright room, and for projectors
      Palette s;
      s.name = "Solarized Light";
      s.dark = false;
      s.windowBg = ImVec4(0.925f, 0.945f, 0.961f, 1.0f);
      s.childBg = ImVec4(0.910f, 0.933f, 0.949f, 1.0f);
      s.panelBg = ImVec4(0.886f, 0.910f, 0.929f, 1.0f);
      s.titleBg = ImVec4(0.851f, 0.882f, 0.906f, 1.0f);
      s.codeBg = ImVec4(0.988f, 0.992f, 0.996f, 1.0f);
      s.gutterBg = ImVec4(0.922f, 0.941f, 0.957f, 1.0f);
      s.gutterText = ImVec4(0.510f, 0.514f, 0.502f, 1.0f);
      s.gutterTextActive = ImVec4(0.239f, 0.322f, 0.361f, 1.0f);
      s.frameBg = ImVec4(0.855f, 0.886f, 0.902f, 1.0f);
      s.frameBgHovered = ImVec4(0.812f, 0.855f, 0.878f, 1.0f);
      s.frameBgActive = ImVec4(0.839f, 0.827f, 0.749f, 1.0f);
      s.header = ImVec4(0.855f, 0.886f, 0.902f, 1.0f);
      s.headerHovered = ImVec4(0.812f, 0.855f, 0.878f, 1.0f);
      s.headerActive = ImVec4(0.839f, 0.827f, 0.749f, 1.0f);
      s.text = ImVec4(0.239f, 0.322f, 0.361f, 1.0f);
      s.textDisabled = ImVec4(0.580f, 0.588f, 0.580f, 1.0f);
      s.border = ImVec4(0.788f, 0.816f, 0.831f, 1.0f);
      s.accent = ImVec4(0.000f, 0.435f, 0.522f, 1.0f);
      s.accentSoft = ImVec4(0.000f, 0.435f, 0.522f, 0.32f);
      s.ok = ImVec4(0.400f, 0.639f, 0.204f, 1.0f);
      s.warn = ImVec4(0.792f, 0.549f, 0.063f, 1.0f);
      s.error = ImVec4(0.839f, 0.180f, 0.129f, 1.0f);
      s.selection = ImVec4(0.839f, 0.898f, 0.863f, 0.700f);
      s.caret = ImVec4(0.792f, 0.549f, 0.063f, 1.0f);
      s.currentLine = ImVec4(0.000f, 0.000f, 0.000f, 0.045f);
      s.indentGuide = ImVec4(0.000f, 0.000f, 0.000f, 0.070f);
      s.matchHighlight = ImVec4(0.859f, 0.631f, 0.416f, 0.550f);
      s.errorLine = ImVec4(0.839f, 0.180f, 0.129f, 0.120f);
      s.token[static_cast<std::size_t>(syntax::Token::Comment)] =
          ImVec4(0.580f, 0.647f, 0.545f, 1.0f);
      s.token[static_cast<std::size_t>(syntax::Token::Keyword)] =
          ImVec4(0.839f, 0.180f, 0.129f, 1.0f);
      s.token[static_cast<std::size_t>(syntax::Token::Type)] =
          ImVec4(0.580f, 0.647f, 0.545f, 1.0f);
      s.token[static_cast<std::size_t>(syntax::Token::Builtin)] =
          ImVec4(0.827f, 0.596f, 0.263f, 1.0f);
      s.token[static_cast<std::size_t>(syntax::Token::Uniform)] =
          ImVec4(0.859f, 0.631f, 0.416f, 1.0f);
      s.token[static_cast<std::size_t>(syntax::Token::Number)] =
          ImVec4(0.859f, 0.631f, 0.416f, 1.0f);
      s.token[static_cast<std::size_t>(syntax::Token::Function)] =
          ImVec4(0.267f, 0.620f, 0.702f, 1.0f);
      s.token[static_cast<std::size_t>(syntax::Token::Preprocessor)] =
          ImVec4(0.549f, 0.463f, 0.671f, 1.0f);
      s.token[static_cast<std::size_t>(syntax::Token::Plain)] =
          ImVec4(0.239f, 0.322f, 0.361f, 1.0f);
      s.token[static_cast<std::size_t>(syntax::Token::Whitespace)] =
          ImVec4(0.239f, 0.322f, 0.361f, 1.0f);
      s.token[static_cast<std::size_t>(syntax::Token::Operator)] =
          ImVec4(0.400f, 0.482f, 0.502f, 1.0f);
      s.token[static_cast<std::size_t>(syntax::Token::Bracket)] =
          ImVec4(0.400f, 0.482f, 0.502f, 1.0f);
      s.token[static_cast<std::size_t>(syntax::Token::Identifier)] =
          ImVec4(0.239f, 0.322f, 0.361f, 1.0f);
      p.push_back(s);
    }
    {  // OLED - true black, for a laptop panel in a dark room
      Palette s;
      s.name = "OLED";
      s.windowBg = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
      s.childBg = ImVec4(0.035f, 0.035f, 0.039f, 1.0f);
      s.panelBg = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
      s.titleBg = ImVec4(0.055f, 0.055f, 0.062f, 1.0f);
      s.codeBg = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
      s.gutterBg = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
      s.gutterText = ImVec4(0.290f, 0.310f, 0.350f, 1.0f);
      s.frameBg = ImVec4(0.098f, 0.102f, 0.118f, 1.0f);
      s.frameBgHovered = ImVec4(0.141f, 0.149f, 0.169f, 1.0f);
      s.frameBgActive = ImVec4(0.145f, 0.301f, 0.360f, 1.0f);
      s.header = ImVec4(0.075f, 0.078f, 0.090f, 1.0f);
      s.headerHovered = ImVec4(0.125f, 0.129f, 0.145f, 1.0f);
      s.headerActive = ImVec4(0.145f, 0.301f, 0.360f, 1.0f);
      s.border = ImVec4(0.110f, 0.113f, 0.125f, 1.0f);
      s.currentLine = ImVec4(1.0f, 1.0f, 1.0f, 0.070f);
      s.caret = ImVec4(0.400f, 0.850f, 1.000f, 1.0f);
      s.token[static_cast<std::size_t>(syntax::Token::Comment)] =
          ImVec4(0.350f, 0.470f, 0.400f, 1.0f);
      p.push_back(s);
    }
    return p;
  }();
  return all;
}

inline int paletteIndexByName(const std::string &name) {
  const auto &all = palettes();
  for (std::size_t i = 0; i < all.size(); ++i)
    if (name == all[i].name)
      return int(i);
  return 0;
}

// ---------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------

/// Which faces the editor uses, and where they came from.
///
/// The mono face is what the code pane, the gutter and every number in the
/// status bar are drawn with - a proportional font there makes a column of line
/// numbers jitter as the numbers change. The UI face is proportional because
/// that is what labels want.
struct Fonts {
  ImFont *ui = nullptr;
  ImFont *uiBold = nullptr;
  ImFont *mono = nullptr;
  ImFont *monoBold = nullptr;
  float uiSize = 16.0f;
  float monoSize = 15.0f;
  bool loaded = false;
  std::string monoName;
  std::string uiName;

  ImFont *pick(ImFont *wanted, ImFont *fallback) const {
    return wanted != nullptr ? wanted : fallback;
  }
};

/// Picks the first existing path out of a list of names, searching the asset
/// path. Empty when none of them is there.
inline std::string firstExistingAsset(const std::vector<std::string> &names) {
  for (const auto &name : names) {
    const cv::String found = cv::samples::findFile(name, false, true);
    if (!found.empty())
      return found;
  }
  return std::string();
}

/// Loads the editor's faces into ImGui's atlas.
///
/// JetBrains Mono is what the repo ships; the DejaVu and Liberation faces are
/// what every Linux desktop has, so a build without the shipped asset still gets
/// a monospaced code pane. When nothing at all is found the default ImGui font
/// stays, and `Fonts::loaded` says so - the editor then draws the code pane with
/// a proportional face and a wider character cell, which is ugly but usable.
inline Fonts loadFonts(float uiScale = 1.0f) {
  static bool done = false;
  static Fonts fonts;
  if (done)
    return fonts;
  done = true;

  ImGuiIO &io = ImGui::GetIO();

  const std::string monoPath = firstExistingAsset(
      {"fonts/JetBrainsMono-Regular.ttf", "JetBrainsMono-Regular.ttf",
       "fonts/dejavu-fonts-ttf-2.37/ttf/DejaVuSansMono.ttf",
       "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
       "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
       "/usr/share/fonts/TTF/DejaVuSansMono.ttf"});
  const std::string monoBoldPath = firstExistingAsset(
      {"fonts/JetBrainsMono-Bold.ttf", "JetBrainsMono-Bold.ttf",
       "fonts/dejavu-fonts-ttf-2.37/ttf/DejaVuSansMono-Bold.ttf",
       "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
       "/usr/share/fonts/truetype/liberation/LiberationMono-Bold.ttf",
       "/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf"});
  const std::string uiPath = firstExistingAsset(
      {"fonts/dejavu-fonts-ttf-2.37/ttf/DejaVuSans.ttf",
       "fonts/DejaVuSans.ttf", "DejaVuSans.ttf",
       "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
       "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
       "/usr/share/fonts/TTF/DejaVuSans.ttf"});
  const std::string uiBoldPath = firstExistingAsset(
      {"fonts/dejavu-fonts-ttf-2.37/ttf/DejaVuSans-Bold.ttf",
       "fonts/DejaVuSans-Bold.ttf", "DejaVuSans-Bold.ttf",
       "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
       "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
       "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf"});

  fonts.uiSize = 16.0f * uiScale;
  fonts.monoSize = 15.0f * uiScale;

  // The glyph range is what stops a stray character in the editor from showing
  // up as a placeholder box: everything a shader can contain, plus the symbols
  // the panels use for their buttons and the arrows in the status bar.
  static const ImWchar kRanges[] = {0x0020, 0x00FF, 0x2190, 0x21FF, 0x2500,
                                    0x25FF, 0x2600, 0x26FF, 0};

  // The UI faces go in first, so that Fonts[0] - the face every widget asks for
  // when nothing is pushed - is the one that is right for labels.
  if (!uiPath.empty()) {
    fonts.ui = io.Fonts->AddFontFromFileTTF(uiPath.c_str(), fonts.uiSize,
                                            nullptr, kRanges);
    fonts.uiName = uiPath;
  }
  if (!uiBoldPath.empty() && fonts.ui != nullptr)
    fonts.uiBold = io.Fonts->AddFontFromFileTTF(uiBoldPath.c_str(), fonts.uiSize,
                                                nullptr, kRanges);
  if (fonts.uiBold == nullptr)
    fonts.uiBold = fonts.ui;

  if (!monoPath.empty()) {
    fonts.mono = io.Fonts->AddFontFromFileTTF(monoPath.c_str(), fonts.monoSize,
                                              nullptr, kRanges);
    fonts.monoName = monoPath;
  }
  if (!monoBoldPath.empty() && fonts.mono != nullptr)
    fonts.monoBold = io.Fonts->AddFontFromFileTTF(monoBoldPath.c_str(),
                                                  fonts.monoSize, nullptr,
                                                  kRanges);
  if (fonts.monoBold == nullptr)
    fonts.monoBold = fonts.mono;
  if (fonts.mono == nullptr)
    fonts.mono = fonts.ui;
  if (fonts.monoBold == nullptr)
    fonts.monoBold = fonts.mono;

  fonts.loaded = fonts.ui != nullptr || fonts.mono != nullptr;
  return fonts;
}

// ---------------------------------------------------------------------------
// The ImGui style
// ---------------------------------------------------------------------------

/// Applies `palette` to ImGui's chrome and to the editor's own drawing.
///
/// Everything ImGui knows about is set here rather than pushed per widget, so a
/// scheme really is one value.
inline void applyPalette(const Palette &palette, const Fonts &fonts) {
  if (palette.dark)
    ImGui::StyleColorsDark();
  else
    ImGui::StyleColorsLight();

  ImGuiStyle &style = ImGui::GetStyle();
  style.WindowRounding = 4.0f;
  style.ChildRounding = 3.0f;
  style.FrameRounding = 3.0f;
  style.PopupRounding = 4.0f;
  style.ScrollbarRounding = 3.0f;
  style.GrabRounding = 3.0f;
  style.TabRounding = 3.0f;
  style.WindowBorderSize = 1.0f;
  style.ChildBorderSize = 1.0f;
  style.PopupBorderSize = 1.0f;
  style.FrameBorderSize = 0.0f;
  style.ItemSpacing = ImVec2(8.0f, 6.0f);
  style.FramePadding = ImVec2(6.0f, 4.0f);
  style.CellPadding = ImVec2(6.0f, 3.0f);
  style.WindowPadding = ImVec2(8.0f, 8.0f);
  style.ScrollbarSize = 13.0f;
  style.GrabMinSize = 10.0f;

  auto set = [&](ImGuiCol col, ImVec4 value) { style.Colors[col] = value; };
  set(ImGuiCol_WindowBg, palette.windowBg);
  set(ImGuiCol_ChildBg, palette.childBg);
  set(ImGuiCol_PopupBg, palette.childBg);
  set(ImGuiCol_Border, palette.border);
  set(ImGuiCol_FrameBg, palette.frameBg);
  set(ImGuiCol_FrameBgHovered, palette.frameBgHovered);
  set(ImGuiCol_FrameBgActive, palette.frameBgActive);
  set(ImGuiCol_TitleBg, palette.titleBg);
  set(ImGuiCol_TitleBgActive, palette.titleBg);
  set(ImGuiCol_TitleBgCollapsed, palette.titleBg);
  set(ImGuiCol_MenuBarBg, palette.panelBg);
  set(ImGuiCol_ScrollbarBg, palette.windowBg);
  set(ImGuiCol_ScrollbarGrab, palette.frameBgHovered);
  set(ImGuiCol_ScrollbarGrabHovered, palette.accentSoft);
  set(ImGuiCol_ScrollbarGrabActive, palette.accent);
  set(ImGuiCol_CheckMark, palette.accent);
  set(ImGuiCol_SliderGrab, palette.accent);
  set(ImGuiCol_SliderGrabActive, palette.accent);
  set(ImGuiCol_Button, palette.frameBg);
  set(ImGuiCol_ButtonHovered, palette.frameBgHovered);
  set(ImGuiCol_ButtonActive, palette.frameBgActive);
  set(ImGuiCol_Header, palette.header);
  set(ImGuiCol_HeaderHovered, palette.headerHovered);
  set(ImGuiCol_HeaderActive, palette.headerActive);
  set(ImGuiCol_Separator, palette.border);
  set(ImGuiCol_SeparatorHovered, palette.accentSoft);
  set(ImGuiCol_SeparatorActive, palette.accent);
  set(ImGuiCol_ResizeGrip, palette.border);
  set(ImGuiCol_Tab, palette.panelBg);
  set(ImGuiCol_TabHovered, palette.headerHovered);
  set(ImGuiCol_TabSelected, palette.childBg);
  set(ImGuiCol_TabSelectedOverline, palette.accent);
  set(ImGuiCol_TabDimmed, palette.panelBg);
  set(ImGuiCol_TabDimmedSelected, palette.childBg);
  set(ImGuiCol_TableHeaderBg, palette.header);
  set(ImGuiCol_TableBorderStrong, palette.border);
  set(ImGuiCol_TableBorderLight, palette.border);
  set(ImGuiCol_TableRowBg, palette.childBg);
  set(ImGuiCol_TableRowBgAlt, palette.panelBg);
  set(ImGuiCol_TextSelectedBg, palette.selection);
  set(ImGuiCol_DragDropTarget, palette.accentSoft);
  set(ImGuiCol_NavCursor, palette.accent);
  set(ImGuiCol_NavWindowingHighlight, palette.accent);
  set(ImGuiCol_NavWindowingDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.35f));
  set(ImGuiCol_Text, palette.text);
  set(ImGuiCol_TextDisabled, palette.textDisabled);

  // A null FontDefault means "Fonts[0]", which is the UI face by construction.
  // Leaving it null when nothing loaded is deliberate: ImGui then falls back to
  // the embedded default face, which is the one thing that is always present.
  ImGuiIO &io = ImGui::GetIO();
  io.FontDefault = fonts.ui != nullptr ? fonts.ui : nullptr;
}

/// The width of one character cell of the code pane, which every x coordinate
/// inside it is derived from.
///
/// Measured rather than guessed from the point size: a font's advance width and
/// its nominal size are two different numbers, and every caret position, gutter
/// column and click hit-test in the editor is a multiple of this one. Ten
/// characters are averaged so that a fallback proportional face gives a stable
/// number instead of one that changes with the letter under the cursor.
inline float cellWidth(ImFont *font, float size = 0.0f) {
  if (font == nullptr)
    return 8.0f;
  if (size <= 0.0f)
    size = font->LegacySize > 0.0f ? font->LegacySize : 13.0f;
  if (!font->IsLoaded())
    return size * 0.6f;
  return font->CalcTextSizeA(size, FLT_MAX, 0.0f, "MMMMMMMMMM").x / 10.0f;
}

}  // namespace shadertoy

#endif  // MODULES_V4D_SAMPLES_SHADERTOY_THEME_HPP_
