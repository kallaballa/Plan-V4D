// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// Tetralis -- the V4D application.
//
// tetralis-rules.hpp holds the whole game: eight tetracubes in a 4x4x15 shaft,
// Plumb, Spines, Anchor Charges and the Struck search. This file is only the
// window around it, and it is deliberately thin:
//
//   * the engine is stepped on the worker thread, a whole number of frames per
//     rendered frame and never from a clock, so that (seed, commands) still
//     replays exactly as §14 requires;
//   * everything the drawing passes need is copied into Snapshot, so neither
//     NanoVG nor ImGui ever touches the live game;
//   * NanoVG draws the shaft as an orbitable box of shaded cubes, with the §16
//     landing and Spine previews; ImGui draws the control panel.
//
//   ./modules/v4d/samples/tetralis-selftest.sh   the rules, checked headless
//   example_v4d_tetralis-demo                   this
//
// The keys are listed in the panel behind H, and along the bottom of the HUD.

#include "samples.hpp"
#include "tetralis-rules.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <opencv2/v4d/v4d.hpp>
#include <string>
#include <vector>

using namespace cv::v4d;
using namespace cv::v4d::event; // Keyboard, Mouse

using tetralis::Cell;
using tetralis::Cells;
using tetralis::Cmd;
using tetralis::Kind;

namespace {

constexpr int kLayers = tetralis::kShaftZMax + 1; // z = 0 .. 15
constexpr int kSlots = tetralis::kFootX * tetralis::kFootY * kLayers;
constexpr size_t kPreviews = 3;

// A slot index into the flat copy of the board, in the board's own [x][y][z]
// order, so that the copy reads the way the engine writes it.
size_t slot(const Cell &c) {
  return static_cast<size_t>((c.z * tetralis::kFootY + c.y) * tetralis::kFootX +
                             c.x);
}

Kind kindAt(const std::array<Kind, kSlots> &cells, const Cell &c) {
  if (c.x < 0 || c.x >= tetralis::kFootX || c.y < 0 ||
      c.y >= tetralis::kFootY || c.z < 0 || c.z >= kLayers)
    return Kind::kEmpty;
  return cells[slot(c)];
}

// ---------------------------------------------------------------------------
// Colours. NanoVG takes BGRA, and they are all shades of one hue family so that
// the piece being steered is always the brightest thing on screen.
// ---------------------------------------------------------------------------

const cv::Scalar kBackdrop(20, 19, 24);
const cv::Scalar kBedrock(58, 58, 66);
const cv::Scalar kNormal(104, 116, 138);
const cv::Scalar kRime(198, 154, 62);
const cv::Scalar kPiece(78, 168, 244);
const cv::Scalar kLanding(78, 168, 244, 56);
const cv::Scalar kSpine(96, 214, 138);
const cv::Scalar kCursor(250, 250, 250);
const cv::Scalar kWire(84, 86, 100);
const cv::Scalar kText(226, 226, 232);
const cv::Scalar kDim(150, 152, 164);
const cv::Scalar kFaint(104, 106, 118);
const cv::Scalar kWarn(96, 132, 244);
const cv::Scalar kCharge(126, 206, 244);
const cv::Scalar kChargeEmpty(58, 60, 70);

// ---------------------------------------------------------------------------
// The projection: an orbit camera around the shaft. Yaw turns it about the
// vertical axis, pitch tips it, and depth() is what the painter's algorithm
// sorts on -- largest first, so the far cubes go down before the near ones.
// ---------------------------------------------------------------------------

struct Projector {
  cv::Point2f origin;
  float scale = 1.0f;
  float cy = 1.0f, sy = 0.0f, cp = 1.0f, sp = 0.0f;

  Projector(cv::Point2f at, float pixelsPerCell, float yaw, float pitch)
      : origin(at), scale(pixelsPerCell) {
    cy = std::cos(yaw);
    sy = std::sin(yaw);
    cp = std::cos(pitch);
    sp = std::sin(pitch);
  }

  cv::Point2f operator()(float x, float y, float z) const {
    const float right = x * cy - y * sy;
    const float away = x * sy + y * cy;
    return cv::Point2f(origin.x + right * scale,
                       origin.y - (away * sp + z * cp) * scale);
  }

  float depth(float x, float y, float z) const {
    return (x * sy + y * cy) * cp - z * sp;
  }

  // A face is drawn when its normal points back at the camera, i.e. against
  // the view direction depth() uses.
  bool faces(float nx, float ny, float nz) const {
    return nx * sy * cp + ny * cy * cp - nz * sp < 0.0f;
  }
};

// The six faces of a unit cube: the corner offsets, and the normal that decides
// whether a face is visible from here.
struct Face {
  cv::Vec3f normal;
  std::array<cv::Vec3f, 4> corners;
};

const std::array<Face, 6> &cubeFaces() {
  static const std::array<Face, 6> faces = {{
      {{1, 0, 0}, {{{1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}}}},
      {{-1, 0, 0}, {{{0, 0, 0}, {0, 0, 1}, {0, 1, 1}, {0, 1, 0}}}},
      {{0, 1, 0}, {{{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}}}},
      {{0, -1, 0}, {{{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}}}},
      {{0, 0, 1}, {{{0, 0, 1}, {1, 0, 1}, {1, 1, 1}, {0, 1, 1}}}},
      {{0, 0, -1}, {{{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}}}},
  }};
  return faces;
}

// One light, from over the player's left shoulder and above, so the top face is
// the brightest and the two sides fall away from it.
cv::Vec3f lightDirection() {
  const cv::Vec3f l(-0.42f, -0.62f, 0.66f);
  return l * (1.0f / std::sqrt(l.dot(l)));
}

float shade(const cv::Vec3f &normal) {
  static const cv::Vec3f kLight = lightDirection();
  return 0.34f + 0.66f * std::max(0.0f, normal.dot(kLight));
}

cv::Scalar lit(cv::Scalar color, float brightness) {
  return cv::Scalar(color[0] * brightness, color[1] * brightness,
                    color[2] * brightness, color[3]);
}

// A box in cell coordinates. One cell is a tetracube, and the bedrock is a box
// that happens to be four by four.
struct Box {
  float x0 = 0, y0 = 0, z0 = 0, x1 = 1, y1 = 1, z1 = 1;
  cv::Scalar color;
  float depth = 0.0f;
};

Box cellBox(const Cell &c, cv::Scalar color) {
  Box box;
  box.x0 = static_cast<float>(c.x);
  box.y0 = static_cast<float>(c.y);
  box.z0 = static_cast<float>(c.z);
  box.x1 = box.x0 + 1.0f;
  box.y1 = box.y0 + 1.0f;
  box.z1 = box.z0 + 1.0f;
  box.color = color;
  return box;
}

void drawBox(const Projector &project, const Box &box) {
  using namespace cv::v4d::nvg;
  // The gap between cubes is what makes a stack of them read as a stack.
  constexpr float kInset = 0.05f;
  for (const Face &face : cubeFaces()) {
    if (!project.faces(face.normal[0], face.normal[1], face.normal[2]))
      continue;
    beginPath();
    for (size_t i = 0; i < face.corners.size(); ++i) {
      const cv::Vec3f corner = face.corners[i];
      // An inset corner is the near edge of that face's own plane.
      const float x = corner[0] > 0.5f ? box.x1 - kInset : box.x0 + kInset;
      const float y = corner[1] > 0.5f ? box.y1 - kInset : box.y0 + kInset;
      const float z = corner[2] > 0.5f ? box.z1 - kInset : box.z0 + kInset;
      const cv::Point2f p = project(x, y, z);
      if (i == 0)
        moveTo(p.x, p.y);
      else
        lineTo(p.x, p.y);
    }
    closePath();
    const float brightness = shade(face.normal);
    fillColor(lit(box.color, brightness));
    fill();
    strokeColor(lit(box.color, brightness * 0.55f));
    strokeWidth(1.0f);
    stroke();
  }
}

void drawWireBox(const Projector &project, float x0, float y0, float z0,
                 float x1, float y1, float z1, cv::Scalar color, float width) {
  using namespace cv::v4d::nvg;
  static const std::array<std::array<int, 2>, 12> kEdges = {{
      {{0, 1}},
      {{1, 2}},
      {{2, 3}},
      {{3, 0}},
      {{4, 5}},
      {{5, 6}},
      {{6, 7}},
      {{7, 4}},
      {{0, 4}},
      {{1, 5}},
      {{2, 6}},
      {{3, 7}},
  }};
  static const std::array<cv::Vec3f, 8> kCorners = {
      cv::Vec3f(0, 0, 0), cv::Vec3f(1, 0, 0), cv::Vec3f(1, 1, 0),
      cv::Vec3f(0, 1, 0), cv::Vec3f(0, 0, 1), cv::Vec3f(1, 0, 1),
      cv::Vec3f(1, 1, 1), cv::Vec3f(0, 1, 1)};
  strokeColor(color);
  strokeWidth(width);
  for (const auto &edge : kEdges) {
    const cv::Vec3f &a = kCorners[size_t(edge[0])];
    const cv::Vec3f &b = kCorners[size_t(edge[1])];
    const cv::Point2f pa = project(x0 + a[0] * (x1 - x0), y0 + a[1] * (y1 - y0),
                                   z0 + a[2] * (z1 - z0));
    const cv::Point2f pb = project(x0 + b[0] * (x1 - x0), y0 + b[1] * (y1 - y0),
                                   z0 + b[2] * (z1 - z0));
    beginPath();
    moveTo(pa.x, pa.y);
    lineTo(pb.x, pb.y);
    stroke();
  }
}

void drawLabel(float x, float y, const cv::String &value, cv::Scalar color,
               float size, int align, bool boldFace) {
  using namespace cv::v4d::nvg;
  fontFace(boldFace ? "sans-bold" : "sans");
  fontSize(size);
  fillColor(color);
  textAlign(align);
  text(x, y, value.c_str(), value.c_str() + value.size());
}

} // namespace

// ---------------------------------------------------------------------------

class TetralisDemoPlan : public V4DPlan {
  using K = V4D::Keys;
  using M = Mouse;

  // Written by the worker, read by the two drawing passes. This is a copy, not
  // a view: no drawing thread ever touches tetralis::Game.
  struct Snapshot {
    std::array<Kind, kSlots> cells{};
    Cells active;
    Cells resting;
    Cells spines;
    Cell cursor;
    bool pieceActive = false;
    bool legal = true;
    bool struck = false;
    bool betweenTurns = false;
    bool showHud = true;
    bool showHelp = false;
    bool razeable = false;
    bool seedable = false;
    bool paused = false;
    bool padSeen = false;
    int piece = -1;
    int depth = 1;
    int charges = 0;
    int chargeProgress = 0;
    int score = 0;
    int cleared = 0;
    int bestChain = 0;
    int gravity = 17;
    int lockDelayLeft = tetralis::kLockDelayFrames;
    int betweenFramesLeft = 0;
    int bagSize = 0;
    int longSpine = 0;
    int speed = 1;
    uint64_t frame = 0;
    uint64_t seed = 1;
    std::array<int, kPreviews> preview{};
    cv::String toast;
    float yaw = 0.62f;
    float pitch = 0.44f;
    float zoom = 1.0f;
  };

  // Written by the GUI thread, read by the worker: the settings the player
  // changes, and the requests the worker turns into engine commands.
  struct Shared {
    bool fullscreen = false;
    bool showHud = true;
    bool showHelp = false;
    bool imguiWantsKeyboard = false;
    int speed = 1;
    // Set by '.': one frame runs even though speed is 0, and it is cleared by
    // the worker in the same step so that holding the key does not free-run.
    bool singleStep = false;
    bool restart = false;
  };

  Snapshot snap_;
  Shared local_;
  tetralis::Game game_;

  // Worker-local: the held keys behind the DAS/ARR auto-shift, and the commands
  // this frame's key presses produced.
  //
  // Three sources, kept apart so that none of them can clear another's hold:
  // the keyboard, the gamepad's buttons, and the gamepad's sticks. A command is
  // held if any of the three says so (§4.1 DAS/ARR needs a stable held flag,
  // and a player who holds a direction on the pad while nudging it on the
  // keyboard should not have the two fight).
  std::array<bool, tetralis::kCmdCount> keyHeld_{};
  std::array<bool, tetralis::kCmdCount> padButtonHeld_{};
  std::array<bool, tetralis::kCmdCount> padAxisHeld_{};
  std::vector<Cmd> presses_;
  // The last value the runtime reported per stick axis, per pad. The stick's
  // held directions are recomputed from this every frame rather than from the
  // events themselves, because a stick reports where it is rather than whether
  // it moved: an event says "now at 0.8", and only the position can say whether
  // that is a hold.
  std::map<int, std::array<float, 7>> padAxes_;
  bool padSeen_ = false;
  float yaw_ = 0.62f;
  float pitch_ = 0.44f;
  float zoom_ = 1.0f;
  bool dragging_ = false;

  Event<M> pressLeft_ = E<M>(M::PRESS, M::LEFT);
  Event<M> releaseLeft_ = E<M>(M::RELEASE, M::LEFT);
  Event<M> scroll_ = E<M>(M::SCROLL);
  Event<M> drag_ = E<M>(M::DRAG);
  Event<Keyboard> keyPress_ = E<Keyboard>(Keyboard::PRESS);
  Event<Keyboard> keyRelease_ = E<Keyboard>(Keyboard::RELEASE);
  // Every gamepad event, all types: buttons carry edges and repeats, the sticks
  // carry motion. One edge and a dispatch below keeps the two apart.
  Event<Joystick> padEvents_ = E<Joystick>();

  Property<cv::Size> size_ = P<cv::Size>(K::SIZE);

  // ---- keys -------------------------------------------------------------

  // The key a command is held down on. While a piece is falling these move the
  // piece; between turns the same keys move the §9.1 target cursor, which is
  // the engine's business and not this file's.
  static int commandOf(Keyboard::Key key) {
    switch (key) {
    case Keyboard::LEFT:
    case Keyboard::A:
      return tetralis::kMoveXNeg;
    case Keyboard::RIGHT:
    case Keyboard::D:
      return tetralis::kMoveXPos;
    case Keyboard::E:
      return tetralis::kMoveYPos;
    case Keyboard::Q:
      return tetralis::kMoveYNeg;
    case Keyboard::UP:
    case Keyboard::W:
      return tetralis::kMoveZPos;
    case Keyboard::DOWN:
    case Keyboard::S:
      return tetralis::kMoveZNeg;
    case Keyboard::I:
      return tetralis::kRotXPos;
    case Keyboard::K:
      return tetralis::kRotXNeg;
    case Keyboard::J:
      return tetralis::kRotYPos;
    case Keyboard::L:
      return tetralis::kRotYNeg;
    case Keyboard::U:
      return tetralis::kRotZPos;
    case Keyboard::O:
      return tetralis::kRotZNeg;
    case Keyboard::SPACE:
      return tetralis::kHardDrop;
    case Keyboard::C:
      return tetralis::kSettle;
    case Keyboard::Z:
      return tetralis::kRaze;
    case Keyboard::X:
      return tetralis::kSeed;
    case Keyboard::ENTER:
      return tetralis::kAdvance;
    default:
      return -1;
    }
  }

  void onPresses(const Keyboard::List &presses, const Keyboard::List &releases,
                 Shared &shared) {
    for (const auto &event : releases) {
      const int cmd = commandOf(event->key());
      if (cmd >= 0)
        keyHeld_[size_t(cmd)] = false;
    }
    // While ImGui owns the keyboard the keys are the panel's, not the game's.
    if (shared.imguiWantsKeyboard)
      return;
    for (const auto &event : presses) {
      const Keyboard::Key key = event->key();
      switch (key) {
      case Keyboard::P:
        shared.speed = shared.speed == 0 ? 1 : 0;
        continue;
      case Keyboard::PERIOD:
        shared.speed = 0;
        keyHeld_.fill(false);
        shared.singleStep = true;
        continue;
      case Keyboard::R:
        shared.restart = true;
        continue;
      case Keyboard::H:
        shared.showHelp = !shared.showHelp;
        continue;
      case Keyboard::TAB:
        shared.showHud = !shared.showHud;
        continue;
      case Keyboard::F:
      case Keyboard::F11:
        shared.fullscreen = !shared.fullscreen;
        continue;
      case Keyboard::ESCAPE:
        shared.speed = 0;
        continue;
      default:
        break;
      }
      const int cmd = commandOf(key);
      if (cmd < 0)
        continue;
      keyHeld_[size_t(cmd)] = true;
      presses_.push_back(static_cast<Cmd>(cmd));
    }
  }

  // ---- gamepad ----------------------------------------------------------
  //
  // The pad speaks the same command language as the keyboard, so it is the same
  // two things: an edge, which is a command, and a hold, which feeds §4.1's
  // auto-shift. What differs is where the hold comes from -- a stick reports
  // its position, not its edges -- and that is handled by recomputing the
  // stick's holds from the last known positions every frame (refreshPadAxes).
  //
  // Every pad is summed into one set of holds, so a second pad can take over
  // mid-game without the first one's stale holds sticking around: the button
  // holds are cleared on RELEASE and the stick holds are recomputed, so what is
  // left is what is physically down right now.

  // How far a stick has to be pushed to count as held. Well above the runtime's
  // own deadzone of 0.10, so that a pad resting with a little drift in it is
  // not read as a hold; the runtime stops reporting an axis once it is back
  // inside its deadzone and says so on the way, which is what clears the hold.
  static constexpr float kPadEngage = 0.5f;

  // How far the shaft turns per frame at a fully deflected right stick. Small
  // enough to aim with, large enough that the shaft comes all the way round
  // while a stick is pushed and held rather than only while it is moving.
  static constexpr float kOrbitRate = 0.035f;

  // The command a pad button is held down on, or -1 for a button that is not a
  // command. The buttons that are not commands (start, back, guide) are handled
  // by onPadButton directly.
  //
  // A pad has 15 buttons and the game needs 6 rotations, a hard drop, a Settle
  // and three housekeeping ones, so all six rotations cannot live on the four
  // face and thumb buttons alone; the shoulders take two of them. That leaves
  // the d-pad and the left stick between them to cover all three movement axes:
  // the stick takes x and z, which is what the arrow keys do, and the d-pad
  // takes x and y. Every direction and every rotation is therefore one control
  // away, with nothing behind a modifier.
  static constexpr int commandOf(Joystick::Button button) {
    switch (button) {
    // The d-pad moves the piece: x across on left and right, y towards the
    // front and back wall on up and down. z is the left stick's up and down.
    case Joystick::DPAD_LEFT:
      return tetralis::kMoveXNeg;
    case Joystick::DPAD_RIGHT:
      return tetralis::kMoveXPos;
    case Joystick::DPAD_UP:
      return tetralis::kMoveYPos;
    case Joystick::DPAD_DOWN:
      return tetralis::kMoveYNeg;
    // The shoulders are two of the six rotations, so that none of the six is
    // more than one button away.
    case Joystick::LB:
      return tetralis::kRotXPos;
    case Joystick::RB:
      return tetralis::kRotXNeg;
    case Joystick::A:
      return tetralis::kHardDrop;
    case Joystick::B:
      return tetralis::kSettle;
    // The face buttons spin the piece about the vertical axis, which is the
    // turn a player makes most often.
    case Joystick::X:
      return tetralis::kRotZPos;
    case Joystick::Y:
      return tetralis::kRotZNeg;
    case Joystick::LEFT_THUMB:
      return tetralis::kRotYPos;
    case Joystick::RIGHT_THUMB:
      return tetralis::kRotYNeg;
    default:
      break;
    }
    return -1;
  }

  // The layout above is checked rather than trusted. A pad has fifteen buttons
  // and the game has more commands than that, so the map has to be tight, and
  // the ways it can be quietly wrong are all invisible until someone holds a
  // controller: a rotation nobody is bound to, two buttons fighting over one
  // command, or a direction reachable from nowhere. This version of the map
  // shipped a rotation nobody could reach, because four face and thumb buttons
  // cannot hold six rotations and the bumpers were already spoken for.
  //
  // kMoveZPos and kMoveZNeg are the left stick's up and down rather than a
  // button's, so the movement check counts them as reached by the pad as a
  // whole.
  static constexpr int kPadAxisCmds[] = {
      tetralis::kMoveXNeg, tetralis::kMoveXPos, tetralis::kMoveZPos,
      tetralis::kMoveZNeg, tetralis::kRaze,     tetralis::kSeed};

  static constexpr bool padLayoutIsComplete() {
    bool seen[tetralis::kCmdCount] = {};
    // NO_BUTTON is 0 and DPAD_LEFT is 15, so this is every button on a pad.
    for (int button = 1; button <= 15; ++button) {
      const int cmd = commandOf(static_cast<Joystick::Button>(button));
      if (cmd < 0)
        continue;
      // Two buttons on one command would double every press of it.
      if (seen[cmd])
        return false;
      seen[cmd] = true;
    }
    for (const int cmd : kPadAxisCmds)
      seen[cmd] = true;
    // Every turn and every direction has to be on the pad somewhere.
    for (int cmd = tetralis::kMoveXNeg; cmd <= tetralis::kSeed; ++cmd)
      if (!seen[cmd])
        return false;
    return true;
  }

  // The three buttons that are not commands, and what they do.
  void onPadButton(Joystick::Button button, Shared &shared) {
    switch (button) {
    case Joystick::START:
      shared.speed = shared.speed == 0 ? 1 : 0;
      break;
    case Joystick::BACK:
      shared.restart = true;
      break;
    case Joystick::GUIDE:
      shared.showHelp = !shared.showHelp;
      break;
    default:
      break;
    }
  }

  // Recompute what the sticks are holding, from the last positions the runtime
  // reported. An axis that has returned to its deadzone reports that too (see
  // poll_joystick_events), so a position read here is the position the stick is
  // at now and not the last one it was pushed to -- which is what stops a stick
  // that has been let go from holding a command for the rest of the game.
  void refreshPadAxes() {
    // The stick's most committed direction wins, so that a pad resting off to
    // one side does not read as neutral.
    const float leftX = stickExtent(Joystick::LEFT_X);
    const float leftY = stickExtent(Joystick::LEFT_Y);
    // GLFW's y axes point down, so a negative value is up the shaft.
    padAxisHeld_.fill(false);
    if (leftX < -kPadEngage)
      padAxisHeld_[size_t(tetralis::kMoveXNeg)] = true;
    else if (leftX > kPadEngage)
      padAxisHeld_[size_t(tetralis::kMoveXPos)] = true;
    if (leftY < -kPadEngage)
      padAxisHeld_[size_t(tetralis::kMoveZPos)] = true;
    else if (leftY > kPadEngage)
      padAxisHeld_[size_t(tetralis::kMoveZNeg)] = true;
    // The triggers are the pad's Raze and Seed, §9.1's two commands on the
    // target cursor: the same places the keyboard's Z and X have.
    if (stickExtent(Joystick::LEFT_TRIGGER) > kPadEngage)
      padAxisHeld_[size_t(tetralis::kRaze)] = true;
    if (stickExtent(Joystick::RIGHT_TRIGGER) > kPadEngage)
      padAxisHeld_[size_t(tetralis::kSeed)] = true;

    // The right stick orbits, like the mouse drag does, but it is a camera and
    // not a command -- so it never reaches the engine.
    //
    // It is read as a rate and not as a movement: a stick reports motion and
    // then nothing while it is held steady, so following its delta would spin
    // the shaft for one frame per nudge and then stop, which is not what
    // holding a stick sideways means. Reading its position instead turns it
    // into the usual analogue look control -- hold it and the shaft keeps
    // turning, let go and it stops, in as many frames as you like.
    yaw_ += stickExtent(Joystick::RIGHT_X) * kOrbitRate;
    pitch_ = std::clamp(pitch_ - stickExtent(Joystick::RIGHT_Y) * kOrbitRate,
                        -0.25f, 1.4f);
  }

  // How far an axis is pushed, across all pads: the largest excursion wins.
  float stickExtent(Joystick::Axis axis) const {
    float best = 0.0f;
    for (const auto &entry : padAxes_) {
      const float value = entry.second[size_t(axis)];
      if (std::fabs(value) > std::fabs(best))
        best = value;
    }
    return best;
  }

  void onPad(const Joystick::List &events, Shared &shared) {
    for (const auto &event : events) {
      padSeen_ = true;
      const Joystick::Axis axis = event->axis();
      if (event->is(Joystick::MOVE)) {
        // Motion, not a button. The value is the position; the delta is how far
        // it moved since the last frame, and is zero on the repeats the runtime
        // sends while a stick is held steady.
        if (event->joystick() >= 0 && size_t(axis) < 7u) {
          // An axis reports a position, never a press, so §9.1's Raze and Seed
          // need their edge made here: pulling the trigger past the threshold
          // is the press, letting it back is the release.
          const float previous = padAxes_[event->joystick()][size_t(axis)];
          const float value = event->value();
          padAxes_[event->joystick()][size_t(axis)] = value;
          const int trigger =
              axis == Joystick::LEFT_TRIGGER
                  ? tetralis::kRaze
                  : (axis == Joystick::RIGHT_TRIGGER ? tetralis::kSeed : -1);
          if (trigger >= 0 && previous <= kPadEngage && value > kPadEngage)
            presses_.push_back(static_cast<Cmd>(trigger));
        }
        // The right stick orbits, like the mouse drag does, but it is a camera
        // and not a command -- so it never reaches the engine. refreshPadAxes
        // below turns its position into the orbit.
        continue;
      }
      const Joystick::Button button = event->button();
      const int cmd = commandOf(button);
      if (cmd >= 0) {
        // A repeat is not an edge: §4.1's auto-shift already repeats a hold, so
        // pushing here too would move the piece twice per step.
        if (event->is(Joystick::PRESS)) {
          presses_.push_back(static_cast<Cmd>(cmd));
          padButtonHeld_[size_t(cmd)] = true;
        } else if (event->is(Joystick::RELEASE)) {
          padButtonHeld_[size_t(cmd)] = false;
        }
        continue;
      }
      if (event->is(Joystick::PRESS))
        onPadButton(button, shared);
    }
    refreshPadAxes();
  }

  // A command is held if the keyboard, a pad button or a stick says so. §4.1
  // counts the frames a command is held for, so all three have to agree on the
  // same flag rather than overwrite each other.
  static bool isHeld(const std::array<bool, tetralis::kCmdCount> &key,
                     const std::array<bool, tetralis::kCmdCount> &button,
                     const std::array<bool, tetralis::kCmdCount> &axis,
                     int cmd) {
    return key[size_t(cmd)] || button[size_t(cmd)] || axis[size_t(cmd)];
  }

  void onMouse(const M::List &presses, const M::List &releases,
               const M::List &scrolls, const M::List &drags) {
    for (size_t i = 0; i < presses.size(); ++i)
      dragging_ = true;
    for (size_t i = 0; i < releases.size(); ++i)
      dragging_ = false;
    for (const auto &event : scrolls)
      zoom_ = std::clamp(zoom_ * (event->data().y > 0 ? 1.1f : 1.0f / 1.1f),
                         0.4f, 2.6f);
    for (const auto &event : drags) {
      if (!dragging_)
        continue;
      yaw_ += event->data().x * 0.01f;
      pitch_ = std::clamp(pitch_ + event->data().y * 0.01f, -0.25f, 1.4f);
    }
  }

  // ---- the worker -------------------------------------------------------

  void publish() {
    const tetralis::Board &board = game_.board();
    snap_.cells.fill(Kind::kEmpty);
    for (int z = tetralis::kShaftZMin; z <= tetralis::kShaftZMax; ++z)
      for (int y = 0; y < tetralis::kFootY; ++y)
        for (int x = 0; x < tetralis::kFootX; ++x) {
          const Cell c{x, y, z};
          snap_.cells[slot(c)] = board.at(c);
        }
    snap_.active = game_.activeCells();
    snap_.resting = game_.restingCells();
    const std::pair<Cells, int> spines = game_.spinePreview();
    snap_.spines = spines.first;
    snap_.longSpine = spines.second;
    snap_.pieceActive = game_.active();
    snap_.legal = game_.cellsLegal();
    snap_.struck = game_.struck();
    snap_.betweenTurns = game_.phase() == tetralis::kBetweenTurns;
    snap_.piece = game_.piece();
    snap_.cursor = game_.cursor();
    snap_.depth = game_.depth();
    snap_.charges = game_.charges();
    snap_.chargeProgress = game_.chargeProgress();
    snap_.score = game_.score();
    snap_.cleared = game_.cellsCleared();
    snap_.bestChain = static_cast<int>(game_.bestChain());
    snap_.gravity = game_.gravity();
    snap_.lockDelayLeft = game_.lockDelay();
    snap_.betweenFramesLeft = game_.betweenFramesLeft();
    snap_.bagSize = game_.bagSize();
    snap_.razeable = game_.cursorRazeable();
    snap_.seedable = game_.cursorSeedable();
    snap_.frame = game_.frame();
    snap_.seed = game_.seed();
    snap_.toast = game_.toast();
    const std::vector<int> next = game_.preview(kPreviews);
    for (size_t i = 0; i < snap_.preview.size(); ++i)
      snap_.preview[i] = i < next.size() ? next[i] : -1;
  }

  void service(Shared &shared) {
    if (shared.restart) {
      shared.restart = false;
      // The same seed, so a restart is the same game again -- which is the
      // point of §14 and the easiest way to see it.
      game_.startGame(game_.seed());
      // A restart drops every hold: the keys and buttons that are physically
      // down at that moment report no edge, so their holds would otherwise
      // outlive the game they were meant for.
      keyHeld_.fill(false);
      padButtonHeld_.fill(false);
      presses_.clear();
    }

    tetralis::Input input;
    for (int i = 0; i < tetralis::kCmdCount; ++i)
      input.held[i] = isHeld(keyHeld_, padButtonHeld_, padAxisHeld_, i);
    input.softDrop = input.held[size_t(tetralis::kMoveZNeg)];
    // This frame's edges: the presses the input nodes queued, taken and dropped
    // here so that a frame at speed 4 does not replay them four times.
    const std::vector<Cmd> edges = presses_;
    presses_.clear();

    // The engine only ever sees whole frames and an explicit command list. A
    // rendered frame is one engine frame times the speed, which is what keeps a
    // replay exact -- there is no clock anywhere near this.
    int frames = shared.speed;
    if (shared.singleStep) {
      frames = 1;
      shared.singleStep = false;
    }
    snap_.paused = frames == 0;
    for (int frame = 0; frame < frames; ++frame) {
      // A key press is an edge and happens once. Repeating the command list on
      // the speed frames would rotate or move twice per step; the holds in the
      // same input are what the engine repeats on its own (§4.1).
      tetralis::Input frameInput = input;
      frameInput.cmds = frame == 0 ? edges : std::vector<Cmd>();
      game_.step(frameInput);
    }

    publish();
    snap_.speed = shared.speed;
    snap_.showHud = shared.showHud;
    snap_.showHelp = shared.showHelp;
    snap_.padSeen = padSeen_;
    snap_.yaw = yaw_;
    snap_.pitch = pitch_;
    snap_.zoom = zoom_;
  }

  // ---- drawing ----------------------------------------------------------

  static Projector projectorFor(const Snapshot &snap, const cv::Size &sz) {
    // The shaft is 4 wide, 4 deep and 15 tall. Fitting its diagonal into the
    // frame keeps the whole volume on screen at every orbit angle.
    const float fit = std::min(sz.width * 0.66f, sz.height * 0.92f) / 17.0f;
    return Projector(cv::Point2f(sz.width * 0.5f, sz.height * 0.63f),
                     fit * snap.zoom, snap.yaw, snap.pitch);
  }

  static std::vector<Box> sceneBoxes(const Snapshot &snap) {
    std::vector<Box> boxes;
    // The bedrock the whole shaft stands on, as one slab.
    Box bedrock;
    bedrock.x1 = static_cast<float>(tetralis::kFootX);
    bedrock.y1 = static_cast<float>(tetralis::kFootY);
    bedrock.color = kBedrock;
    boxes.push_back(bedrock);
    for (int z = tetralis::kShaftZMin; z <= tetralis::kShaftZMax; ++z)
      for (int y = 0; y < tetralis::kFootY; ++y)
        for (int x = 0; x < tetralis::kFootX; ++x) {
          const Kind kind = kindAt(snap.cells, Cell{x, y, z});
          if (kind == Kind::kEmpty)
            continue;
          boxes.push_back(
              cellBox(Cell{x, y, z}, kind == Kind::kRime ? kRime : kNormal));
        }
    // §16: where the piece would land, and which Spines that would complete.
    for (const Cell &c : snap.resting)
      boxes.push_back(cellBox(c, kLanding));
    for (const Cell &c : snap.spines)
      boxes.push_back(cellBox(c, kSpine));
    if (snap.pieceActive) {
      for (const Cell &c : snap.active)
        boxes.push_back(cellBox(c, kPiece));
    }
    return boxes;
  }

  static void drawScene(const Snapshot &snap, const cv::Size &sz) {
    using namespace cv::v4d::nvg;
    const Projector project = projectorFor(snap, sz);

    // The volume the rules allow: z = 1 .. 15, with ticks every four layers so
    // that the vertical scale is readable at a glance.
    drawWireBox(project, 0.0f, 0.0f, static_cast<float>(tetralis::kShaftZMin),
                static_cast<float>(tetralis::kFootX),
                static_cast<float>(tetralis::kFootY),
                static_cast<float>(tetralis::kShaftZMax + 1), kWire, 1.0f);
    for (int z = 4; z <= tetralis::kShaftZMax; z += 4) {
      const cv::Point2f a = project(0.0f, 0.0f, static_cast<float>(z));
      const cv::Point2f b = project(0.7f, 0.0f, static_cast<float>(z));
      beginPath();
      moveTo(a.x, a.y);
      lineTo(b.x, b.y);
      strokeColor(kWire);
      strokeWidth(1.5f);
      stroke();
      drawLabel(a.x - 6.0f, a.y, cv::String(std::to_string(z)), kFaint, 11.0f,
                NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE, false);
    }

    std::vector<Box> boxes = sceneBoxes(snap);
    for (Box &box : boxes)
      box.depth =
          project.depth((box.x0 + box.x1) * 0.5f, (box.y0 + box.y1) * 0.5f,
                        (box.z0 + box.z1) * 0.5f);
    std::sort(boxes.begin(), boxes.end(),
              [](const Box &a, const Box &b) { return a.depth > b.depth; });
    for (const Box &box : boxes)
      drawBox(project, box);

    // The §9.1 target cursor, drawn where Raze and Seed would act.
    if (!snap.struck && snap.charges > 0 && !snap.pieceActive) {
      const cv::Scalar color =
          snap.razeable ? kCharge : (snap.seedable ? kCursor : kFaint);
      drawWireBox(project, static_cast<float>(snap.cursor.x),
                  static_cast<float>(snap.cursor.y),
                  static_cast<float>(snap.cursor.z),
                  static_cast<float>(snap.cursor.x + 1),
                  static_cast<float>(snap.cursor.y + 1),
                  static_cast<float>(snap.cursor.z + 1), color, 2.0f);
    }
  }

  static void drawHud(const Snapshot &snap, const cv::Size &sz) {
    using namespace cv::v4d::nvg;
    constexpr float kPad = 22.0f;
    constexpr float kLine = 21.0f;

    drawLabel(kPad, kPad, "TETRALIS 3D", kText, 22.0f,
              NVG_ALIGN_LEFT | NVG_ALIGN_TOP, true);
    float y = kPad + 32.0f;
    drawLabel(kPad, y, "depth " + std::to_string(snap.depth), kDim, 14.0f,
              NVG_ALIGN_LEFT | NVG_ALIGN_TOP, false);
    y += kLine;
    drawLabel(kPad, y, "score " + std::to_string(snap.score), kDim, 14.0f,
              NVG_ALIGN_LEFT | NVG_ALIGN_TOP, false);
    y += kLine;
    drawLabel(kPad, y, "cleared " + std::to_string(snap.cleared) + " cells",
              kDim, 14.0f, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, false);
    y += kLine;
    drawLabel(kPad, y, "best chain x" + std::to_string(snap.bestChain), kDim,
              14.0f, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, false);

    // Charges as pips, with the distance to the next one underneath.
    const float right = sz.width - kPad;
    y = kPad;
    drawLabel(right, y, "charges", kDim, 14.0f, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP,
              false);
    y += 19.0f;
    for (int i = 0; i < tetralis::kChargeCap; ++i) {
      const float left = right - 15.0f - static_cast<float>(i) * 20.0f;
      beginPath();
      roundedRect(left, y, 15.0f, 15.0f, 3.0f);
      fillColor(i < snap.charges ? kCharge : kChargeEmpty);
      fill();
    }
    y += 23.0f;
    drawLabel(
        right, y,
        "next in " +
            std::to_string(tetralis::kPiecesPerCharge - snap.chargeProgress) +
            " pieces",
        kDim, 13.0f, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP, false);

    // The next pieces as top-view footprints. In a 4x4 shaft the footprint is
    // the part a player actually plans with.
    const float cell = 13.0f;
    float px = kPad;
    y = sz.height - kPad - 5.0f * cell - 22.0f;
    drawLabel(px, y, "next", kDim, 14.0f, NVG_ALIGN_LEFT | NVG_ALIGN_TOP,
              false);
    y += 20.0f;
    for (size_t i = 0; i < snap.preview.size(); ++i) {
      const float ox = px + static_cast<float>(i) * 5.0f * cell;
      beginPath();
      roundedRect(ox - cell, y - cell, 5.0f * cell + cell, 5.0f * cell, 4.0f);
      fillColor(cv::Scalar(32, 32, 40));
      fill();
      if (snap.preview[i] < 0)
        continue;
      for (const Cell &c :
           tetralis::pieceInfo(snap.preview[i]).orientations[0].cells) {
        beginPath();
        roundedRect(ox + static_cast<float>(c.x) * cell,
                    y + static_cast<float>(c.y) * cell, cell - 1.5f,
                    cell - 1.5f, 2.0f);
        fillColor(kPiece);
        fill();
      }
    }

    // What the engine is doing right now, along the bottom right.
    const float bottom = sz.height - kPad;
    if (snap.struck) {
      drawLabel(right, bottom - 66.0f, "STRUCK", kWarn, 34.0f,
                NVG_ALIGN_RIGHT | NVG_ALIGN_TOP, true);
      drawLabel(right, bottom - 24.0f,
                "score " + std::to_string(snap.score) +
                    "   press R to run the same seed again",
                kDim, 14.0f, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP, false);
    } else {
      cv::String status;
      if (snap.paused)
        status = "paused";
      else if (snap.betweenTurns)
        status = "between turns, " + std::to_string(snap.betweenFramesLeft) +
                 " frames left";
      else if (!snap.legal)
        status = "spawning into occupied cells";
      else if (snap.longSpine > 0)
        status = std::string("spine of ") + std::to_string(snap.longSpine) +
                 " on landing";
      else
        status = tetralis::pieceName(snap.piece);
      drawLabel(right, bottom - 44.0f, status, kDim, 15.0f,
                NVG_ALIGN_RIGHT | NVG_ALIGN_TOP, false);
      drawLabel(right, bottom - 24.0f,
                "gravity " + std::to_string(snap.gravity) + "f   lock " +
                    std::to_string(snap.lockDelayLeft) + "f   bag " +
                    std::to_string(snap.bagSize) + "   seed " +
                    std::to_string(snap.seed) + "   frame " +
                    std::to_string(snap.frame),
                kDim, 13.0f, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP, false);
    }

    // The last resolution: the only feedback the rules give, and the reason
    // chains matter.
    if (!snap.toast.empty())
      drawLabel(sz.width * 0.5f, kPad, snap.toast, kSpine, 20.0f,
                NVG_ALIGN_CENTER | NVG_ALIGN_TOP, true);
    // A pad needs no separate onboarding: the hint says one is in play, and H
    // lists its buttons alongside the keys.
    drawLabel(
        sz.width * 0.5f, bottom,
        snap.padSeen
            ? "drag or right stick to orbit, wheel to zoom, H for controls"
            : "drag to orbit, wheel to zoom, H for help, P pause, R restart",
        kFaint, 13.0f, NVG_ALIGN_CENTER | NVG_ALIGN_BOTTOM, false);
  }

  static void drawHelp(const Snapshot &snap, const cv::Size &sz) {
    using namespace cv::v4d::nvg;
    struct Row {
      const char *keys;
      const char *what;
    };
    static const Row kRows[] = {
        {"left / right, A / D", "x, across the footprint"},
        {"Q / E", "y, towards the front and back wall"},
        {"up / down, W / S", "z: lift, and soft drop on S"},
        {"space", "hard drop, locks at once"},
        {"I / K, J / L, U / O", "rotate about x, y, z"},
        {"Z / X", "Raze / Seed under the cursor"},
        {"C", "Settle: Plumb, this piece blocking"},
        {"enter", "skip the between-turns window"},
        {"P / .", "pause / one frame"},
        {"R", "restart on the same seed"},
        {"H / tab", "this panel / the HUD"},
        {"F, F11", "fullscreen"},
    };
    // The pad says the same things. Every pad is live at once, so a second one
    // can be picked up mid-game and its sticks become the ones in charge.
    static const Row kPadRows[] = {
        {"left stick", "x across, z: lift and soft drop"},
        {"d-pad", "x across on left/right, y on up/down"},
        {"A / B", "hard drop / Settle"},
        {"X / Y", "rotate about z, the vertical axis"},
        {"L3 / R3", "rotate about y"},
        {"LB / RB", "rotate about x"},
        {"LT / RT", "Raze / Seed under the cursor"},
        {"right stick", "orbit the shaft"},
        {"start / back / guide", "pause / restart / this panel"},
    };
    const float w = 470.0f;
    const float rows = float(sizeof(kRows) / sizeof(kRows[0]));
    const float padRows = float(sizeof(kPadRows) / sizeof(kPadRows[0]));
    const float h = 74.0f + 23.0f * rows + 30.0f + 23.0f * padRows;
    const float x = (sz.width - w) * 0.5f;
    const float y = (sz.height - h) * 0.5f;
    beginPath();
    roundedRect(x, y, w, h, 8.0f);
    fillColor(cv::Scalar(24, 24, 30, 244));
    fill();
    strokeColor(cv::Scalar(64, 66, 78));
    strokeWidth(1.0f);
    stroke();

    drawLabel(x + 24.0f, y + 20.0f, "CONTROLS", kText, 18.0f,
              NVG_ALIGN_LEFT | NVG_ALIGN_TOP, true);
    float ty = y + 52.0f;
    for (const Row &row : kRows) {
      drawLabel(x + 24.0f, ty, row.keys, kCharge, 14.0f,
                NVG_ALIGN_LEFT | NVG_ALIGN_TOP, false);
      drawLabel(x + 210.0f, ty, row.what, kDim, 14.0f,
                NVG_ALIGN_LEFT | NVG_ALIGN_TOP, false);
      ty += 23.0f;
    }
    ty += 12.0f;
    drawLabel(x + 24.0f, ty,
              snap.padSeen ? "GAMEPAD" : "GAMEPAD (no pad seen yet)", kText,
              15.0f, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, true);
    ty += 22.0f;
    for (const Row &row : kPadRows) {
      drawLabel(x + 24.0f, ty, row.keys, kCharge, 14.0f,
                NVG_ALIGN_LEFT | NVG_ALIGN_TOP, false);
      drawLabel(x + 210.0f, ty, row.what, kDim, 14.0f,
                NVG_ALIGN_LEFT | NVG_ALIGN_TOP, false);
      ty += 23.0f;
    }
  }

public:
  // The shared declarations belong here and not in setup(): gui() runs on the
  // display thread and setup() on the worker, and the display thread gets here
  // first -- so a _shared() in setup() is already too late, and the first
  // shared access from gui() would be rejected as a non-shared variable used as
  // shared.
  explicit TetralisDemoPlan(uint64_t seed) {
    _shared(snap_);
    _shared(local_);
    game_.startGame(seed);
  }

  void setup() override {
    set(GlobalState::Keys::TIME_TRACKER, V(false));
    set(GlobalState::Keys::SHOW_FRAME_TIME, V(false));
    set(K::CLEAR_COLOR, V(kBackdrop));
  }

  void gui() override {
    imgui(
        [](Shared &shared, const Snapshot &snap) {
          using namespace ImGui;
          shared.imguiWantsKeyboard = ImGui::GetCurrentContext() != nullptr &&
                                      ImGui::GetIO().WantCaptureKeyboard;
          if (!Begin("Tetralis", nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings)) {
            End();
            return;
          }
          Text("seed %llu, frame %llu",
               static_cast<unsigned long long>(snap.seed),
               static_cast<unsigned long long>(snap.frame));
          Text("score %d, depth %d, cleared %d", snap.score, snap.depth,
               snap.cleared);
          Text("piece %s, lock delay %d frames left",
               snap.piece < 0 ? "-" : tetralis::pieceName(snap.piece),
               snap.lockDelayLeft);
          Separator();
          Text("engine frames per rendered frame");
          for (int speed = 0; speed <= 4; ++speed) {
            SameLine();
            if (Button(speed == 0 ? "pause" : std::to_string(speed).c_str(),
                       ImVec2(0, 0)))
              shared.speed = speed;
          }
          Separator();
          Checkbox("HUD (tab)", &shared.showHud);
          Checkbox("controls (H)", &shared.showHelp);
          if (Button("restart on the same seed (R)"))
            shared.restart = true;
          End();
        },
        RWS(local_), RWS(snap_));
  }

  void infer() override {
    clear();
    set(K::FULLSCREEN, CS(local_.fullscreen));

    // The order is the graph: keys first, so that the commands they produced
    // are the ones this frame's engine step consumes. The pad is read on the
    // same footing, and before the mouse only because the mouse and the right
    // stick both write the camera.
    plain([this](const Keyboard::List &presses, const Keyboard::List &releases,
                 Shared &shared) { onPresses(presses, releases, shared); },
          keyPress_, keyRelease_, RWS(local_));

    plain([this](const Joystick::List &events,
                 Shared &shared) { onPad(events, shared); },
          padEvents_, RWS(local_));

    plain(
        [this](const M::List &presses, const M::List &releases,
               const M::List &scrolls, const M::List &drags) {
          onMouse(presses, releases, scrolls, drags);
        },
        pressLeft_, releaseLeft_, scroll_, drag_);

    // RWS(snap_) as well: service publishes the snapshot the nvg pass below
    // reads, so the worker must declare that edge or the pass runs unordered.
    // The unnamed Snapshot& is the edge itself: service writes it, so the pass
    // below is ordered after this node and sees the published frame.
    plain([this](Shared &shared, Snapshot &) { service(shared); }, RWS(local_),
          RWS(snap_));

    nvg(
        [](const Snapshot &snap, const cv::Size &sz) {
          drawScene(snap, sz);
          if (snap.showHud)
            drawHud(snap, sz);
          if (snap.showHelp)
            drawHelp(snap, sz);
        },
        RWS(snap_), size_);
  }
};

V4D_DEMO_MAIN(int argc, char **argv) {
  cv::v4d::add_asset_search_paths();

  uint64_t seed = 1;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
      seed = std::strtoull(argv[++i], nullptr, 10);
    } else if (std::strcmp(argv[i], "--help") == 0 ||
               std::strcmp(argv[i], "-h") == 0) {
      std::cout << "Usage: " << argv[0] << " [--seed N]" << std::endl
                << "  --seed N  the seed to deal from; R restarts on it"
                << std::endl;
      return 0;
    } else {
      std::cerr << "unknown option " << argv[i] << std::endl;
      return 1;
    }
  }

  // No source: the plan clears the framebuffer and draws into it, so there is
  // nothing for a Source to produce.
  cv::Ptr<V4D> runtime =
      V4D::init(cv::Rect(0, 0, 1280, 900), "Tetralis 3D",
                AllocateFlags::NANOVG | AllocateFlags::IMGUI);
  // 0 extra workers: the worker thread the runtime provides is the only one
  // that touches the engine, and the two drawing passes are the runtime's own.
  V4DPlan::run<TetralisDemoPlan>(0, seed);
  return 0;
}
