// Tetralis 3D — Rules Engine
//
// A dependency-free implementation of `tetralis_rules.md`. Nothing in this file
// touches a window, a GPU, a clock or a random device: the engine advances in
// whole frames of exactly 1/60 s and only ever consumes an explicit command
// list, so a (seed, command sequence) pair reproduces exactly the same game
// (§14). `tetralis-demo.cpp` is the V4D application layered on top of it; the
// engine is header-only so that it can also be exercised on its own.
//
// Structure, in the order the rules build the game up:
//
//   §2  Cell, Kind, Board — the shaft lattice and its 240 cells
//   §3  Roster, Orientation, kicks, spawn — the eight tetracubes
//   §5  components(), fallDistance(), plumb() — the rigid fall
//   §6  findSpines() — the only thing that clears
//   §7  resolve() — the pass loop
//   §9  Rime and Anchor Charges — the player resource
//   §10 hasLegalPlacement() — the Struck search
//   §11 gravityFrames(), Bag — depth, gravity and the piece queue
//   §12 PassRecord, awardPasses() — scoring
//   §4  Game — the turn machine
//
// Four specification conflicts are resolved in favour of the normative
// sentence over the illustrative table, and are marked at their use site:
//
//   §5.3/Appendix A  a component's own cells never block it (Appendix A's
//                    worked cell (0,0,3) contradicts its own narrative)
//   §11.2           the gravity formula beats the printed frame table, which
//                    disagrees with it from depth 11 on
//   §3.6/§10.3      the §3.6 spawn is the position of the piece's minimum
//                    corner, so the cells are `spawn + cell` and not
//                    `spawn + (cell - pivot)`
//   §10.3           a spawn overlap locks immediately from the spawn position
//
// On §3.6: the worked spawn table is computed from the bounding box alone,
// ((4-w)//2, (4-d)//2, 15-(h-1)), so it is the minimum corner of the box in a
// shaft that is wider and deeper than every piece. Taken literally with the
// pivot subtracted, four of the eight pieces -- Bar, Tee, Elbow and Zigzag --
// would spawn with a cell outside the shaft, and the Bar would fill only three
// of the four columns. Reading `spawn` as the minimum corner keeps every spawn
// inside, keeps the whole table reproducible, and matches "horizontally
// centred" as closely as a 4-wide shaft allows. The pivot still does what
// §3.3 says it does: it is the cell a rotation swings about, and each
// orientation stores the normed image of it, so kicks move the pivot position
// and nothing else.

#ifndef OPENCV_V4D_SAMPLES_TETRALIS_RULES_HPP
#define OPENCV_V4D_SAMPLES_TETRALIS_RULES_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace tetralis {

// ---------------------------------------------------------------------------
// §2 Coordinate system and the shaft
// ---------------------------------------------------------------------------

inline constexpr int kFootX = 4;
inline constexpr int kFootY = 4;
inline constexpr int kBedrockZ = 0;
inline constexpr int kShaftZMin = 1;
inline constexpr int kShaftZMax = 15; // the Crown, §15
inline constexpr int kCapacity = kFootX * kFootY * kShaftZMax;
inline constexpr int kCellsPerLayer = kFootX * kFootY;

// §6.1 A Spine is a vertical run of at least seven cells.
inline constexpr int kSpineLength = 7;

// §4 Fixed-rate simulation at 60 Hz; every timer below is counted in frames.
inline constexpr int kFramesPerSecond = 60;
inline constexpr int kDasFrames = 10;
inline constexpr int kArrFrames = 2;
inline constexpr int kLockDelayFrames = 30;
inline constexpr int kMoveResetCap = 15;

// §9.2 One charge per fifteen locked pieces, starting at one, capped at five.
inline constexpr int kChargesAtStart = 1;
inline constexpr int kChargeCap = 5;
inline constexpr int kPiecesPerCharge = 15;

// §9.1 Raze cannot target z = 1; that is what keeps Bedrock out of reach.
inline constexpr int kRazeMinZ = 2;

// §4, §9.1 The spec leaves the window between turns to the implementation. This
// engine uses a 300 frame (5 s) window with a visible countdown, so that
// spending a charge on Raze or Seed is a deliberate act rather than a reflex.
inline constexpr int kBetweenTurnFrames = 300;

struct Cell {
  int x = 0;
  int y = 0;
  int z = 0;

  friend bool operator==(const Cell &a, const Cell &b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
  }
  friend bool operator!=(const Cell &a, const Cell &b) { return !(a == b); }
  friend bool operator<(const Cell &a, const Cell &b) {
    if (a.x != b.x)
      return a.x < b.x;
    if (a.y != b.y)
      return a.y < b.y;
    return a.z < b.z;
  }
  friend Cell operator+(const Cell &a, const Cell &b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
  }
  friend Cell operator-(const Cell &a, const Cell &b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
  }
  Cell &operator+=(const Cell &o) {
    x += o.x;
    y += o.y;
    z += o.z;
    return *this;
  }
};

using Cells = std::vector<Cell>;

enum class Kind : uint8_t { kEmpty = 0, kNormal = 1, kRime = 2 };

inline bool occupied(Kind kind) { return kind != Kind::kEmpty; }

// §3.2 Normed form: subtract the component-wise minimum, then sort.
inline Cells normed(Cells cells) {
  if (cells.empty())
    return cells;
  Cell lo = cells.front();
  for (const Cell &c : cells) {
    lo.x = std::min(lo.x, c.x);
    lo.y = std::min(lo.y, c.y);
    lo.z = std::min(lo.z, c.z);
  }
  for (Cell &c : cells) {
    c.x -= lo.x;
    c.y -= lo.y;
    c.z -= lo.z;
  }
  std::sort(cells.begin(), cells.end());
  return cells;
}

inline bool insideShaft(const Cell &c) {
  return c.x >= 0 && c.x < kFootX && c.y >= 0 && c.y < kFootY &&
         c.z >= kShaftZMin && c.z <= kShaftZMax;
}

// ---------------------------------------------------------------------------
// §2 The board: a complete map from cell to kind, and nothing else
// ---------------------------------------------------------------------------

class Board {
public:
  static bool inside(int x, int y, int z) {
    return x >= 0 && x < kFootX && y >= 0 && y < kFootY && z >= kShaftZMin &&
           z <= kShaftZMax;
  }
  static bool inside(const Cell &c) { return inside(c.x, c.y, c.z); }

  Kind at(const Cell &c) const {
    if (c.x < 0 || c.x >= kFootX || c.y < 0 || c.y >= kFootY) {
      return Kind::kEmpty;
    }
    if (c.z == kBedrockZ)
      return Kind::kNormal; // permanently solid
    if (c.z < kShaftZMin || c.z > kShaftZMax)
      return Kind::kEmpty;
    return cells_[c.x][c.y][c.z];
  }

  Kind at(int x, int y, int z) const { return at(Cell{x, y, z}); }

  // True for anything a falling component collides with, Bedrock included.
  bool isBlocked(const Cell &c) const { return occupied(at(c)); }

  // True only for a shaft cell that is currently empty, i.e. what a piece or a
  // Rime placement may claim.
  bool isFree(const Cell &c) const { return at(c) == Kind::kEmpty; }

  void set(const Cell &c, Kind kind) {
    if (!inside(c))
      return;
    cells_[c.x][c.y][c.z] = kind;
  }

  void clear(const Cell &c) { set(c, Kind::kEmpty); }

  // Every cell inside the shaft and empty — the legality test of §3.5, §4.1.
  bool accepts(const Cells &cells) const {
    for (const Cell &c : cells) {
      if (!inside(c) || !isFree(c))
        return false;
    }
    return true;
  }

  int cellCount() const {
    int n = 0;
    for (int z = kShaftZMin; z <= kShaftZMax; ++z) {
      for (int y = 0; y < kFootY; ++y) {
        for (int x = 0; x < kFootX; ++x) {
          if (cells_[x][y][z] != Kind::kEmpty)
            ++n;
        }
      }
    }
    return n;
  }

  int countKind(Kind kind) const {
    int n = 0;
    for (int z = kShaftZMin; z <= kShaftZMax; ++z) {
      for (int y = 0; y < kFootY; ++y) {
        for (int x = 0; x < kFootX; ++x) {
          if (cells_[x][y][z] == kind)
            ++n;
        }
      }
    }
    return n;
  }

  // B3: assert nothing was ever written outside the shaft.
  bool allCellsInside() const {
    for (int z = kShaftZMin; z <= kShaftZMax; ++z) {
      for (int y = 0; y < kFootY; ++y) {
        for (int x = 0; x < kFootX; ++x) {
          if (cells_[x][y][z] != Kind::kEmpty && !inside(x, y, z))
            return false;
        }
      }
    }
    return true;
  }

private:
  Kind cells_[kFootX][kFootY][kShaftZMax + 1] = {};
};

// ---------------------------------------------------------------------------
// §3 The pieces
// ---------------------------------------------------------------------------

enum Piece {
  kBar = 0,
  kBlock,
  kTee,
  kElbow,
  kZigzag,
  kProng,
  kScrewL,
  kScrewR,
  kPieceCount,
};

inline const char *pieceName(int piece) {
  switch (piece) {
  case kBar:
    return "Bar";
  case kBlock:
    return "Block";
  case kTee:
    return "Tee";
  case kElbow:
    return "Elbow";
  case kZigzag:
    return "Zigzag";
  case kProng:
    return "Prong";
  case kScrewL:
    return "ScrewL";
  case kScrewR:
    return "ScrewR";
  default:
    return "?";
  }
}

// The player-facing commands of §4.1 and §3.4, as an explicit ordered list so
// that a replay is a list of integers (§14).
enum Cmd {
  kMoveXNeg = 0,
  kMoveXPos,
  kMoveYPos,
  kMoveYNeg,
  kMoveZNeg,
  kMoveZPos,
  kRotXPos,
  kRotXNeg,
  kRotYPos,
  kRotYNeg,
  kRotZPos,
  kRotZNeg,
  kHardDrop,
  kSettle,
  kRaze,
  kSeed,
  kAdvance,
  kCmdCount,
};

// A signed permutation matrix with determinant +1, row-major.
struct Rotation {
  int m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

  Cell apply(const Cell &c) const {
    return {m[0][0] * c.x + m[0][1] * c.y + m[0][2] * c.z,
            m[1][0] * c.x + m[1][1] * c.y + m[1][2] * c.z,
            m[2][0] * c.x + m[2][1] * c.y + m[2][2] * c.z};
  }

  int determinant() const {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
           m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  }
};

// §3.4 The six player-facing rotation commands, verbatim from the table.
inline Rotation rotationFor(int cmd) {
  Rotation r;
  switch (cmd) {
  case kRotXPos: // (x, -z, y)
    r.m[0][0] = 1;
    r.m[0][1] = 0;
    r.m[0][2] = 0;
    r.m[1][0] = 0;
    r.m[1][1] = 0;
    r.m[1][2] = -1;
    r.m[2][0] = 0;
    r.m[2][1] = 1;
    r.m[2][2] = 0;
    break;
  case kRotXNeg: // (x, z, -y)
    r.m[0][0] = 1;
    r.m[0][1] = 0;
    r.m[0][2] = 0;
    r.m[1][0] = 0;
    r.m[1][1] = 0;
    r.m[1][2] = 1;
    r.m[2][0] = 0;
    r.m[2][1] = -1;
    r.m[2][2] = 0;
    break;
  case kRotYPos: // (z, y, -x)
    r.m[0][0] = 0;
    r.m[0][1] = 0;
    r.m[0][2] = 1;
    r.m[1][0] = 0;
    r.m[1][1] = 1;
    r.m[1][2] = 0;
    r.m[2][0] = -1;
    r.m[2][1] = 0;
    r.m[2][2] = 0;
    break;
  case kRotYNeg: // (-z, y, x)
    r.m[0][0] = 0;
    r.m[0][1] = 0;
    r.m[0][2] = -1;
    r.m[1][0] = 0;
    r.m[1][1] = 1;
    r.m[1][2] = 0;
    r.m[2][0] = 1;
    r.m[2][1] = 0;
    r.m[2][2] = 0;
    break;
  case kRotZPos: // (-y, x, z)
    r.m[0][0] = 0;
    r.m[0][1] = -1;
    r.m[0][2] = 0;
    r.m[1][0] = 1;
    r.m[1][1] = 0;
    r.m[1][2] = 0;
    r.m[2][0] = 0;
    r.m[2][1] = 0;
    r.m[2][2] = 1;
    break;
  default: // kRotZNeg: (y, -x, z)
    r.m[0][0] = 0;
    r.m[0][1] = 1;
    r.m[0][2] = 0;
    r.m[1][0] = -1;
    r.m[1][1] = 0;
    r.m[1][2] = 0;
    r.m[2][0] = 0;
    r.m[2][1] = 0;
    r.m[2][2] = 1;
    break;
  }
  return r;
}

// §3.5 The kick offsets, in their fixed order. A kick moves the pivot position.
inline constexpr Cell kKickOffsets[7] = {{0, 0, 0}, {1, 0, 0},  {-1, 0, 0},
                                         {0, 1, 0}, {0, -1, 0}, {0, 0, 1},
                                         {0, 0, -1}};

// One orientation: the piece's four cells in normed form, plus its pivot in
// the same normed space.
struct Orientation {
  Cells cells;
  Cell pivot;
};

struct PieceInfo {
  const char *name = "";
  Cells canonical;
  Cell pivot; // §3.3, in canonical coordinates
  Cell spawn; // §3.6, the pivot position at deal time
  int w = 0, d = 0, h = 0;
  std::vector<Orientation> orientations;
};

inline std::vector<Rotation> properRotations() {
  // All 24 signed permutation matrices with determinant +1, identity first, in
  // a fixed enumeration order so that orientation indices are stable (§14).
  std::vector<Rotation> out;
  int perm[3] = {0, 1, 2};
  do {
    int inversions = 0;
    for (int i = 0; i < 3; ++i) {
      for (int j = i + 1; j < 3; ++j) {
        if (perm[i] > perm[j])
          ++inversions;
      }
    }
    const int permSign = (inversions % 2 == 0) ? 1 : -1;
    for (int bits = 0; bits < 8; ++bits) {
      const int sx = (bits & 4) ? -1 : 1;
      const int sy = (bits & 2) ? -1 : 1;
      const int sz = (bits & 1) ? -1 : 1;
      const int det = permSign * sx * sy * sz;
      if (det != 1)
        continue;
      Rotation r;
      for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j)
          r.m[i][j] = 0;
      }
      r.m[0][perm[0]] = sx;
      r.m[1][perm[1]] = sy;
      r.m[2][perm[2]] = sz;
      out.push_back(r);
    }
  } while (std::next_permutation(perm, perm + 3));
  return out;
}

inline std::array<PieceInfo, kPieceCount> buildRoster() {
  static const Cells kCanonical[kPieceCount] = {
      {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {3, 0, 0}}, // Bar
      {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}}, // Block
      {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {1, 1, 0}}, // Tee
      {{0, 0, 0}, {0, 1, 0}, {0, 2, 0}, {1, 2, 0}}, // Elbow
      {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {2, 1, 0}}, // Zigzag
      {{0, 0, 0}, {0, 0, 1}, {0, 1, 0}, {1, 0, 0}}, // Prong
      {{0, 0, 0}, {0, 0, 1}, {0, 1, 0}, {1, 0, 1}}, // ScrewL
      {{0, 0, 0}, {0, 0, 1}, {0, 1, 0}, {1, 1, 0}}, // ScrewR
  };
  static const int kOrientations[kPieceCount] = {3, 3, 12, 24, 12, 8, 12, 12};
  const std::vector<Rotation> rotations = properRotations();

  std::array<PieceInfo, kPieceCount> roster;
  for (int p = 0; p < kPieceCount; ++p) {
    PieceInfo &info = roster[p];
    info.name = pieceName(p);
    info.canonical = normed(kCanonical[p]);
    info.w = 0;
    info.d = 0;
    info.h = 0;
    for (const Cell &c : info.canonical) { // extent per axis, not the last cell
      info.w = std::max(info.w, c.x + 1);
      info.d = std::max(info.d, c.y + 1);
      info.h = std::max(info.h, c.z + 1);
    }
    // §3.3 pivot = min + (floor((w-1)/2), floor((d-1)/2), floor((h-1)/2)),
    // and every canonical form is already normed so min is (0,0,0).
    info.pivot = {(info.w - 1) / 2, (info.d - 1) / 2, (info.h - 1) / 2};
    // §3.6 spawn = ((4-w)/2, (4-d)/2, 15-(h-1)).
    info.spawn = {(kFootX - info.w) / 2, (kFootY - info.d) / 2,
                  kShaftZMax - (info.h - 1)};

    for (const Rotation &rot : rotations) {
      Cells rotated;
      rotated.reserve(info.canonical.size());
      Cell lo{0, 0, 0};
      bool first = true;
      for (const Cell &c : info.canonical) {
        const Cell rc = rot.apply(c);
        rotated.push_back(rc);
        if (first) {
          lo = rc;
          first = false;
        } else {
          lo.x = std::min(lo.x, rc.x);
          lo.y = std::min(lo.y, rc.y);
          lo.z = std::min(lo.z, rc.z);
        }
      }
      Orientation orientation;
      orientation.cells = normed(rotated);
      // §3.3 the pivot is a cell of the piece, so the handle has to follow the
      // rotation: the orientation's pivot is the normed image of the canonical
      // pivot cell, which is R(pivot) - lo. Rotating about the normed origin
      // instead would give every piece the same corner handle, and would turn
      // the Tee about its end rather than about its middle.
      const Cell image = rot.apply(info.pivot);
      orientation.pivot = {image.x - lo.x, image.y - lo.y, image.z - lo.z};
      // §3.3 "for all eight pieces this yields a cell that is actually part of
      // the piece (verified)".
      bool pivotInPiece = false;
      for (const Cell &c : orientation.cells)
        if (c == orientation.pivot)
          pivotInPiece = true;
      if (!pivotInPiece)
        throw std::logic_error(
            "tetralis: §3.3 pivot is not a cell of the piece");
      bool duplicate = false;
      for (const Orientation &known : info.orientations) {
        if (known.cells == orientation.cells) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate)
        info.orientations.push_back(orientation);
    }
    (void)kOrientations[p]; // the counts of §3.1, checked by the test suite
  }
  return roster;
}

inline const std::array<PieceInfo, kPieceCount> &roster() {
  static const std::array<PieceInfo, kPieceCount> table = buildRoster();
  return table;
}

inline const PieceInfo &pieceInfo(int piece) { return roster()[piece]; }

// The index of the orientation whose normed form is `cells`.
inline int findOrientation(int piece, const Cells &cells) {
  const PieceInfo &info = pieceInfo(piece);
  for (int i = 0; i < static_cast<int>(info.orientations.size()); ++i) {
    if (info.orientations[i].cells == cells)
      return i;
  }
  return -1;
}

// §3.5 A rotation is legal if, after rotating about the §3.3 pivot and adding
// one of the kick offsets, every cell is inside the shaft and empty. The seven
// offsets are tried in their fixed order and the first legal one is used; an
// offset moves the pivot position, not the cells. If none is legal the rotation
// is rejected and, per §3.5, nothing at all changes.
//
// Written as a function of the board so that the rule is testable without a
// game around it. `kickIndex` receives which offset was used, so a caller --
// and the check suite -- can tell a clean rotation from a kicked one.
// The rotation is taken about the *pivot cell*, so that cell maps to itself and
// the piece turns about the handle §3.3 defines. That is why the function takes
// the live cell list instead of an orientation index. The orientation table
// cannot stand in for it: each entry stores the pivot of whichever of the 24
// rotations happened to produce that entry first, and the six commands of §3.4
// do not agree with that choice. Standing the Bar up from the Crown gives
// y = 0..3 with `z+` and y = -1..2 with `z-` -- the same solid one cell along
// -- and looking the result up as an orientation, then rebuilding the cells
// from that orientation's stored pivot, would make the two commands
// indistinguishable. For the Elbow and the two Screws that does not merely
// shift the piece: it would make a proper rotation behave like its mirror,
// which §3.4 excludes.
inline bool rotatedPlacement(const Board &board, const Cells &cells,
                             const Cell &pivot, int cmd, Cell *newPivot,
                             Cells *newCells, int *kickIndex) {
  const Rotation rotation = rotationFor(cmd);
  Cells turned;
  turned.reserve(cells.size());
  for (const Cell &c : cells)
    turned.push_back(rotation.apply(c - pivot));

  // The pivot cell mapped to the origin and so does not move; the kicks are
  // then tried in order from there.
  const int kicks =
      static_cast<int>(sizeof(kKickOffsets) / sizeof(kKickOffsets[0]));
  for (int k = 0; k < kicks; ++k) {
    const Cell position = pivot + kKickOffsets[k];
    Cells candidate;
    candidate.reserve(turned.size());
    for (const Cell &c : turned)
      candidate.push_back(position + c);
    if (board.accepts(candidate)) {
      if (newPivot != nullptr)
        *newPivot = position;
      if (newCells != nullptr)
        *newCells = candidate;
      if (kickIndex != nullptr)
        *kickIndex = k;
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// §5 Plumb
// ---------------------------------------------------------------------------

struct Component {
  Cells cells; // sorted
  bool anchored = false;
  int minZ = 0;
};

inline bool blocking(const Board &board, const Cells *blockers, const Cell &c) {
  if (board.isBlocked(c))
    return true;
  if (blockers == nullptr)
    return false;
  for (const Cell &b : *blockers) {
    if (b == c)
      return true;
  }
  return false;
}

// §5.1 Face-connected maximal sets; §5.2 anchoring; §5.6 the fixed total order
// (min z, then -size, then the sorted cell list).
inline std::vector<Component> components(const Board &board) {
  std::vector<Component> found;
  bool seen[kFootX][kFootY][kShaftZMax + 1] = {};
  static const int kDx[6] = {1, -1, 0, 0, 0, 0};
  static const int kDy[6] = {0, 0, 1, -1, 0, 0};
  static const int kDz[6] = {0, 0, 0, 0, 1, -1};

  for (int z = kShaftZMin; z <= kShaftZMax; ++z) {
    for (int y = 0; y < kFootY; ++y) {
      for (int x = 0; x < kFootX; ++x) {
        if (seen[x][y][z] || board.at(x, y, z) == Kind::kEmpty)
          continue;
        Component component;
        std::vector<Cell> stack{Cell{x, y, z}};
        seen[x][y][z] = true;
        while (!stack.empty()) {
          const Cell c = stack.back();
          stack.pop_back();
          component.cells.push_back(c);
          const Kind kind = board.at(c);
          if (kind == Kind::kRime)
            component.anchored = true;
          if (c.z == kShaftZMin)
            component.anchored = true; // rests on Bedrock
          for (int dir = 0; dir < 6; ++dir) {
            const Cell next{c.x + kDx[dir], c.y + kDy[dir], c.z + kDz[dir]};
            if (!insideShaft(next) || seen[next.x][next.y][next.z])
              continue;
            if (board.at(next) == Kind::kEmpty)
              continue;
            seen[next.x][next.y][next.z] = true;
            stack.push_back(next);
          }
        }
        std::sort(component.cells.begin(), component.cells.end());
        component.minZ = component.cells.front().z;
        found.push_back(component);
      }
    }
  }

  std::sort(found.begin(), found.end(),
            [](const Component &a, const Component &b) {
              if (a.minZ != b.minZ)
                return a.minZ < b.minZ;
              if (a.cells.size() != b.cells.size()) {
                return a.cells.size() > b.cells.size();
              }
              return a.cells < b.cells;
            });
  return found;
}

// §5.3 How far a component may descend. Two traps, both avoided here: only
// cells strictly below a cell block it, and a component's own cells never
// block it. Bedrock supplies the implicit floor of 0.
inline int fallDistance(const Board &board, const Component &component,
                        const Cells *blockers) {
  int distance = kShaftZMax;
  for (const Cell &c : component.cells) {
    int obstacle = kBedrockZ; // the floor of 0 from §5.3
    for (int z = c.z - 1; z >= kShaftZMin; --z) {
      if (blocking(board, blockers, Cell{c.x, c.y, z})) {
        obstacle = z;
        break;
      }
    }
    distance = std::min(distance, c.z - 1 - obstacle);
  }
  return std::max(0, distance);
}

// §5.5 Plumb is a fixpoint: after every fall the components are recomputed, and
// only one component moves per step, in §5.6 order.
inline void plumb(Board &board, const Cells *blockers = nullptr) {
  for (;;) {
    bool fell = false;
    for (const Component &component : components(board)) {
      if (component.anchored)
        continue;
      const int distance = fallDistance(board, component, blockers);
      if (distance <= 0)
        continue;
      // The kinds are read before anything is written: a component may fall
      // into the column it just vacated, so clearing first would let a cell
      // read back as Empty (§5.4, a component never blocks itself).
      std::vector<std::pair<Cell, Kind>> moved;
      moved.reserve(component.cells.size());
      for (const Cell &c : component.cells) {
        const Kind kind = board.at(c);
        board.clear(c);
        moved.emplace_back(Cell{c.x, c.y, c.z - distance}, kind);
      }
      for (const auto &entry : moved)
        board.set(entry.first, entry.second);
      fell = true;
      break; // one component per step, then recompute (§5.6)
    }
    if (!fell)
      return;
  }
}

// ---------------------------------------------------------------------------
// §6 Spines
// ---------------------------------------------------------------------------

struct Spine {
  Cells cells;
  bool hasNormal = false;
};

// §6.1 A run is a maximal stretch of consecutive occupied z in one column; it
// is a Spine iff it is at least seven long and holds at least one Normal cell.
inline std::vector<Spine> findSpines(const Board &board) {
  std::vector<Spine> spines;
  for (int y = 0; y < kFootY; ++y) {
    for (int x = 0; x < kFootX; ++x) {
      int z = kShaftZMin;
      while (z <= kShaftZMax) {
        if (board.at(x, y, z) == Kind::kEmpty) {
          ++z;
          continue;
        }
        int end = z;
        bool hasNormal = false;
        while (end <= kShaftZMax && board.at(x, y, end) != Kind::kEmpty) {
          if (board.at(x, y, end) == Kind::kNormal)
            hasNormal = true;
          ++end;
        }
        Spine spine;
        for (int i = z; i < end; ++i)
          spine.cells.push_back(Cell{x, y, i});
        spine.hasNormal = hasNormal;
        if (static_cast<int>(spine.cells.size()) >= kSpineLength && hasNormal) {
          spines.push_back(spine);
        }
        z = end;
      }
    }
  }
  return spines;
}

// ---------------------------------------------------------------------------
// §7 Resolution and §12 Scoring
// ---------------------------------------------------------------------------

struct PassRecord {
  int chain = 0;
  int cells = 0;
  int spines = 0;
  int longestSpine = 0;
  bool layerCleared = false; // §12 Crown Sweep
  bool shaftEmpty = false;   // §12 Deep Clear
};

struct ResolveResult {
  int chain = 0;
  int cellsCleared = 0;
  std::vector<PassRecord> passes;
};

// §7 The pass loop: plumb to fixpoint, evaluate every Spine at once, delete
// once. `blockers` is non-null only for a Settle (§5.4).
inline ResolveResult resolve(Board &board, const Cells *blockers = nullptr) {
  ResolveResult result;
  for (;;) {
    plumb(board, blockers);
    const std::vector<Spine> spines = findSpines(board);
    if (spines.empty())
      return result;

    Cells doomed;
    PassRecord pass;
    pass.chain = result.chain + 1;
    pass.spines = static_cast<int>(spines.size());
    for (const Spine &spine : spines) {
      pass.longestSpine =
          std::max(pass.longestSpine, static_cast<int>(spine.cells.size()));
      doomed.insert(doomed.end(), spine.cells.begin(), spine.cells.end());
    }
    std::sort(doomed.begin(), doomed.end());
    doomed.erase(std::unique(doomed.begin(), doomed.end()), doomed.end());

    pass.cells = static_cast<int>(doomed.size());
    for (int z = kShaftZMin; z <= kShaftZMax; ++z) { // §12 Crown Sweep
      int inLayer = 0;
      for (const Cell &c : doomed) {
        if (c.z == z)
          ++inLayer;
      }
      if (inLayer == kCellsPerLayer)
        pass.layerCleared = true;
    }
    for (const Cell &c : doomed)
      board.clear(c);
    pass.shaftEmpty = board.cellCount() == 0;
    result.passes.push_back(pass);
    result.cellsCleared += pass.cells;
    result.chain = pass.chain;
  }
}

// §12 Bonuses are scored per pass, with `d` taken after this turn's depth
// increase, so scoring is deliberately left to the caller.
inline int64_t scorePass(const PassRecord &pass, int depth) {
  int64_t points = 100LL * pass.cells * pass.chain;
  if (pass.spines >= 2)
    points += 300LL * pass.spines * pass.chain;
  if (pass.longestSpine >= 10)
    points += 150LL * pass.chain;
  if (pass.layerCleared)
    points += 800LL * pass.chain;
  if (pass.shaftEmpty)
    points += 5000LL * depth;
  return points;
}

// ---------------------------------------------------------------------------
// §10 Struck
// ---------------------------------------------------------------------------

// §10.1 Legal placement search: every orientation against every position inside
// the shaft, with no regard for kicks or reachability.
inline bool hasLegalPlacement(const Board &board, int piece) {
  const PieceInfo &info = pieceInfo(piece);
  for (const Orientation &orientation : info.orientations) {
    // Cells are at pivot position + (normed cell - orientation pivot).
    Cell lo{0, 0, 0};
    Cell hi{0, 0, 0};
    bool first = true;
    for (const Cell &c : orientation.cells) {
      const Cell rel = c - orientation.pivot;
      if (first) {
        lo = hi = rel;
        first = false;
      } else {
        lo.x = std::min(lo.x, rel.x);
        lo.y = std::min(lo.y, rel.y);
        lo.z = std::min(lo.z, rel.z);
        hi.x = std::max(hi.x, rel.x);
        hi.y = std::max(hi.y, rel.y);
        hi.z = std::max(hi.z, rel.z);
      }
    }
    for (int pivotZ = kShaftZMin - lo.z; pivotZ <= kShaftZMax - hi.z;
         ++pivotZ) {
      for (int pivotY = 0 - lo.y; pivotY < kFootY - hi.y; ++pivotY) {
        for (int pivotX = 0 - lo.x; pivotX < kFootX - hi.x; ++pivotX) {
          const Cell pivot{pivotX, pivotY, pivotZ};
          bool ok = true;
          for (const Cell &c : orientation.cells) {
            const Cell world = pivot + (c - orientation.pivot);
            if (!insideShaft(world) || !board.isFree(world)) {
              ok = false;
              break;
            }
          }
          if (ok)
            return true;
        }
      }
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// §11 Depth, gravity and the bag
// ---------------------------------------------------------------------------

// Q(d) = 15 * d * (d + 1) / 2, the cells needed to reach depth d.
inline int64_t cellsNeededForDepth(int depth) {
  return 15LL * depth * (depth + 1) / 2;
}

// G(d) = max(2, floor(17 * 0.87^(d-1))). The printed table of §11.2 agrees
// only up to depth 10; the formula is the normative one and is what is used.
inline int gravityFrames(int depth) {
  double value = 17.0;
  for (int d = 1; d < depth; ++d)
    value *= 0.87;
  return std::max(2, static_cast<int>(std::floor(value)));
}

// splitmix64: a fixed, portable, self-contained generator. A replay is
// described by (seed, inputs), so the generator state has to be explicit and
// restorable (§14).
class Rng {
public:
  Rng() = default;
  explicit Rng(uint64_t seed) : state_(seed) {}

  uint64_t state() const { return state_; }
  void setState(uint64_t state) { state_ = state; }

  uint64_t next() {
    uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }

  uint32_t below(uint32_t bound) {
    return static_cast<uint32_t>(next() % bound);
  }

private:
  uint64_t state_;
};

// §11.3 The bag: a shuffled multiset of the pieces available at the current
// depth, refilled when it empties.
class Bag {
public:
  Bag() = default;
  explicit Bag(uint64_t seed) : rng_(seed) {}

  void reseed(uint64_t seed) {
    rng_.setState(seed);
    queue_.clear();
    depth_ = 1;
  }

  void setDepth(int depth) { depth_ = depth; }

  int rosterFor(int depth) const {
    if (depth < 3)
      return 5;
    if (depth < 6)
      return 6;
    return kPieceCount;
  }

  void refill() {
    std::vector<int> pieces;
    for (int i = 0; i < rosterFor(depth_); ++i)
      pieces.push_back(i);
    // A depth increase never disturbs the bag in flight: it takes effect when
    // the bag empties, which is what §11.3's last sentence describes.
    shuffle(pieces, rng_);
    queue_ = std::move(pieces);
  }

  int front() {
    if (queue_.empty())
      refill();
    return queue_.front();
  }

  int draw() {
    const int piece = front();
    queue_.erase(queue_.begin());
    return piece;
  }

  // The next `n` pieces without consuming generator state: a bag that would be
  // needed sooner is generated speculatively and the state is restored, so the
  // preview can never perturb the game (§14).
  std::vector<int> peek(size_t n) const {
    std::vector<int> pieces;
    std::vector<int> queue = queue_;
    Rng speculator = rng_;
    while (pieces.size() < n) {
      if (queue.empty()) {
        std::vector<int> fresh;
        for (int i = 0; i < rosterFor(depth_); ++i)
          fresh.push_back(i);
        shuffle(fresh, speculator);
        queue = std::move(fresh);
      }
      pieces.push_back(queue.front());
      queue.erase(queue.begin());
    }
    return pieces;
  }

  int remaining() const { return static_cast<int>(queue_.size()); }
  int bagSize() const { return rosterFor(depth_); }

private:
  // Fisher-Yates, walking downwards so that each index draws from exactly the
  // entries above it.
  static void shuffle(std::vector<int> &pieces, Rng &rng) {
    for (int i = static_cast<int>(pieces.size()) - 1; i > 0; --i) {
      const int j = static_cast<int>(rng.below(static_cast<uint32_t>(i + 1)));
      std::swap(pieces[i], pieces[j]);
    }
  }

  Rng rng_;
  std::vector<int> queue_;
  int depth_ = 1;
};

// ---------------------------------------------------------------------------
// §4 Input and the turn machine
// ---------------------------------------------------------------------------

// One frame of player input. `cmds` holds the edges in the order they arrived;
// the held flags drive the DAS/ARR auto-shift, and `softDrop` is the soft drop
// held down continuously.
struct Input {
  std::vector<Cmd> cmds;
  bool held[kCmdCount] = {};
  bool softDrop = false;
};

enum Phase {
  kBetweenTurns = 0, // the window in which Raze and Seed are legal (§9.1)
  kFalling,          // a piece is active
  kStruck,           // game over (§10)
};

class Game {
public:
  Game() { startGame(1); }
  explicit Game(uint64_t seed) { startGame(seed); }

  // ----------------------------------------------------------------- lifecycle

  void startGame(uint64_t seed) {
    board_ = Board();
    seed_ = seed;
    bag_.reseed(seed);
    bag_.setDepth(1);
    score_ = 0;
    depth_ = 1;
    charges_ = kChargesAtStart;
    chargeProgress_ = 0;
    piecesLocked_ = 0;
    cellsCleared_ = 0;
    bestChain_ = 0;
    graceUsed_ = false;
    fault_ = false;
    phase_ = kBetweenTurns;
    betweenFrames_ = 1;
    piece_ = kBlock;
    orient_ = 0;
    pivot_ = {0, 0, 0};
    cells_.clear();
    lockTimer_ = 0;
    lockStarted_ = false;
    fallTimer_ = 0;
    moveResets_ = 0;
    softTimer_ = 0;
    clearToast();
    cursor_ = {kFootX / 2, kFootY / 2, 2};
  }

  void step(const Input &input) {
    ++frame_;
    if (toastFrames_ > 0 && --toastFrames_ == 0)
      toast_.clear();
    switch (phase_) {
    case kFalling:
      stepFalling(input);
      break;
    case kBetweenTurns:
      stepBetweenTurns(input);
      break;
    case kStruck:
      break; // the board is frozen; the demo shows the overlay
    }
  }

  // ------------------------------------------------------------------ querying

  const Board &board() const { return board_; }
  Board &board() { return board_; }
  Phase phase() const { return phase_; }
  bool active() const { return phase_ == kFalling; }
  bool struck() const { return phase_ == kStruck; }
  bool fault() const { return fault_; } // §10.3, never expected to happen
  uint64_t seed() const { return seed_; }
  int64_t frame() const { return frame_; }
  int score() const { return static_cast<int>(score_); }
  int64_t score64() const { return score_; }
  int depth() const { return depth_; }
  int charges() const { return charges_; }
  int chargeProgress() const { return chargeProgress_; }
  int piecesLocked() const { return piecesLocked_; }
  int cellsCleared() const { return cellsCleared_; }
  int64_t bestChain() const { return bestChain_; }
  int lastChain() const { return lastChain_; }
  int gravity() const { return gravityFrames(depth_); }
  int bagSize() const { return bag_.bagSize(); }
  int betweenFramesLeft() const { return betweenFrames_; }
  int lockTimer() const { return lockTimer_; }
  int lockDelay() const { return kLockDelayFrames - lockTimer_; }
  // §4.1: the lock timer only runs while the piece is resting on something.
  bool resting() const {
    for (const Cell &c : cells_) {
      const Cell below = c + Cell{0, 0, -1};
      if (!board_.isFree(below))
        return true;
    }
    return false;
  }

  Cell cursor() const { return cursor_; }
  bool cursorOnRime() const { return board_.at(cursor_) == Kind::kRime; }
  bool cursorOnNormal() const { return board_.at(cursor_) == Kind::kNormal; }
  bool cursorRazeable() const {
    return board_.at(cursor_) == Kind::kNormal && cursor_.z >= kRazeMinZ;
  }
  bool cursorSeedable() const {
    return board_.isFree(cursor_) && insideShaft(cursor_);
  }
  const std::string &toast() const { return toast_; }
  int toastFrames() const { return toastFrames_; }

  int piece() const { return piece_; }
  // Which entry of §3.4's orientation table the live cells match, or -1 when
  // the piece is in the mirrored image of one: about the pivot, `z+` and `z-`
  // of a Bar occupy different cells while both are proper rotations of it, and
  // only one of the two is in the table.
  int orientation() const { return orient_; }
  Cell pivot() const { return pivot_; }

  std::vector<int> preview(size_t n = 4) const { return bag_.peek(n); }

  Cells activeCells() const { return cells_; }

  // False only for a piece still overlapping where it spawned (§10.3).
  bool cellsLegal() const { return board_.accepts(cells_); }

  // §16 Landing preview: the cells the piece will lock in.
  Cells restingCells() const {
    if (!active() || !cellsLegal())
      return {};
    Cells cells = cells_;
    while (true) {
      const Cells lower = shift(cells, -1);
      if (!board_.accepts(lower))
        break;
      cells = lower;
    }
    return cells;
  }

  // §16 Spine preview: every Spine that locking the piece where it would land
  // would complete, together with the longest of them. This is exactly the
  // first Spine evaluation of the resolution loop after the next lock.
  std::pair<Cells, int> spinePreview() const {
    Cells highlighted;
    int longest = 0;
    if (!active())
      return {highlighted, longest};
    const Cells rest = restingCells();
    if (!board_.accepts(rest))
      return {highlighted, longest};
    Board probe = board_;
    for (const Cell &c : rest)
      probe.set(c, Kind::kNormal);
    plumb(probe, nullptr);
    for (const Spine &spine : findSpines(probe)) {
      highlighted.insert(highlighted.end(), spine.cells.begin(),
                         spine.cells.end());
      longest = std::max(longest, static_cast<int>(spine.cells.size()));
    }
    std::sort(highlighted.begin(), highlighted.end());
    return {highlighted, longest};
  }

  // --------------------------------------------------------------- charge use

  bool actSettle() {
    if (phase_ != kFalling || charges_ <= 0)
      return false;
    --charges_;
    const Cells blockers = activeCells();
    runResolution(&blockers);
    return true;
  }

  bool actRaze(Cell target) {
    if (phase_ == kFalling || charges_ <= 0)
      return false;
    if (board_.at(target) != Kind::kNormal || target.z < kRazeMinZ)
      return false;
    --charges_;
    board_.clear(target);
    runResolution(nullptr);
    return true;
  }

  bool actSeed(Cell target) {
    if (phase_ == kFalling || charges_ <= 0)
      return false;
    if (!board_.isFree(target) || !insideShaft(target))
      return false;
    --charges_;
    board_.set(target, Kind::kRime);
    return true;
  }

private:
  // ----------------------------------------------------------------- internals

  static Cells shift(const Cells &cells, int dz) {
    Cells out = cells;
    for (Cell &c : out)
      c.z += dz;
    return out;
  }

  // The active piece is held as four explicit cells plus the world position of
  // its §3.3 pivot, because that pair is the state the rules are written in:
  // §3.5 rotates the cells about the pivot and offsets the pivot position, and
  // neither is recoverable from an orientation index once a piece has been
  // rotated into the mirrored image of a table entry.
  void adopt(const Cells &cells, const Cell &pivot) {
    cells_ = cells;
    std::sort(cells_.begin(), cells_.end());
    pivot_ = pivot;
    orient_ = findOrientation(piece_, normed(cells_));
  }

  void clearToast() {
    toast_.clear();
    toastFrames_ = 0;
    lastChain_ = 0;
  }

  void announce(int64_t points, int chain, int cells, int spines) {
    if (chain <= 0)
      return;
    std::string text = "+" + std::to_string(points);
    if (chain > 1)
      text += "   CHAIN x" + std::to_string(chain);
    if (spines > 1)
      text += "   " + std::to_string(spines) + " SPINES";
    text += "   " + std::to_string(cells) + " CELLS";
    toast_ = text;
    toastFrames_ = 120; // two seconds of HUD time, frames all the way (§14)
  }

  // §4 step 4 plus §4 step 5 and the depth update of §11.1, then §12 scoring.
  void runResolution(const Cells *blockers) {
    const ResolveResult result = resolve(board_, blockers);
    cellsCleared_ += result.cellsCleared;
    bag_.setDepth(depth_);
    while (cellsCleared_ >= cellsNeededForDepth(depth_))
      ++depth_;
    bag_.setDepth(depth_);
    bestChain_ = std::max(bestChain_, static_cast<int64_t>(result.chain));

    int64_t points = 0;
    for (const PassRecord &pass : result.passes) {
      points += scorePass(pass, depth_); // d is the post-increase depth (§12)
    }
    score_ += points;
    lastChain_ = result.chain;
    if (result.chain > 0) {
      int cells = 0;
      int spines = 0;
      for (const PassRecord &pass : result.passes) {
        cells += pass.cells;
        spines += pass.spines;
      }
      announce(points, result.chain, cells, spines);
    }
  }

  // §4 steps 3 to 6.
  void lockPiece() {
    for (const Cell &c : cells_) {
      // B3: nothing is ever written outside the shaft. Every position the
      // player can reach passes through accepts(), which requires the cells to
      // be inside, and §3.6's spawn is inside for all eight pieces under the
      // reading of §3.6 documented above -- so this is an invariant guard, not
      // a reachable branch.
      if (!insideShaft(c))
        fault_ = true;
      board_.set(c, Kind::kNormal);
    }

    phase_ = kBetweenTurns;
    betweenFrames_ = kBetweenTurnFrames;

    ++piecesLocked_;
    if (++chargeProgress_ >= kPiecesPerCharge) {
      chargeProgress_ = 0;
      if (charges_ < kChargeCap)
        ++charges_; // at the cap the counter resets
    }

    runResolution(nullptr);
  }

  // §4 steps 1 and 6 and 7, with the §10 Struck check at the top of the turn.
  void startTurn() {
    const int next = bag_.front();
    if (!hasLegalPlacement(board_, next)) {
      if (!graceUsed_) {
        // §10.2 one free Settle, no charge, no active piece and no blockers.
        graceUsed_ = true;
        runResolution(nullptr);
      }
      if (!hasLegalPlacement(board_, bag_.front())) {
        phase_ = kStruck;
        return;
      }
    }
    piece_ = bag_.draw();
    // §3.6 places the cells at `spawn + cell`, and `pivot_` holds the position
    // of the §3.3 pivot cell, which for the canonical form is that cell's
    // normed offset into the box. The two together reproduce the §3.6 spawn
    // table exactly: Bar spawns with its four cells across x = 0..3, not
    // shifted one column left.
    const PieceInfo &info = pieceInfo(piece_);
    Cells spawned;
    spawned.reserve(info.canonical.size());
    for (const Cell &c : info.canonical)
      spawned.push_back(info.spawn + c);
    adopt(spawned, info.spawn + info.orientations[0].pivot);
    lockTimer_ = 0;
    lockStarted_ = false;
    fallTimer_ = 0;
    softTimer_ = 0;
    moveResets_ = 0;
    for (int i = 0; i < kCmdCount; ++i)
      heldFrames_[i] = 0;
    phase_ = kFalling;
    clearToast();

    // §10.3 the spawn position is allowed to overlap, and then the piece locks
    // immediately, exactly as if it had been dropped: it is dealt, it cannot
    // move down, and it locks on top of the stack. This is the normal way a
    // nearly-full shaft ends a game -- it is not a fault, and §10.1's
    // exhaustive search has already decided that the piece had somewhere to go
    // when it was dealt.
    if (!board_.accepts(cells_))
      lockPiece();
  }

  bool translate(const Cell &offset) {
    Cells cells;
    cells.reserve(cells_.size());
    for (const Cell &c : cells_)
      cells.push_back(c + offset);
    if (!board_.accepts(cells))
      return false;
    adopt(cells, pivot_ + offset);
    return true;
  }

  // §3.4 and §3.5: rotate about the §3.3 pivot, then let §3.5's kick table
  // decide whether the result fits. A rejected rotation changes nothing at all,
  // including the lock timer.
  bool rotate(int cmd) {
    Cell position = pivot_;
    Cells cells;
    if (!rotatedPlacement(board_, cells_, pivot_, cmd, &position, &cells,
                          nullptr))
      return false;
    adopt(cells, position);
    return true;
  }

  void registerMove() {
    if (moveResets_ < kMoveResetCap) { // §4.1 anti-stall cap
      ++moveResets_;
      lockTimer_ = 0;
      // The reset is itself the first of the thirty frames, so that a piece
      // nudged into place gets the whole delay rather than thirty-one frames of
      // it.
      lockStarted_ = true;
    }
  }

  void stepAutoRepeat(const Input &input) {
    // The initial press is a command; this is the DAS/ARR repeat behind it.
    for (int dir :
         {kMoveXNeg, kMoveXPos, kMoveYPos, kMoveYNeg, kMoveZNeg, kMoveZPos}) {
      const bool held = input.held[dir];
      if (!held) {
        heldFrames_[dir] = 0;
        continue;
      }
      const int frames = ++heldFrames_[dir] - 1; // frames since the press
      if (frames >= kDasFrames && (frames - kDasFrames) % kArrFrames == 0) {
        moveBy(dir);
      }
    }
  }

  void moveBy(int dir) {
    switch (dir) {
    case kMoveXNeg:
      translate(Cell{-1, 0, 0});
      break;
    case kMoveXPos:
      translate(Cell{1, 0, 0});
      break;
    case kMoveYPos:
      translate(Cell{0, 1, 0});
      break;
    case kMoveYNeg:
      translate(Cell{0, -1, 0});
      break;
    case kMoveZNeg:
      translate(Cell{0, 0, -1});
      break;
    default:
      translate(Cell{0, 0, 1});
      break;
    }
  }

  void moveCursorBy(int dir) {
    Cell target = cursor_;
    switch (dir) {
    case kMoveXNeg:
      --target.x;
      break;
    case kMoveXPos:
      ++target.x;
      break;
    case kMoveYPos:
      ++target.y;
      break;
    case kMoveYNeg:
      --target.y;
      break;
    case kMoveZNeg:
      --target.z;
      break;
    default:
      ++target.z;
      break;
    }
    target.x = std::clamp(target.x, 0, kFootX - 1);
    target.y = std::clamp(target.y, 0, kFootY - 1);
    target.z = std::clamp(target.z, kShaftZMin, kShaftZMax);
    cursor_ = target;
  }

  void stepFalling(const Input &input) {
    for (const Cmd cmd : input.cmds) {
      switch (cmd) {
      case kMoveXNeg:
      case kMoveXPos:
      case kMoveYPos:
      case kMoveYNeg:
      case kMoveZNeg:
      case kMoveZPos:
        if (translate(stepOffset(cmd)))
          registerMove();
        break;
      case kRotXPos:
      case kRotXNeg:
      case kRotYPos:
      case kRotYNeg:
      case kRotZPos:
      case kRotZNeg:
        if (rotate(cmd))
          registerMove();
        break;
      case kSettle:
        actSettle();
        break;
      case kRaze:
      case kSeed:
      case kAdvance:
        break; // §9.1: rejected while a piece is active
      case kHardDrop: {
        // §4.1 hard drop bypasses the lock delay entirely.
        int drop = 0;
        while (translate(Cell{0, 0, -1}))
          ++drop;
        score_ += 2LL * drop;
        lockPiece();
        return;
      }
      default:
        break;
      }
      if (phase_ != kFalling)
        return; // a Settle can end the game
    }

    stepAutoRepeat(input);

    if (input.softDrop) { // §4.1 soft drop, on its own one frame timer
      if (++softTimer_ >= 1) {
        softTimer_ = 0;
        if (translate(Cell{0, 0, -1}))
          score_ += 1;
      }
    } else {
      softTimer_ = 0;
      if (++fallTimer_ >= gravityFrames(depth_)) {
        fallTimer_ = 0;
        translate(Cell{0, 0, -1});
      }
    }

    // §4.1 the thirty frame delay begins on the frame *after* the piece is
    // first seen at rest, so a piece dropped from the Crown spends exactly its
    // descent times gravityFrames frames falling and then thirty more before it
    // locks, with no off-by-one depending on which frame it landed in.
    if (!resting()) {
      lockStarted_ = false;
      lockTimer_ = 0;
    } else if (lockStarted_) {
      if (++lockTimer_ >= kLockDelayFrames)
        lockPiece();
    } else {
      lockStarted_ = true;
    }
  }

  void stepBetweenTurns(const Input &input) {
    for (const Cmd cmd : input.cmds) {
      switch (cmd) {
      case kMoveXNeg:
      case kMoveXPos:
      case kMoveYPos:
      case kMoveYNeg:
      case kMoveZNeg:
      case kMoveZPos:
        moveCursorBy(cmd);
        break;
      case kRaze:
        actRaze(cursor_);
        break;
      case kSeed:
        actSeed(cursor_);
        break;
      case kSettle:
        break; // §9.1: Settle needs an active piece
      case kAdvance:
        betweenFrames_ = 0;
        break;
      default:
        break;
      }
      if (phase_ != kBetweenTurns)
        return;
    }

    stepAutoRepeat(input);

    if (--betweenFrames_ <= 0)
      startTurn();
  }

  static Cell stepOffset(int dir) {
    switch (dir) {
    case kMoveXNeg:
      return {-1, 0, 0};
    case kMoveXPos:
      return {1, 0, 0};
    case kMoveYPos:
      return {0, 1, 0};
    case kMoveYNeg:
      return {0, -1, 0};
    case kMoveZNeg:
      return {0, 0, -1};
    default:
      return {0, 0, 1};
    }
  }

  Board board_;
  Bag bag_;
  uint64_t seed_ = 1;
  int64_t frame_ = 0;
  int64_t score_ = 0;
  int depth_ = 1;
  int charges_ = kChargesAtStart;
  int chargeProgress_ = 0;
  int piecesLocked_ = 0;
  int cellsCleared_ = 0;
  int64_t bestChain_ = 0;
  int lastChain_ = 0;
  bool graceUsed_ = false;
  bool fault_ = false;
  Phase phase_ = kBetweenTurns;
  int betweenFrames_ = 1;
  int piece_ = kBlock;
  int orient_ = 0;
  Cell pivot_;
  Cells cells_;
  int lockTimer_ = 0;
  bool lockStarted_ = false;
  int fallTimer_ = 0;
  int moveResets_ = 0;
  int softTimer_ = 0;
  int heldFrames_[kCmdCount] = {};
  Cell cursor_;
  std::string toast_;
  int toastFrames_ = 0;
};

} // namespace tetralis

#endif // OPENCV_V4D_SAMPLES_TETRALIS_RULES_HPP