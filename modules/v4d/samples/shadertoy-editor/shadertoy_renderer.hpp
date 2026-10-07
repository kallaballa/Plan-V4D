// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The Shadertoy rendering model, as a plain OpenGL renderer.
//
// One GLSL program per render pass, "common" blocks prepended to all of them,
// "buffer" passes rendered into ping-pong framebuffers and the "image" pass
// writing the visible result; the Shadertoy uniforms are supplied and a pass's
// channel inputs resolve to buffer outputs, textures or the keyboard texture.
// That is what shadertoy-editor.cpp draws with. It is split out to keep the
// editor self-contained.
//
// It knows nothing about where a shader came from: load() takes a
// shadertoy::Shader plus its already decoded textures, so the editor can
// compile what the user just typed.
//
// Assembled sources keep a line map (see PassSource), because a driver reports
// errors as "0:317(4): error: ..." and 317 is a line of the assembled program,
// not of the code anybody wrote. parseCompileError() translates that back to a
// pass and a line inside it.

#ifndef MODULES_V4D_SAMPLES_SHADERTOY_RENDERER_HPP_
#define MODULES_V4D_SAMPLES_SHADERTOY_RENDERER_HPP_

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/v4d/v4d.hpp>

#include "shadertoy_model.hpp"

namespace shadertoy {

// The Shadertoy key codes are the legacy ones the reference keyboard texture
// documents, and Keyboard::Key is where V4D reports them.
using namespace cv::v4d::event;

// ---------------------------------------------------------------------------
// Shader source assembly
// ---------------------------------------------------------------------------

// One full-screen quad, drawn as two triangles in NDC.
const string kQuadVert = std::string(OPENCV_V4D_GL_SHADER_VERSION) + R"(
layout(location = 0) in vec2 position;
void main()
{
    gl_Position = vec4(position, 0.0, 1.0);
}
)";

// The same quad, but for the final blit: it maps the window onto the canvas
// rectangle (in normalized device coordinates) so the shader keeps its aspect
// ratio and the letterbox area can be painted in the background color.
const string kBlitVert = std::string(OPENCV_V4D_GL_SHADER_VERSION) + R"(
layout(location = 0) in vec2 position;
out vec2 vUV;
void main()
{
    vUV = position * 0.5 + 0.5;
    gl_Position = vec4(position, 0.0, 1.0);
}
)";

const string kBlitFrag = std::string(OPENCV_V4D_GL_SHADER_VERSION) + R"(
// GLSL ES fragment shaders have no default float precision; strict drivers
// (Mali) refuse to compile without one even though desktop GL accepts it.
precision highp float;
in vec2 vUV;
uniform sampler2D uTex;
uniform vec2 uMin;
uniform vec2 uMax;
uniform vec4 uBackground;
out vec4 outColor;

void main()
{
    if (any(lessThan(vUV, uMin)) || any(greaterThan(vUV, uMax))) {
        outColor = uBackground;
        return;
    }
    vec2 uv = (vUV - uMin) / (uMax - uMin);
    outColor = vec4(texture(uTex, uv).rgb, 1.0);
}
)";

// Everything a Shadertoy render pass may reference. Shadertoy hands out these
// uniforms to the pass and hides them from its source, so they have to be
// declared here instead.
const string kPassUniforms = R"(
precision lowp sampler2D;
precision lowp float;
precision lowp int;
uniform vec3  iResolution;
uniform float iTime;
uniform float iTimeDelta;
uniform int   iFrame;
uniform float iFrameRate;
uniform vec4  iMouse;
uniform vec4  iDate;
uniform float iSampleRate;
uniform float iChannelTime[4];
uniform vec3  iChannelResolution[4];
uniform sampler2D iChannel0;
uniform sampler2D iChannel1;
uniform sampler2D iChannel2;
uniform sampler2D iChannel3;
uniform sampler2D iChannelKeyboard;
)";

/// One pass' GLSL after the Shadertoy conventions have been applied to it.
///
/// `text` is what the driver compiles. `map` has one entry per line of `text`
/// and says which line of which piece of user code that line came from, so an
/// error at "0:317" can be pointed at line 12 of Buffer A.
struct PassSource {
  std::string text;
  /// Origin of a line of the source: a pass index and a 1-based line in it.
  struct Origin {
    int pass = -1;
    int line = 0;
  };
  std::vector<Origin> map;

  /// The origin of assembled line `assembledLine` (1-based), or pass -1 when
  /// the line came from the scaffolding (the #version line, the uniforms,
  /// main()).
  Origin originOf(int assembledLine) const {
    if (assembledLine < 1 || size_t(assembledLine) > map.size())
      return {};
    return map[size_t(assembledLine - 1)];
  }
};

/// Assembles one pass of `shader` - the concatenated `common` blocks, the
/// uniforms Shadertoy hides from the pass, and a main() that calls mainImage().
///
/// `passes` is the whole shader because the common blocks live in passes of
/// their own; `passIndex` is the pass this program belongs to. The line map the
/// result carries therefore points into whichever pass a line really came from.
PassSource buildPassSource(const std::vector<Pass> &passes, int passIndex) {
  PassSource out;
  // Splits on '\n' and keeps the empty tail, so a chunk always has as many
  // lines as it had newlines plus one - which is what makes the map exact.
  auto append = [&](const std::string &chunk, int pass, int firstLine) {
    int line = firstLine;
    size_t from = 0;
    while (true) {
      const size_t nl = chunk.find('\n', from);
      const size_t end = (nl == std::string::npos) ? chunk.size() : nl;
      out.text += chunk.substr(from, end - from);
      out.text += '\n';
      out.map.push_back({pass, line});
      if (nl == std::string::npos)
        break;
      ++line;
      from = nl + 1;
    }
  };
  // Scaffolding: pass -1, i.e. not attributable to any user line.
  auto appendPlain = [&](const std::string &chunk) { append(chunk, -1, 0); };

  appendPlain(std::string(OPENCV_V4D_GL_SHADER_VERSION) + "\n");
  // Shadertoy shaders are written against WebGL 1, where these are still
  // spelled the old way.
  appendPlain("#define texture2D texture\n");
  appendPlain("#define textureCube texture\n");
  // The uniforms come before the common blocks: GLSL wants a declaration before
  // the first use, and a common block that mentions iTime or iResolution (many
  // do) would otherwise fail to compile with "undeclared identifier".
  appendPlain(kPassUniforms);
  for (size_t i = 0; i < passes.size(); ++i)
    if (passes[i].type == "common")
      append(passes[i].code, int(i), 1);
  appendPlain("out vec4 outColor;\n");
  append(passes[size_t(passIndex)].code, passIndex, 1);
  // Shadertoy only looks at fragColor.rgb, the alpha is always opaque.
  appendPlain("\nvoid main()\n{\n    vec4 c = vec4(0.0, 0.0, 0.0, 1.0);\n"
              "    mainImage(c, gl_FragCoord.xy);\n"
              "    outColor = vec4(c.rgb, 1.0);\n}\n");
  return out;
}

/// A driver error, resolved back to the user's code.
struct CompileError {
  int pass = -1; // index into Shader::passes, -1 when the origin is unknown
  int line = 0;  // 1-based line inside that pass, 0 when unknown
  int column = 0;
  std::string message;
  /// The offending line, and the two around it, as the user sees them.
  std::string context;
  /// True when the error could be attributed to a pass and a line.
  bool located() const { return pass >= 0 && line > 0; }
};

/// The line `line` (1-based) of `code`, with its neighbours, as "  12 | text".
std::string lineContext(const std::string &code, int line) {
  std::string out;
  if (line < 1)
    return out;
  int current = 1;
  size_t from = 0;
  int shown = 0;
  while (from <= code.size() && shown < 3) {
    const size_t nl = code.find('\n', from);
    const size_t end = (nl == std::string::npos) ? code.size() : nl;
    if (current >= line - 1 && current <= line + 1) {
      out += "  ";
      out += std::to_string(current);
      out += (current == line) ? " >| " : " |  ";
      out += code.substr(from, end - from);
      out += '\n';
      ++shown;
    }
    if (nl == std::string::npos)
      break;
    ++current;
    from = nl + 1;
  }
  return out;
}

/// Turns what the driver said about one pass into something an editor can act
/// on. `log` may hold several lines; the first one carrying a "0:<line>"
/// position is the one that is mapped, and the rest is kept as the message.
///
/// Drivers are not all identical here: Mesa and NVIDIA write "0:317(4): error:
/// ..." while some report "0:317: error: ...". Both are accepted.
CompileError parseCompileError(const std::string &log,
                               const std::vector<Pass> &passes,
                               const PassSource &source) {
  CompileError error;
  auto trim = [](std::string s) {
    const size_t first = s.find_first_not_of(" \t\r");
    if (first == std::string::npos)
      return std::string();
    s.erase(0, first);
    while (!s.empty() &&
           (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
      s.pop_back();
    return s;
  };
  size_t pos = 0;
  int line = 0;
  int column = 0;
  bool found = false;
  while (pos < log.size()) {
    size_t nl = log.find('\n', pos);
    if (nl == std::string::npos)
      nl = log.size();
    const std::string lineText = log.substr(pos, nl - pos);
    if (!found) {
      // "0:317(4): error: ..." or "0:317: error: ..." - strip the position
      // prefix and keep the rest of the line as the message.
      const size_t colon = lineText.find(':');
      char *end = nullptr;
      const long value =
          (colon == std::string::npos || colon == 0)
              ? 0
              : std::strtol(lineText.c_str() + colon + 1, &end, 10);
      if (end != nullptr && value > 0) {
        size_t messageAt = colon + 1;
        if (*end == '(') {
          column = int(std::strtol(end + 1, nullptr, 10));
          const size_t close =
              lineText.find(')', size_t(end - lineText.c_str()));
          if (close != std::string::npos)
            messageAt = close + 1;
        } else {
          messageAt = size_t(end - lineText.c_str());
        }
        if (messageAt < lineText.size() && lineText[messageAt] == ':')
          ++messageAt;
        line = int(value);
        found = true;
        error.message = trim(lineText.substr(messageAt));
        pos = nl + 1;
        continue;
      }
      // Keep whatever precedes a located line: it is the useful part when the
      // driver put the position on a line of its own.
      error.message += lineText;
      if (!error.message.empty())
        error.message += '\n';
    } else {
      // After the first located line, keep the follow-ups that are errors in
      // their own right. Mesa repeats a long "candidates are:" list after every
      // one of them, which would bury the actual complaint.
      const size_t first = lineText.find_first_not_of(" \t\r");
      const std::string text =
          first == std::string::npos ? std::string() : lineText.substr(first);
      const bool isError = !text.empty() &&
                           std::isdigit(static_cast<unsigned char>(text[0])) &&
                           (text.find("error") != std::string::npos ||
                            text.find("warning") != std::string::npos);
      if (isError) {
        error.message += text;
        error.message += '\n';
      }
    }
    pos = nl + 1;
  }
  while (!error.message.empty() &&
         (error.message.back() == '\n' || error.message.back() == ' '))
    error.message.pop_back();
  if (found) {
    const PassSource::Origin origin = source.originOf(line);
    error.pass = origin.pass;
    error.line = origin.line;
    if (error.pass >= 0 && size_t(error.pass) < passes.size())
      error.context = lineContext(passes[size_t(error.pass)].code, origin.line);
    if (error.pass < 0) {
      // The error is in the scaffolding (or in a common block that came from
      // nowhere): say so instead of pointing at a line of a pass that has it.
      error.context = lineContext(source.text, line);
    }
  }
  return error;
}

// ---------------------------------------------------------------------------
// Small GL helpers
// ---------------------------------------------------------------------------

GLuint compileShader(GLenum type, const std::string &source, std::string &log) {
  const GLuint shader = glCreateShader(type);
  const char *text = source.c_str();
  glShaderSource(shader, 1, &text, nullptr);
  glCompileShader(shader);
  GLint ok = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (ok != GL_FALSE)
    return shader;
  GLint length = 0;
  glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
  if (length > 1) {
    std::vector<char> buffer(size_t(length) + 1);
    glGetShaderInfoLog(shader, length, nullptr, buffer.data());
    log = buffer.data();
  } else {
    log = "the shader compiler said nothing useful";
  }
  glDeleteShader(shader);
  return 0;
}

/// Renders a CompileError the way a terminal log wants it: "pass 'Buffer A',
/// line 12: error: ..." followed by the offending line.
std::string describe(const CompileError &error, const std::string &passName) {
  std::string out;
  if (error.pass >= 0)
    out += "pass '" + passName + "'";
  if (error.located()) {
    out += ", line " + std::to_string(error.line);
    if (error.column > 0)
      out += ":" + std::to_string(error.column);
  } else if (error.pass >= 0) {
    out += ": ";
  }
  if (!error.message.empty())
    out += ": " + error.message;
  if (!error.context.empty())
    out += "\n" + error.context;
  return out;
}

GLuint linkProgram(const std::string &vertSource, const std::string &fragSource,
                   std::string &log) {
  std::string stageLog;
  const GLuint vert = compileShader(GL_VERTEX_SHADER, vertSource, stageLog);
  if (vert == 0) {
    log = "vertex shader: " + stageLog;
    return 0;
  }
  const GLuint frag = compileShader(GL_FRAGMENT_SHADER, fragSource, stageLog);
  if (frag == 0) {
    glDeleteShader(vert);
    log = stageLog;
    return 0;
  }
  const GLuint program = glCreateProgram();
  glAttachShader(program, vert);
  glAttachShader(program, frag);
  glLinkProgram(program);
  // The shader objects are only needed for linking.
  glDeleteShader(vert);
  glDeleteShader(frag);
  GLint ok = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (ok != GL_FALSE)
    return program;
  GLint length = 0;
  glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
  if (length > 1) {
    std::vector<char> buffer(size_t(length) + 1);
    glGetProgramInfoLog(program, length, nullptr, buffer.data());
    log = buffer.data();
  } else {
    log = "linking failed without a message";
  }
  glDeleteProgram(program);
  return 0;
}

GLint minFilterOf(const std::string &filter) {
  if (filter == "nearest")
    return GL_NEAREST;
  if (filter == "mipmap")
    return GL_LINEAR_MIPMAP_LINEAR;
  return GL_LINEAR;
}

GLint wrapModeOf(const std::string &wrap) {
  if (wrap == "repeat")
    return GL_REPEAT;
  if (wrap == "mirror")
    return GL_MIRRORED_REPEAT;
  return GL_CLAMP_TO_EDGE;
}

GLuint createTexture(const cv::Mat &rgba8, const std::string &filter,
                     const std::string &wrap, bool flipVertically) {
  cv::Mat src = rgba8;
  if (flipVertically)
    cv::flip(src, src, 0);
  CV_Assert(src.type() == CV_8UC4);
  CV_Assert(src.isContinuous());

  GLuint texture = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, src.cols, src.rows, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, src.data);
  const GLint minFilter = minFilterOf(filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minFilter);
  // GL_LINEAR_MIPMAP_LINEAR is not a legal MAG filter: magnification never
  // blends mip levels. Mapping every non-nearest choice to GL_LINEAR keeps a
  // "mipmap" preset from raising GL_INVALID_ENUM and leaving stale state.
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                  minFilter == GL_NEAREST ? GL_NEAREST : GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapModeOf(wrap));
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapModeOf(wrap));
  if (minFilter == GL_LINEAR_MIPMAP_LINEAR)
    glGenerateMipmap(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, 0);
  return texture;
}

GLuint createRenderTarget(cv::Size size, GLuint &textureOut) {
  size.width = std::max(size.width, 1);
  size.height = std::max(size.height, 1);
  glGenTextures(1, &textureOut);
  glBindTexture(GL_TEXTURE_2D, textureOut);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size.width, size.height, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  GLuint fbo = 0;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         textureOut, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
  return fbo;
}

// ---------------------------------------------------------------------------
// The Shadertoy keyboard texture
// ---------------------------------------------------------------------------
// Shadertoy's "Input - Keyboard" texture is 256 texels wide and three rows
// tall. A texel is addressed by its legacy key code (the ASCII value of the
// printable character, or the old virtual-key code for everything else) and the
// row selects the meaning:
//
//   row 0 - held: set while the key is down
//   row 1 - press: a one frame pulse, cleared again right after
//   row 2 - toggle: flipped on every press
//
// This is the layout the official reference shader lsXGzf documents and reads
// with texelFetch(iChannel0, ivec2(keyCode, row), 0), so it is what shaders
// that support keyboard input expect. All four channels get the same value so
// that shaders sampling .rgb see the key too.

constexpr int keyboardTextureWidth = 256;
constexpr int keyboardTextureHeight = 3;
constexpr int keyboardRowHeld = 0;
constexpr int keyboardRowPressed = 1;
constexpr int keyboardRowToggled = 2;

// Shadertoy's key code for `key`, or -1 when the key has no cell.
int shadertoyKeyCode(Keyboard::Key key) {
  if (key >= Keyboard::A && key <= Keyboard::Z)
    return 'A' + int(key - Keyboard::A);
  if (key >= Keyboard::N0 && key <= Keyboard::N9)
    return '0' + int(key - Keyboard::N0);
  if (key >= Keyboard::KP_0 && key <= Keyboard::KP_9)
    return 96 + int(key - Keyboard::KP_0); // the numpad has no ASCII of its own
  switch (key) {
  case Keyboard::SPACE:
    return 32;
  case Keyboard::ENTER:
  case Keyboard::KP_ENTER:
    return 13;
  case Keyboard::KP_EQUAL:
  case Keyboard::EQUAL:
    return '=';
  case Keyboard::BACKSPACE:
    return 8;
  case Keyboard::TAB:
    return 9;
  case Keyboard::ESCAPE:
    return 27;
  case Keyboard::UP:
    return 38;
  case Keyboard::DOWN:
    return 40;
  case Keyboard::LEFT:
    return 37;
  case Keyboard::RIGHT:
    return 39;
  case Keyboard::HOME:
    return 36;
  case Keyboard::END:
    return 35;
  case Keyboard::PAGE_UP:
    return 33;
  case Keyboard::PAGE_DOWN:
    return 34;
  case Keyboard::INSERT:
    return 45;
  case Keyboard::DELETE:
    return 46;
  case Keyboard::APOSTROPHE:
    return '\'';
  case Keyboard::COMMA:
    return ',';
  case Keyboard::MINUS:
  case Keyboard::KP_SUBTRACT:
    return '-';
  case Keyboard::PERIOD:
  case Keyboard::KP_DECIMAL:
    return '.';
  case Keyboard::SLASH:
  case Keyboard::KP_DIVIDE:
    return '/';
  case Keyboard::SEMICOLON:
    return ';';
  case Keyboard::LEFT_BRACKET:
    return '[';
  case Keyboard::BACKSLASH:
    return '\\';
  case Keyboard::RIGHT_BRACKET:
    return ']';
  case Keyboard::GRAVE_ACCENT:
    return '`';
  case Keyboard::CAPS_LOCK:
    return 20;
  case Keyboard::SCROLL_LOCK:
    return 145;
  case Keyboard::NUM_LOCK:
    return 144;
  case Keyboard::PRINT_SCREEN:
    return 44;
  case Keyboard::PAUSE:
    return 19;
  case Keyboard::LEFT_SHIFT:
  case Keyboard::RIGHT_SHIFT:
    return 16;
  case Keyboard::LEFT_CONTROL:
  case Keyboard::RIGHT_CONTROL:
    return 17;
  case Keyboard::LEFT_ALT:
  case Keyboard::RIGHT_ALT:
    return 18;
  case Keyboard::LEFT_SUPER:
  case Keyboard::RIGHT_SUPER:
    return 91;
  case Keyboard::MENU:
    return 93;
  default:
    break;
  }
  if (key >= Keyboard::F1 && key <= Keyboard::F12)
    return 112 + int(key - Keyboard::F1);
  return -1;
}

/// Writes one texel of the 256x3 iChannelKeyboard texture.
inline void putTexel(cv::Mat &keyboard, int row, int code, int value) {
  if (keyboard.empty() || code < 0 || code >= keyboardTextureWidth)
    return;
  keyboard(cv::Rect(code, row, 1, 1)).setTo(cv::Scalar::all(value));
}

inline void keyPress(cv::Mat &keyboard, Keyboard::Key key) {
  const int code = shadertoyKeyCode(key);
  if (code < 0)
    return;
  putTexel(keyboard, keyboardRowHeld, code, 255);
  putTexel(keyboard, keyboardRowPressed, code, 255);
  putTexel(keyboard, keyboardRowToggled, code,
           keyboard.at<cv::Vec4b>(keyboardRowToggled, code)[0] != 0 ? 0 : 255);
}

inline void keyRelease(cv::Mat &keyboard, Keyboard::Key key) {
  const int code = shadertoyKeyCode(key);
  if (code < 0)
    return;
  putTexel(keyboard, keyboardRowHeld, code, 0);
  // The pressed row is a one frame pulse that the frame loop clears, and the
  // toggle row survives the release.
}

// ---------------------------------------------------------------------------
// The renderer
// ---------------------------------------------------------------------------

/// Owns every GL resource of the editor and runs a Shadertoy shader.
class ShadertoyRenderer {
  // Declared up front: a nested type has to be known before it can appear in
  // the parameter list of a member that is declared above its definition.
  struct GpuPass {
    std::string name_;       // for the pass selector
    std::string shaderName_; // the name Shadertoy's channel inputs use
    bool isBuffer_ = false;
    int buffer_ = -1;   // index into ShadertoyRenderer::buffers_
    size_t source_ = 0; // index into ShadertoyRenderer::shaderPasses_
    GLuint program_ = 0;
    GLint iResolution_ = -1;
    GLint iTime_ = -1;
    GLint iTimeDelta_ = -1;
    GLint iFrame_ = -1;
    GLint iFrameRate_ = -1;
    GLint iMouse_ = -1;
    GLint iDate_ = -1;
    GLint iSampleRate_ = -1;
    GLint iChannelTime_ = -1;
    GLint iChannelResolution_ = -1;
    std::array<GLint, 4> iChannel_{-1, -1, -1, -1};
    GLint iKeyboard_ = -1;
  };

  struct GpuBuffer {
    std::array<GLuint, 2> texture_{{0, 0}};
    std::array<GLuint, 2> fbo_{{0, 0}};
  };

public:
  /// What the shader is fed with for one frame. All sizes are in pixels of
  /// `target`, which is what iResolution reports.
  struct Frame {
    cv::Size target{1, 1};         // render target size == iResolution
    cv::Rect canvas;               // canvas inside the window, window pixels
    double time = 0.0;             // iTime
    float timeDelta = 0.0f;        // iTimeDelta
    int frame = 0;                 // iFrame
    float frameRate = 60.0f;       // iFrameRate
    cv::Vec4f mouse{0, 0, 0, 0};   // iMouse, y already flipped to Shadertoy's
    cv::Vec4f date{1970, 1, 1, 0}; // iDate: year, month, day, seconds today
    float sampleRate = 44100.0f;   // iSampleRate
    std::array<float, 4> channelTime{{0, 0, 0, 0}};
    // iChannelResolution wants a flat vec3[4], which is what a std::array of
    // 12 floats is; cv::Vec3f would need a copy per frame.
    std::array<float, 12> channelResolution{{0}};
  };

  // ---- setup -------------------------------------------------------------

  bool init(std::string &error) {
    // A single quad, attribute 0, shared by every program.
    static const float vertices[12] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f,
                                       -1.0f, 1.0f,  1.0f, -1.0f, 1.0f,  1.0f};
    glGenVertexArrays(1, &quadVao_);
    glBindVertexArray(quadVao_);
    glGenBuffers(1, &quadVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);

    blitProgram_ = linkProgram(kBlitVert.c_str(), kBlitFrag.c_str(), error);
    if (blitProgram_ == 0)
      return false;
    blitTexture_ = uniformLocation(blitProgram_, "uTex");
    blitMin_ = uniformLocation(blitProgram_, "uMin");
    blitMax_ = uniformLocation(blitProgram_, "uMax");
    blitBackground_ = uniformLocation(blitProgram_, "uBackground");

    // A 1x1 black texture stands in for channels the shader asks for but the
    // editor cannot provide (cubemaps, sound, missing texture files).
    const cv::Mat black(1, 1, CV_8UC4, cv::Scalar(0, 0, 0, 255));
    blackTexture_ = createTexture(black, "nearest", "clamp", false);
    // The keyboard texture is read with texelFetch, so it must not be filtered
    // or flipped. Row 0 (held) starts cleared, row 1 (pressed) is rewritten
    // every frame by the plan.
    keyboardTexture_ =
        createTexture(cv::Mat(keyboardTextureHeight, keyboardTextureWidth,
                              CV_8UC4, cv::Scalar(0, 0, 0, 255)),
                      "nearest", "clamp", false);
    return true;
  }

  void destroy() {
    unload();
    for (GLuint texture : {blackTexture_, keyboardTexture_})
      if (texture != 0)
        glDeleteTextures(1, &texture);
    blackTexture_ = keyboardTexture_ = 0;
    if (blitProgram_ != 0)
      glDeleteProgram(blitProgram_);
    blitProgram_ = 0;
    if (quadVbo_ != 0)
      glDeleteBuffers(1, &quadVbo_);
    if (quadVao_ != 0)
      glDeleteVertexArrays(1, &quadVao_);
    quadVbo_ = quadVao_ = 0;
  }

  // ---- shader ------------------------------------------------------------

  /// One texture a pass input refers to, decoded and ready to upload. `src` is
  /// the pass input's `src`, which is how a pass finds its texture again.
  struct TextureInput {
    std::string src;
    cv::Mat rgba; // CV_8UC4, continuous
    std::string filter = "linear";
    std::string wrap = "clamp";
  };

  bool hasShader() const { return !passes_.empty(); }

  /// Names of the passes that can be put on screen: image passes first, then
  /// the buffers, each in shader order. The index in this list is what
  /// render() and grab() take as `displayIndex`.
  const std::vector<std::string> &displayablePasses() const {
    return displayNames_;
  }

  /// Compiles `shader` and uploads `textures`. The previous shader keeps
  /// playing when this returns false - a shader that does not compile should
  /// not blank the window.
  ///
  /// `error` is the whole story in one string (for a status line or a log);
  /// `errors` is the same failure resolved to pass/line pairs, which is what an
  /// editor needs to put the caret in the right place.
  bool load(const shadertoy::Shader &shader,
            const std::vector<TextureInput> &textures,
            std::vector<CompileError> &errors, std::string &error) {
    errors.clear();
    // Compile into scratch objects first: nothing is committed until every
    // pass built.
    std::vector<GpuPass> compiled;
    // iChannel0..3 of every pass, by pass index. Buffer inputs name their
    // producer pass ("Buffer A", ...); pass names are not unique in general -
    // the image pass is usually unnamed - so the map is only used for buffer
    // inputs and everything else is keyed by the input's `src` path.
    std::map<std::string, size_t> bufferByName;
    int bufferIndex = 0;
    for (size_t i = 0; i < shader.passes.size(); ++i) {
      const auto &pass = shader.passes[i];
      if (pass.type != "image" && pass.type != "buffer")
        continue;

      GpuPass gpu;
      gpu.source_ = i;
      gpu.isBuffer_ = (pass.type == "buffer");
      gpu.name_ = pass.name.empty() ? ("pass " + std::to_string(i)) : pass.name;
      gpu.shaderName_ = pass.name;
      std::string passLog;
      const PassSource source = buildPassSource(shader.passes, int(i));
      gpu.program_ = linkProgram(kQuadVert.c_str(), source.text, passLog);
      if (gpu.program_ == 0) {
        CompileError compileError =
            parseCompileError(passLog, shader.passes, source);
        if (compileError.pass < 0)
          compileError.pass = int(i);
        errors.push_back(compileError);
        error = describe(compileError, gpu.name_);
        for (auto &done : compiled)
          glDeleteProgram(done.program_);
        return false;
      }
      if (gpu.isBuffer_) {
        gpu.buffer_ = bufferIndex++;
        // Shadertoy refers to a buffer pass by "Name.0" in a pass input, so it
        // is registered under both spellings - otherwise every buffer channel
        // silently falls back to black.
        if (!gpu.shaderName_.empty()) {
          bufferByName.emplace(gpu.shaderName_, compiled.size());
          bufferByName.emplace(gpu.shaderName_ + ".0", compiled.size());
        }
      }
      fetchUniforms(gpu);
      compiled.push_back(gpu);
    }
    if (compiled.empty()) {
      error = "the shader has no renderable pass";
      return false;
    }

    // The display list is image passes first, buffers after, each in shader
    // order: the image pass is what the window is expected to show by default,
    // while passes_ itself stays in shader order so the buffers render before
    // the image that reads them. render() and grab() index through the map.
    std::vector<std::string> displayNames;
    std::vector<int> displayToPass;
    for (int wantBuffer = 0; wantBuffer <= 1; ++wantBuffer)
      for (size_t i = 0; i < compiled.size(); ++i)
        if ((compiled[i].isBuffer_ ? 1 : 0) == wantBuffer) {
          displayNames.push_back(
              compiled[i].name_ + (compiled[i].isBuffer_ ? " (buffer)" : ""));
          displayToPass.push_back(int(i));
        }

    // Upload the texture inputs. One that is missing or could not be decoded
    // becomes a black texture; the shader still runs, it just misses that
    // input.
    std::map<std::string, GLuint> textureIds;
    std::map<GLuint, cv::Size> textureSizesById;
    for (const auto &input : textures) {
      if (input.rgba.empty() || input.rgba.type() != CV_8UC4 ||
          !input.rgba.isContinuous())
        continue;
      const GLuint texture =
          createTexture(input.rgba, input.filter, input.wrap, false);
      if (texture == 0)
        continue;
      textureIds.emplace(input.src, texture);
      textureSizesById.emplace(texture,
                               cv::Size(input.rgba.cols, input.rgba.rows));
    }

    // All good - throw the old one away.
    unload();
    passes_ = std::move(compiled);
    displayNames_ = std::move(displayNames);
    displayToPass_ = std::move(displayToPass);
    bufferByName_ = std::move(bufferByName);
    shaderPasses_ = shader.passes;
    textureIds_ = std::move(textureIds);
    textureSizesById_ = std::move(textureSizesById);
    framePhase_ = 0;
    shaderId_ = shader.id;
    // The new shader starts at t = 0, like on Shadertoy.
    return true;
  }

  void unload() {
    for (auto &pass : passes_)
      if (pass.program_ != 0)
        glDeleteProgram(pass.program_);
    passes_.clear();
    displayNames_.clear();
    displayToPass_.clear();
    bufferByName_.clear();
    shaderPasses_.clear();
    for (auto &buffer : buffers_) {
      for (GLuint fbo : buffer.fbo_)
        if (fbo != 0)
          glDeleteFramebuffers(1, &fbo);
      for (GLuint texture : buffer.texture_)
        if (texture != 0)
          glDeleteTextures(1, &texture);
    }
    buffers_.clear();
    if (finalFbo_ != 0)
      glDeleteFramebuffers(1, &finalFbo_);
    if (finalTexture_ != 0)
      glDeleteTextures(1, &finalTexture_);
    finalFbo_ = finalTexture_ = 0;
    targetSize_ = cv::Size();
    for (auto &entry : textureIds_)
      glDeleteTextures(1, &entry.second);
    textureIds_.clear();
    textureSizesById_.clear();
    shaderId_.clear();
  }

  const std::string &shaderId() const { return shaderId_; }

  // ---- grabbing a frame ---------------------------------------------------

  /// Reads the pixels of pass `displayIndex` (the same one render() shows) into
  /// a BGRA8 Mat, so a still of the shader can be written to disk. Must be
  /// called on the GL context, after the frame was rendered.
  bool grab(int displayIndex, cv::Mat &out) {
    if (displayIndex < 0 || size_t(displayIndex) >= displayToPass_.size())
      return false;
    const GpuPass &pass = passes_[size_t(displayToPass_[size_t(displayIndex)])];
    if (pass.isBuffer_ && buffers_.empty())
      return false;
    const GLuint fbo =
        pass.isBuffer_
            ? buffers_[size_t(pass.buffer_)].fbo_[size_t(framePhase_)]
            : finalFbo_;
    if (fbo == 0 || targetSize_.width < 1 || targetSize_.height < 1)
      return false;

    GLint previousFramebuffer = 0;
    GLint previousViewport[4] = {0, 0, 0, 0};
    GLint previousPackAlignment = 4;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    glGetIntegerv(GL_PACK_ALIGNMENT, &previousPackAlignment);

    cv::Mat flipped(targetSize_.height, targetSize_.width, CV_8UC4);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    // GL reads bottom-up, an image file is top-down.
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, targetSize_.width, targetSize_.height, GL_RGBA,
                 GL_UNSIGNED_BYTE, flipped.data);
    cv::flip(flipped, out, 0);
    // GL handed out RGBA, cv::imwrite wants BGRA for a four channel image.
    if (out.channels() == 4)
      cv::cvtColor(out, out, cv::COLOR_RGBA2BGRA);

    glPixelStorei(GL_PACK_ALIGNMENT, previousPackAlignment);
    glBindFramebuffer(GL_FRAMEBUFFER, GLuint(previousFramebuffer));
    glViewport(previousViewport[0], previousViewport[1], previousViewport[2],
               previousViewport[3]);
    return true;
  }

  // ---- keyboard texture --------------------------------------------------

  /// `image` is CV_8UC4, keyboardTextureWidth x keyboardTextureHeight, with row
  /// 0 at v = 0 exactly as the shaders index it.
  void setKeyboardTexture(const cv::Mat &image) {
    if (keyboardTexture_ == 0 || image.empty())
      return;
    if (image.type() != CV_8UC4 || image.cols != keyboardTextureWidth ||
        image.rows != keyboardTextureHeight || !image.isContinuous()) {
      // Better to leave the old texture alone than to upload garbage; the shape
      // mismatch is a bug in the caller and cannot happen from the graph.
      return;
    }
    glBindTexture(GL_TEXTURE_2D, keyboardTexture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, image.cols, image.rows, GL_RGBA,
                    GL_UNSIGNED_BYTE, image.data);
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  // ---- per frame ---------------------------------------------------------

  /// Resolves iChannel0..3 of `pass` and records what landed on each unit, so
  /// the caller can fill iChannelResolution from channelResolution().
  GLuint resolveChannel(const GpuPass &pass, int channel, int phase) const {
    if (pass.source_ < shaderPasses_.size()) {
      for (const auto &input : shaderPasses_[pass.source_].inputs) {
        if (input.channel != channel)
          continue;
        if (input.ctype == "buffer") {
          auto it = bufferByName_.find(input.src);
          // "Buffer" and "Buffer.0" both name the same pass; accept either.
          if (it == bufferByName_.end() && input.src.size() > 2 &&
              input.src.compare(input.src.size() - 2, 2, ".0") == 0)
            it = bufferByName_.find(input.src.substr(0, input.src.size() - 2));
          if (it != bufferByName_.end())
            return textureOfPass(passes_[it->second], phase);
        } else if (input.ctype == "keyboard") {
          return keyboardTexture_;
        } else if (input.ctype == "texture") {
          const auto it = textureIds_.find(input.src);
          if (it != textureIds_.end())
            return it->second;
        }
        // "cubemap", "sound", "video", "webcam" and unresolvable presets fall
        // back to the 1x1 black texture.
        break;
      }
    }
    return blackTexture_;
  }

  /// True when unit `channel` of `pass` is fed by a ping-pong buffer pass,
  /// which is what iChannelTime should report the frame of.
  bool channelIsBuffer(const GpuPass &pass, int channel) const {
    if (pass.source_ >= shaderPasses_.size())
      return false;
    for (const auto &input : shaderPasses_[pass.source_].inputs)
      if (input.channel == channel)
        return input.ctype == "buffer";
    return false;
  }

  /// iChannelResolution and iChannelTime describe the pass that is being shown,
  /// so they are filled here - from the bindings that pass will get this frame
  /// - and reused by every pass. `frame` is taken by reference for that.
  void render(Frame &frame, int displayIndex) {
    if (passes_.empty())
      return;
    const cv::Size target = frame.target;
    if (target.width < 1 || target.height < 1) {
      frame.channelResolution.fill(0.0f);
      return;
    }
    const bool showSomething =
        displayIndex >= 0 && displayIndex < int(displayToPass_.size());
    const GpuPass *shown =
        showSomething
            ? &passes_[size_t(displayToPass_[size_t(displayIndex)])]
            : nullptr;

    // The render targets must exist before a channel can be resolved, because
    // resolving a buffer input yields one of them.
    ensureBuffers(target);
    framePhase_ ^= 1;

    // A buffer pass reads the target the previous frame wrote, i.e. the other
    // phase than the one it writes this frame. framePhase_ has just been
    // flipped, so the phase to read is framePhase_ ^ 1.
    if (shown != nullptr) {
      for (int channel = 0; channel < 4; ++channel) {
        const cv::Size size =
            sizeOfTexture(resolveChannel(*shown, channel, framePhase_ ^ 1));
        const cv::Size clamped(std::max(size.width, 1),
                               std::max(size.height, 1));
        frame.channelResolution[size_t(3 * channel + 0)] = float(clamped.width);
        frame.channelResolution[size_t(3 * channel + 1)] =
            float(clamped.height);
        frame.channelResolution[size_t(3 * channel + 2)] = 1.0f;
        // iChannelTime is the frame count of the producer, which for a texture
        // is the time the shader has been running.
        frame.channelTime[size_t(channel)] = channelIsBuffer(*shown, channel)
                                                 ? float(frame.frame)
                                                 : float(frame.time);
      }
    }

    // V4D hands out a worker context with scissor test and blend enabled for
    // its own drawing; Shadertoy passes are opaque and must not be clipped.
    const GLboolean scissorWasEnabled = glIsEnabled(GL_SCISSOR_TEST);
    const GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
    GLint previousFramebuffer = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousFramebuffer);
    GLint previousViewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, previousViewport);

    // Diagnostics: std::cerr never reaches logcat on Android, so the first
    // frames and then every 120th go through the OpenCV logger instead. The
    // entry drain also shows whether earlier nodes left GL errors behind.
    static int stDbgRender = 0;
    const bool stDbg = stDbgRender < 4 || (stDbgRender % 120) == 0;
    ++stDbgRender;
    const GLenum stErrIn = glGetError();
    if (stDbg)
      CV_LOG_INFO(&cv::v4d::v4d_tag,
                  "STDBG render #" << stDbgRender << " shown="
                                   << (shown ? shown->name_ : std::string("-"))
                                   << " target=" << target.width << "x"
                                   << target.height << " canvas=("
                                   << frame.canvas.x << "," << frame.canvas.y
                                   << " " << frame.canvas.width << "x"
                                   << frame.canvas.height << ") prevFbo="
                                   << previousFramebuffer << " prevVp=["
                                   << previousViewport[0] << ","
                                   << previousViewport[1] << ","
                                   << previousViewport[2] << ","
                                   << previousViewport[3]
                                   << "] blitProg=" << blitProgram_
                                   << " errIn=0x" << std::hex << stErrIn
                                   << std::dec);

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    glBindVertexArray(quadVao_);
    for (auto &pass : passes_) {
      const GLuint fbo =
          pass.isBuffer_
              ? buffers_[size_t(pass.buffer_)].fbo_[size_t(framePhase_)]
              : finalFbo_;
      glBindFramebuffer(GL_FRAMEBUFFER, fbo);
      glViewport(0, 0, target.width, target.height);
      glUseProgram(pass.program_);

      if (pass.iResolution_ >= 0)
        glUniform3f(pass.iResolution_, float(target.width),
                    float(target.height), 1.0f);
      if (pass.iTime_ >= 0)
        glUniform1f(pass.iTime_, float(frame.time));
      if (pass.iTimeDelta_ >= 0)
        glUniform1f(pass.iTimeDelta_, frame.timeDelta);
      if (pass.iFrame_ >= 0)
        glUniform1i(pass.iFrame_, frame.frame);
      if (pass.iFrameRate_ >= 0)
        glUniform1f(pass.iFrameRate_, frame.frameRate);
      if (pass.iMouse_ >= 0)
        glUniform4f(pass.iMouse_, frame.mouse[0], frame.mouse[1],
                    frame.mouse[2], frame.mouse[3]);
      if (pass.iDate_ >= 0)
        glUniform4f(pass.iDate_, frame.date[0], frame.date[1], frame.date[2],
                    frame.date[3]);
      if (pass.iSampleRate_ >= 0)
        glUniform1f(pass.iSampleRate_, frame.sampleRate);
      if (pass.iChannelTime_ >= 0)
        glUniform1fv(pass.iChannelTime_, 4, frame.channelTime.data());
      if (pass.iChannelResolution_ >= 0)
        glUniform3fv(pass.iChannelResolution_, 4,
                     frame.channelResolution.data());

      // Channel i always lives on texture unit i, and a pass reading its own
      // output would otherwise sample the target it is drawing into.
      bindChannels(pass, pass.isBuffer_ ? (framePhase_ ^ 1) : framePhase_);
      glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    glBindVertexArray(0);

    // ---- show the selected pass on the canvas ----------------------------
    if (shown != nullptr) {
      glBindFramebuffer(GL_FRAMEBUFFER, GLuint(previousFramebuffer));
      glViewport(previousViewport[0], previousViewport[1], previousViewport[2],
                 previousViewport[3]);
      glUseProgram(blitProgram_);
      if (blitTexture_ >= 0)
        glUniform1i(blitTexture_, 0);
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D,
                    textureOfPass(*shown, shown->isBuffer_ ? (framePhase_ ^ 1)
                                                           : framePhase_));
      // The canvas is given in window pixels with y pointing down; vUV points
      // up, so flip it here.
      const cv::Size window(std::max(previousViewport[2], 1),
                            std::max(previousViewport[3], 1));
      const float u0 = float(frame.canvas.x) / float(window.width);
      const float u1 =
          float(frame.canvas.x + frame.canvas.width) / float(window.width);
      const float v0 = 1.0f - float(frame.canvas.y + frame.canvas.height) /
                                  float(window.height);
      const float v1 = 1.0f - float(frame.canvas.y) / float(window.height);
      glUniform2f(blitMin_, std::min(u0, u1), std::min(v0, v1));
      glUniform2f(blitMax_, std::max(u0, u1), std::max(v0, v1));
      glUniform4f(blitBackground_, 0.07f, 0.07f, 0.08f, 1.0f);
      glBindVertexArray(quadVao_);
      glDrawArrays(GL_TRIANGLES, 0, 6);
      glBindVertexArray(0);
      glBindTexture(GL_TEXTURE_2D, 0);
      if (stDbg) {
        // Did the blit actually write into the framebuffer the display thread
        // presents? Probe three points: inside the ImGui panel zone, below the
        // panel outside the canvas (expects uBackground ~18,18,20) and the
        // canvas sliver on the right (expects shader pixels). Screen-style
        // coordinates (top-left origin) are converted to GL's bottom-left.
        const GLenum stErrDraw = glGetError();
        const int vh = std::max(previousViewport[3], 1);
        auto probe = [&](int sx, int sy) {
          unsigned char px[4] = {0, 0, 0, 0};
          glReadPixels(std::max(sx, 0), vh - sy, 1, 1, GL_RGBA,
                       GL_UNSIGNED_BYTE, px);
          return std::string("(") + std::to_string(int(px[0])) + "," +
                 std::to_string(int(px[1])) + "," +
                 std::to_string(int(px[2])) + ")";
        };
        const std::string stPanelPx = probe(100, 300);
        const std::string stBelowPx = probe(100, 1000);
        const std::string stCanvasPx = probe(640, 1000);
        const GLenum stErrRead = glGetError();
        CV_LOG_INFO(&cv::v4d::v4d_tag,
                    "STDBG blit drawErr=0x" << std::hex << stErrDraw
                                            << " panelPx=" << stPanelPx
                                            << " belowPx=" << stBelowPx
                                            << " canvasPx=" << stCanvasPx
                                            << " readErr=0x" << stErrRead
                                            << std::dec);
      }
    }

    glUseProgram(0);
    if (scissorWasEnabled == GL_TRUE)
      glEnable(GL_SCISSOR_TEST);
    else
      glDisable(GL_SCISSOR_TEST);
    if (blendWasEnabled == GL_TRUE)
      glEnable(GL_BLEND);
    else
      glDisable(GL_BLEND);
  }

private:
  static GLint uniformLocation(GLuint program, const char *name) {
    return glGetUniformLocation(program, name);
  }

  /// The size of whatever resolveChannel() put on a texture unit. A texture the
  /// editor did not upload is 1x1 (the black texture); a render target is the
  /// size iResolution reports.
  cv::Size sizeOfTexture(GLuint texture) const {
    if (texture == 0 || texture == blackTexture_)
      return cv::Size(1, 1);
    if (texture == keyboardTexture_)
      return cv::Size(keyboardTextureWidth, keyboardTextureHeight);
    const auto it = textureSizesById_.find(texture);
    if (it != textureSizesById_.end())
      return it->second;
    return targetSize_;
  }

  /// Sampler uniforms are per program state, so they can only be set while the
  /// program is in use - hence this runs from render() and not from load().
  void bindChannels(const GpuPass &pass, int phase) {
    for (int channel = 0; channel < 4; ++channel) {
      if (pass.iChannel_[size_t(channel)] >= 0)
        glUniform1i(pass.iChannel_[size_t(channel)], channel);
      glActiveTexture(GL_TEXTURE0 + GLenum(channel));
      glBindTexture(GL_TEXTURE_2D, resolveChannel(pass, channel, phase));
    }
    // iChannelKeyboard is Shadertoy's own name for the keyboard, but the
    // texture still occupies the channel the pass input asked for.
    const int keyboard = keyboardChannelOf(pass);
    if (pass.iKeyboard_ >= 0 && keyboard >= 0)
      glUniform1i(pass.iKeyboard_, keyboard);
  }

  /// The channel a pass reads the keyboard texture from, or -1.
  int keyboardChannelOf(const GpuPass &pass) const {
    if (pass.source_ >= shaderPasses_.size())
      return -1;
    for (const auto &input : shaderPasses_[pass.source_].inputs)
      if (input.ctype == "keyboard")
        return input.channel;
    return -1;
  }

  void fetchUniforms(GpuPass &pass) {
    pass.iResolution_ = uniformLocation(pass.program_, "iResolution");
    pass.iTime_ = uniformLocation(pass.program_, "iTime");
    pass.iTimeDelta_ = uniformLocation(pass.program_, "iTimeDelta");
    pass.iFrame_ = uniformLocation(pass.program_, "iFrame");
    pass.iFrameRate_ = uniformLocation(pass.program_, "iFrameRate");
    pass.iMouse_ = uniformLocation(pass.program_, "iMouse");
    pass.iDate_ = uniformLocation(pass.program_, "iDate");
    pass.iSampleRate_ = uniformLocation(pass.program_, "iSampleRate");
    pass.iChannelTime_ = uniformLocation(pass.program_, "iChannelTime");
    pass.iChannelResolution_ =
        uniformLocation(pass.program_, "iChannelResolution");
    for (int i = 0; i < 4; ++i) {
      const std::string name = "iChannel" + std::to_string(i);
      pass.iChannel_[size_t(i)] = uniformLocation(pass.program_, name.c_str());
    }
    pass.iKeyboard_ = uniformLocation(pass.program_, "iChannelKeyboard");
  }

  /// (Re)creates the ping-pong targets of every buffer pass plus the target of
  /// the image pass when the requested size changed.
  void ensureBuffers(const cv::Size &size) {
    if (size == targetSize_)
      return;
    targetSize_ = size;
    // Drop the old allocations first so the count of buffer passes can change.
    for (auto &buffer : buffers_) {
      for (GLuint fbo : buffer.fbo_)
        if (fbo != 0)
          glDeleteFramebuffers(1, &fbo);
      for (GLuint texture : buffer.texture_)
        if (texture != 0)
          glDeleteTextures(1, &texture);
    }
    buffers_.clear();
    if (finalFbo_ != 0)
      glDeleteFramebuffers(1, &finalFbo_);
    if (finalTexture_ != 0)
      glDeleteTextures(1, &finalTexture_);
    finalFbo_ = finalTexture_ = 0;

    for (const auto &pass : passes_) {
      if (!pass.isBuffer_)
        continue;
      GpuBuffer buffer;
      for (int phase = 0; phase < 2; ++phase)
        buffer.fbo_[size_t(phase)] =
            createRenderTarget(size, buffer.texture_[size_t(phase)]);
      buffers_.push_back(buffer);
    }
    finalFbo_ = createRenderTarget(size, finalTexture_);
  }

  GLuint textureOfPass(const GpuPass &pass, int phase) const {
    if (pass.isBuffer_)
      return buffers_[size_t(pass.buffer_)].texture_[size_t(phase)];
    return finalTexture_;
  }

  // GL objects
  GLuint quadVao_ = 0;
  GLuint quadVbo_ = 0;
  GLuint blitProgram_ = 0;
  GLint blitTexture_ = -1, blitMin_ = -1, blitMax_ = -1, blitBackground_ = -1;
  GLuint blackTexture_ = 0;
  GLuint keyboardTexture_ = 0;

  // The shader
  std::vector<GpuPass> passes_;
  std::vector<std::string> displayNames_;
  /// display list index -> index into passes_ (image passes come first).
  std::vector<int> displayToPass_;
  std::vector<shadertoy::Pass> shaderPasses_; // CPU side, for input lookup
  std::map<std::string, size_t> bufferByName_;
  std::map<std::string, GLuint> textureIds_;
  std::map<GLuint, cv::Size> textureSizesById_;
  std::vector<GpuBuffer> buffers_;
  GLuint finalTexture_ = 0;
  GLuint finalFbo_ = 0;
  cv::Size targetSize_;
  std::string shaderId_;
  int framePhase_ = 0;
};

} // namespace shadertoy

#endif // MODULES_V4D_SAMPLES_SHADERTOY_RENDERER_HPP_
