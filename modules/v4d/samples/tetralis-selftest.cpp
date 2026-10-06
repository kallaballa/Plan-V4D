// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// Tetralis -- the check suite.
//
// The nine checks of Appendix D of tetralis_rules.md, plus the invariants of
// Appendix B and the constant tables of §3.1, §3.6, §9.2, §11.1, §11.2 and §12.
// It includes only `tetralis-rules.hpp`, which touches no window, no clock and
// no random device, so it builds and runs as a plain console program:
//
//   ./modules/v4d/samples/tetralis-selftest.sh
//
// Exit status is the number of failed checks, capped at 125, so a non-zero
// status always means something is wrong.

#include "tetralis-rules.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using tetralis::Board;
using tetralis::Cell;
using tetralis::Cells;
using tetralis::Cmd;
using tetralis::Component;
using tetralis::Game;
using tetralis::Input;
using tetralis::Kind;
using tetralis::PassRecord;
using tetralis::ResolveResult;
using tetralis::Rng;
using tetralis::Spine;

namespace {

int g_checks = 0;
int g_failures = 0;
const char *g_section = "";

void section(const char *name) {
  g_section = name;
  std::printf("%s\n", name);
}

void check(bool ok, const std::string &what) {
  ++g_checks;
  if (ok)
    return;
  ++g_failures;
  std::printf("  FAIL [%s] %s\n", g_section, what.c_str());
}

void checkEq(long long got, long long want, const std::string &what) {
  ++g_checks;
  if (got == want)
    return;
  ++g_failures;
  std::printf("  FAIL [%s] %s: got %lld, want %lld\n", g_section, what.c_str(),
              got, want);
}

void checkEq(const std::string &got, const std::string &want,
             const std::string &what) {
  ++g_checks;
  if (got == want)
    return;
  ++g_failures;
  std::printf("  FAIL [%s] %s:\n    got  %s\n    want %s\n", g_section,
              what.c_str(), got.c_str(), want.c_str());
}

std::string cellText(const Cell &c) {
  return "(" + std::to_string(c.x) + "," + std::to_string(c.y) + "," +
         std::to_string(c.z) + ")";
}

std::string cellsText(const Cells &cells) {
  std::string out;
  for (const Cell &c : cells) {
    if (!out.empty())
      out += " ";
    out += cellText(c);
  }
  return out;
}

/** @brief The absolute cells of an orientation whose pivot cell sits at `pos`.
 */
Cells at(const tetralis::PieceInfo &info, int orientation, const Cell &pos) {
  const tetralis::Orientation &o = info.orientations[orientation];
  Cells cells;
  for (const Cell &c : o.cells)
    cells.push_back(pos + (c - o.pivot));
  std::sort(cells.begin(), cells.end());
  return cells;
}

/** @brief The §3.4 rotation alone, about the pivot, with no kick applied. */
Cells bareRotation(const Cells &cells, const Cell &pivot, int cmd) {
  const tetralis::Rotation rotation = tetralis::rotationFor(cmd);
  Cells turned;
  for (const Cell &c : cells)
    turned.push_back(pivot + rotation.apply(c - pivot));
  std::sort(turned.begin(), turned.end());
  return turned;
}

int sumZ(const Board &board) {
  int total = 0;
  for (int x = 0; x < tetralis::kFootX; ++x) {
    for (int y = 0; y < tetralis::kFootY; ++y) {
      for (int z = 1; z <= tetralis::kShaftZMax; ++z) {
        if (board.at(x, y, z) != Kind::kEmpty)
          total += z;
      }
    }
  }
  return total;
}

std::string boardHash(const Board &board) {
  uint64_t hash = 1469598103934665603ULL;
  for (int x = 0; x < tetralis::kFootX; ++x) {
    for (int y = 0; y < tetralis::kFootY; ++y) {
      for (int z = 1; z <= tetralis::kShaftZMax; ++z) {
        hash = (hash ^ static_cast<uint64_t>(board.at(x, y, z))) *
               1099511628211ULL;
      }
    }
  }
  char text[32];
  std::snprintf(text, sizeof(text), "%016llx",
                static_cast<unsigned long long>(hash));
  return text;
}

/** @brief The index of the component whose cells are exactly `want`, or -1.

An index rather than a pointer on purpose: the component lists are built by
value, and a pointer into a temporary would outlive it.
*/
int findComponent(const std::vector<Component> &components, const Cells &want) {
  for (std::size_t i = 0; i < components.size(); ++i)
    if (components[i].cells == want)
      return static_cast<int>(i);
  return -1;
}

Board boardWith(const Cells &cells, Kind kind = Kind::kNormal) {
  Board board;
  for (const Cell &c : cells)
    board.set(c, kind);
  return board;
}

// ---------------------------------------------------------------------------
// §15 constants
// ---------------------------------------------------------------------------

void testConstants() {
  section("constants (§15)");
  checkEq(tetralis::kFootX, 4, "footprint x");
  checkEq(tetralis::kFootY, 4, "footprint y");
  checkEq(tetralis::kBedrockZ, 0, "bedrock");
  checkEq(tetralis::kShaftZMin, 1, "shaft floor");
  checkEq(tetralis::kShaftZMax, 15, "crown");
  checkEq(tetralis::kCapacity, 240, "capacity");
  checkEq(tetralis::kCellsPerLayer, 16, "cells per layer");
  checkEq(tetralis::kSpineLength, 7, "Spine minimum");
  checkEq(tetralis::kFramesPerSecond, 60, "simulation rate");
  checkEq(tetralis::kDasFrames, 10, "DAS");
  checkEq(tetralis::kArrFrames, 2, "ARR");
  checkEq(tetralis::kLockDelayFrames, 30, "lock delay");
  checkEq(tetralis::kMoveResetCap, 15, "move reset cap");
  checkEq(tetralis::kChargesAtStart, 1, "starting charges");
  checkEq(tetralis::kChargeCap, 5, "charge cap");
  checkEq(tetralis::kPiecesPerCharge, 15, "pieces per charge");
  checkEq(tetralis::kRazeMinZ, 2, "Raze cannot reach z = 1");
  checkEq(sizeof(tetralis::kKickOffsets) / sizeof(Cell), 7, "kick offsets");

  // §11.1 the depth thresholds.
  checkEq(tetralis::cellsNeededForDepth(1), 15, "Q(1)");
  checkEq(tetralis::cellsNeededForDepth(2), 45, "Q(2)");
  checkEq(tetralis::cellsNeededForDepth(3), 90, "Q(3)");
  checkEq(tetralis::cellsNeededForDepth(4), 150, "Q(4)");

  // §11.2 the printed table; the formula is normative and agrees to depth 10.
  const int kTable[11] = {17, 14, 12, 11, 9, 8, 7, 6, 5, 4, 4};
  for (int depth = 1; depth <= 11; ++depth)
    checkEq(tetralis::gravityFrames(depth), kTable[depth - 1],
            "G(" + std::to_string(depth) + ")");
  check(tetralis::gravityFrames(64) >= 2, "gravity floors at two frames");
}

// ---------------------------------------------------------------------------
// §3 the roster
// ---------------------------------------------------------------------------

void testRoster() {
  section("roster (§3.1, §3.3, §3.4, §3.6)");
  const int kExpectedOrientations[tetralis::kPieceCount] = {3,  3, 12, 24,
                                                            12, 8, 12, 12};
  const int kExpectedBox[tetralis::kPieceCount][3] = {
      {4, 1, 1}, {2, 2, 1}, {3, 2, 1}, {2, 3, 1},
      {3, 2, 1}, {2, 2, 2}, {2, 2, 2}, {2, 2, 2}};
  const int kExpectedPivot[tetralis::kPieceCount][3] = {
      {1, 0, 0}, {0, 0, 0}, {1, 0, 0}, {0, 1, 0},
      {1, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
  const int kExpectedSpawn[tetralis::kPieceCount][3] = {
      {0, 1, 15}, {1, 1, 15}, {0, 1, 15}, {1, 0, 15},
      {0, 1, 15}, {1, 1, 14}, {1, 1, 14}, {1, 1, 14}};

  int totalOrientations = 0;
  for (int p = 0; p < tetralis::kPieceCount; ++p) {
    const tetralis::PieceInfo &info = tetralis::pieceInfo(p);
    const std::string name = info.name;
    checkEq(info.w, kExpectedBox[p][0], name + " w");
    checkEq(info.d, kExpectedBox[p][1], name + " d");
    checkEq(info.h, kExpectedBox[p][2], name + " h");
    checkEq(info.pivot.x, kExpectedPivot[p][0], name + " pivot x");
    checkEq(info.pivot.y, kExpectedPivot[p][1], name + " pivot y");
    checkEq(info.pivot.z, kExpectedPivot[p][2], name + " pivot z");
    checkEq(info.spawn.x, kExpectedSpawn[p][0], name + " spawn x");
    checkEq(info.spawn.y, kExpectedSpawn[p][1], name + " spawn y");
    checkEq(info.spawn.z, kExpectedSpawn[p][2], name + " spawn z");
    checkEq(static_cast<int>(info.orientations.size()),
            kExpectedOrientations[p], name + " orientations");
    totalOrientations += static_cast<int>(info.orientations.size());

    bool pivotIsACell = false;
    for (const Cell &c : info.canonical)
      if (c == info.pivot)
        pivotIsACell = true;
    check(pivotIsACell, name + ": the §3.3 pivot is a cell of the piece");

    for (std::size_t o = 0; o < info.orientations.size(); ++o) {
      const tetralis::Orientation &orientation = info.orientations[o];
      const std::string label = name + " orientation " + std::to_string(o);
      checkEq(static_cast<int>(orientation.cells.size()), 4,
              label + " has four cells");
      Cells sorted = orientation.cells;
      std::sort(sorted.begin(), sorted.end());
      checkEq(cellsText(sorted), cellsText(orientation.cells),
              label + " cell list is sorted");
      checkEq(std::unique(sorted.begin(), sorted.end()) == sorted.end() ? 1 : 0,
              1, label + " cells are distinct");
      checkEq(cellsText(tetralis::normed(orientation.cells)),
              cellsText(orientation.cells), label + " is in normed form");
      bool pivotInOrientation = false;
      for (const Cell &c : orientation.cells)
        if (c == orientation.pivot)
          pivotInOrientation = true;
      check(pivotInOrientation,
            label + ": the pivot is a cell of this orientation");
      const Cells spawnCells =
          at(info, static_cast<int>(o), info.spawn + orientation.pivot);
      if (o == 0) {
        // §3.6 "horizontally centred and flush with the Crown": every piece
        // must spawn inside the shaft, and the Bar must occupy x = 0..3.
        bool inside = true;
        for (const Cell &c : spawnCells)
          if (!tetralis::insideShaft(c))
            inside = false;
        check(inside, name + " spawns inside the shaft");
      }
      if (p == tetralis::kBar && o == 0) {
        checkEq(cellsText(spawnCells), "(0,1,15) (1,1,15) (2,1,15) (3,1,15)",
                "Bar spawns centred across the footprint");
      }
    }
  }
  checkEq(totalOrientations, 86, "total distinct orientations");

  // §3.4 there are exactly 24 proper rotations, all of determinant +1, and the
  // six player-facing commands are among them.
  const std::vector<tetralis::Rotation> rotations = tetralis::properRotations();
  checkEq(static_cast<int>(rotations.size()), 24, "proper rotations");
  int unitDet = 0;
  for (const tetralis::Rotation &rotation : rotations)
    if (rotation.determinant() == 1)
      ++unitDet;
  checkEq(unitDet, 24, "every proper rotation has determinant +1");
  for (int cmd = tetralis::kRotXPos; cmd <= tetralis::kRotZNeg; ++cmd)
    checkEq(tetralis::rotationFor(cmd).determinant(), 1,
            std::string("command ") + tetralis::pieceName(cmd) +
                " has determinant +1");
}

// ---------------------------------------------------------------------------
// Check 1: Appendix A mechanics
// ---------------------------------------------------------------------------

void testPlumbMechanics() {
  section("check 1: Plumb, rigid fall, anchoring, Spine detection (App. A)");
  const Cells seeded = {{0, 0, 1}, {0, 0, 2}, {0, 1, 1}, {0, 1, 2},
                        {0, 2, 1}, {0, 2, 2}, {1, 1, 2}};
  Board board = boardWith(seeded);
  checkEq(board.cellCount(), 7, "the seeded shaft holds seven cells");

  // §5.1 the seven cells are one face-connected component and it is anchored,
  // because it touches z = 1. Appendix A's prose instead treats (0,0,3) as a
  // component of its own and (1,1,2) as unanchored; both readings contradict
  // §5.1 face connectivity, so what is asserted here is the normative rule.
  const std::vector<Component> before = tetralis::components(board);
  checkEq(static_cast<int>(before.size()), 1, "one component");
  checkEq(cellsText(before[0].cells), cellsText(seeded), "its cells");
  check(before[0].anchored, "it is anchored: it touches the floor layer");

  // A cell stacked on top stays in the same, anchored component.
  Board stacked = board;
  stacked.set(Cell{0, 0, 3}, Kind::kNormal);
  checkEq(static_cast<int>(tetralis::components(stacked).size()), 1,
          "a cell above the stack is not a separate component");
  checkEq(tetralis::fallDistance(stacked, tetralis::components(stacked)[0],
                                 nullptr),
          0, "the stack cannot fall");

  // The Block of Appendix A: four cells at z = 4, disconnected from the stack.
  const Cells blockCells = {{0, 1, 4}, {1, 1, 4}, {0, 2, 4}, {1, 2, 4}};
  for (const Cell &c : blockCells)
    board.set(c, Kind::kNormal);
  checkEq(board.cellCount(), 11, "the shaft now has eleven cells");

  const std::vector<Component> withBlock = tetralis::components(board);
  checkEq(static_cast<int>(withBlock.size()), 2, "two components");
  const Cells blockComponent = {{0, 1, 4}, {0, 2, 4}, {1, 1, 4}, {1, 2, 4}};
  const int floating = findComponent(withBlock, blockComponent);
  check(floating >= 0, "the Block is one rigid body");
  if (floating >= 0) {
    check(!withBlock[floating].anchored, "the Block is unanchored");
    // (0,1,4) has its highest blocker at z = 2, so 4 - 1 - 2 = 1; (1,2,4) has
    // nothing beneath it and could fall 3. The component takes the minimum.
    checkEq(tetralis::fallDistance(board, withBlock[floating], nullptr), 1,
            "the Block falls exactly one layer");
  }

  tetralis::plumb(board);
  checkEq(board.cellCount(), 11, "Plumb conserves the cell count");
  checkEq(board.at(0, 1, 3) != Kind::kEmpty &&
              board.at(1, 1, 3) != Kind::kEmpty &&
              board.at(0, 2, 3) != Kind::kEmpty &&
              board.at(1, 2, 3) != Kind::kEmpty,
          1, "the Block landed one layer lower, as one body");
  checkEq(board.at(0, 1, 4) == Kind::kEmpty, 1, "nothing was left at z = 4");
  // 1 + 2 + 1 + 2 + 1 + 2 + 2 for the seeded shaft, plus 3 + 3 + 3 + 3 for the
  // Block after it fell one layer.
  checkEq(sumZ(board), 23, "the board is lower by exactly four than before");

  // §6.1 the tallest run here is three, so resolution finds nothing and the
  // chain is 0 -- the outcome Appendix A states.
  const std::vector<Spine> spines = tetralis::findSpines(board);
  checkEq(static_cast<int>(spines.size()), 0, "no Spine qualifies");
  Board probe = board;
  const ResolveResult result = tetralis::resolve(probe);
  checkEq(result.chain, 0, "the chain is 0");
  checkEq(result.cellsCleared, 0, "nothing clears");
  checkEq(boardHash(probe), boardHash(board), "resolve leaves it alone");
}

// ---------------------------------------------------------------------------
// Checks 2 and 3: the Elbow, and the drop distance being a minimum
// ---------------------------------------------------------------------------

void testElbowAndDropDistance() {
  section("checks 2-3: Elbow fall, drop distance is the minimum");
  const tetralis::PieceInfo &elbow = tetralis::pieceInfo(tetralis::kElbow);
  checkEq(cellsText(elbow.canonical), "(0,0,0) (0,1,0) (0,2,0) (1,2,0)",
          "Elbow canonical form");
  checkEq(cellText(elbow.pivot), "(0,1,0)", "Elbow pivot");

  // Pivot position (0,1,10), with a single blocker under the (0,1,10) cell.
  Board board = boardWith({{0, 1, 4}});
  const Cells placed = at(elbow, 0, Cell{0, 1, 10});
  checkEq(cellsText(placed), "(0,0,10) (0,1,10) (0,2,10) (1,2,10)",
          "the placed Elbow");
  for (const Cell &c : placed)
    board.set(c, Kind::kNormal);
  const std::vector<Component> comps = tetralis::components(board);
  checkEq(static_cast<int>(comps.size()), 2, "the obstruction and the Elbow");
  const int obstruction = findComponent(comps, Cells{{0, 1, 4}});
  const int arm = findComponent(comps, placed);
  check(obstruction >= 0 && arm >= 0, "both are single components");
  if (obstruction >= 0 && arm >= 0) {
    check(!comps[obstruction].anchored,
          "the obstruction is itself floating, which does not matter here: "
          "fallDistance is a property of the component asked about");
    // (0,1,10) is limited by (0,1,4): 10 - 1 - 4 = 5. The other three cells
    // could fall 9 each, and the component takes the minimum: five layers.
    checkEq(tetralis::fallDistance(board, comps[arm], nullptr), 5,
            "the Elbow falls five layers, as predicted");
  }

  // Check 3, and the first bug of Appendix D: a cell above must not count.
  Board column = boardWith({{0, 0, 2}, {0, 0, 8}});
  Board one = column;
  one.set(Cell{0, 0, 5}, Kind::kNormal);
  const std::vector<Component> tower = tetralis::components(one);
  checkEq(static_cast<int>(tower.size()), 3, "three separate components");
  const int floater = findComponent(tower, Cells{{0, 0, 5}});
  const int above = findComponent(tower, Cells{{0, 0, 8}});
  check(floater >= 0 && !tower[floater].anchored, "a lone cell is unanchored");
  if (floater >= 0) {
    checkEq(tetralis::fallDistance(one, tower[floater], nullptr), 2,
            "only cells strictly below count: (0,0,8) is ignored");
    tetralis::plumb(one);
    checkEq(one.at(0, 0, 3) != Kind::kEmpty, 1,
            "the whole column settles into one stack on the floor");
    checkEq(one.at(0, 0, 1) != Kind::kEmpty && one.at(0, 0, 2) != Kind::kEmpty,
            1, "at (0,0,1) and (0,0,2)");
    checkEq(one.at(0, 0, 4) == Kind::kEmpty, 1,
            "and nothing is left above z = 3, so the cell at z = 8 never held "
            "up the one below it");
  }
  check(above >= 0, "the cell at (0,0,8) was its own component");
}

// ---------------------------------------------------------------------------
// Check 4: Rime
// ---------------------------------------------------------------------------

void testRime() {
  section("check 4: Rime is inert, anchors, and dies only in a mixed Spine");
  Cells tower;
  for (int z = 1; z <= 7; ++z)
    tower.push_back(Cell{0, 0, z});
  Board rime = boardWith(tower, Kind::kRime);
  checkEq(static_cast<int>(tetralis::findSpines(rime).size()), 0,
          "seven Rime is not a Spine: §6.1 needs a Normal");
  checkEq(tetralis::components(rime)[0].anchored, 1,
          "Rime anchors its component");

  // A component holding Rime does not fall even though it is in mid-air.
  Board held = boardWith({{2, 3, 5}}, Kind::kRime);
  const std::vector<Component> heldComponents = tetralis::components(held);
  checkEq(static_cast<int>(heldComponents.size()), 1, "one component");
  check(!heldComponents.empty() && heldComponents[0].anchored,
        "Rime in mid-air anchors");
  tetralis::plumb(held);
  checkEq(held.at(2, 3, 5) != Kind::kEmpty, 1, "so it stays where it is");

  Board normal = boardWith({{2, 3, 5}});
  tetralis::plumb(normal);
  checkEq(normal.at(2, 3, 1) != Kind::kEmpty, 1,
          "the same cell in Normal falls to the floor");

  // One Normal in the tower makes the run a Spine, and the Spine takes the
  // Rime with it.
  Board mixed = boardWith(tower, Kind::kRime);
  mixed.set(Cell{0, 0, 1}, Kind::kNormal);
  const std::vector<Spine> spines = tetralis::findSpines(mixed);
  checkEq(static_cast<int>(spines.size()), 1, "the run is now a Spine");
  checkEq(static_cast<int>(spines[0].cells.size()), 7, "its length");
  Board resolved = mixed;
  const ResolveResult result = tetralis::resolve(resolved);
  checkEq(result.cellsCleared, 7, "the whole run clears, Rime included");
  checkEq(resolved.cellCount(), 0, "including the Rime cells");

  // A lone Rime that a clear does not touch survives.
  Board keep = boardWith(tower, Kind::kNormal);
  keep.set(Cell{3, 3, 9}, Kind::kRime);
  Board keepResolved = keep;
  const ResolveResult keepResult = tetralis::resolve(keepResolved);
  checkEq(keepResult.cellsCleared, 7, "the tower clears");
  checkEq(keepResolved.at(3, 3, 9) != Kind::kEmpty, 1,
          "the distant Rime is untouched");
}

// ---------------------------------------------------------------------------
// Check 7: blockers
// ---------------------------------------------------------------------------

void testBlockers() {
  section("check 7: blockers pin a component");
  // The tower has to reach Bedrock: §5.2 anchors on z = 1, and a cell at z = 2
  // with nothing under it is itself unanchored and would fall first.
  Board board = boardWith({{2, 2, 1}, {2, 2, 2}});
  board.set(Cell{2, 2, 5}, Kind::kNormal);
  const std::vector<Component> comps = tetralis::components(board);
  const int floater = findComponent(comps, Cells{{2, 2, 5}});
  check(floater >= 0, "the floating cell is a component");
  if (floater < 0)
    return;
  checkEq(tetralis::fallDistance(board, comps[floater], nullptr), 2,
          "its free distance is 2");
  const Cells blockers = {{2, 2, 4}};
  checkEq(tetralis::fallDistance(board, comps[floater], &blockers), 0,
          "a blocker underneath pins it");
  tetralis::plumb(board, &blockers);
  checkEq(board.at(2, 2, 5) != Kind::kEmpty, 1, "and Plumb leaves it alone");
  tetralis::plumb(board, nullptr);
  checkEq(board.at(2, 2, 2) != Kind::kEmpty, 1,
          "without blockers it falls onto the floor stack");
}

// ---------------------------------------------------------------------------
// Checks 5, 6 and 9: randomised play, B4, and the real turn machine
// ---------------------------------------------------------------------------

/** @brief The invariants of Appendix B, asserted after every lock. */
void assertAtRest(const Game &game, const std::string &where) {
  check(game.board().allCellsInside(), where + ": B3, every cell is inside");
  checkEq(static_cast<long long>(tetralis::findSpines(game.board()).size()), 0,
          where + ": B5, no Spine is available at rest");
  check(game.bestChain() <= 34, where + ": B6, the chain is bounded by 34");
  check(!game.fault(), where + ": no §10.3 fault");
}

/** @brief A deterministic random command stream, independent of the game. */
struct Script {
  Rng rng;
  int locks = 0;
  bool charges = false;

  explicit Script(uint64_t seed) : rng(seed) {}

  /** @brief One frame of input for `game`, aimed at keeping it playing. */
  Input frame(Game &game) {
    Input input;
    const uint64_t roll = rng.next();
    if (game.active()) {
      if (roll % 7 == 0) {
        input.cmds.push_back(Cmd(rng.next() % 12)); // a translate or a rotate
      } else if (roll % 23 == 0) {
        input.cmds.push_back(Cmd(rng.next() % 12));
        input.cmds.push_back(Cmd(rng.next() % 12));
      } else if (roll % 5 == 0) {
        input.cmds.push_back(tetralis::kHardDrop);
      }
      if (charges && roll % 11 == 0)
        input.cmds.push_back(tetralis::kSettle);
    } else {
      // Between turns, skip ahead; occasionally spend a charge on the cursor.
      input.cmds.push_back(tetralis::kAdvance);
      if (charges && roll % 4 == 0 && game.charges() > 0 &&
          game.cursorRazeable()) {
        input.cmds.push_back(tetralis::kRaze);
      }
    }
    return input;
  }
};

struct RunResult {
  int64_t score = 0;
  int depth = 0;
  int frames = 0;
  int locks = 0;
  int restarts = 0;
  std::string hash;
};

/** @brief Play until `lockTarget` locks have happened, restarting when Struck.
 */
RunResult play(uint64_t seed, int lockTarget, bool charges) {
  Game game(seed);
  Script script(seed ^ 0x9E3779B97F4A7C15ULL);
  RunResult result;
  int locked = 0;
  int steps = 0;
  const int kStepCap = lockTarget * 2000 + 1000000;
  while (locked < lockTarget && steps++ < kStepCap) {
    if (game.struck()) {
      assertAtRest(game, "a struck board");
      game.startGame(seed + 1000 + ++result.restarts);
    }
    const int before = game.piecesLocked();
    game.step(script.frame(game));
    if (game.piecesLocked() != before) {
      locked += game.piecesLocked() - before;
      assertAtRest(game, "after a lock");
    }
  }
  check(locked >= lockTarget,
        "the run reached " + std::to_string(lockTarget) + " locks");
  result.score = game.score64();
  result.depth = game.depth();
  result.frames = static_cast<int>(game.frame());
  result.locks = locked;
  result.hash = boardHash(game.board());
  return result;
}

void testRandomisedLocks() {
  section("check 5: 20,000 randomised locks");
  const RunResult run = play(20240617u, 20000, false);
  std::printf("  %d locks, %d restarts, depth %d, %lld points\n", run.locks,
              run.restarts, run.depth, static_cast<long long>(run.score));
}

void testBendOfPlumb() {
  section("check 6: B4, an unanchored component that cannot fall");
  Rng rng(987654321u);
  int violations = 0;
  int rises = 0;
  for (int trial = 0; trial < 3000; ++trial) {
    Board board;
    const int cells = 4 + static_cast<int>(rng.next() % 60);
    for (int i = 0; i < cells; ++i) {
      const Cell c{static_cast<int>(rng.next() % tetralis::kFootX),
                   static_cast<int>(rng.next() % tetralis::kFootY),
                   1 + static_cast<int>(rng.next() % tetralis::kShaftZMax)};
      board.set(c, (rng.next() % 4 == 0) ? Kind::kRime : Kind::kNormal);
    }
    const int before = sumZ(board);
    tetralis::plumb(board);
    if (sumZ(board) > before)
      ++rises; // B1, Σ z may never increase
    if (!board.allCellsInside())
      ++violations;
    for (const Component &component : tetralis::components(board)) {
      if (component.anchored)
        continue;
      // At a fixpoint with no blockers, an unanchored component cannot still
      // have room to fall: if it did, Plumb had not finished.
      if (tetralis::fallDistance(board, component, nullptr) > 0)
        ++violations;
    }
  }
  checkEq(rises, 0, "B1: Plumb never raises the board");
  checkEq(violations, 0, "B3 and B4 hold over 3,000 random boards");
}

void testRealGame() {
  section("check 9: a long game through the real turn machine");
  const RunResult run = play(4242u, 3000, true);
  std::printf("  %d locks, %d restarts, depth %d, %lld points\n", run.locks,
              run.restarts, run.depth, static_cast<long long>(run.score));

  section("§14: the same seed and the same commands replay exactly");
  const RunResult again = play(4242u, 3000, true);
  checkEq(again.hash, run.hash, "the boards are identical");
  checkEq(again.score, run.score, "the scores are identical");
  checkEq(again.depth, run.depth, "the depths are identical");
  checkEq(again.frames, run.frames, "the frame counts are identical");

  const RunResult other = play(4243u, 3000, true);
  check(other.hash != run.hash || other.score != run.score,
        "a different seed plays differently");
}

// ---------------------------------------------------------------------------
// §8, §9, §10: Rime and charges, Raze and Seed, Struck
// ---------------------------------------------------------------------------

void testCharges() {
  section("§9: Anchor Charges, Raze, Seed, Settle");
  Game game(7u);
  checkEq(game.charges(), tetralis::kChargesAtStart, "one charge to start");
  check(!game.active(), "the first frame is a between-turns frame");
  Input advance;
  advance.cmds.push_back(tetralis::kAdvance);
  game.step(advance);
  check(game.active(), "a piece is dealt");

  // §9.1 Settle needs an active piece; Raze and Seed must not have one.
  const Cell crown{0, 0, tetralis::kShaftZMax};
  const Cell floor{1, 1, 1};
  const int64_t scoreBefore = game.score64();
  check(!game.actSeed(crown), "Seed is refused while a piece is active");
  check(!game.actRaze(floor), "Raze is refused while a piece is active");
  checkEq(game.charges(), 1, "a refused action costs nothing");

  check(game.actSettle(), "Settle is allowed while a piece is active");
  checkEq(game.charges(), 0, "Settle costs one charge");
  checkEq(game.score64(), scoreBefore, "Settle itself scores nothing");

  // Between turns, Seed places a Rime and Raze deletes a Normal cell.
  Game seeding(11u);
  seeding.step(advance);
  check(seeding.active(), "a piece is dealt and is active");
  Input drop;
  drop.cmds.push_back(tetralis::kHardDrop);
  seeding.step(drop);
  check(!seeding.active(), "a lock ends the turn, so no piece is active");
  const Cell corner{3, 3, 14};
  const Cell bedrock{3, 3, tetralis::kBedrockZ};
  check(seeding.actSeed(corner), "Seed places Rime between turns");
  checkEq(seeding.charges(), 0, "Seed costs one charge");
  check(seeding.board().at(corner) != Kind::kEmpty, "a cell was filled");
  check(!seeding.actSeed(corner), "Seed refuses an occupied cell");
  check(!seeding.actSeed(bedrock), "Seed refuses bedrock");

  Game razing(13u);
  razing.step(advance);
  razing.step(drop);
  check(!razing.active(), "a lock ends the turn, so Raze is legal again");
  const Cell tower{1, 1, 4};
  razing.board().set(tower, Kind::kNormal);
  razing.board().set(floor, Kind::kNormal);
  check(razing.actRaze(tower), "Raze deletes a Normal cell at z = 4");
  checkEq(razing.board().at(tower) == Kind::kEmpty, 1, "the cell is gone");
  check(!razing.actRaze(floor),
        "Raze cannot touch the floor layer (§9.1, which is what keeps Bedrock "
        "out of reach)");
  checkEq(razing.board().at(floor) != Kind::kEmpty, 1,
          "and the floor cell is still there");
}

void testChargeAccrual() {
  section("§9.2: one charge per fifteen locked pieces, capped at five");
  Game game(5u);
  int locked = 0;
  int guard = 0;
  int peak = 0;
  while (locked < 16 * 6 && guard++ < 100000) {
    Input input;
    if (game.active()) {
      input.cmds.push_back(tetralis::kHardDrop);
    } else {
      input.cmds.push_back(tetralis::kAdvance);
    }
    const int before = game.piecesLocked();
    game.step(input);
    if (game.piecesLocked() != before) {
      locked += game.piecesLocked() - before;
      peak = std::max(peak, game.charges());
    }
    if (game.struck())
      break;
  }
  check(locked >= 15, "at least fifteen pieces locked");
  checkEq(game.chargeProgress(), locked % tetralis::kPiecesPerCharge,
          "the progress counter tracks the pieces");
  check(peak <= tetralis::kChargeCap, "charges never exceed the cap");
  if (locked >= 15)
    check(peak >= 2, "a charge arrives after fifteen pieces");
}

// ---------------------------------------------------------------------------
// Check 8 and the balance claim of Appendix C: a bot that can lose
// ---------------------------------------------------------------------------

struct Placement {
  int orientation = 0;
  Cell pivot;
};

/** @brief §10.1: every orientation at every position with four empty cells.

This is the definition of a legal placement, written out rather than delegated,
because it is the definition a loss is decided by and it is worth having in the
suite twice.
*/
std::vector<Placement> legalPlacements(const Board &board, int piece) {
  std::vector<Placement> out;
  const tetralis::PieceInfo &info = tetralis::pieceInfo(piece);
  for (std::size_t o = 0; o < info.orientations.size(); ++o) {
    const tetralis::Orientation &orientation = info.orientations[o];
    Cell lo{0, 0, 0};
    Cell hi{0, 0, 0};
    bool first = true;
    for (const Cell &c : orientation.cells) {
      const Cell rel = c - orientation.pivot;
      if (first) {
        lo = hi = rel;
        first = false;
        continue;
      }
      lo.x = std::min(lo.x, rel.x);
      lo.y = std::min(lo.y, rel.y);
      lo.z = std::min(lo.z, rel.z);
      hi.x = std::max(hi.x, rel.x);
      hi.y = std::max(hi.y, rel.y);
      hi.z = std::max(hi.z, rel.z);
    }
    for (int z = tetralis::kShaftZMin - lo.z; z <= tetralis::kShaftZMax - hi.z;
         ++z) {
      for (int y = -lo.y; y < tetralis::kFootY - hi.y; ++y) {
        for (int x = -lo.x; x < tetralis::kFootX - hi.x; ++x) {
          const Cell pivot{x, y, z};
          bool ok = true;
          for (const Cell &c : orientation.cells) {
            const Cell world = pivot + (c - orientation.pivot);
            if (!tetralis::insideShaft(world) || !board.isFree(world)) {
              ok = false;
              break;
            }
          }
          if (ok)
            out.push_back({static_cast<int>(o), pivot});
        }
      }
    }
  }
  return out;
}

struct BotRun {
  int turns = 0;
  int locks = 0;
  int cells = 0;
  int depth = 1;
  int longestChain = 0;
  bool struck = false;
};

/** @brief Appendix C's "lucky" bot: a legal placement, chosen uniformly at
random, every turn. It never fails to place and never aims.

The point of running this is the one Appendix D insists on: a harness that
cannot lose proves nothing. This one has to reach §10 and it has to reach it in
roughly the recorded number of turns.
*/
BotRun luckyBot(uint64_t seed, int maxTurns) {
  Board board;
  tetralis::Bag bag(seed);
  Rng rng(seed * 0x2545F4914F6CDD1DULL + 1);
  BotRun run;
  bool graceUsed = false;
  while (run.turns < maxTurns) {
    ++run.turns;
    int piece = bag.front();
    if (legalPlacements(board, piece).empty()) {
      // §10.2 one free Settle, no charge, no active piece, no blockers.
      if (!graceUsed) {
        graceUsed = true;
        const ResolveResult grace = tetralis::resolve(board);
        run.cells += grace.cellsCleared;
        run.longestChain =
            std::max(run.longestChain, static_cast<int>(grace.chain));
        while (run.cells >= tetralis::cellsNeededForDepth(run.depth))
          ++run.depth;
        bag.setDepth(run.depth);
      }
      piece = bag.front();
      if (legalPlacements(board, piece).empty()) {
        run.struck = true;
        break;
      }
    }
    const std::vector<Placement> options = legalPlacements(board, piece);
    if (options.empty())
      continue;
    bag.draw();
    const Placement &pick =
        options[static_cast<std::size_t>(rng.next() % options.size())];
    const tetralis::PieceInfo &info = tetralis::pieceInfo(piece);
    for (const Cell &c : at(info, pick.orientation, pick.pivot)) {
      check(tetralis::insideShaft(c), "the bot only places inside the shaft");
      board.set(c, Kind::kNormal); // §4.2 a lock writes four Normal cells
    }
    const ResolveResult result = tetralis::resolve(board);
    check(board.allCellsInside(), "B3, the bot never writes outside");
    checkEq(static_cast<long long>(tetralis::findSpines(board).size()), 0,
            "B5, no Spine is available at rest");
    check(result.chain <= 34, "B6, the chain is bounded by 34");
    run.cells += result.cellsCleared;
    run.longestChain =
        std::max(run.longestChain, static_cast<int>(result.chain));
    while (run.cells >= tetralis::cellsNeededForDepth(run.depth))
      ++run.depth;
    bag.setDepth(run.depth);
    ++run.locks;
  }
  return run;
}

void testTheGameCanBeLost() {
  section("check 8 and Appendix C: a random bot reaches Struck");
  std::vector<int> turns;
  for (uint64_t seed = 1; seed <= 8; ++seed) {
    const BotRun run = luckyBot(seed * 7919u, 5000);
    check(run.struck, "seed " + std::to_string(seed) + " is Struck eventually");
    if (run.struck)
      turns.push_back(run.turns);
  }
  std::sort(turns.begin(), turns.end());
  if (!turns.empty()) {
    std::printf("  turns to Struck:");
    for (int turn : turns)
      std::printf(" %d", turn);
    std::printf("  (median %d)\n", turns[turns.size() / 2]);
  }
  checkEq(static_cast<int>(turns.size()), 8,
          "all eight seeds are Struck: 8/8, as Appendix C records");
}

void testStruck() {
  section("check 8: Struck, by exhaustive search");
  Board empty;
  for (int p = 0; p < tetralis::kPieceCount; ++p)
    check(tetralis::hasLegalPlacement(empty, p),
          std::string(tetralis::pieceName(p)) + " fits an empty shaft");
  checkEq(static_cast<int>(legalPlacements(empty, tetralis::kBlock).size()),
          3 * 3 * 15 + 3 * 4 * 14 + 4 * 3 * 14,
          "the Block has three orientations, at every position of each");

  // A shaft full to the Crown: nothing fits anywhere.
  Board full;
  for (int x = 0; x < tetralis::kFootX; ++x)
    for (int y = 0; y < tetralis::kFootY; ++y)
      for (int z = 1; z <= tetralis::kShaftZMax; ++z)
        full.set(Cell{x, y, z}, Kind::kNormal);
  for (int p = 0; p < tetralis::kPieceCount; ++p)
    check(!tetralis::hasLegalPlacement(full, p),
          std::string(tetralis::pieceName(p)) + " does not fit a full shaft");
  checkEq(static_cast<int>(legalPlacements(full, tetralis::kBar).size()), 0,
          "and the enumeration agrees with the search");

  // Only the Bar fits: one free column, four layers at the top.
  Board narrow;
  for (int x = 0; x < tetralis::kFootX; ++x)
    for (int y = 0; y < tetralis::kFootY; ++y)
      for (int z = 1; z <= tetralis::kShaftZMax; ++z)
        if (!(x == 0 && y == 0 && z >= 12))
          narrow.set(Cell{x, y, z}, Kind::kNormal);
  check(tetralis::hasLegalPlacement(narrow, tetralis::kBar),
        "the Bar fits the one free column");
  check(!tetralis::hasLegalPlacement(narrow, tetralis::kBlock),
        "the Block does not");
  check(!tetralis::hasLegalPlacement(narrow, tetralis::kTee),
        "the Tee does not");

  // §10.2 the free Settle is a real rescue: a shaft full at the Crown is
  // sixteen Spines of fifteen, so the grace Settle empties it and play
  // continues. Struck is not merely "full".
  Game game(3u);
  for (int x = 0; x < tetralis::kFootX; ++x)
    for (int y = 0; y < tetralis::kFootY; ++y)
      for (int z = 1; z <= tetralis::kShaftZMax; ++z)
        game.board().set(Cell{x, y, z}, Kind::kNormal);
  Input advance;
  advance.cmds.push_back(tetralis::kAdvance);
  game.step(advance);
  check(game.board().cellCount() < tetralis::kCapacity,
        "the grace Settle clears a shaft that was full at the Crown");
  check(!game.struck(), "and the game is not over");
  check(game.active(), "the player keeps the piece that caused the problem");
}

void testSpawnOverlap() {
  section("§3.6 and §10.3: a piece that spawns overlapping");
  // Fill the Crown layer so that every piece spawns overlapping. The rules say
  // such a piece is dealt anyway and locks at once, and §10.3 promises that is
  // not a fault.
  Game game(17u);
  for (int x = 0; x < tetralis::kFootX; ++x)
    for (int y = 0; y < tetralis::kFootY; ++y)
      game.board().set(Cell{x, y, tetralis::kShaftZMax}, Kind::kNormal);
  Input advance;
  advance.cmds.push_back(tetralis::kAdvance);
  int locked = 0;
  int guard = 0;
  while (!game.struck() && guard++ < 60) {
    const int before = game.piecesLocked();
    game.step(advance);
    locked += game.piecesLocked() - before;
    check(!game.fault(), "an overlapping spawn is not a fault");
    check(game.board().allCellsInside(), "B3 holds through the overlap");
    checkEq(static_cast<long long>(tetralis::findSpines(game.board()).size()),
            0, "B5 holds through the overlap");
  }
  check(locked >= 1, "the overlapping piece locked immediately");
  check(game.board().countKind(Kind::kNormal) >= tetralis::kCellsPerLayer,
        "and the Crown layer is still occupied afterwards");
}

// ---------------------------------------------------------------------------
// §12 scoring
// ---------------------------------------------------------------------------

void testScoring() {
  section("§12: scoring");
  PassRecord plain;
  plain.chain = 1;
  plain.cells = 7;
  plain.spines = 1;
  plain.longestSpine = 7;
  checkEq(tetralis::scorePass(plain, 1), 700,
          "100 x cells x chain, and nothing else");

  PassRecord deep;
  deep.chain = 2;
  deep.cells = 14;
  deep.spines = 2;
  deep.longestSpine = 8;
  checkEq(tetralis::scorePass(deep, 1), 2800 + 1200,
          "Deep Run: +300 x spines x chain");

  PassRecord longSpine = plain;
  longSpine.longestSpine = 10;
  checkEq(tetralis::scorePass(longSpine, 1), 700 + 150, "Long Spine: +150");

  PassRecord crown = plain;
  crown.layerCleared = true;
  checkEq(tetralis::scorePass(crown, 1), 700 + 800, "Crown Sweep: +800");

  PassRecord sweep = plain;
  sweep.shaftEmpty = true;
  checkEq(tetralis::scorePass(sweep, 3), 700 + 15000,
          "Deep Clear: +5000 x depth");

  // A resolution that clears a real tower scores as the table says. A column
  // of eight Normals is a Spine of eight, in one pass. There is a stray cell at
  // the other corner, so the shaft does not end up empty and the Deep Clear
  // bonus must not be paid.
  Board tower;
  for (int z = 1; z <= 8; ++z)
    tower.set(Cell{2, 1, z}, Kind::kNormal);
  tower.set(Cell{3, 3, 1}, Kind::kNormal);
  Board resolved = tower;
  const ResolveResult result = tetralis::resolve(resolved);
  checkEq(result.chain, 1, "one pass");
  checkEq(result.passes.size(), 1, "one record");
  checkEq(result.cellsCleared, 8, "eight cells");
  checkEq(result.passes[0].longestSpine, 8, "the Spine is eight long");
  checkEq(tetralis::scorePass(result.passes[0], 1), 800,
          "100 x 8 x 1, and no bonus applies");
  checkEq(resolved.cellCount(), 1, "the stray cell survives the clear");

  // Without the stray cell the same clear is a Deep Clear.
  Board alone;
  for (int z = 1; z <= 8; ++z)
    alone.set(Cell{2, 1, z}, Kind::kNormal);
  Board aloneResolved = alone;
  const ResolveResult aloneResult = tetralis::resolve(aloneResolved);
  check(aloneResult.passes[0].shaftEmpty, "the shaft is left empty");
  checkEq(tetralis::scorePass(aloneResult.passes[0], 2), 800 + 10000,
          "Deep Clear: +5000 x the depth after the increase");

  // §4.1 drop scoring: two points a cell hard, one soft, nothing for a lock.
  Game game(19u);
  Input advance;
  advance.cmds.push_back(tetralis::kAdvance);
  game.step(advance);
  check(game.active(), "a piece is active");
  Input hard;
  hard.cmds.push_back(tetralis::kHardDrop);
  game.step(hard);
  // The Block spawns at z = 15 and lands on the floor, so it falls 14 cells.
  checkEq(game.score64(), 2 * 14, "a hard drop scores two a cell");
}

// ---------------------------------------------------------------------------
// §3.5 and §4.1: kicks, lock delay, the between-turn window
// ---------------------------------------------------------------------------

void testKicksAndLocks() {
  section("§3.5 and §4.1: kicks, lock delay, the between-turn window");
  // The Bar spans the footprint at the Crown. Rotating it upright at the
  // position given does not fit, so §3.5 has to find a kick that does.
  Game game(23u);
  Input advance;
  advance.cmds.push_back(tetralis::kAdvance);
  game.step(advance);
  check(game.active(), "a piece is active");

  // §3.5 The Bar spawns lying across the footprint at y = 1. Standing it up
  // about its §3.3 pivot is only possible at y = 0..3, which is the column the
  // pivot cell is already in; standing it at y = 3 would need y = -1..2 and
  // needs the (0,-1,0) kick to come back into the shaft.
  const tetralis::PieceInfo &info = tetralis::pieceInfo(tetralis::kBar);
  const Cell spawn = info.spawn + info.orientations[0].pivot;
  const Cells flat = at(info, 0, spawn);
  checkEq(cellsText(flat), "(0,1,15) (1,1,15) (2,1,15) (3,1,15)",
          "the Bar lies across the footprint");

  Board empty;
  Cell pivot = spawn;
  Cells turned;
  int kick = -1;
  check(tetralis::rotatedPlacement(empty, flat, spawn, tetralis::kRotZPos,
                                   &pivot, &turned, &kick),
        "the Bar stands upright");
  checkEq(kick, 0, "no kick is needed there: y = 0..3 all fit");
  checkEq(cellsText(turned), "(1,0,15) (1,1,15) (1,2,15) (1,3,15)",
          "and it is vertical in one column, all four layers");

  // §3.4 `z+` and `z-` are different proper rotations of the same solid, and a
  // pivot that is not in the middle of the piece makes them different
  // placements: the same four cells along y, shifted by one. A rotation is
  // about the §3.3 pivot cell, so the cell the pivot sits in stays put and the
  // rod hangs off it -- and an implementation that looked the result up as an
  // orientation and rebuilt the cells from that orientation's stored pivot
  // would report the same answer for both.
  kick = -1;
  check(tetralis::rotatedPlacement(empty, flat, spawn, tetralis::kRotZNeg,
                                   &pivot, &turned, &kick),
        "the Bar stands upright the other way as well");
  checkEq(kick, 3,
          "but z- wants y = -1..2, so the (0,+1,0) kick pulls it back in");
  checkEq(tetralis::kKickOffsets[kick].y, 1, "the fourth offset of the order");
  checkEq(cellsText(turned), "(1,3,15) (1,2,15) (1,1,15) (1,0,15)",
          "and it ends up a whole column along from z+");

  // The asymmetry is in the rotation itself, before any kick is applied: both
  // commands leave the pivot cell where it was and hang the rod off it, one way
  // and the other.
  Cells bare = bareRotation(flat, spawn, tetralis::kRotZPos);
  checkEq(cellsText(bare), "(1,0,15) (1,1,15) (1,2,15) (1,3,15)",
          "the bare z+ rotation reaches y = 0..3");
  bare = bareRotation(flat, spawn, tetralis::kRotZNeg);
  checkEq(cellsText(bare), "(1,-1,15) (1,0,15) (1,1,15) (1,2,15)",
          "and the bare z- rotation reaches y = -1..2, off the front edge");
  check(bare != turned, "so they are different placements, not the same one");

  // Block the column the Bar would stand in. The origin offset fails there and
  // the (+1,0,0) offset -- the second in §3.5's fixed order -- puts it in the
  // next column, which is clear.
  Board blocked;
  blocked.set(Cell{1, 0, 15}, Kind::kNormal);
  check(blocked.accepts(flat), "the Bar lying across the footprint still fits");
  check(!blocked.accepts(bareRotation(flat, spawn, tetralis::kRotZPos)),
        "so standing it in x = 1 collides with the obstruction");
  pivot = spawn;
  check(tetralis::rotatedPlacement(blocked, flat, spawn, tetralis::kRotZPos,
                                   &pivot, &turned, &kick),
        "and the Bar needs a kick to stand at all");
  checkEq(kick, 1, "which uses the second offset of the fixed order");
  checkEq(tetralis::kKickOffsets[kick].x, 1, "the (+1,0,0) one");
  checkEq(cellsText(turned), "(2,0,15) (2,1,15) (2,2,15) (2,3,15)",
          "and lands the standing Bar in the next column");
  checkEq(pivot.x, 2, "moving the pivot, not the cells");
  check(blocked.accepts(turned),
        "and that placement really is clear of the obstruction");

  // Every proper rotation is reachable: walk all six commands on an empty
  // shaft and check that the piece is always inside it and always four cells.
  for (int cmd : {tetralis::kRotXPos, tetralis::kRotXNeg, tetralis::kRotYPos,
                  tetralis::kRotYNeg, tetralis::kRotZPos, tetralis::kRotZNeg}) {
    Cells live = at(info, 0, {1, 1, 8});
    for (int round = 0; round < 4; ++round) {
      checkEq(static_cast<long long>(live.size()), 4,
              "the piece still has four cells after rotating about its pivot");
      if (!tetralis::rotatedPlacement(empty, live, {1, 1, 8}, cmd, nullptr,
                                      &turned, nullptr))
        break;
      live = turned;
    }
  }

  // A rejected rotation changes nothing, so a column packed to the Crown
  // cannot be rotated in place either.
  Board packed;
  for (int z = 12; z <= tetralis::kShaftZMax; ++z)
    for (int x = 0; x < tetralis::kFootX; ++x)
      for (int y = 0; y < tetralis::kFootY; ++y)
        packed.set(Cell{x, y, z}, Kind::kNormal);
  check(!tetralis::rotatedPlacement(packed, flat, spawn, tetralis::kRotZPos,
                                    nullptr, nullptr, nullptr),
        "with the Crown packed, no kick saves the rotation");

  // §4.1 the lock delay only starts once the piece is resting, so a piece
  // falling from the Crown takes its whole descent first.
  Game lock(31u);
  lock.step(advance);
  check(lock.active(), "a piece is active for the lock test");
  Input none;
  int frames = 0;
  while (lock.active() && frames < 1000) {
    lock.step(none);
    ++frames;
  }
  check(lock.piecesLocked() >= 1, "an untouched piece falls and then locks");
  // §3.6 puts the bottom of the piece at z = 16 - h when it deals, and it rests
  // with the bottom at z = 1, so it falls 15 - h cells.
  const int height = tetralis::pieceInfo(lock.piece()).h;
  const int descent = tetralis::kShaftZMax - height;
  const int falling = descent * tetralis::gravityFrames(1);
  check(frames > falling,
        "the delay ran after it came to rest: " + std::to_string(frames) +
            " frames, falling alone takes " + std::to_string(falling));
  check(frames <= falling + tetralis::kLockDelayFrames,
        "and it took at most the thirty frames of delay");
}

} // namespace

int main(int argc, char **argv) {
  const uint64_t seed = (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 1u;
  std::printf("tetralis self-test, seed %llu\n",
              static_cast<unsigned long long>(seed));

  testConstants();
  testRoster();
  testPlumbMechanics();
  testElbowAndDropDistance();
  testRime();
  testBlockers();
  testBendOfPlumb();
  testRandomisedLocks();
  testRealGame();
  testCharges();
  testChargeAccrual();
  testStruck();
  testTheGameCanBeLost();
  testSpawnOverlap();
  testScoring();
  testKicksAndLocks();

  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures > 125 ? 125 : g_failures;
}