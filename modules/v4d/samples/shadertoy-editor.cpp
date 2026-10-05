// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// A Shadertoy editor that never touches the network.
//
// A workbench for the GLSL itself. It opens and saves Shadertoy JSON (the same
// format the site exports), edits every pass in a multiline field, recompiles
// as you type, and puts the caret on the line the driver complained about. The
// rendering is the site's own model, so the uniform block, the ping pong
// buffers behind buffer passes, iChannelKeyboard and the rest behave the way a
// shader written for shadertoy.com expects.
//
// Nothing in here needs an app key, a keypress or a network route: only
// shadertoy_model.hpp (the data), shadertoy_project.hpp (the files) and
// shadertoy_renderer.hpp (the GL) are linked in.

#include "samples.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/v4d/v4d.hpp>

#include "shadertoy-editor/shadertoy_code.hpp"
#include "shadertoy-editor/shadertoy_project.hpp"
#include "shadertoy-editor/shadertoy_renderer.hpp"
#include "shadertoy-editor/shadertoy_theme.hpp"

using namespace cv;
using namespace cv::v4d;
using namespace cv::v4d::event;
using namespace shadertoy;

// ---------------------------------------------------------------------------
// The exit code of --verify, read by main()
// ---------------------------------------------------------------------------

static int verifyExitCode = 0;

// ---------------------------------------------------------------------------
// Built-in projects
// ---------------------------------------------------------------------------

namespace {

struct BuiltIn {
  const char *name;
  Shader shader;
};

/// The samples the editor starts from when no file is given. They are here so
/// that a fresh window shows something that moves, and so that the editor is
/// useful without a project file at hand. Each one exercises a different part
/// of the renderer: a plain image pass, a feedback buffer chain, the keyboard
/// texture, and a common block prepended to a pass.
std::vector<BuiltIn> builtIns() {
  std::vector<BuiltIn> out;

  {
    BuiltIn b;
    b.name = "Gradient";
    b.shader.name = "Gradient";
    b.shader.author = "Plan-V4D";
    Pass pass;
    pass.type = "image";
    pass.name = "Image";
    pass.code = "void mainImage(out vec4 fragColor, in vec2 fragCoord)\n"
                "{\n"
                "    vec2 uv = fragCoord / iResolution.xy;\n"
                "    fragColor = vec4(uv, 0.5 + 0.5 * sin(iTime), 1.0);\n"
                "}\n";
    pass.outputs = {{0, 0}};
    b.shader.passes.push_back(pass);
    out.push_back(std::move(b));
  }

  {
    BuiltIn b;
    b.name = "Feedback (buffer pass)";
    b.shader.name = "Feedback";
    b.shader.author = "Plan-V4D";
    Pass buffer;
    buffer.type = "buffer";
    buffer.name = "Buffer";
    buffer.code = "// A buffer pass writes its own output; reading it back "
                  "next frame is\n"
                  "// the feedback loop. Note iChannel0 is this buffer, one "
                  "phase behind.\n"
                  "void mainImage(out vec4 fragColor, in vec2 fragCoord)\n"
                  "{\n"
                  "    vec2 uv = fragCoord / iResolution.xy;\n"
                  "    vec2 p = uv - 0.5;\n"
                  "    vec3 prev = texture2D(iChannel0, uv - 0.004 * "
                  "vec2(sin(iTime), cos(iTime))).rgb;\n"
                  "    fragColor = vec4(0.5 * prev + 0.25 * vec3(uv.x, uv.y, "
                  "0.5), 1.0);\n"
                  "}\n";
    buffer.outputs = {{0, 0}};
    buffer.inputs = {{0, 0, "buffer", "Buffer.0"}};

    Pass image;
    image.type = "image";
    image.name = "Image";
    image.code =
        "void mainImage(out vec4 fragColor, in vec2 fragCoord)\n"
        "{\n"
        "    fragColor = texture2D(iChannel0, fragCoord / iResolution.xy);\n"
        "}\n";
    image.inputs = {{0, 0, "buffer", "Buffer.0"}};
    image.outputs = {{0, 0}};

    b.shader.passes = {buffer, image};
    out.push_back(std::move(b));
  }

  {
    BuiltIn b;
    b.name = "Keyboard (iChannelKeyboard)";
    b.shader.name = "Keyboard";
    b.shader.author = "Plan-V4D";
    Pass pass;
    pass.type = "image";
    pass.name = "Image";
    pass.code =
        "// iChannelKeyboard is a 256x3 texture: row 0 keys held down, row 1\n"
        "// keys pressed this frame, row 2 the toggles that have been "
        "flipped.\n"
        "void mainImage(out vec4 fragColor, in vec2 fragCoord)\n"
        "{\n"
        "    vec2 uv = fragCoord / iResolution.xy;\n"
        "    vec3 held = texture2D(iChannelKeyboard, vec2(uv.x, 0.02)).rgb;\n"
        "    vec3 pressed = texture2D(iChannelKeyboard, vec2(uv.x, 0.4)).rgb;\n"
        "    vec3 toggled = texture2D(iChannelKeyboard, vec2(uv.x, 0.8)).rgb;\n"
        "    fragColor = vec4(held * 0.7 + pressed * 0.5 + toggled * 0.25, "
        "1.0);\n"
        "}\n";
    pass.inputs = {{0, 0, "keyboard", ""}};
    pass.outputs = {{0, 0}};
    b.shader.passes.push_back(pass);
    out.push_back(std::move(b));
  }

  {
    BuiltIn b;
    b.name = "Common block + plasma";
    b.shader.name = "Common and plasma";
    b.shader.author = "Plan-V4D";
    Pass common;
    common.type = "common";
    common.name = "";
    common.code =
        "// Everything in a common block is prepended to every other pass.\n"
        "vec3 palette(float t)\n"
        "{\n"
        "    return 0.5 + 0.5 * cos(6.28318 * (t + vec3(0.0, 0.33, 0.67)));\n"
        "}\n";
    Pass pass;
    pass.type = "image";
    pass.name = "Image";
    pass.code =
        "void mainImage(out vec4 fragColor, in vec2 fragCoord)\n"
        "{\n"
        "    vec2 p = (2.0 * fragCoord - iResolution.xy) / iResolution.y;\n"
        "    float v = 0.0;\n"
        "    for (int i = 0; i < 4; ++i)\n"
        "        v += sin(p.x * float(i + 1) + iTime) *\n"
        "             cos(p.y * float(i + 2) - iTime * 0.7);\n"
        "    fragColor = vec4(palette(v * 0.1 + 0.5), 1.0);\n"
        "}\n";
    pass.outputs = {{0, 0}};
    b.shader.passes = {common, pass};
    out.push_back(std::move(b));
  }

  return out;
}

/// iChannel0 can also be a file on disk, so the editor has one image to fall
/// back on when no texture has been loaded yet.
bool loadCheckerboard(cv::Mat &rgba) {
  const int side = 64;
  rgba = cv::Mat(side, side, CV_8UC4, cv::Scalar(0, 0, 0, 255));
  for (int y = 0; y < side; ++y)
    for (int x = 0; x < side; ++x) {
      const bool on = ((x / 8) + (y / 8)) % 2 == 0;
      rgba.at<cv::Vec4b>(y, x) =
          on ? cv::Vec4b(220, 200, 160, 255) : cv::Vec4b(60, 70, 90, 255);
    }
  return true;
}

} // namespace

// ---------------------------------------------------------------------------
// The plan
// ---------------------------------------------------------------------------

class ShadertoyEditorPlan : public V4DPlan {
  using K = V4D::Keys;
  using M = Mouse::Type;

public:
  /// Export rewrites a project into the format Shadertoy itself reads, without
  /// a window: it is how the built-ins become fixtures for the tests, and how a
  /// document gets handed to somebody else's tool.
  enum class Mode { Interactive, Verify, Shot, Export };

private:
  /// Everything the GUI thread and the worker share, behind one lock.
  ///
  /// The editor mutates the document (typing, adding a pass, rewiring a
  /// channel) and the worker reads it, so a single shared object is what keeps
  /// that safe: there is no ordering to get wrong between two locks. The work
  /// that must not block the GUI - reading a file, decoding a texture,
  /// compiling - happens on a snapshot in Pending.
  struct Shared {
    // ---- what the GUI asks for -------------------------------------------
    bool load_ = false;
    bool save_ = false;
    std::string pathToOpen_;
    std::string pathToSave_;
    bool reload_ = false;
    bool compile_ = false; // compile now, ignoring the debounce
    bool resetTime_ = false;
    bool screenshot_ = false;
    int texturePass_ = 0;
    int textureChannel_ = 0;
    std::string texturePath_;

    // ---- the document ----------------------------------------------------
    Shader shader_;
    /// Local textures by their src, as the renderer wants them.
    std::map<std::string, cv::Mat> textures_;
    std::string path_;
    /// Bumped by every text change; the worker recompiles when it changes.
    int edits_ = 0;
    /// When the last edit happened, so the worker can wait for the typing to
    /// stop before it compiles.
    double lastEdit_ = 0.0;
    /// Bumped whenever the shape of the document changes, which is when the
    /// GUI has to rebuild its text buffers.
    int serial_ = 0;
    bool dirty_ = false;

    // ---- what the GUI shows ----------------------------------------------
    bool playing_ = true;
    bool showPanel_ = true;
    bool showHud_ = true;
    bool fullscreen_ = false;
    bool inputActive_ = true;
    bool wrapText_ = true;
    /// The code pane's own switches, kept beside wrapText_ so that one header
    /// lists everything that changes what the code looks like instead of
    /// splitting them across two panels.
    bool showLineNumbers_ = true;
    bool showWhitespace_ = false;
    bool autoIndent_ = true;
    bool matchingBrackets_ = true;
    bool lineFolding_ = false;
    bool minimap_ = false;
    /// Index into shadertoy::palettes(). Read only by the GUI thread, which is
    /// the thread that applies a scheme.
    int scheme_ = 0;
    float resolutionScale_ = 1.0f;
    int displayPass_ = 0;
    int selected_ = 0; // the pass tab the editor is on
    int builtIn_ = 1;

    std::string status_ = "ready";
    std::vector<CompileError> errors_;
    std::vector<std::string> passNames_;
    bool hasShader_ = false;
    bool compiling_ = false;
    double time_ = 0.0;
    float timeDelta_ = 0.0f;
    int frame_ = 0;
    float fps_ = 0.0f;
    cv::Size resolution_;
    double compileSeconds_ = 0.0;
  };

  /// Worker-local: the result of one round of file, texture or compile work.
  struct Pending {
    Shader shader_;
    std::vector<ShadertoyRenderer::TextureInput> textures_;
    std::map<std::string, cv::Mat> decoded_;
    bool compile_ = false;
    int generation_ = 0;        // edits_ the snapshot was taken at
    bool haveDocument_ = false; // a new document was read from disk
    std::string status_;
  } pending_;

  /// Worker-local: what changes every frame and must not be locked.
  struct Anim {
    double lastWallClock_ = 0.0;
    double time_ = 0.0;
    int frame_ = 0;
    cv::Vec4f mouse_{0, 0, 0, 0};
    bool mouseKnown_ = false;
    cv::Mat keyboard_;
    int framesRendered_ = 0;
    bool shotTaken_ = false;
  } anim_;

  static Shared shared_;

  Property<cv::Size> size_ = P<cv::Size>(K::SIZE);
  Property<cv::Size> windowSize_ = P<cv::Size>(K::WINDOW_SIZE);

  Event<Mouse> move_ = E<Mouse>(M::MOVE);
  Event<Mouse> press_ = E<Mouse>(M::PRESS);
  Event<Mouse> release_ = E<Mouse>(M::RELEASE);
  Event<Mouse> hoverEnter_ = E<Mouse>(M::HOVER_ENTER);
  Event<Mouse> hoverExit_ = E<Mouse>(M::HOVER_EXIT);
  Event<Keyboard> keyPress_ = E<Keyboard>(Keyboard::PRESS);
  Event<Keyboard> keyRelease_ = E<Keyboard>(Keyboard::RELEASE);

  // GUI-thread-only: one code widget per pass, created when the document's
  // shape changes and edited in place in between. A widget cannot be copied and
  // holds its own undo history, so it is held by pointer - which is also why
  // the change callback below has to name its own pass rather than capture one.
  struct CodePane {
    std::unique_ptr<TextEditor> editor;
    /// The document serial the text was last loaded from. A pass whose code is
    /// rewritten from outside the widget - undo is not the only way that can
    /// happen, but a pass reorder or a reload is - has to be pushed back into
    /// it.
    int loadedSerial = -1;
    /// The serial the error markers were last applied at, so that a compile
    /// that found the same errors again does not re-add them every frame.
    int markedSerial = -1;
    int markedPass = -1;
    int markedCount = -1;
  };
  std::vector<CodePane> panes_;
  int panesSerial_ = -1;
  /// Set when the user asks to jump to a compiler error. The widget owns the
  /// caret, so this is a request rather than a caret position.
  struct CaretRequest {
    int pass = -1;
    int line = 0;   // 1-based, as the driver and the error list report it
    int column = 0; // 1-based
  };
  bool wantCaret_ = false;
  CaretRequest caret_;
  shadertoy::Fonts fonts_;
  /// The scheme applied to the panes last frame, so a re-theme is one
  /// assignment per widget rather than one per frame.
  int appliedScheme_ = -1;

  const Mode mode_;
  const std::string shotPath_;
  const int shotFrames_;
  const std::string exportPath_;

public:
  ShadertoyEditorPlan(Mode mode, std::string projectPath, int builtIn,
                      std::string shotPath, int shotFrames,
                      std::string exportPath = std::string(),
                      bool startFullscreen = false)
      : mode_(mode), shotPath_(std::move(shotPath)), shotFrames_(shotFrames),
        exportPath_(std::move(exportPath)) {
    _shared(shared_);
    anim_.keyboard_ = cv::Mat(keyboardTextureHeight, keyboardTextureWidth,
                              CV_8UC4, cv::Scalar(0, 0, 0, 0));
    shared_.builtIn_ = builtIn;
    shared_.fullscreen_ = startFullscreen;
    // The window starts with a working shader rather than an empty document, so
    // that something is on screen before the first keystroke. The unattended
    // modes have no panel to do this for them, and they need it just as much.
    if (builtIn >= 0 && size_t(builtIn) < builtIns().size())
      shared_.shader_ = builtIns()[size_t(builtIn)].shader;
    ++shared_.serial_;
    shared_.lastEdit_ = seconds();
    if (!projectPath.empty()) {
      shared_.load_ = true;
      shared_.pathToOpen_ = std::move(projectPath);
    }
  }

  // -------------------------------------------------------------------------
  // GUI (display thread)
  // -------------------------------------------------------------------------

  void gui() override {
    imgui(
        [this](Shared &shared) {
          const auto &schemes = shadertoy::palettes();
          if (shared.scheme_ < 0 || shared.scheme_ >= int(schemes.size()))
            shared.scheme_ = 0;
          fonts_ = shadertoy::loadFonts(1.0f);
          shadertoy::applyPalette(schemes[size_t(shared.scheme_)], fonts_);
          drawPanel(shared);
        },
        RWS(shared_));
  }

  // -------------------------------------------------------------------------
  // Setup / teardown (worker threads)
  // -------------------------------------------------------------------------

  void setup() override {
    if (mode_ == Mode::Interactive) {
      set(GlobalState::Keys::SHOW_GUI, V(true));
      set(K::FULLSCREEN, CS(shared_.fullscreen_));
    }
    gl(
        [](ShadertoyRenderer &renderer) {
          std::string error;
          if (!renderer.init(error))
            std::cerr << "shadertoy-editor: " << error << std::endl;
        },
        RW(renderer_));
  }

  void teardown() override {
    gl([](ShadertoyRenderer &renderer) { renderer.destroy(); }, RW(renderer_));
  }

  // -------------------------------------------------------------------------
  // Per frame
  // -------------------------------------------------------------------------

  void infer() override {
    if (mode_ == Mode::Interactive) {
      set(K::FULLSCREEN, CS(shared_.fullscreen_));
      set(GlobalState::Keys::SHOW_GUI, CS(shared_.showPanel_));
    }

    // (1) Shortcuts. Single key shortcuts stay out of the way while ImGui owns
    //     the keyboard.
    plain(
        [this](const Keyboard::List &presses, Shared &shared) {
          if (mode_ != Mode::Interactive)
            return;
          if (imguiWantsKeyboard())
            return;
          for (const auto &key : presses) {
            if (key->is(Keyboard::SPACE))
              shared.playing_ = !shared.playing_;
            else if (key->is(Keyboard::F5))
              shared.compile_ = true;
            else if (key->is(Keyboard::F9))
              shared.save_ = true;
            else if (key->is(Keyboard::R))
              shared.resetTime_ = true;
            else if (key->is(Keyboard::TAB))
              shared.showPanel_ = !shared.showPanel_;
            else if (key->is(Keyboard::F))
              shared.fullscreen_ = !shared.fullscreen_;
            else if (key->is(Keyboard::H))
              shared.showHud_ = !shared.showHud_;
            else if (key->is(Keyboard::P))
              shared.screenshot_ = true;
          }
        },
        keyPress_, RWS(shared_));

    // (2) Files: open, save, reload, and decoding a local texture. This is the
    //     only node that touches the filesystem, so a slow disk cannot stall
    //     the panel, and the GUI thread never blocks on it.
    plain(
        [this](Shared &shared, Pending &pending) { service(shared, pending); },
        RWS(shared_), RW(pending_));

    // (3) Decide when to compile. Typing produces an edit per keystroke and
    //     compiling is not free, so the worker waits for the text to settle.
    //     A structural change (a new file, another pass) is compiled at once:
    //     there is no typing to wait for.
    plain(
        [this](Shared &shared, Pending &pending) {
          if (shared.shader_.passes.empty())
            return;
          // One compile in flight is enough; the next edit queues the next one.
          if (pending.compile_)
            return;
          const bool structural = shared.serial_ != snapshottedSerial_;
          const bool edited = shared.edits_ != pending.generation_;
          if (!structural && !edited)
            return;
          if (!structural && !shared.compile_ &&
              seconds() - shared.lastEdit_ < kDebounceSeconds)
            return;
          shared.compile_ = false;
          shared.compiling_ = true;
          snapshot(shared, pending);
          snapshottedSerial_ = shared.serial_;
        },
        RWS(shared_), RW(pending_));

    // (4) Compile on the GL thread, and publish what came out of it. A
    //     failure keeps the last good shader on screen: an editor that blanks
    //     the preview on every typo cannot be used.
    gl(
        [this](Pending &pending, Shared &shared, ShadertoyRenderer &renderer,
               Anim &anim) {
          if (!pending.compile_)
            return;
          pending.compile_ = false;

          std::vector<CompileError> errors;
          std::string error;
          const double started = seconds();
          const bool ok =
              renderer.load(pending.shader_, pending.textures_, errors, error);
          shared.compileSeconds_ = seconds() - started;

          shared.errors_ = std::move(errors);
          if (ok) {
            shared.passNames_ = renderer.displayablePasses();
            shared.hasShader_ = true;
            shared.compiling_ = false;
            shared.status_ =
                "compiled " + std::to_string(pending.shader_.passes.size()) +
                " pass(es) in " +
                std::to_string(int(shared.compileSeconds_ * 1000.0)) + " ms";
            // A fresh shader starts from t = 0, like the site does.
            anim.time_ = 0.0;
            anim.frame_ = 0;
            anim.mouseKnown_ = false;
          } else {
            shared.compiling_ = false;
            shared.status_ = "compile failed - " + error;
            std::cerr << "shadertoy-editor: " << shared.status_ << std::endl;
            for (const auto &e : shared.errors_)
              if (e.located())
                std::cerr << "shadertoy-editor:   pass " << e.pass << " line "
                          << e.line << ": " << e.message << std::endl;
          }

          if (mode_ == Mode::Verify) {
            verifyExitCode = ok ? 0 : 1;
            std::cout << (ok ? "ok" : "FAILED") << ": " << shared.shader_.name
                      << " (" << pending.shader_.passes.size() << " pass(es))"
                      << std::endl;
            for (const auto &e : shared.errors_) {
              std::cout << "  pass " << e.pass << " line " << e.line << ": "
                        << e.message << std::endl;
              std::cout << e.context;
            }
            V4D::instance()->requestFinish();
          } else if (mode_ == Mode::Shot && !ok) {
            // Nothing can be rendered, so the shot would never come.
            finishRun(1, "compile failed - " + error);
          }
        },
        RW(pending_), RWS(shared_), RW(renderer_), RW(anim_));

    // (5) The clock, the pointer and the keyboard texture.
    plain(
        [this](const Mouse::List &moves, const Mouse::List &hoverEnters,
               const Mouse::List &hoverExits, const Mouse::List &presses,
               const Mouse::List &releases, const Keyboard::List &keyPresses,
               const Keyboard::List &keyReleases, const cv::Size &fbSize,
               const cv::Size &winSize, Shared &shared, Anim &anim) {
          const cv::Rect canvas =
              canvasOf(cv::Size(fbSize.width, fbSize.height), shared, mode_);
          const double now = seconds();

          float dt = 0.0f;
          if (anim.lastWallClock_ != 0.0)
            dt = float(std::clamp(now - anim.lastWallClock_, 0.0, 0.1));
          anim.lastWallClock_ = now;
          if (shared.resetTime_) {
            anim.time_ = 0.0;
            anim.frame_ = 0;
            shared.resetTime_ = false;
          }
          if (shared.playing_ && mode_ != Mode::Verify) {
            anim.time_ += dt;
            ++anim.frame_;
          }
          shared.time_ = anim.time_;
          shared.timeDelta_ = dt;
          shared.frame_ = anim.frame_;
          if (dt > 0.0f) {
            const float instant = 1.0f / dt;
            shared.fps_ = (shared.fps_ <= 0.0f)
                              ? instant
                              : (0.9f * shared.fps_ + 0.1f * instant);
          }

          // -- pointer ---------------------------------------------------
          const cv::Size window(winSize.width, winSize.height);
          const double scaleX =
              window.width > 0 ? double(fbSize.width) / window.width : 1.0;
          const double scaleY =
              window.height > 0 ? double(fbSize.height) / window.height : 1.0;
          cv::Point2d cursor(-1.0, -1.0);
          bool inside = hoverExits.empty();
          for (const auto &event : hoverEnters)
            cursor = cv::Point2d(event->position().x, event->position().y);
          for (const auto &event : moves)
            cursor = cv::Point2d(event->position().x, event->position().y);
          if (inside && cursor.x >= 0.0 && shared.inputActive_) {
            const int px = int(cursor.x * scaleX);
            const int py = int(cursor.y * scaleY);
            const bool overCanvas = px >= canvas.x && py >= canvas.y &&
                                    px < canvas.x + canvas.width &&
                                    py < canvas.y + canvas.height;
            if (overCanvas) {
              const double u =
                  double(px - canvas.x) / std::max(canvas.width, 1);
              const double v =
                  (canvas.y + canvas.height - py) / std::max(canvas.height, 1);
              const cv::Size target = targetOf(canvas, shared, mode_);
              anim.mouse_[0] = float(u * target.width);
              anim.mouse_[1] = float(v * target.height);
              anim.mouseKnown_ = true;
              for (const auto &event : presses)
                if (event->button() == Mouse::LEFT)
                  anim.mouse_[2] = anim.mouse_[0];
              for (const auto &event : releases)
                if (event->button() == Mouse::LEFT)
                  anim.mouse_[3] = anim.mouse_[0];
            }
          }
          shared.resolution_ =
              shared.hasShader_ ? targetOf(canvas, shared, mode_) : cv::Size();

          // -- keyboard texture -------------------------------------------
          if (shared.inputActive_) {
            for (const auto &key : keyPresses)
              keyPress(anim.keyboard_, key->key());
            for (const auto &key : keyReleases)
              keyRelease(anim.keyboard_, key->key());
          }
          anim.keyboard_(
                  cv::Rect(0, keyboardRowPressed, keyboardTextureWidth, 1))
              .setTo(cv::Scalar::all(0));
        },
        move_, hoverEnter_, hoverExit_, press_, release_, keyPress_,
        keyRelease_, size_, windowSize_, RWS(shared_), RW(anim_));

    // (6) Run the passes.
    gl(
        [this](ShadertoyRenderer &renderer, Anim &anim, Shared &shared,
               const cv::Size &fbSize) {
          if (!renderer.hasShader())
            return;
          ShadertoyRenderer::Frame frame;
          const cv::Rect canvas =
              canvasOf(cv::Size(fbSize.width, fbSize.height), shared, mode_);
          frame.canvas = canvas;
          frame.target = targetOf(canvas, shared, mode_);
          frame.time = anim.time_;
          frame.timeDelta = shared.timeDelta_;
          frame.frame = anim.frame_;
          frame.frameRate =
              shared.timeDelta_ > 0.0f ? 1.0f / shared.timeDelta_ : 60.0f;
          frame.mouse =
              shared.inputActive_ ? anim.mouse_ : cv::Vec4f(0, 0, 0, 0);
          const auto now = std::chrono::system_clock::now();
          const auto today = std::chrono::floor<std::chrono::days>(now);
          const std::chrono::year_month_day ymd{today};
          frame.date =
              cv::Vec4f(float(static_cast<int>(ymd.year())),
                        float(static_cast<unsigned>(ymd.month())),
                        float(static_cast<unsigned>(ymd.day())),
                        float(std::chrono::duration_cast<std::chrono::seconds>(
                                  now - today)
                                  .count()));
          frame.sampleRate = 44100.0f;
          renderer.setKeyboardTexture(anim.keyboard_);
          renderer.render(frame, shared.displayPass_);
          ++anim.framesRendered_;

          // -- screenshot ---------------------------------------------------
          if (shared.screenshot_) {
            shared.screenshot_ = false;
            writeShot(renderer, shared, anim);
          }
          if (mode_ == Mode::Shot && !anim.shotTaken_ &&
              anim.framesRendered_ >= shotFrames_) {
            anim.shotTaken_ = true;
            writeShot(renderer, shared, anim);
            V4D::instance()->requestFinish();
          }
        },
        RW(renderer_), RW(anim_), RWS(shared_), size_);

    // (7) HUD, only in the interactive mode: a shot of the window should look
    //     like the editor, not like a bare canvas.
    if (mode_ == Mode::Interactive) {
      branch(CS(shared_.showHud_))
          ->nvg([this](const cv::Size &sz,
                       const Shared &shared) { drawHud(sz, shared); },
                size_, CS(shared_))
          ->endBranch();
    }
  }

private:
  ShadertoyRenderer renderer_;
  /// The document serial the last snapshot was taken at; a structural change
  /// (a new file, a pass added or removed) has to be compiled without waiting
  /// for the debounce.
  int snapshottedSerial_ = -1;
  bool exportDone_ = false;

  static constexpr double kDebounceSeconds = 0.4;
  static constexpr int kPanelWidth = 560;

  // ---- layout -------------------------------------------------------------

  static cv::Rect canvasOf(const cv::Size &fbSize, const Shared &shared,
                           Mode mode) {
    const int inset =
        (mode == Mode::Interactive && shared.showPanel_ && !shared.fullscreen_)
            ? std::min(kPanelWidth, fbSize.width / 2)
            : 0;
    return cv::Rect(inset, 0, std::max(fbSize.width - inset, 1),
                    std::max(fbSize.height, 1));
  }

  static cv::Size targetOf(const cv::Rect &canvas, const Shared &shared,
                           Mode mode) {
    if (mode == Mode::Verify)
      return cv::Size(256, 256);
    const float scale = std::clamp(shared.resolutionScale_, 0.1f, 2.0f);
    return cv::Size(std::max(int(canvas.width * scale), 16),
                    std::max(int(canvas.height * scale), 16));
  }

  static bool imguiWantsKeyboard() {
    ImGuiContext *ctx = ImGui::GetCurrentContext();
    return ctx != nullptr && ImGui::GetIO().WantCaptureKeyboard;
  }

  // ---- files --------------------------------------------------------------

  /// Ends an unattended run with a verdict. --verify and --shot cannot leave
  /// the window to the user to close, so every outcome has to stop the frame
  /// loop. The code becomes the process exit code.
  static void finishRun(int code, const std::string &message) {
    // The first outcome is the answer. The rest of the frame keeps running
    // until the loop notices, and a stale "it compiled" would otherwise
    // overwrite a file that could not even be opened.
    if (verifyExitCode == 0) {
      verifyExitCode = code;
      std::cout << message << std::endl;
    }
    V4D::instance()->requestFinish();
  }

  void service(Shared &shared, Pending &pending) {
    pending.haveDocument_ = false;

    if (shared.load_ || shared.reload_) {
      shared.load_ = false;
      const std::string path =
          shared.reload_ ? shared.path_ : std::move(shared.pathToOpen_);
      shared.reload_ = false;
      if (path.empty()) {
        shared.status_ = "no project file given";
        if (mode_ == Mode::Verify)
          finishRun(1, shared.status_);
      } else {
        Shader shader;
        std::string error;
        if (!readProjectFile(path, shader, error)) {
          shared.status_ = "cannot open: " + error;
          // Nothing will ever compile, so an unattended run has to stop here
          // instead of waiting for a first result that cannot come. The
          // built-in that was on screen is dropped too: reporting "ok" for a
          // shader the caller never asked for would be a lie.
          if (mode_ != Mode::Interactive) {
            shared.shader_.passes.clear();
            ++shared.serial_;
            finishRun(1, shared.status_);
          }
        } else if (mode_ == Mode::Export) {
          // Export renders nothing, so the document goes straight into the
          // shared one below instead of waiting for a worker snapshot.
          shared.shader_ = std::move(shader);
          shared.status_ = "loaded " + path;
        } else {
          pending.shader_ = std::move(shader);
          pending.decoded_.clear();
          pending.haveDocument_ = true;
          shared.status_ = "loaded " + path;
        }
      }
    }

    // Export ends the run here: it has a file to write, and nothing to draw.
    if (mode_ == Mode::Export && !exportDone_) {
      exportDone_ = true;
      std::string error;
      if (writeProjectFile(exportPath_, shared.shader_, error)) {
        shared.status_ = "wrote " + exportPath_;
        finishRun(0, shared.status_);
      } else {
        shared.status_ = "export failed: " + error;
        finishRun(1, shared.status_);
      }
    }

    if (shared.save_) {
      shared.save_ = false;
      std::string path = shared.path_;
      if (!shared.pathToSave_.empty()) {
        path = shared.pathToSave_;
        shared.pathToSave_.clear();
      }
      if (path.empty()) {
        shared.status_ = "save needs a file name - use Save as";
      } else {
        std::string error;
        if (writeProjectFile(path, shared.shader_, error)) {
          shared.path_ = path;
          shared.dirty_ = false;
          shared.status_ = "saved " + path;
        } else {
          shared.status_ = "save failed: " + error;
        }
      }
    }

    // A local texture. The path is what goes into the pass input, so the
    // document stays readable and portable.
    if (!shared.texturePath_.empty() && shared.texturePass_ >= 0) {
      const std::string path = std::move(shared.texturePath_);
      const int passIndex = shared.texturePass_;
      const int channel = shared.textureChannel_;
      cv::Mat rgba;
      std::string error;
      if (!loadLocalTexture(path, rgba, error)) {
        shared.status_ = error;
      } else if (passIndex < int(shared.shader_.passes.size())) {
        Pass &pass = shared.shader_.passes[size_t(passIndex)];
        PassInput *input = nullptr;
        for (auto &in : pass.inputs)
          if (in.channel == channel)
            input = &in;
        if (input == nullptr) {
          PassInput fresh;
          fresh.id = int(pass.inputs.size());
          fresh.channel = channel;
          pass.inputs.push_back(fresh);
          input = &pass.inputs.back();
        }
        input->ctype = "texture";
        input->src = path;
        input->filter = "linear";
        input->wrap = "clamp";
        input->vflip = false; // a local file is stored the right way up
        pending.decoded_[path] = std::move(rgba);
        ++shared.edits_;
        shared.lastEdit_ = seconds();
        shared.dirty_ = true;
        ++shared.serial_;
        shared.status_ = "loaded texture " + path;
      }
    }

    // Publish a document that was read from disk. The buffers in the GUI are
    // rebuilt from the serial, so the fields show the new file at once.
    if (pending.haveDocument_) {
      shared.shader_ = pending.shader_;
      shared.textures_.clear();
      pending.textures_.clear();
      // A project names its textures by path, so opening the file has to bring
      // the images with it - otherwise every channel shows up black and the
      // shader looks broken.
      {
        std::string missing;
        for (const auto &pass : pending.shader_.passes)
          for (const auto &input : pass.inputs) {
            if (input.ctype != "texture" || input.src.empty())
              continue;
            cv::Mat rgba;
            std::string error;
            if (loadLocalTexture(input.src, rgba, error))
              pending.decoded_[input.src] = std::move(rgba);
            else if (missing.empty())
              missing = error;
          }
        if (!missing.empty())
          shared.status_ += "; " + missing;
      }
      ++shared.serial_;
      shared.edits_ = 0;
      shared.lastEdit_ = seconds();
      shared.dirty_ = false;
      shared.selected_ = 0;
      shared.displayPass_ = 0;
      shared.errors_.clear();
      pending.shader_ = Shader{};
    }

    // Decoded textures belong to the document, and snapshot() reads them from
    // there, so they have to land before it runs.
    if (!pending.decoded_.empty()) {
      for (auto &kv : pending.decoded_)
        shared.textures_[kv.first] = kv.second;
      pending.decoded_.clear();
      snapshot(shared, pending);
    }
    if (pending.haveDocument_) {
      pending.haveDocument_ = false;
      shared.hasShader_ = !shared.shader_.passes.empty();
    }
    if (!pending.status_.empty()) {
      shared.status_ = std::move(pending.status_);
      pending.status_.clear();
    }
  }

  /// Copies the document into the pending compile unit. The texture list is
  /// rebuilt from the pass inputs: every channel of type "texture" names a file
  /// next to the project, the way the site names a downloaded preset.
  void snapshot(const Shared &shared, Pending &pending) {
    pending.shader_ = shared.shader_;
    pending.textures_.clear();
    for (const auto &pass : pending.shader_.passes) {
      for (const auto &in : pass.inputs) {
        if (in.ctype != "texture" || in.src.empty())
          continue;
        const auto it = shared.textures_.find(in.src);
        if (it == shared.textures_.end())
          continue;
        const bool known =
            std::any_of(pending.textures_.begin(), pending.textures_.end(),
                        [&](const ShadertoyRenderer::TextureInput &t) {
                          return t.src == in.src;
                        });
        if (known)
          continue;
        ShadertoyRenderer::TextureInput texture;
        texture.src = in.src;
        texture.rgba = it->second;
        texture.filter = in.filter;
        texture.wrap = in.wrap;
        pending.textures_.push_back(std::move(texture));
      }
    }
    pending.generation_ = shared.edits_;
    pending.compile_ = true;
  }

  void writeShot(ShadertoyRenderer &renderer, Shared &shared, Anim &anim) {
    std::string path = shotPath_;
    if (path.empty())
      path = "shadertoy-editor-" + std::to_string(anim.frame_) + ".png";
    cv::Mat image;
    if (!renderer.grab(shared.displayPass_, image)) {
      std::cerr << "shadertoy-editor: nothing to capture yet" << std::endl;
      shared.status_ = "nothing to capture yet";
      return;
    }
    if (cv::imwrite(path, image)) {
      std::cerr << "shadertoy-editor: wrote " << path << std::endl;
      shared.status_ = "wrote " + path;
    } else {
      shared.status_ = "cannot write " + path;
    }
  }

  // ---- the panel -----------------------------------------------------------

  void drawPanel(Shared &shared) {
    using namespace ImGui;

    SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Once);
    SetNextWindowSize(ImVec2(float(kPanelWidth) - 20.0f, 0.0f), ImGuiCond_Once);
    if (!Begin("Shadertoy Editor")) {
      End();
      return;
    }

    if (shared.hasShader_) {
      TextWrapped("%s", shared.shader_.name.c_str());
      if (!shared.shader_.author.empty())
        TextDisabled("by %s", shared.shader_.author.c_str());
    }
    TextDisabled("%s%s",
                 shared.path_.empty() ? "(unsaved)" : shared.path_.c_str(),
                 shared.dirty_ ? " *" : "");

    drawFileRow(shared);
    Separator();
    drawPassTabs(shared);
    drawEditor(shared);
    // After the pane that can change the scheme has been drawn, so a new pick
    // takes effect in the same frame rather than the next one.
    rethemePanes(shared);
    drawErrors(shared);
    drawChannels(shared);
    drawPlayback(shared);

    Separator();
    if (shared.compiling_)
      TextDisabled("compiling...");
    else
      TextWrapped("%s", shared.status_.c_str());
    End();
  }

  void drawFileRow(Shared &shared) {
    using namespace ImGui;

    PushItemWidth(160.0f);
    if (Button("Open"))
      OpenPopup("Open file");
    if (BeginPopupModal("Open file", nullptr,
                        ImGuiWindowFlags_AlwaysAutoResize)) {
      char path[512] = "";
      InputTextWithHint("##open", "path/to/shader.json", path, sizeof(path));
      if (Button("Open", ImVec2(120, 0))) {
        shared.pathToOpen_ = path;
        shared.load_ = true;
        CloseCurrentPopup();
      }
      SameLine();
      if (Button("Cancel", ImVec2(120, 0)))
        CloseCurrentPopup();
      EndPopup();
    }
    SameLine();
    // Save goes back to the file it came from; without one there is nowhere to
    // write, so ask for a name first.
    if (Button("Save")) {
      if (shared.path_.empty())
        OpenPopup("Save as");
      else
        shared.save_ = true;
    }
    if (BeginPopupModal("Save as", nullptr,
                        ImGuiWindowFlags_AlwaysAutoResize)) {
      char path[512] = "";
      if (!shared.path_.empty())
        std::snprintf(path, sizeof(path), "%s", shared.path_.c_str());
      InputTextWithHint("##save", "path/to/shader.json", path, sizeof(path));
      if (Button("Save", ImVec2(120, 0))) {
        shared.pathToSave_ = path;
        shared.save_ = true;
        CloseCurrentPopup();
      }
      SameLine();
      if (Button("Cancel", ImVec2(120, 0)))
        CloseCurrentPopup();
      EndPopup();
    }
    SameLine();
    const bool saved = !shared.path_.empty();
    BeginDisabled(!saved);
    if (Button("Reload"))
      shared.reload_ = true;
    EndDisabled();

    const auto samples = builtIns();
    std::vector<const char *> names;
    names.reserve(samples.size());
    for (const auto &sample : samples)
      names.push_back(sample.name);
    int chosen = shared.builtIn_;
    if (chosen >= int(samples.size()))
      chosen = 0;
    if (Combo("##builtin", &chosen, names.data(), int(names.size()))) {
      shared.shader_ = samples[size_t(chosen)].shader;
      shared.path_.clear();
      shared.textures_.clear();
      shared.selected_ = 0;
      shared.displayPass_ = 0;
      shared.errors_.clear();
      shared.dirty_ = false;
      ++shared.serial_;
      shared.edits_ = 0;
      shared.lastEdit_ = seconds();
      shared.builtIn_ = chosen;
      shared.status_ = std::string("new shader from \"") +
                       samples[size_t(chosen)].name + "\"";
    }
    PopItemWidth();
  }

  void drawPassTabs(Shared &shared) {
    using namespace ImGui;

    if (!BeginTabBar("passes", ImGuiTabBarFlags_Reorderable |
                                   ImGuiTabBarFlags_AutoSelectNewTabs)) {
      // An empty shader still has to show the "add a pass" control.
      EndTabBar();
      return;
    }
    for (size_t i = 0; i < shared.shader_.passes.size(); ++i) {
      Pass &pass = shared.shader_.passes[i];
      std::string label = pass.name.empty() ? pass.type : pass.name;
      if (label.empty())
        label = "pass " + std::to_string(i);
      if (BeginTabItem(label.c_str())) {
        shared.selected_ = int(i);
        EndTabItem();
      }
    }
    if (BeginTabItem("+"))
      addBufferPass(shared);
    SameLine();
    // Removing a pass is destructive, so it gets its own control next to the
    // tabs instead of a close button that is one stray click away.
    if (Button("Remove") && shared.selected_ >= 0 &&
        shared.selected_ < int(shared.shader_.passes.size()))
      removePass(shared, shared.selected_);
    EndTabBar();
  }

  void addBufferPass(Shared &shared) {
    Shader &shader = shared.shader_;
    // One image pass has to exist: the image pass is what the window shows.
    bool hasImage = false;
    for (const auto &pass : shader.passes)
      hasImage = hasImage || pass.type == "image";
    if (!hasImage) {
      Pass image;
      image.type = "image";
      image.name = "Image";
      image.code =
          "void mainImage(out vec4 fragColor, in vec2 fragCoord)\n"
          "{\n"
          "    fragColor = vec4(fragCoord / iResolution.xy, 0.0, 1.0);\n"
          "}\n";
      image.outputs = {{0, 0}};
      shader.passes.push_back(image);
    }
    Pass buffer;
    buffer.type = "buffer";
    buffer.name = "Buffer " + std::to_string(bufferPassCount(shader) + 1);
    buffer.code = "void mainImage(out vec4 fragColor, in vec2 fragCoord)\n"
                  "{\n"
                  "    fragColor = vec4(0.0);\n"
                  "}\n";
    buffer.inputs = {{0, 0, "buffer", buffer.name + ".0"}};
    buffer.outputs = {{0, 0}};
    shader.passes.push_back(buffer);
    shared.selected_ = int(shader.passes.size() - 1);
    ++shared.serial_;
    ++shared.edits_;
    shared.lastEdit_ = seconds();
    shared.dirty_ = true;
    shared.status_ = "added " + buffer.name;
  }

  static int bufferPassCount(const Shader &shader) {
    int count = 0;
    for (const auto &pass : shader.passes)
      count += (pass.type == "buffer") ? 1 : 0;
    return count;
  }

  void removePass(Shared &shared, int index) {
    if (index < 0 || index >= int(shared.shader_.passes.size()))
      return;
    const std::string name = shared.shader_.passes[size_t(index)].name;
    shared.shader_.passes.erase(shared.shader_.passes.begin() + index);
    shared.selected_ =
        std::min(shared.selected_, int(shared.shader_.passes.size()) - 1);
    shared.selected_ = std::max(shared.selected_, 0);
    ++shared.serial_;
    ++shared.edits_;
    shared.lastEdit_ = seconds();
    shared.dirty_ = true;
    shared.status_ = "removed pass " + name;
  }

  /// The code pane: one code widget for the selected pass, plus the two rows
  /// under it. This is where the document is edited.
  void drawEditor(Shared &shared) {
    using namespace ImGui;

    syncPanes(shared);
    if (shared.selected_ < 0 ||
        shared.selected_ >= int(shared.shader_.passes.size()) ||
        panes_.empty()) {
      TextDisabled("no pass selected");
      return;
    }
    const int passIndex = shared.selected_;
    CodePane &pane = panes_[size_t(passIndex)];
    TextEditor &editor = *pane.editor;

    applyPaneOptions(shared, editor);
    syncPaneText(shared, passIndex, pane);
    syncPaneErrors(shared, passIndex, pane);

    // The widget is a child window and draws nothing below itself, so it is
    // told how much room to leave for the button row and the status line -
    // three lines of chrome, which is also the least the pane is ever given, so
    // a shrunken panel still shows a signature rather than a sliver.
    const float rowHeight = GetTextLineHeightWithSpacing();
    const float avail = std::max(GetContentRegionAvail().y - rowHeight * 3.0f,
                                 rowHeight * 3.0f);

    // The widget measures its glyphs with ImGui::GetFont(), so the mono face
    // has to be the current one for the gutter to line up with the code. `pick`
    // falls back to whatever is current rather than pushing null, which ImGui
    // does not accept.
    PushFont(fonts_.pick(fonts_.mono, ImGui::GetFont()));
    editor.Render("##code", ImVec2(0.0f, avail));
    PopFont();

    // The widget reports a change at the very end of Render(), after its child
    // window is closed, so the write-back and the caret move happen here rather
    // than from inside a callback.
    drainPaneEdits(shared, passIndex, editor);
    applyCaret(shared, passIndex, editor);
    drawEditorRow(shared, editor);
    drawEditorStatus(editor);
  }

  /// Pushes the code pane's switches onto the widget. Every one of these is a
  /// setter rather than a render argument, so they are applied once per frame
  /// for the pane that is actually on screen and cost nothing.
  static void applyPaneOptions(const Shared &shared, TextEditor &editor) {
    editor.SetLanguage(code::language());
    editor.SetWordWrapEnabled(shared.wrapText_);
    editor.SetShowLineNumbersEnabled(shared.showLineNumbers_);
    editor.SetShowWhitespacesEnabled(shared.showWhitespace_);
    editor.SetAutoIndentEnabled(shared.autoIndent_);
    editor.SetShowMatchingBrackets(shared.matchingBrackets_);
    editor.SetLineFoldingEnabled(shared.lineFolding_);
    editor.SetShowMiniMapEnabled(shared.minimap_);
  }

  /// Copies the document into a pane, but only when that pane's text is not
  /// already what the document says.
  ///
  /// The serial alone is not enough to decide this. Typing bumps the serial -
  /// every edit does - and reloading the text on each keystroke would throw
  /// away the caret and the undo stack every time, so the text itself is
  /// compared. The comparison is a pass of a few kilobytes once per frame for
  /// the pane on screen, which is not worth optimising away; doing the string
  /// compare before the string assign is what keeps the caret from jumping.
  static void syncPaneText(const Shared &shared, int passIndex,
                           CodePane &pane) {
    const Pass &pass = shared.shader_.passes[size_t(passIndex)];
    if (pane.editor->GetText() == pass.code)
      return;
    const bool hadText = pane.loadedSerial >= 0;
    pane.editor->SetText(pass.code);
    // SetText drops the undo history and the markers, and both are wanted back:
    // the history because losing it on an unrelated structural change is
    // infuriating, and the markers because SetText clears them and the error
    // list has not changed, so they are re-added below without being
    // re-counted.
    pane.loadedSerial = shared.serial_;
    if (hadText)
      pane.markedSerial = shared.serial_;
  }

  /// Puts the driver's complaints onto the lines it complained about.
  ///
  /// One marker per line is all the widget keeps, so the first message for a
  /// line is the one that is shown - which is the one the driver emitted first
  /// and therefore the one that caused the rest. The list above the pane
  /// carries them all.
  static void syncPaneErrors(const Shared &shared, int passIndex,
                             CodePane &pane) {
    const bool samePass = pane.markedPass == passIndex;
    const bool sameErrors = samePass &&
                            pane.markedCount == int(shared.errors_.size()) &&
                            pane.markedSerial == shared.serial_;
    if (sameErrors)
      return;
    pane.editor->ClearMarkers();
    ImU32 textColor = 0;
    ImU32 gutterColor = 0;
    if (code::errorMarker(textColor, gutterColor,
                          shadertoy::palettes()[size_t(shared.scheme_)])) {
      for (const CompileError &error : shared.errors_) {
        if (error.pass != passIndex || !error.located())
          continue;
        pane.editor->AddMarker(size_t(std::max(error.line - 1, 0)), gutterColor,
                               textColor, error.message, error.message);
      }
    }
    pane.markedSerial = shared.serial_;
    pane.markedPass = passIndex;
    pane.markedCount = int(shared.errors_.size());
  }

  /// Writes the widget's text back into the document, once per frame and only
  /// if it moved.
  ///
  /// The widget's change callback is the tidier place for this, but it fires
  /// from inside Render() after the widget has closed its own child window, so
  /// the text has to be read here instead - where the pass index is known and
  /// the comparison against the document is one string equality rather than a
  /// flag that has to be kept honest.
  static void drainPaneEdits(Shared &shared, int passIndex,
                             TextEditor &editor) {
    std::string text = editor.GetText();
    Pass &pass = shared.shader_.passes[size_t(passIndex)];
    if (text == pass.code)
      return;
    pass.code = std::move(text);
    ++shared.edits_;
    shared.lastEdit_ = seconds();
    shared.dirty_ = true;
    // The serial is left alone on purpose: it marks a structural change, and
    // typing into a pass is not one. The worker recompiles off `edits_`.
    shared.status_ =
        "edited " + (pass.name.empty() ? std::string("pass") : pass.name);
  }

  /// Puts the caret on the line an error pointed at. The widget owns the caret,
  /// so this is its supported way in - no reaching into ImGui's internals, and
  /// no dependency on the field happening to be the active one.
  void applyCaret(Shared &shared, int passIndex, TextEditor &editor) {
    if (!wantCaret_ || caret_.pass != passIndex)
      return;
    wantCaret_ = false;
    // The driver and the error list count lines and columns from one; the
    // widget counts from zero.
    const size_t line = size_t(std::max(caret_.line - 1, 0));
    const size_t column = size_t(std::max(caret_.column - 1, 0));
    editor.SetCursor(TextEditor::DocPos(line, column));
    editor.ScrollToLine(line, TextEditor::Scroll::alignTop);
    shared.status_ = "line " + std::to_string(line + 1) + ", column " +
                     std::to_string(column + 1);
  }

  /// The row of editing controls. Undo and redo are here rather than only on
  /// ctrl+z / ctrl+y because the widget's history is per pass, and it is the
  /// one thing a reader of a shader wants to be able to see the state of.
  void drawEditorRow(Shared &shared, TextEditor &editor) {
    using namespace ImGui;

    BeginDisabled(!editor.CanUndo());
    if (Button("Undo"))
      editor.Undo();
    EndDisabled();
    SameLine();
    BeginDisabled(!editor.CanRedo());
    if (Button("Redo"))
      editor.Redo();
    EndDisabled();
    SameLine();
    if (Button("Compile now"))
      shared.compile_ = true;
    SameLine();
    if (Button("Reset time"))
      shared.resetTime_ = true;

    if (Button("Code"))
      OpenPopup("Code pane");
    if (BeginPopup("Code pane")) {
      Checkbox("Wrap long lines", &shared.wrapText_);
      Checkbox("Line numbers", &shared.showLineNumbers_);
      Checkbox("Show spaces and tabs", &shared.showWhitespace_);
      Checkbox("Auto-indent new lines", &shared.autoIndent_);
      Checkbox("Highlight matching bracket", &shared.matchingBrackets_);
      Checkbox("Fold blocks", &shared.lineFolding_);
      Checkbox("Minimap", &shared.minimap_);
      Separator();
      // Every scheme is offered by name, so a screenshot of the editor and the
      // name in this list agree on what "Solarized Dark" looks like.
      const auto &schemes = shadertoy::palettes();
      int chosen = std::clamp(shared.scheme_, 0, int(schemes.size()) - 1);
      if (Combo("Colour scheme", &chosen, schemeNames().data(),
                int(schemeNames().size()))) {
        shared.scheme_ = chosen;
        // The markers carry colours of their own, so a re-theme invalidates
        // them even though the errors did not change.
        for (CodePane &pane : panes_)
          pane.markedSerial = -1;
      }
      EndPopup();
    }
  }

  /// The status line: where the caret is, what is selected, and how much
  /// history is behind it. Without this the pane has no way to answer "which of
  /// the three tabs am I in" after the tab bar has scrolled out of view.
  static void drawEditorStatus(const TextEditor &editor) {
    using namespace ImGui;

    const TextEditor::DocPos caret = editor.GetCurrentCursorPosition();
    const TextEditor::DocSelection selection =
        editor.GetCurrentCursorSelection();
    const size_t lines = editor.GetLineCount();
    const size_t history = editor.GetUndoIndex();
    char status[160];
    if (selection.start != selection.end) {
      // A DocSelection is a pair of glyph positions, so the length of the
      // selection is the difference of their indices.
      const long long chars = static_cast<long long>(selection.end.index) -
                              static_cast<long long>(selection.start.index);
      std::snprintf(
          status, sizeof(status),
          "line %zu column %zu   %lld selected   %zu lines   undo %zu",
          caret.line + 1, caret.index + 1, chars, lines, history);
    } else {
      std::snprintf(status, sizeof(status),
                    "line %zu column %zu   %zu lines   undo %zu",
                    caret.line + 1, caret.index + 1, lines, history);
    }
    TextDisabled("%s", status);
  }

  /// The scheme names, as an ImGui combo wants them: a flat array of pointers.
  static const std::vector<const char *> &schemeNames() {
    static const std::vector<const char *> names = [] {
      std::vector<const char *> out;
      for (const Palette &scheme : shadertoy::palettes())
        out.push_back(scheme.name);
      return out;
    }();
    return names;
  }

  /// Creates one code widget per pass, and hands each one the text of the pass
  /// it belongs to. Only when the document's shape changes - typing goes the
  /// other way, through drainPaneEdits().
  void syncPanes(Shared &shared) {
    if (panesSerial_ == shared.serial_ &&
        panes_.size() == shared.shader_.passes.size())
      return;
    panes_.clear();
    panes_.resize(shared.shader_.passes.size());
    for (size_t i = 0; i < panes_.size(); ++i) {
      auto editor = std::make_unique<TextEditor>();
      editor->SetText(shared.shader_.passes[i].code);
      editor->SetPalette(
          code::editorPalette(shadertoy::palettes()[size_t(std::clamp(
              shared.scheme_, 0, int(shadertoy::palettes().size()) - 1))]));
      // The pane fills the width it is given and draws its own gutter, so the
      // editor's own margins would only take room away from the code.
      editor->SetLineNumberLeftMargin(3);
      editor->SetTextLeftMargin(1);
      editor->SetTabSize(4);
      editor->SetInsertSpacesOnTabs(true);
      editor->SetLineSpacing(1.0f);
      panes_[i].editor = std::move(editor);
      panes_[i].loadedSerial = shared.serial_;
      panes_[i].markedSerial = -1;
    }
    panesSerial_ = shared.serial_;
    if (shared.selected_ >= int(panes_.size()))
      shared.selected_ = panes_.empty() ? 0 : int(panes_.size()) - 1;
  }

  /// Applies a newly picked scheme to every pane. The ImGui chrome is restyled
  /// every frame in gui(), but a widget only reads its palette when it is told
  /// to, and telling one widget is cheaper than re-theming it each frame.
  void rethemePanes(Shared &shared) {
    if (appliedScheme_ == shared.scheme_)
      return;
    appliedScheme_ = shared.scheme_;
    const auto &schemes = shadertoy::palettes();
    const Palette &scheme =
        schemes[size_t(std::clamp(shared.scheme_, 0, int(schemes.size()) - 1))];
    for (CodePane &pane : panes_) {
      pane.editor->SetPalette(code::editorPalette(scheme));
      // The error markers carry colours taken from the old scheme.
      pane.markedSerial = -1;
    }
  }

  void drawErrors(Shared &shared) {
    using namespace ImGui;

    if (!CollapsingHeader("Errors", ImGuiTreeNodeFlags_DefaultOpen))
      return;
    if (shared.errors_.empty()) {
      TextDisabled("none");
      return;
    }
    for (size_t i = 0; i < shared.errors_.size(); ++i) {
      const CompileError &error = shared.errors_[i];
      PushID(int(i));
      if (error.located()) {
        // Clicking the message switches to its pass and asks that pass's pane
        // for the caret. Every pass keeps its own widget, so nothing is
        // reloaded and what was typed into the other tabs survives the jump.
        if (Selectable(error.message.c_str(), false)) {
          shared.selected_ = error.pass;
          caret_.pass = error.pass;
          caret_.line = error.line;
          caret_.column = error.column;
          wantCaret_ = true;
        }
        SameLine(0.0f, 8.0f);
        TextDisabled("pass %d, line %d", error.pass, error.line);
      } else {
        TextUnformatted(error.message.c_str());
      }
      PopID();
    }
    if (!shared.errors_.front().context.empty()) {
      Separator();
      TextDisabled("%s", shared.errors_.front().context.c_str());
    }
  }

  void drawChannels(Shared &shared) {
    using namespace ImGui;

    if (!CollapsingHeader("Channel inputs"))
      return;
    if (shared.selected_ < 0 ||
        shared.selected_ >= int(shared.shader_.passes.size())) {
      TextDisabled("no pass selected");
      return;
    }
    Pass &pass = shared.shader_.passes[size_t(shared.selected_)];
    TextDisabled("%s pass \"%s\"", pass.type.c_str(), pass.name.c_str());

    static const char *kTypes[] = {"none", "texture", "buffer", "keyboard"};
    for (int channel = 0; channel < 4; ++channel) {
      PassInput *input = nullptr;
      for (auto &in : pass.inputs)
        if (in.channel == channel)
          input = &in;

      PushID(channel);
      Separator();
      int type = 0;
      if (input != nullptr)
        for (int i = 0; i < 4; ++i)
          if (input->ctype == kTypes[i]) {
            type = i;
            break;
          }
      Text("iChannel%d", channel);
      SameLine(70.0f);
      SetNextItemWidth(90.0f);
      bool changed = false;
      if (Combo("##type", &type, kTypes, 4)) {
        changed = true;
        if (type == 0) {
          if (input != nullptr)
            pass.inputs.erase(pass.inputs.begin() +
                              (input - pass.inputs.data()));
        } else {
          if (input == nullptr) {
            PassInput fresh;
            fresh.id = int(pass.inputs.size());
            fresh.channel = channel;
            pass.inputs.push_back(fresh);
            input = &pass.inputs.back();
          }
          input->ctype = kTypes[type];
          if (type == 1 && input->src.empty())
            input->src = "";
          if (type == 2 && input->src.empty())
            input->src = bufferNameList(shared.shader_);
        }
      }
      SameLine();
      if (input == nullptr) {
        TextDisabled("not connected");
        PopID();
        continue;
      }

      if (input->ctype == "texture") {
        char src[512];
        std::snprintf(src, sizeof(src), "%s", input->src.c_str());
        PushItemWidth(-1.0f);
        if (InputText("##src", src, sizeof(src))) {
          input->src = src;
          changed = true;
        }
        PopItemWidth();
        TextDisabled("not decoded yet - use Load");
      } else if (input->ctype == "buffer") {
        char src[128];
        std::snprintf(src, sizeof(src), "%s", input->src.c_str());
        if (InputText("##src", src, sizeof(src))) {
          input->src = src;
          changed = true;
        }
      } else {
        TextDisabled("iChannelKeyboard");
      }

      static const char *kFilters[] = {"linear", "nearest", "mipmap"};
      static const char *kWraps[] = {"clamp", "repeat", "mirror"};
      int filter = 0;
      for (int i = 0; i < 3; ++i)
        filter = (input->filter == kFilters[i]) ? i : filter;
      int wrap = 0;
      for (int i = 0; i < 3; ++i)
        wrap = (input->wrap == kWraps[i]) ? i : wrap;
      SameLine();
      SetNextItemWidth(90.0f);
      if (Combo("##filter", &filter, kFilters, 3)) {
        input->filter = kFilters[filter];
        changed = true;
      }
      SameLine();
      SetNextItemWidth(90.0f);
      if (Combo("##wrap", &wrap, kWraps, 3)) {
        input->wrap = kWraps[wrap];
        changed = true;
      }
      if (input->ctype == "texture") {
        if (Checkbox("vflip", &input->vflip))
          changed = true;
        SameLine();
        if (Button("Load##load")) {
          shared.texturePass_ = shared.selected_;
          shared.textureChannel_ = channel;
          shared.texturePath_ = input->src;
          if (shared.texturePath_.empty()) {
            shared.status_ = "set a file name first";
            changed = false;
          }
        }
      }
      if (changed) {
        ++shared.edits_;
        shared.lastEdit_ = seconds();
        shared.dirty_ = true;
      }
      PopID();
    }
  }

  /// The buffer names a pass may read, in the "Name.0" form Shadertoy uses.
  static std::string bufferNameList(const Shader &shader) {
    for (const auto &pass : shader.passes)
      if (pass.type == "buffer" && !pass.name.empty())
        return pass.name + ".0";
    return std::string();
  }

  void drawPlayback(Shared &shared) {
    using namespace ImGui;

    if (!CollapsingHeader("Playback", ImGuiTreeNodeFlags_DefaultOpen))
      return;
    Checkbox("Play", &shared.playing_);
    SameLine();
    if (Button("Save PNG"))
      shared.screenshot_ = true;
    SliderFloat("Resolution scale", &shared.resolutionScale_, 0.1f, 2.0f,
                "%.2fx");
    Checkbox("Mouse + keyboard feed the shader", &shared.inputActive_);

    if (!shared.passNames_.empty()) {
      std::vector<const char *> names;
      names.reserve(shared.passNames_.size());
      for (const auto &name : shared.passNames_)
        names.push_back(name.c_str());
      int pass = std::clamp(shared.displayPass_, 0, int(names.size()) - 1);
      if (Combo("Show pass", &pass, names.data(), int(names.size())))
        shared.displayPass_ = pass;
      shared.displayPass_ =
          std::clamp(shared.displayPass_, 0, int(names.size()) - 1);
    }
    Checkbox("HUD", &shared.showHud_);
    Checkbox("Panel", &shared.showPanel_);
    Checkbox("Fullscreen", &shared.fullscreen_);
    TextDisabled("space play  f5 compile  f9 save  r reset  p screenshot  tab "
                 "panel  f fullscreen  h hud");
    char stats[192];
    std::snprintf(stats, sizeof(stats),
                  "t %.2fs  frame %d  %.0f fps  %dx%d  compile %.0f ms",
                  shared.time_, shared.frame_, double(shared.fps_),
                  shared.resolution_.width, shared.resolution_.height,
                  shared.compileSeconds_ * 1000.0);
    TextDisabled("%s", stats);
  }

  // ---- HUD -----------------------------------------------------------------

  void drawHud(const cv::Size &sz, const Shared &shared) {
    using namespace cv::v4d::nvg;
    if (!shared.hasShader_ || shared.shader_.name.empty())
      return;
    const cv::Rect canvas =
        canvasOf(cv::Size(sz.width, sz.height), shared, mode_);

    fontFace("sans-bold");
    fontSize(17.0f);
    beginPath();
    rect(float(canvas.x), float(canvas.y), float(canvas.width), 54.0f);
    fillColor(Scalar(0, 0, 0, 150));
    fill();

    fillColor(Scalar(255, 255, 255, 235));
    textAlign(NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    const std::string title =
        shared.shader_.name +
        (shared.shader_.author.empty() ? "" : "  -  " + shared.shader_.author);
    cv::v4d::nvg::text(float(canvas.x) + 14.0f, float(canvas.y) + 8.0f,
                       title.c_str(), title.c_str() + title.size());

    fontFace("sans");
    fontSize(13.0f);
    fillColor(Scalar(190, 190, 190, 220));
    char line[256];
    const std::string pass =
        shared.passNames_.empty()
            ? std::string()
            : ("   showing: " + shared.passNames_[size_t(std::clamp(
                                    shared.displayPass_, 0,
                                    int(shared.passNames_.size()) - 1))]);
    std::snprintf(line, sizeof(line),
                  "%s   t %.2fs   frame %d   %.0f fps   %dx%d%s",
                  shared.dirty_ ? "*edited*" : "saved", shared.time_,
                  shared.frame_, double(shared.fps_), shared.resolution_.width,
                  shared.resolution_.height, pass.c_str());
    cv::v4d::nvg::text(float(canvas.x) + 14.0f, float(canvas.y) + 30.0f, line,
                       line + std::strlen(line));

    fontSize(12.0f);
    const char *help =
        "edit the GLSL on the left - the shader recompiles as you type  -  "
        "space play/pause  -  f5 compile now  -  f9 save";
    float bounds[4] = {0, 0, 0, 0};
    textBounds(float(canvas.x) + 14.0f, float(canvas.y + canvas.height) - 26.0f,
               help, help + std::strlen(help), bounds);
    beginPath();
    rect(float(canvas.x), bounds[1] - 6.0f, float(canvas.width),
         (bounds[3] - bounds[1]) + 12.0f);
    fillColor(Scalar(0, 0, 0, 120));
    fill();
    fillColor(Scalar(150, 150, 150, 210));
    textAlign(NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    cv::v4d::nvg::text(float(canvas.x) + 14.0f,
                       float(canvas.y + canvas.height) - 26.0f, help,
                       help + std::strlen(help));
  }
};

ShadertoyEditorPlan::Shared ShadertoyEditorPlan::shared_;

// ---------------------------------------------------------------------------

namespace {

void usage(const char *self) {
  std::cerr
      << "Usage: " << self << " [options] [project.json]\n"
      << "\n"
      << "  --new <name>     start from a built-in sample instead of a file\n"
      << "                   (gradient, feedback, keyboard, common)\n"
      << "  --verify         compile the project, print the result and exit\n"
      << "                   0 when it compiles, 1 when it does not\n"
      << "  --shot <file>    render a few frames, write a PNG and exit\n"
      << "  --frames <n>     frames to wait before the shot (default 12)\n"
      << "  --export <file>  write the project back out as Shadertoy JSON\n"
      << "  --size <WxH>     window size (default 1600x900)\n"
      << "  --fullscreen     start fullscreen: the render fills the window\n"
      << "                   instead of being inset by the panel\n"
      << "\n"
      << "Example:\n"
      << "  " << self << " shader.json\n"
      << "  " << self << " --verify shader.json\n"
      << "  " << self << " --shot out.png shader.json\n";
}

/// Matches "--name value" or "--name=value". Returns true when `arg` is this
/// option, leaving `value` empty when it was given without one.
bool option(const char *arg, const char *name, std::string &value, int argc,
            char **argv, int &i) {
  const size_t len = std::strlen(name);
  if (std::strncmp(arg, name, len) != 0)
    return false;
  if (arg[len] == '=') {
    value = arg + len + 1;
    return true;
  }
  if (arg[len] != '\0')
    return false;
  if (i + 1 >= argc) {
    std::cerr << name << " needs a value" << std::endl;
    value.clear();
    return true;
  }
  value = argv[++i];
  return true;
}

/// The built-in samples, by name, for --new. The short names are what the help
/// text promises; the long ones are the titles the panel shows.
int builtInByName(const std::string &name) {
  static const char *kShort[] = {"gradient", "feedback", "keyboard", "common"};
  const auto samples = builtIns();
  for (size_t i = 0; i < samples.size(); ++i)
    if (name == samples[i].name ||
        (i < sizeof(kShort) / sizeof(kShort[0]) && name == kShort[i]))
      return int(i);
  std::cerr << "no built-in sample called \"" << name << "\"";
  for (const auto &sample : samples)
    std::cerr << "  " << sample.name;
  std::cerr << std::endl;
  return -1;
}

} // namespace

V4D_DEMO_MAIN(int argc, char **argv) {
  cv::v4d::add_asset_search_paths();

  std::string project;
  std::string shot;
  std::string exportTo;
  int frames = 12;
  int builtIn = 1; // the feedback sample: it shows a buffer chain at once
  bool verify = false;
  bool fullscreen = false;
  cv::Size window(1600, 900);

  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    if (arg[0] != '-') {
      project = arg;
      continue;
    }
    if (std::strcmp(arg, "-h") == 0 || std::strcmp(arg, "--help") == 0) {
      usage(argv[0]);
      return 0;
    }
    std::string value;
    if (option(arg, "--new", value, argc, argv, i)) {
      builtIn = builtInByName(value);
      if (builtIn < 0)
        return 1;
      project.clear();
    } else if (option(arg, "--shot", value, argc, argv, i)) {
      shot = value;
      if (shot.empty())
        return 1;
    } else if (option(arg, "--export", value, argc, argv, i)) {
      exportTo = value;
      if (exportTo.empty())
        return 1;
    } else if (option(arg, "--frames", value, argc, argv, i)) {
      frames = std::max(std::atoi(value.c_str()), 1);
    } else if (option(arg, "--size", value, argc, argv, i)) {
      int w = 1600, h = 900;
      if (std::sscanf(value.c_str(), "%dx%d", &w, &h) != 2) {
        std::cerr << "--size wants WxH, e.g. 1280x720" << std::endl;
        return 1;
      }
      window = cv::Size(w, h);
    } else if (std::strcmp(arg, "--fullscreen") == 0) {
      fullscreen = true;
    } else if (std::strcmp(arg, "--verify") == 0) {
      verify = true;
    } else {
      std::cerr << "unknown option " << arg << std::endl;
      usage(argv[0]);
      return 1;
    }
  }

  // --verify and --shot are the unattended paths: no panel, no window, and the
  // run ends by itself.
  const bool interactive = !verify && shot.empty() && exportTo.empty();
  const auto allocFlags = interactive
                              ? (AllocateFlags::NANOVG | AllocateFlags::IMGUI)
                              : AllocateFlags::NANOVG;
  auto configFlags =
      interactive ? ConfigFlags::RESIZEABLE : ConfigFlags::OFFSCREEN;

  V4D::init(cv::Rect(0, 0, window.width, window.height), "Shadertoy Editor",
            allocFlags, configFlags);

  const auto mode = verify              ? ShadertoyEditorPlan::Mode::Verify
                    : !exportTo.empty() ? ShadertoyEditorPlan::Mode::Export
                    : shot.empty()      ? ShadertoyEditorPlan::Mode::Interactive
                                        : ShadertoyEditorPlan::Mode::Shot;
  V4DPlan::run<ShadertoyEditorPlan>(0, mode, project, builtIn, shot, frames,
                                    exportTo, fullscreen);
  // The unattended modes report through their exit code: 0 for a clean run,
  // 1 for a file that would not open or a shader that would not compile.
  return interactive ? 0 : verifyExitCode;
}
