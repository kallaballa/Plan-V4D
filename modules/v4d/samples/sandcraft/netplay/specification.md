# Sandcraft Netplay — Specification

Status: **Design (pre-implementation)**
Applies to: `modules/v4d/samples/sandcraft.cpp` and the `modules/v4d/samples/sandcraft/` header set
Supersedes: the upstream `kallaballa/Sandcraft` WebRTC/Nexus networking that the Plan-V4D port deliberately removed
Companion document: [`roadmap.md`](roadmap.md)

---

## 0. Reading guide

This document is the authoritative design for turning the single-process Sandcraft demo into a
host-authoritative networked game for **2–4 players**. It defines the roles, the data model, the
tick model, the input model, the replication codec, the wire protocol, the concurrency contract
with V4D/Plan, connectivity/NAT strategy, and the acceptance criteria for each subsystem.

`roadmap.md` turns this specification into ordered milestones with deliverables, tests and
risks. Where this document says *the implementation shall/must*, the roadmap is where *when* and
*in what order* lives.

The key words **MUST**, **MUST NOT**, **SHALL**, **SHOULD**, **MAY** are used in the RFC-2119
sense.

---

## 1. Scope

### 1.1 In scope

* A **host-authoritative** simulation: exactly one instance runs the cellular automaton; every
  other instance is a display/replica.
* **2–4 players** total (one host + up to three clients).
* Clients transmit **every** local mouse/touch/keyboard input event to the host, with **no rate
  limiting and no temporal coalescing**.
* The host integrates all players' inputs into the *next* automaton frame (the frame is computed
  from the previous frame and the current tick's inputs of all players).
* The host replicates the resulting world state to all clients as a **zstd-compressed delta of the
  authoritative grid** relative to the last transmitted state.
* Clients render the replicated grid with the existing NanoVG path and show a **local optimistic
  stroke** for their own painting while it is in flight.
* Direct/LAN play, plus a documented path to NAT traversal (UPnP, STUN/hole-punch, TURN relay).

### 1.2 Out of scope (for v1; see roadmap "Backlog")

* Lockstep/deterministic simulation across peers (impossible with the current RNG; see §7.6).
* Client-side prediction of the **automaton** (only of the local brush stroke; see §9).
* Voice/chat, matchmaking, accounts, anti-cheat beyond input validation.
* Host migration (designed for, but deferred; §17.5).
* More than one simultaneous host per session.

---

## 2. Goals and non-goals

| # | Goal | Non-goal |
|---|---|---|
| G1 | A guest sees the same world as the host. | Bit-identical rendering (each client keeps its own style) |
| G2 | A guest's brush feels immediate (optimistic stroke). | Zero perceived latency |
| G3 | One frame of simulation is driven by the previous frame + all players' current input. | Deterministic replay |
| G4 | No artificial throttling of input or frames. | Surviving arbitrary loss without a keyframe |
| G5 | Up to four players, exactly one authority. | Peer-to-peer mesh or consensus |
| G6 | Same binary plays host or client. | A dedicated server build |
| G7 | Works on the existing targets (Linux/macOS/RPi/Android). | A new platform in v1 |

---

## 3. Terminology

| Term | Meaning |
|---|---|
| **Host** | The single authoritative instance. Simulates. `isHost == true`. |
| **Client / guest** | A replica instance. Displays only. |
| **Player** | A human participant; hosts and clients are each one player. |
| **Player id** | Stable `0..3`. The host is **always player 0**. |
| **Tick** | One host simulation frame. Identified by `uint64` tick id. |
| **Authoritative grid** | The host's settled `Sandbox::cells_`. |
| **Replica grid** | A client's copy of the authoritative grid. |
| **Keyframe** | Full authoritative state (grid + world + roster) for a tick. |
| **Delta** | zstd-compressed XOR of the grid at `baseTick` and the grid at `tick`. |
| **Watermark** | `appliedSeq[p]`: highest input sequence from player `p` folded into a publish. |
| **Predicted stroke** | A client-local, not-yet-acknowledged painted segment. |
| **Settled** | Every `MOVED*` flavour folded back to its resting type (§7.4). |

---

## 4. Relationship to the current sample

### 4.1 What is reused unchanged

* The automaton itself (`sandcraft_automaton.hpp`): `emit`, `drawParticles`, `drawLine`,
  `moveParticle`, `updateVirtualScreen`.
* The run/settle/bucket pass (`sandcraft_grid.hpp`): `settledType`, `collectRuns`.
* The renderer (`sandcraft_render.hpp`, `sandcraft_scene.hpp`, `sandcraft_vangogh.hpp`,
  `sandcraft_post.hpp`).
* The pointer→cell geometry (`sandcraft_view.hpp`): `fitView`, `cellAt`.
* The material table and palette (`sandcraft_materials.hpp`).
* The V4D runtime, the ImGui panel, the GL post pass.

### 4.2 What changes

1. **Ownership of the grid.** Today one `static Sandbox sandbox_` is shared by the display thread,
   the worker, the input node and the GUI. Netplay keeps the single `Sandbox` per process but adds
   *player* and *world* sub-state, and defines who may write which field (§7).
2. **Input becomes events, not effects.** Today `SandInputPlan::apply` paints into the grid
   directly. Netplay makes local input produce a *stream of intents* that the authority applies
   (§9).
3. **The simulation becomes role-gated.** `SandSimulationPlan` runs only on the host; on a client a
   new node applies received deltas instead (§12, §16.4).
4. **A settle refactor.** `collectRuns` currently both settles the grid *and* buckets runs during
   rendering. Settlement is promoted to an explicit, render-independent step so the authoritative
   state is well defined per tick (§7.4).
5. **A network subsystem.** A new `netplay/` header set plus a vendored zstd (§14, §22).

### 4.3 Invariant preserved

The single-process, zero-argument experience MUST be preserved: with no network configuration the
binary boots exactly as today with the host role and player 0, and the panel behaves as before.
Netplay is strictly additive.

---

## 5. Roles and topology

```
                 host (player 0)
        +-----------------------------+
        |  authoritative automaton    |
        |  per-player input state     |
        |  world state                |
        +------+-------+-------+------+
               |       |       |
          full |  full |  full |
        duplex |  dupl |  dupl |
               v       v       v
          client 1  client 2  client 3        (players 1..3)
        +--------+ +--------+ +--------+
        |replica | |replica | |replica |   display + local optimistic stroke
        +--------+ +--------+ +--------+
```

* Star topology. The host is the only server; clients never talk to each other.
* Player ids are assigned host=0, then connection order 1,2,3.
* The host is also a player and its own input is injected locally (loopback) so that *one* code
  path applies input to the simulation (§16.4).
* Spectators (read-only clients) are a trivial extension: a client that sends no input and is not
  counted in the player cap. Deferred to the backlog.

---

## 6. Authority model

| State | Owner | Notes |
|---|---|---|
| Authoritative grid | **Host** | Only the host steps the automaton. |
| RNG / randomness | **Host** | `moveParticle`, `emit`, `updateVirtualScreen` all draw from it. |
| World state (pause, emitters, density, swaps) | **Host** | Clients may only *request* changes (§9.5). |
| Player roster | **Host** | Names, ids, colors, presence. |
| Per-player brush, pen, cursor, stroke cursor | **Each client** (its own) | Relayed to the host, mirrored to all peers for display. |
| Visual style (Van Gogh, impasto) | **Each client** (local) | Never networked. |
| Panel/help open, fps | **Each client** (local) | Never networked. |

**Rule A1.** A client MUST NOT mutate the authoritative grid. It mutates only its replica (from
received frames) and its local prediction buffer.

**Rule A2.** A client's brush/pen/cursor changes MUST be sent as events; the host applies painting
with the *sender's* brush and pen, not the host's.

**Rule A3.** World-state mutations originate only from the host. A client keystroke or panel click
that would change world state becomes a `COMMAND` event; the host validates, applies it to the next
tick, and broadcasts the new `WORLD` state.

---

## 7. State model

### 7.1 The grid

`Sandbox::cells_` remains a `std::vector<ParticleType>` (`ParticleType : uint8_t`), row-major,
`width_ * height_`. Default `640 x 360` = **230,400 cells** = 230,400 bytes uncompressed.
`PARTICLE_TYPE_COUNT == 36`, so one byte per cell is exact and `uint8_t` is sufficient for the wire.

### 7.2 Proposed `Sandbox` extensions

The existing `Sandbox` is retained; the following sub-structures are added. Field names are
indicative; the exact refactor is a roadmap M0 deliverable.

```cpp
struct PlayerState {
  bool           active    = false;
  ParticleType   brush     = SAND;      // this player's selected material
  int            penSize   = 2;         // 1..32
  cv::Point2d    paintCell {-1.0, -1.0}; // host: stroke cursor for THIS player
  cv::Point2d    cursorCell{-1.0, -1.0}; // hover cell, from MOVE events
  uint32_t       appliedSeq = 0;         // host: highest seq folded into the last publish
  uint8_t        colorIdx  = 0;          // 0..3 display colour
};

struct WorldState {                    // host-authoritative, sim-affecting
  bool                       paused        = false;
  bool                       particleSwaps = true;
  std::array<Emitter, 4>     emitters      = /* default: Water/Sand/Salt/Oil on, 0.30 */;
  uint32_t                   epoch         = 0;   // ++ on clear/randomize (diagnostics)
};

struct LocalState {                    // never networked
  bool   vanGogh   = false;
  float  impasto   = 1.0f;
  bool   panelOpen = true;
  bool   helpOpen  = false;
  double fps       = 0.0;
  double lastFrameTime = 0.0;
};
```

The current flat fields (`brush_`, `penSize_`, `paintCell_`, `penCell_`, `paused_`, `emitters_`,
`particleSwaps_`, `vanGogh_`, `impasto_`, `panelOpen_`, `helpOpen_`) are migrated into these
structures. To keep the single-player path intact, `players_[0]` is the local player and
`world_`/`local_` hold today's globals.

### 7.3 Roster and prediction buffer

```cpp
struct RosterEntry { uint8_t id; uint8_t colorIdx; bool active; char name[16]; };

struct PredictedSegment {          // client-only
  uint32_t   seq;                  // input seq that produced it
  cv::Point2d from, to;            // cell coords; from == to for the first stamp
  ParticleType brush;
  int        penSize;
};
// Client: pending segments, oldest first.
std::vector<PredictedSegment> predicted_;
```

`predicted_` is owned exclusively by the client's plan nodes (never touched by the net thread).

### 7.4 The settle problem (and the prerequisite refactor)

`collectRuns()` currently **mutates** the grid: it folds every non-inert odd (`MOVED*`) cell back to
its resting type (`settledType`). It runs inside `drawWorld()` during rendering. Therefore:

* The "true" state after a frame in the current demo is the *post-render-settle* grid.
* A raw delta of the pre-settle grid would carry transient `MOVED*` bytes, inflating the diff and
  making the replica's converge non-obvious.

**Refactor (behavior-preserving):** extract the settle into its own function, e.g.

```cpp
inline void settleGrid(Sandbox& sb);   // folds MOVED* -> resting, in place
```

call it at the **end of the simulation step**, and keep `collectRuns`'s in-place settle (it is
idempotent on an already-settled grid). This makes the authoritative state
**post-sim, pre-render**, independent of rendering, which is what a delta is taken from.

**Rule A4.** The authoritative state published for tick `T` is the settled grid after the tick-`T`
simulation step and after applying all tick-`T` inputs.

### 7.5 Derived / visual state

`particleCount_`, `fps_`, `simFrames_`, run buckets and the Van Gogh uniforms are **derived**. They
are recomputed locally per instance and MUST NOT be transmitted.

### 7.6 Determinism

The automaton is **not reproducible** across instances unless both are given the same seed:
`Sandbox` carries the original's rotate/cross-add RNG (`s_rand_high_`/`s_rand_low_`, see
`sandcraft_sandbox.hpp`), seeded from `time(0)` by default, and `rand32()`/`rand()` is consulted
pervasively inside a step. With a fixed seed the step *is* deterministic, but the design does not
rely on it, because only the host steps. Consequences that MUST be respected:

* No lockstep and no RNG-state synchronization.
* Clients MUST NOT run the automaton on the replica.
* Host migration (backlog) can only ship the full settled grid and let the new host reseed; future
  randomness will differ. The world continues; the history does not.

---

## 8. Time and tick model

### 8.1 Tick source

The host's simulation tick is its V4D frame. In `ConfigFlags::DISPLAY_MODE` (which the sample uses)
the worker graph is synchronised with the display thread by `RunState::frameSyncSemaSwap` /
`frameSyncRender` and the display is vsync'd, so the nominal tick rate is the display refresh
(≈60 Hz). The monotonic tick id is `GlobalState::Keys::FRAME_CNT` (`uint64`), incremented by the
worker once per iteration — read it, do not invent a parallel counter.

### 8.2 Tick contract

For each host tick `T`:

1. The host drains **all** input events received since the previous tick from all players.
2. It applies them to the authoritative grid (§11) and updates world/per-player state.
3. It runs one automaton step and `settleGrid`.
4. `appliedSeq[p]` records, per player, the highest input sequence folded in.
5. The settled grid + world + watermark is published to the network thread (§12).

The host MUST NOT simulate "ahead"; there is no rollback. Input arriving after the drain is applied
on the next tick. This bounds input latency to `<tick>` + network, without prediction of the
automaton.

### 8.3 No fixed-step accumulator (v1)

The design deliberately does **not** decouple a fixed simulation timestep from the render rate in
v1: the tick *is* the frame, exactly as the demo behaves today. A fixed-step accumulator is a
backlog item (it would change the automaton's frame coupling and is out of scope).

---

## 9. Input model

### 9.1 Principle

Clients send **intents in cell coordinates**, not raw window-pixel GLFW events.

Rationale: `cellAt()` depends on the *sender's* `fitView` (`fbSize`, `windowSize`, resize state).
The host cannot reconstruct a client's view, and the two windows almost never match. Converting
pointer→cell on the sender makes the protocol resolution-independent and lets every player use a
different window size.

**Rule I1.** Painting events carry **absolute cell coordinates** (`x`,`y`). A value of `(-1,-1)`
means "outside the world".

**Rule I2.** A client converts using its own `fitView(fbSize, gridW, gridH)` and `cellAt(...)`,
identical to today's `SandInputPlan`. The conversion code MUST be shared, not duplicated.

### 9.2 Event taxonomy

| Kind | Source | Carries | Simulation effect (host) |
|---|---|---|---|
| `MOVE` | cursor pos callback | cell | update `players_[p].cursorCell` |
| `PRESS` | L/R/M press | cell, button, brush, pen | begin stroke: stamp a disc at cell, set `paintCell` |
| `DRAG` | cursor pos while pressed | cell, button, brush, pen | extend stroke: `drawLine(paintCell → cell)`, update `paintCell` |
| `RELEASE` | L/R/M release | cell, button | end stroke: `paintCell = (-1,-1)` |
| `SCROLL` | scroll callback | cell, scroll delta | brush cycling / pen / density per modifiers |
| `KEY` | key callback | key, state, modifiers | per-player or command, per §9.5 |
| `SET_BRUSH` | panel / brush cycle | brush | set `players_[p].brush` |
| `SET_PEN` | panel / pen keys | penSize | set `players_[p].penSize` |
| `COMMAND` | key / panel | command id (+arg) | world-state change (§9.5) |
| `TOUCH_*` | Android | cell, phase | normalised to the mouse kinds above before the wire |

Touch MUST be normalised to the mouse kinds on the sender so the wire has one pointer model. A
`TOUCH` kind MAY be added later for multi-touch; v1 supports single-pointer.

### 9.3 No rate limiting, no temporal coalescing

**Rule I3.** The client emits one wire event for **every** input event it consumes, immediately.
It MUST NOT throttle by time, MUST NOT drop `MOVE` events, and MUST NOT merge multiple physical
events into one logical event. Multiple events MAY share a single TCP write (**packing**, not
throttling) purely to amortise framing bytes.

Consequences the implementation MUST accept:

* A fast drag can produce several hundred events/second per client. At 16 bytes/event that is a few
  KB/s per client — small, but unbounded in principle.
* The host processes every event; because only one host tick consumes them, events between ticks
  are processed as a batch **in arrival order** and the net effect is the ordered composition of
  all of them. For painting this reproduces the exact polyline.
* Backpressure exists only as a *safety valve* (§18.3): bounded queues may drop the **oldest**
  events if a peer stalls, and MUST count the drops. This is not a rate limiter; it never fires
  under normal operation.

### 9.4 Sequence numbers

Each client maintains a monotonic `uint32 seq`, incremented per emitted event, starting at 1. The
sequence is per-client and never reused within a session. It is used for:

* the **watermark** (`appliedSeq[p]`) that drives optimistic-stroke reconciliation (§10),
* detecting out-of-order or missing input in diagnostics.

Input is carried over an ordered reliable transport (§13), so gaps do not occur in the absence of a
disconnect.

### 9.5 Per-player vs. global (host booleans, keymap split)

The current keymap (documented in `sandcraft_panel.hpp`) is split by authority:

| Input | Scope | Handling |
|---|---|---|
| brush selection (LEFT/RIGHT, wheel, palette click) | per-player | `SET_BRUSH` |
| pen size (UP/DOWN, SHIFT+wheel, slider) | per-player | `SET_PEN` |
| eraser (BACKSPACE) | per-player | `SET_BRUSH` (NIL/NOTHING semantics preserved) |
| painting (PRESS/DRAG) | per-player | `PRESS`/`DRAG`/`RELEASE` |
| hover (MOVE) | per-player | `MOVE` |
| pause (SPACE, checkbox) | **global** | `COMMAND::PAUSE_TOGGLE` |
| emitters on/off (ENTER, ALT+wheel, buttons) | **global** | `COMMAND::EMITTER_TOGGLE(i)` |
| emitter density (CTRL+wheel, `-`/`=`, sliders) | **global** | `COMMAND::EMITTER_DENSITY(i,delta)` |
| swaps checkbox | **global** | `COMMAND::SWAPS_TOGGLE` |
| clear (DELETE, button) | **global** | `COMMAND::CLEAR` |
| random strokes (R, button) | **global** | `COMMAND::RANDOMIZE` |
| panel/help toggle (TAB, About) | local | none |
| Van Gogh / impasto | local | none |

**Rule I4.** A client's `COMMAND` is a *request*. The host validates it (bounds, known ids),
applies it atomically at the next tick, and broadcasts the resulting `WORLD`. Clients apply the
broadcast `WORLD`; they MUST NOT apply their own request locally (so a rejected request cannot
desync the UI).

### 9.6 Input validation (host, mandatory)

Before applying any event the host MUST:

* clamp `x`,`y` to `[-1, width-1]`/`[-1, height-1]` and treat `(-1,-1)` as "no cell";
* reject `brush >= PARTICLE_TYPE_COUNT` (and for painting, require it be a member of `kBrushes`);
* clamp `penSize` to `[1,32]`;
* ignore unknown `kind`/`command` ids;
* bound the per-batch event `count` (see §13.6) before allocating.

A malformed or hostile event stream MUST NOT crash the host.

---

## 10. Optimistic local stroke

### 10.1 Goal

When a player drags the brush, the material appears under the cursor **immediately**, before the
authoritative frame that contains it has completed a round trip. Without this, painting feels
broken on any link with non-trivial latency.

### 10.2 Mechanism (predicted display grid)

The client maintains, per frame:

* `sandbox_.cells_` — the **replica** (authoritative, from keyframes/deltas). Never written by local
  input.
* `predicted_` — the ordered list of not-yet-acknowledged local segments.
* `display_` — a scratch grid rebuilt every rendered frame as `cells_` followed by `predicted_`.

Rendering reads `display_` (a small refactor of `drawWorld`/`collectRuns` to accept the render
grid; see roadmap M5). Because segments are stamped with **resting** types (exactly as the host
stamps them before its settle), and the host's replica of the same input produces the same resting
cells on the authoritative grid, the transient difference is only which side of the round trip the
brush is on.

### 10.3 Reconciliation

The host publishes `appliedSeq[p]` with every frame. On receiving a frame:

```
ack = appliedSeq[myPlayerId]
remove from predicted_ every segment with segment.seq <= ack
reapply the remainder to the freshly-updated replica when building display_
```

When the round trip completes and `ack` catches up, `predicted_` empties and `display_ == cells_`.

**Rule O1.** `predicted_` MUST be bounded. Because the client is ordered and the host acks every
frame, the list holds at most (RTT / tick) segments. A safety cap (e.g. 4096 segments) MUST exist;
on overflow the oldest are dropped and a counter is incremented.

**Rule O2.** A `RELEASE` ends a stroke but does not by itself mark prediction complete; completion
is only by watermark.

### 10.4 What is *not* predicted

The **automaton** is not predicted. Predicted segments are the brush marks only; the motion of
material, emitter output, and reactions are seen only after the authoritative frame arrives. This
is a deliberate consequence of the host-only RNG (§7.6) and is acceptable for a painting sandbox.

### 10.5 Host's own responsiveness

The host does not need prediction: it applies its own input at the next tick and renders its own
authoritative grid, so its brush latency is `<tick>` (~1 frame). The host's local input therefore
takes the loopback path (§16.4) and never populates `predicted_`.

---

## 11. Host simulation step

Per tick, before the automaton:

1. **Drain** all `INPUT` batches queued since the last tick, ordered by `(playerId, seq)` per
   player and by arrival order across players (not globally ordered; players are independent).
2. **Apply per-player painting** for each `PRESS`/`DRAG`:
   ```cpp
   auto& p = sb.players_[playerId];
   const int px = int(p.paintCell.x), py = int(p.paintCell.y);
   const bool cont = px >= 0 && py >= 0;
   drawLine(sb, cell.x, cell.y, cont ? px : cell.x, cont ? py : cell.y, p.brush, p.penSize);
   p.paintCell = cell;
   ```
   This is byte-for-byte the logic in the current `SandInputPlan::apply`, with the per-player,
   per-machine `paintCell_`/`brush_`/`penSize_` replaced by `players_[p]`.
3. **Apply** `SET_BRUSH`/`SET_PEN` to `players_[p]`; `RELEASE` clears `paintCell`; `MOVE` updates
   `cursorCell`.
4. **Apply** validated `COMMAND`s to `sb.world_` (pause, emitters, swaps, clear, randomize;
   clear/randomize set `clearRequested_`/`randomizeRequested_`, which the existing step consumes).
5. Run the existing step: consume `clearRequested_`/`randomizeRequested_`, `paused_` early-out,
   run emitters, clear top/bottom rows, `updateVirtualScreen`.
6. `settleGrid(sb)` (§7.4).
7. Set `appliedSeq[playerId]` = highest consumed seq per player; `++simFrames_`.

**Rule S1.** `clearRequested_`, `randomizeRequested_`, `paused_`, `emitters_`, `particleSwaps_` are
read only from `sb.world_` — the host has a single source of truth.

**Rule S2.** The host's `players_[0]` is populated by the same `apply` code from loopback events, so
host-local painting cannot diverge from remote painting.

---

## 12. Replication and the delta codec

### 12.1 Baseline and chaining

The host replicates by **diffing the grid against the last state it transmitted**:

```
keyframe(T)      : full settled grid at tick T
delta(baseB, T)  : XOR( grid_at_B , grid_at_T )
```

The replica holds `grid_at_T` for its `lastTick`. A delta is applicable iff `baseB == replica.lastTick`.
This chains: a lost or skipped delta does not break correctness *provided the next delta is taken
from the state actually transmitted*, not from the state at the moment of sending. The host's net
thread therefore owns `lastSentTick` and a copy of the last sent grid, and takes each new diff
against **that**.

**Rule R1.** Deltas are relative to the last transmitted state. The net thread, not the simulation,
owns the baseline.

**Rule R2.** Intermediate ticks MAY be skipped (frame dropping) when the net thread cannot keep up;
the delta then spans `lastSentTick → latestTick` and remains correct. This is not rate limiting.

### 12.2 Encoding

**Baseline codec (v1): `XOR` then `zstd`.**

```
diff[i] = base[i] ^ cur[i]            for i in [0, W*H)
wire    = ZSTD_compress(diff, W*H, level)
```

Falling-sand frames change locally, so `diff` is dominated by long zero runs and repeated byte
patterns; zstd encodes those extremely compactly. XOR has no side metadata, cannot desynchronise,
and is trivially verifiable. A `crc32` of the *raw* `diff` or of the *resulting grid* SHALL be
transmitted and checked (mismatch ⇒ request keyframe).

**Optional codec (post-v1, only if measured necessary): changed-span list.**
A predelta of `(uint32 offset, uint16 run, bytes…)` spans, then zstd. This avoids handing 230 KB to
the compressor when changes are very sparse. It MUST NOT be adopted without measurement showing the
baseline is the bottleneck (roadmap M1/M2 produce those numbers).

### 12.3 Keyframes and resync

* A full **keyframe** is sent: on join; on reconnect; and periodically (default **every 300 ticks**,
  ≈5 s) so any undetected divergence is bounded in time.
* On any of {crc mismatch, `delta.baseTick != replica.lastTick`, decode error} the client requests a
  keyframe (`RESYNC`) and, until one arrives, keeps rendering the last good replica.
* A keyframe resets `lastTick`, `lastSentTick`, the net thread's baseline **and `lastKeyframeTick`**.
* The periodic interval MUST be measured from the last **keyframe** tick, not from the last
  *transmitted* tick. Delta transmission advances the transmit baseline on (almost) every tick, so
  testing the interval against it would push the deadline forward forever and the periodic keyframe
  would never fire — the M2 defect recorded in the roadmap's progress log. `HostReplicator`
  therefore tracks a separate `lastKeyframeTick_`.

### 12.4 Compression parameters

* zstd level: default **3** for the host (favour ratio) with a fallback knob to level 1; measure.
* Reuse one `ZSTD_CCtx`/`ZSTD_DCtx` for all frames (context reuse matters).
* Allocate the destination once at `ZSTD_compressBound(W*H)`.
* An optional **trained dictionary** (a few KB sampled from real frames) MAY be shipped to improve
  early-frame ratio; it is a backlog item.
* Decompression uses the known `rawLen == W*H`; `ZSTD_decompress` needs no dictionary unless one was
  used to compress — if a dictionary is adopted, both sides MUST agree on its id in the handshake.

### 12.5 Bandwidth (measured, M2)

* Raw grid: 230,400 B. Raw XOR: same.
* Codec measurement (representative settled 640×360 frame, ~150 stamps): keyframe **6,985 B (3.0 %)**,
  delta **2,223 B (1.0 %)**.
* Pipeline measurement (`--net-selftest`, 600 ticks, one delta per tick over the real 640×360 grid
  driven by scripted input through the genuine simulation and emitters; 594 deltas framed):
  **~9.1 KB per delta average** (`hostBytes` 5.39 MB), **~45 KB worst observed delta**
  (`maxDeltaBytes`). This sits at the low end of the 3–40 KB estimate; the 45 KB case is a heavily
  repainting tick.
* Per client: one delta per tick. At 60 Hz and 9 KB/delta that is ~**0.55 MB/s per client**, ~1.7 MB/s
  to three clients. LAN/loopback: fine. WAN: measure (roadmap M3, M10).
* Input: `16 B` per event; a heavy painter at 500 events/s = 8 KB/s/client. Negligible.
* The numbers above are produced by the `sandcraft` toolkit (`sandcraft.netplay`, actions
  `codec-test`/`codec-bench`/`selftest`) and supersede the earlier estimate.

---

## 13. Wire protocol

### 13.1 Framing

All messages are **length-prefixed** over an ordered byte stream:

```
+--------------------+----------------------+
| uint32 length (LE) | payload (length B)   |
+--------------------+----------------------+
```

`length` counts the payload only. A reader rejects `length > MAX_PAYLOAD` (§13.6). There is no
magic on the wire (the stream is established by connect); the payload begins with a fixed header.

### 13.2 Payload header (8 bytes)

```c
struct MsgHeader {          // little-endian, packed
  uint8_t  type;            // MsgType
  uint8_t  version;         // PROTOCOL_VERSION
  uint16_t reserved;        // 0
  uint32_t seq;             // sender-local message counter (diagnostics)
};
```
`PROTOCOL_VERSION == 1`.

### 13.3 Message catalog

| Id | Name | Direction | Purpose |
|---|---|---|---|
| 1 | `HELLO` | client→host | protocol version, name, optional join token |
| 2 | `WELCOME` | host→client | player id, roster size, dims, tick rate |
| 3 | `REJECT` | host→client | reason (full / version / token) |
| 4 | `KEYFRAME` | host→client | full grid + world + roster |
| 5 | `DELTA` | host→client | XOR+zstd diff + watermark + cursors |
| 6 | `INPUT` | client→host | batch of input events |
| 7 | `WORLD` | host→client | world-state change |
| 8 | `PING` | both | RTT probe (echo payload) |
| 9 | `PONG` | both | RTT reply |
| 10 | `RESYNC` | client→host | request keyframe |
| 11 | `BYE` | both | graceful close with reason |

### 13.4 Structs (little-endian, packed; sizes asserted)

```c
enum MsgType : uint8_t { HELLO=1, WELCOME, REJECT, KEYFRAME, DELTA, INPUT, WORLD, PING, PONG, RESYNC, BYE };

struct Hello { uint8_t version; uint8_t nameLen; uint8_t reserved[2]; /* name[nameLen] */ };

struct Welcome {
  uint8_t  playerId;      // 0 == host
  uint8_t  playerCount;   // total players incl. host
  uint8_t  tickHz;        // nominal
  uint8_t  reserved;
  uint16_t gridW, gridH;
  uint32_t clientSeqStart;
  uint64_t startTick;
};

struct Reject { uint8_t reason; };   // 1 full, 2 version, 3 token

struct EmitterWire { uint8_t type; uint8_t on; float density; uint8_t pad[2]; }; // 8 B

struct KeyframeHeader {
  uint64_t tick;
  uint16_t gridW, gridH;
  uint32_t rawLen;        // == gridW*gridH
  uint32_t compLen;       // zstd bytes that follow
  uint32_t crc32;         // of the raw grid
  uint32_t worldEpoch;
  uint8_t  playerCount; uint8_t paused; uint8_t particleSwaps; uint8_t reserved;
  EmitterWire emitters[4];
  // followed by: compLen bytes, then playerCount * PlayerWire
};
struct PlayerWire { uint8_t id, colorIdx, active, reserved; char name[16]; };   // 20 B

struct PlayerCursorWire { int16_t x, y; uint8_t brush, colorIdx; };            // 6 B

struct DeltaHeader {
  uint64_t baseTick, tick;
  uint32_t appliedSeq[4];
  uint32_t rawLen, compLen, crc32;
  uint8_t  playerCount, reserved[3];
  PlayerCursorWire cursors[4];
  // followed by: compLen bytes (zstd of XOR)
};

struct InputEventWire {
  uint8_t  kind;      // EventKind
  uint8_t  flags;     // button | key | modifiers | command
  int16_t  x, y;      // cell coords, or -1
  int16_t  dx, dy;    // scroll / drag delta
  uint8_t  brush;     // ParticleType (context-dependent)
  uint8_t  penSize;   // 1..32
  uint32_t seq;       // per-client monotonic
};                    // 16 B

struct InputBatch {
  uint32_t batchTimeMs;
  uint32_t firstSeq;
  uint16_t count; uint16_t reserved;
  // followed by count * InputEventWire
};

struct World {
  uint8_t paused, particleSwaps, reserved[2];
  uint32_t epoch;
  EmitterWire emitters[4];
};

struct PingPong { uint32_t senderTimeMs; uint32_t peerTimeMs; };
struct Resync   { uint64_t haveTick; };
struct Bye      { uint8_t reason; };
```

**`KEYFRAME` carries no watermark or cursors.** It needs only what a joiner cannot
reconstruct from the grid: the grid itself, the world state, the emitters and the roster. The
per-player input watermark (`appliedSeq[]`) and the cursors are per-tick, cosmetic state and ride in
the `DELTA`s that follow (§13.4 `DeltaHeader`); a fresh replica starts with a zero watermark and
finds the authoritative one in the first delta. The replica's `gridW`/`gridH` are taken from the
keyframe header on receipt — the first keyframe sizes the replica, and every later keyframe must
match because a session's dimensions are fixed for its lifetime (§23).

**The join token is deferred.** §17.2 lists a token in `HELLO` and `REJECT_TOKEN` exists, but the
`Hello` structure above carries no token field, and `PROTOCOL_VERSION == 1` is already exercised
without one. v1 therefore runs an **open session**: `--token` is parsed, echoed in the join string
and otherwise unused, and no peer is ever rejected for a token. A length-prefixed token field is
added to `Hello`, and `REJECT_TOKEN` wired, when the rendezvous work (M8) actually needs a gate
(decision D24). This is the only `REJECT` reason v1 does not produce.

`EventKind`: `MOVE=0, PRESS=1, RELEASE=2, DRAG=3, SCROLL=4, KEY=5, SET_BRUSH=6, SET_PEN=7,
COMMAND=8`. `flags` bit layout: bits 0–3 = mouse button `0..8`; bits 4–6 = modifiers
(shift/ctrl/alt); bit 7 = key edge (0 press / 1 release). For `KEY`, `x` holds the `Keyboard::Key`
value and `flags` bit 7 holds the edge. For `COMMAND`, `flags` = command id and `brush` = emitter
index where applicable; `dx` = signed delta (density in millis).

`CommandId`: `PAUSE_TOGGLE=1, CLEAR=2, RANDOMIZE=3, EMITTER_TOGGLE=4, EMITTER_DENSITY=5,
SWAPS_TOGGLE=6`.

### 13.5 Endianness and layout safety

All targets (x86_64, aarch64, armv7) are little-endian. The implementation SHALL:

* `static_assert` `sizeof` of every wire struct;
* `static_assert` host byte order is little-endian (`__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__`);
* use `#pragma pack`/`__attribute__((packed))` (the repository already uses packed structs elsewhere).

### 13.6 Limits

* `MAX_PAYLOAD` = 1 MiB. Larger frames are rejected and the connection closed.
* `MAX_EVENTS_PER_BATCH` = 4096 (host rejects excess).
* `MAX_NAME` = 16 bytes.

### 13.7 Integrity and sequencing

* `DELTA`/`KEYFRAME` carry `crc32` — of the raw grid for a keyframe, and of the **resulting grid**
  (the state the delta reconstructs) for a delta. The delta form is the stronger of the two options
  §12.2 allows: because XOR is its own inverse, the receiver applies the diff, checksums the result
  and rolls back on mismatch, so it detects both a corrupt diff *and* a delta applied to the wrong
  base (roadmap M1 acceptance). A mismatch ⇒ `RESYNC`.
* `MsgHeader.seq` is a per-sender message counter; gaps are logged, not fatal (streams are ordered
  by the transport).

---

## 14. Compression (vendored zstd)

### 14.1 Decision

Use **zstd** (not LZ4) for the delta. Reasons: better ratio at comparable decode speed; a stable C
API; a small, permissive-licensed library that builds with CMake on every target including Android;
dictionary support if ever needed. The user directive is explicit: vendor zstd.

### 14.2 Vendoring

* Location: `modules/v4d/third/zstd/` (a git submodule pinned to a release tag, e.g. `v1.5.7`),
  matching the `third/glfw`, `third/nanovg`, `third/imgui` convention.
* Added to `.gitmodules` and to the CMake submodule-existence check used for
  `V4D_THIRD_PARTY_DEPS` (`modules/v4d/CMakeLists.txt`).
* Built via `add_subdirectory(third/zstd)` with programs/tests/legacy off and static library on;
  the resulting static target is exposed to the sample.
* The sample target `example_v4d_sandcraft` (registered at `modules/v4d/CMakeLists.txt:672`)
  links the zstd static target and adds `third/zstd/lib` to its include path.
* Android: the same submodule builds under the NDK; the sample's link list gains the zstd target.

### 14.3 API use

* Encoder: `ZSTD_compressCCtx(cctx, dst, dstCap, src, srcLen, level)`; context per host.
* Decoder: `ZSTD_decompressDCtx(dctx, dst, rawLen, src, compLen)`; context per client.
* Sizes: `ZSTD_compressBound(srcLen)` for the destination.
* A thin RAII wrapper (`zstd_codec.hpp`) owns the contexts and offers `compress()`/`decompress()`
  with explicit `rawLen`, returning a status enum, unit-testable without sockets.

### 14.4 Fallback

If zstd cannot be vendored for a target, the codec MAY fall back to **raw** (uncompressed) with a
`compLen == rawLen` marker and a flag in `MsgHeader`. `LZ4` SHALL NOT be used. This fallback exists
only to keep the build honest on a hypothetical target; it is not a runtime mode.

---

## 15. Transport and connectivity

### 15.1 Reference transport: TCP

TCP is the v1 transport. Justification:

* Deltas form a dependency chain (§12.1); ordered reliable delivery removes an entire class of
  correctness bugs (resend buffers, rewrite windows, ack tracking).
* Input is small and its ordering matters; TCP gives it for free.
* Head-of-line blocking and the resulting latency effects are acceptable for a painting sandbox and
  are strictly better than a hand-rolled reliability layer in v1.

Connection model: the **host listens** on a TCP port; each client opens one connection. For ≤3
clients a single `std::thread` running a `poll(2)`/`epoll` loop over the listen socket, the client
sockets and a wakeup `eventfd`/pipe is sufficient and is the reference implementation. Non-blocking
sockets; length-prefixed reads with a per-connection reassembly buffer.

### 15.2 Alternative transport: reliable UDP (backlog)

If TCP latency proves unacceptable, a UDP transport with an ack/resend layer (ENet/KCP/
GameNetworkingSockets) MAY replace TCP **behind the same `Transport` interface**. Requirements:
deltas resend against the same baseline; input is reliable+ordered; a loss of a delta triggers
`RESYNC` rather than a stalled stream. Not v1.

### 15.3 Connectivity ladder

Ranked by cost. Each rung is a valid place to stop for a given audience.

| Rung | Mechanism | Works when | Needs |
|---|---|---|---|
| 0 | **Loopback** (same process) | testing | nothing |
| 1 | **LAN / direct** `ip:port` | same LAN, or port forwarded by hand | host prints endpoint; client dials |
| 2 | **UPnP IGD / NAT-PMP** | most home routers | `miniupnpc`/`libnatpmp` (vendored; backlog) |
| 3 | **STUN + UDP hole punch** | cone / restricted NATs (~80 %) | rendezvous + STUN (public STUN for MVP) |
| 4 | **ICE + TURN relay** | nearly all, incl. symmetric/CGNAT | rendezvous + STUN + TURN (carries traffic) |
| 5 | **Overlay VPN** (Tailscale/ZeroTier/WireGuard) | anything | third-party or self-hosted; docs only |

The game is **host-authoritative star** (§5), so only host↔client paths are needed — never a mesh.

**`pwnat` is explicitly rejected.** Rationale, recorded so it is not re-proposed: it is ICMP-based,
requires a cooperating third-party proxy host anyway (so it does not remove the rendezvous),
is routinely blocked by middleboxes, is non-standard/research-grade, and adds a security-flag.
It solves neither the rendezvous problem nor the symmetric-NAT problem.

For v1 the roadmap implements rungs 0–1. Rungs 2–4 are staged in roadmap M8/M9. Rung 5 is
documented, not implemented.

### 15.4 Join code (rung 1)

The host prints/display a join string. The minimal form is `ip:port`. A friendlier form encodes the
endpoint plus a short token, e.g. `sc1-<base32(ip:port)>-<token>`, copy-pasted out of band. The
panel shows it and accepts a peer's string.

---

## 16. Concurrency and V4D/Plan integration

### 16.1 Threads

* **Display thread** — V4D, NanoVG, ImGui (unchanged).
* **Worker thread** — the recorded graph, including the simulation (host) or delta application
  (client) and rendering (unchanged).
* **Network thread(s)** — new. One `std::thread` running the poll loop. It owns the sockets and the
  last-sent baseline, and it MUST NOT touch the `Sandbox`, the graph storage, or any `_shared`
  object except through the self-synchronised `NetQueues` (§16.2).

### 16.2 The hand-off: `NetQueues`

A single struct, registered with the plan via `_shared(queues_)`, holding self-synchronised queues:

```cpp
struct InputEnvelope {                  // the shared input queue serves many senders
  uint8_t        player;                // 0..3, the sender of `event`
  InputEventWire event;
};

struct NetQueues {
  std::mutex mtx;                       // guards everything below; also the net-thread lock
  std::deque<InputEnvelope>  input;     // host: net + loopback -> sim (per-player)
  std::deque<Frame>          inbound;   // client: net -> apply node (parsed frame)
  std::deque<std::vector<uint8_t>> outbound; // client: input node -> net
  std::shared_ptr<const GridSnapshot> snapshot;  // host: sim -> net (single slot, latest)
  std::atomic<uint64_t>      drops{0};  // safety-valve counter (§9.3, §18.3)
};
```

Input crosses the boundary as an **`InputEnvelope`**, not a bare `InputEventWire`: the queue is fed
by every sender (each peer *and* the host's own loopback path), so the sender id must travel with the
event. The host's own input is enqueued as player `0` and drained by the simulation node through the
same `applyInputEvent` a peer's event takes (Rule S2, §16.4) — there is exactly one apply path.

**Rule C1.** Every accessor (`pushInput`, `popInput`, `publishSnapshot`, `takeSnapshot`,
`pushInbound`, `popInbound`) locks `queues_.mtx`. The net thread uses only these accessors. Nodes
access the queues via `RWS(queues_)` edges, which acquire V4D's per-variable mutex and *then* the
internal `queues_.mtx`; the net thread acquires only `queues_.mtx`. Lock ordering is therefore
`V4D-mutex → queues_.mtx` and must never be inverted.

**Rule C2.** The net thread MUST NOT read or write any field of the `Sandbox`. All grid/world/
roster state crosses the boundary as encoded byte buffers or as plain value structs inside
`NetQueues`.

**Rule C3.** `NetQueues` is a process-wide singleton alongside `static Sandbox sandbox_` (the plan
is constructed once per worker *and* once for the display thread; the queues must exist once).

**Wake-up.** The net thread blocks in `poll(2)` with a short timeout, so it must be roused when the
plan produces work — a published snapshot on the host, a queued `INPUT` on a client. The plan signals
it through a wake pipe (§15.1) that is part of the net thread's poll set, so outbound and publish
latency are bounded by the wake and not by the poll timeout. The net thread registers its pipe with
`NetQueues` once, at start; `publishSnapshot` and `pushOutbound` signal it after releasing `mtx`.

### 16.3 The snapshot slot

The snapshot slot holds the **latest** published frame and its `uint64 tick`, as a
`std::shared_ptr<const GridSnapshot>`:

```cpp
struct GridSnapshot {
  uint64_t tick;
  uint16_t width, height;       // the session's fixed dimensions, so the net
                                // thread can build a KEYFRAME without the Sandbox
  uint32_t appliedSeq[4];
  std::vector<uint8_t> cells;   // settled grid copy (or a shared_ptr into a pool)
  WorldWire world;              // plain wire struct, not the Sandbox's WorldState
  PlayerCursorWire cursors[4];
  std::vector<PlayerWire> roster;
};
```

`world` is the **wire** struct (`WorldWire`), not the Sandbox's `WorldState`: the net thread must be
able to frame a `KEYFRAME`/`WORLD` from a snapshot alone and is forbidden from touching `Sandbox`
(Rule C2), so the snapshot carries the already-encodable form. `width`/`height` are carried for the
same reason — the snapshot is framed into a keyframe without consulting the grid's owner.

The sim node writes a snapshot (a 230 KB copy, or a double-buffered swap; copy is the baseline) via
`publishSnapshot`, which replaces the slot. The net thread reads via `takeSnapshot` (shared_ptr
copy). Latest-wins; intermediate ticks MAY be coalesced into one delta spanning the gap.

**Optimization (backlog):** double-buffer the grid inside `Sandbox` so the sim writes the buffer not
being read, removing the per-tick copy.

### 16.4 Graph structure

```cpp
void SandcraftPlan::infer() override {
  // ... existing clear color etc. ...

  // A. Local input -> events + (client) predicted segments
  subInfer(input_);                 // LocalInputPlan
                                   //   host: pushInput(player 0, loopback)
                                   //   client: enqueue outbound INPUT + predicted_

  // B. Authority
  // _shared(bool hostMode_), read with RS(...); NO GlobalState USER key exists (see below).
  // A flat if/else takes exactly ONE endBranch(); each endBranch() closes the
  // innermost open branch, so `endBranch()->elseBranch()` only appears when a
  // branch is nested (cf. beauty-demo.cpp, where the inner endBranch() closes
  // the nested branch and elseBranch() binds to the enclosing one).
  branch(RS(hostMode_))
     ->subInfer(sim_)              // SandSimulationPlan: drain input, apply, step, settle, publish
  ->elseBranch()
     ->subInfer(netApply_)         // NetApplyPlan: popInbound -> apply keyframe/delta/world/roster
  ->endBranch();

  // C. Render
  subInfer(render_);               // renders display_ (replica, or replica+predicted on client)
  gl(&SandcraftPlan::postProcess, RWS(sandbox_), RW(post_));
  nvg(&SandcraftPlan::overlayDraw, RWS(sandbox_), size_);   // chrome + net HUD
}
```

**Note on the role flag.** There is **no** `GlobalState::Keys::USER_*` key (`GlobalKeys` in
`modules/plan/include/opencv2/plan/util.hpp` enumerates only `FRAME_CNT`, `CAPTURE_CNT`, `FPS_CNT`,
`RUN_CNT`, `START_TIME`, `FPS`, `WORKERS_READY`, `WORKERS_STARTED`, `LOCKING`, `DISPLAY_READY`,
`LOCK_CONTENTION_CNT`, `LOCK_CONTENTION_RATE`, `LCR_CNT`, `SHOW_GUI`, `SHOW_FRAME_TIME`,
`TIME_TRACKER`). The host/client flag SHALL be a `_shared(bool hostMode_)` member, read with
**`RS(hostMode_)`** — `RS` is the accessor that enforces `checkShared`, whereas `R()` does not
declare the shared dependency correctly for a `_shared` variable — and set once before the first
frame (it is fixed for the process lifetime).

### 16.5 Node ownership

* `LocalInputPlan` replaces the event-reading half of the current `SandInputPlan`. It keeps the
  existing event edges (`press_`, `drag_`, `scroll_`, `move_`, `keys_`) and the `guiWantsMouse_`/
  `guiWantsKeyboard_` mirroring, but *emits events* instead of painting.
* `SandSimulationPlan` gains the input-application and publish steps; its automaton core is
  unchanged. It drains `NetQueues::input` and applies each `InputEnvelope` through the shared
  `applyInputEvent(sandbox, player, event)` in `netplay/input_apply.hpp` — the *single* sample-side
  implementation of §9.2/§9.6, also used by the M2 harness, so the plan and the self-test cannot
  drift apart.
* `NetApplyPlan` is new: drains `inbound`, applies keyframe/delta/world/roster, reconciles
  `predicted_` against `appliedSeq`.
* `SandRenderPlan` renders `display_` instead of `sandbox_.cells_` (§10.2).

### 16.6 Lifecycle

* The `NetRuntime` singleton is configured from `argv`/env (host vs. client, bind/connect, name,
  token) before `V4DPlan::run`, starts its thread lazily on first use, and is stopped by a static
  destructor / `atexit`. `setup()`/`teardown()` run per instance and MUST NOT start/stop it more
  than once (guard with `std::once_flag`).
* **Networking is opt-in.** The default `NetConfig` has `role == Host` and `enabled == false`; an
  instance started with no network arguments opens no socket and runs exactly today's single-player
  demo (§4.3). A listener is opened, and `netActive()` becomes true, only when the process is *asked*
  to network — `--host` (listen) or `--connect HOST[:PORT]` (dial). This resolves the tension
  between §4.3 ("no network configuration ... boots exactly as today") and §17.1 ("the instance
  launched with `--host` ... is the host"): the **role** defaults to host, the **socket** does not.
  The flag is what gates the per-tick snapshot publish, so the no-argument demo still pays no
  230 KB copy (Rule C2/§16.3).
* The role flag MUST be applied with `NetRuntime::configure()` from the demo entry point, before
  `V4D::init` constructs any plan instance. Reading the singleton without configuring it leaves
  `hostMode_` at its default (`Host`) and makes the client arm of the §16.4 branch unreachable
  (this was a latent M2 wiring omission; see the roadmap's M3 entry).
* `--net-selftest` is the loopback harness (roadmap M2): it runs the whole host==client pipeline
  in-process over the in-memory transport (`loopback_transport.hpp`) *before* the first frame and
  *before* `V4D::init` — no window, no GL — and exits non-zero if any check fails. It is the headless
  proof that the codec, protocol, queues and replication agree with no socket in the loop.
* On Android there is no `argv`; the default is host (rung 0/1) until a lobby UI exists (backlog).
* `V4D::Keys::DISABLE_INPUT_EVENTS` is available if a headless dedicated host is ever wanted; not
  used by v1.

---

## 17. Session lifecycle

### 17.1 Host selection

Out-of-band in v1: the instance launched with `--host` is the host and is player 0, and its listener
is opened. An instance with no network arguments is still the host and player 0 but opens no socket
(§4.3, §16.6): the role defaults to host, the socket does not. There is no election.
(Election/migration: backlog.)

### 17.2 Join

1. Client connects; sends `HELLO` (version, name, token).
2. Host validates (capacity, version, token) and replies `WELCOME` or `REJECT`.
3. Host immediately sends `KEYFRAME` (grid + world + roster).
4. Host sends `WORLD` on any world change; `DELTA` every tick.
5. Client may `RESYNC` at any time; host replies with a fresh `KEYFRAME`.

### 17.3 Late join / rejoin

A late joiner is handled by the normal join path (keyframe). A rejoining client SHOULD be
re-admitted with its previous player id when its token matches; otherwise it takes the next free id.
On re-admission the host sends a fresh `KEYFRAME` and clears that player's `paintCell`.

### 17.4 Leaving

A client closing sends `BYE`; the host frees its id, marks it inactive in the roster, and
broadcasts the roster change (folded into the next frame's `playerCount`/cursors, or a `WORLD`
message). Abrupt disconnects are detected by socket error/`poll` hangup; the same cleanup happens.

### 17.5 Host loss

v1: if the host disconnects, the session ends; clients show "host left" and keep the last replica.
Host migration is designed for but deferred: because every client holds the full settled grid, a
new host can be elected (lowest remaining player id) and reseed; only future randomness differs
(§7.6). The `WELCOME`/`tick` fields already carry everything needed to rebase.

---

## 18. Failure handling and backpressure

### 18.1 Disconnect

Detected by `read()==0`, `ECONNRESET`, or `poll` hangup. Cleanup is idempotent; the net thread MUST
never block indefinitely on a dead peer (non-blocking sockets + `poll`).

### 18.2 Desync

Any of {crc mismatch, `baseTick` gap, decompression failure, `world.epoch` jump it cannot explain}
causes `RESYNC`. Rendering continues from the last good replica so the window never blanks.

### 18.3 Backpressure (not rate limiting)

**Rule B1.** Queues are bounded. On overflow the **oldest** items are dropped and `NetQueues::drops`
is incremented; the count is surfaced in the net HUD and logged. This is a memory-safety valve; it
MUST NOT be used to shape traffic and MUST NOT fire under normal operation.

Specifically:

* `input` bounded (default 65536 events). Dropping `MOVE` before painting events is acceptable;
  dropping painting events is logged as a correctness incident.
* `outbound` bounded (default 8192 messages). TCP write backpressure propagates from the socket;
  the input node never blocks the render thread.
* `inbound` bounded (default 8 frames). If the client's apply node lags, older non-keyframe frames
  MAY be dropped, but a keyframe MUST be applied in order.

### 18.4 Malformed input

See §9.6 and §13.6. A structurally invalid message closes that connection; the host keeps running.

---

## 19. Security and validation

* Trust model: a shared join token (if configured) is the only gate; there is no auth. This is a
  hobby-scale design and MUST be stated in the docs/UI.
* Host is authoritative, so a malicious client cannot fabricate world state — it can only inject
  input, which is validated (§9.6).
* All client-supplied lengths/indices/coords are bounded before use.
* No secrets on the wire; no TLS in v1 (a TURN/rendezvous deployment would need it, §20).
* A hostile peer can still consume the host's CPU with a flood of valid events; the bounded input
  queue (§18.3) caps memory, not CPU. A per-connection sanity cap on events/second MAY be added as
  a *safety* measure if abuse is observed — but that is explicitly **not** part of v1, per the "no
  rate limit" directive.

---

## 20. Rendezvous service (interface sketch, for M8)

Not implemented in v1; specified so the client code and roadmap can target it.

* Minimal HTTPS+JSON or WebSocket service:
  * `POST /session` → `{ sessionId, joinCode, token }` (host registers an endpoint/candidates).
  * `POST /session/{joinCode}/join` → `{ hostCandidates[], token }`.
  * `PUT /session/{id}/signal` + `GET /session/{id}/signal` (long-poll or WS) → opaque candidate
    exchange for hole punching.
  * Optional `STUN`/`TURN` endpoints (or use `libnice`/`coturn`).
* The service MUST be as thin as a BitTorrent tracker: it brokers an introduction, then steps out of
  the data path — unless relaying (rung 4), which is the only bandwidth-bearing role.

---

## 21. Cross-platform

* **Android:** input is touch. The glfw-android shim's touch→mouse mapping MUST be verified; the
  normalisation to mouse kinds happens on the sender (§9.2). The net thread and vendored zstd MUST
  build under the NDK (roadmap M10). No `argv` ⇒ default role is host until a lobby exists.
* **Endianness:** all targets little-endian; asserted (§13.5).
* **Sockets:** POSIX `socket`/`poll` are available on Linux, macOS, Android. No new portability
  layer is required for v1 (Windows is not a target). A `Transport` interface still isolates the
  choice so a future backend is possible.

---

## 22. Dependencies and vendoring

| Dependency | Use | Status | Vendoring |
|---|---|---|---|
| **zstd** | delta compression | to vendor | `modules/v4d/third/zstd/` submodule + CMake target (§14.2) |
| POSIX sockets | transport | system | none (Linux/macOS/Android) |
| `miniupnpc` | UPnP (M8) | to vendor | `modules/v4d/third/miniupnpc/` (backlog) |
| STUN/TURN client | rungs 3–4 (M8/M9) | to vendor | `libnice` or in-tree minimal STUN (backlog) |

The directive permits vendoring any dependency. The rule for this project: prefer a
`third/<name>/` submodule built by CMake, mirroring `glfw`/`nanovg`/`imgui`, over system packages,
so host and Android builds are reproducible.

---

## 23. Compatibility and versioning

* `PROTOCOL_VERSION` is bumped on any wire-incompatible change. A mismatch ⇒ `REJECT{version}`.
* Grid dimensions are transmitted; a session's dimensions are fixed for its lifetime in v1 (the
  sample always uses 640×360; the protocol does not prevent other sizes).
* `RawLen`/`compLen` let the decoder allocate correctly regardless of codec evolution; the baseline
  codec is identified implicitly by version.

---

## 24. Decision log

| # | Decision | Rationale |
|---|---|---|
| D1 | Host-authoritative, star topology | Automaton needs a single RNG/steps; no consensus; only host↔client paths needed. |
| D2 | Replicate the **grid**, not the rendered image | 230 KB vs ~8 MB/frame; clients keep local style/resolution; enables spectators. |
| D3 | `XOR + zstd` delta, chained against last **transmitted** state | Sparse diffs compress hard; no metadata to desync; skipping ticks stays correct. |
| D4 | zstd, not LZ4 | Better ratio at comparable speed; CMake/Android-friendly; user directive. |
| D5 | Ordered reliable transport (TCP) in v1 | Deltas chain; ordering/reliability matter; HOL blocking acceptable for a painting sandbox. |
| D6 | Send **intents in cell coordinates**, not raw pixel events | Host cannot reconstruct a client's view; resolution independence. |
| D7 | **No rate limiting / no temporal coalescing** of input | User directive; packing into TCP writes is the only aggregation. Bounded queues remain a safety valve only. |
| D8 | **Optimistic local stroke** as a predicted display grid, reconciled by watermark | Immediate feedback without predicting the automaton. |
| D9 | Role flag is a `_shared(bool)`, not a `GlobalState` key | No `USER_*` key exists; introducing one would touch the shared Plan engine. |
| D10 | Net thread ↔ plan only via `NetQueues`; never touches `Sandbox` | V4D's `_shared` mutex is only held during node execution; a foreign writer would race. |
| D11 | `pwnat` rejected | ICMP-based, needs a proxy host anyway, blocked in practice, security smell. |
| D12 | Settle promoted out of the render pass | Authoritative state must be render-independent and well defined per tick. |
| D13 | v1 ends the session on host loss; migration deferred but designed for | Every client holds the full grid; only future randomness differs. |
| D14 | `NetQueues` are OpenCV-free and carry only wire types; the snapshot carries `WorldWire`/`width`/`height`/`roster` | Proves the whole pipeline headless and keeps the net thread unable to touch the `Sandbox` (Rule C2). |
| D15 | The replica is sized by its first `KEYFRAME`, which carries no watermark/cursors | A joiner can reconstruct the world from a keyframe alone; per-tick cosmetic state rides the deltas. |
| D16 | The M2 loopback proof is `--net-selftest` (in-process, before `V4D::init`) plus standalone `protocol`/`codec` test binaries, not the `plan` gtest suite | The V4D-off `plan` build cannot reach vendored zstd, and the harness must run with no window and no socket. |
| D17 | The host's own input takes the **loopback queue** like a peer's | One apply path, so the authority cannot diverge from a guest (Rule S2, §16.4); it also exercises the guest path on every single-player frame. |
| D18 | The shared input queue carries `InputEnvelope{player, event}`, not a bare `InputEventWire` | The queue is fed by every sender; the demux id must be explicit rather than implied by position (amends §16.2/Appendix A). |
| D19 | The `hostMode_` branch predicate is read with `RS(...)`, not `R(...)` | `RS` is the accessor that enforces `checkShared`; `R()` does not declare the shared dependency for a `_shared` variable (amends §16.4). |
| D20 | `applyInputEvent` lives in a sample-side header (`netplay/input_apply.hpp`) | One definition of "what an event means", shared by the plan node and the M2 harness; neither can drift (§9.2/§9.6). |
| D21 | The periodic keyframe interval is measured from the last **keyframe** tick | Measuring against the last *transmitted* tick moves the deadline forward every delta, so the periodic keyframe never fires (§12.3, M2 defect). |
| D22 | Networking is opt-in: `NetConfig::enabled` is set only by `--host`/`--connect` | Preserves §4.3 (no arguments == today's single-player demo) while §17.1 still calls the default role host; gates the per-tick snapshot publish. |
| D23 | The role flag is applied by `NetRuntime::configure()` from the demo entry point, before `V4D::init` | Without the call `hostMode_` stays `Host` and the client arm of the §16.4 branch is unreachable — a latent M2 wiring omission (roadmap §7 M3 entry). |
| D24 | The `HELLO` join token is deferred; v1 sessions are open | `Hello` carries no token field and `PROTOCOL_VERSION` is exercised without one; adding it changes a wire shape no v1 peer needs (M8 rendezvous owns the gate). |
| D25 | M3 carries the input path end-to-end as well as replication | M2 already wired `LocalInputPlan`→`MSG_INPUT` and the host `drainInput()`, so a one-way M3 would have to disable landed code; M4 then covers multi-player painting semantics and the no-rate-limit proof. |
| D26 | Per-player roster/cursor propagation stays out of the net thread; the keyframe roster is whatever the `Sandbox` holds | Rule C2 forbids the net thread touching the `Sandbox`; `WELCOME.playerCount` and the keyframe roster may differ until M6 owns the roster. M3 asserts only grid replication. |
| D27 | `NetStats` carries both directions (`keyframesIn`/`deltasIn` as well as `*Out`), and the HUD/log print the pair that belongs to the role | Keyframes/deltas travel one way, so the out-only counters made a client read `kf=0 dl=0 tick=0` while it was being replicated to. The client counts `KEYFRAME`/`DELTA` on arrival and takes `tick` from their headers without touching the compressed payload (the apply node still owns it). |

---

## Appendix A — Data structures (summary)

```
Sandbox
├── cells_        std::vector<ParticleType>   (authoritative: host; replica: client)
├── width_, height_
├── players_[4]   PlayerState  (brush, penSize, paintCell, cursorCell, appliedSeq, color)
├── world_        WorldState   (paused, particleSwaps, emitters[4], epoch)
├── local_        LocalState   (vanGogh, impasto, panelOpen, helpOpen, fps)   [never networked]
├── predicted_    std::vector<PredictedSegment>                               [client only]
└── s_rand_high_/s_rand_low_   uint32_t (rotate/cross-add RNG)               [host only]

NetQueues (self-synchronised, _shared)
├── input     InputEnvelope[]    (host consumes; {player, InputEventWire})
├── inbound   Frame[]            (client consumes; parsed type + payload)
├── outbound  byte[][]           (net thread sends)
├── snapshot  GridSnapshot*      (host publishes; latest-wins shared_ptr)
└── drops     atomic<uint64>

NetRuntime (net thread owns)
├── sockets / poll set
├── lastSentTick, lastSentGrid   (delta baseline)
├── zstd cctx/dctx
└── per-peer FrameParser         (length-prefix reassembly)
```

## Appendix B — Example flow: a guest paints

```
guest (player 2)                              host (player 0)
  MOVE  cell(120,40)  ──INPUT seq=17────────▶  players_[2].cursorCell = (120,40)
  SET_BRUSH SAND      ──INPUT seq=18────────▶  players_[2].brush = SAND
  PRESS cell(120,40)  ──INPUT seq=19────────▶  stamp disc @ (120,40), paintCell=(120,40)
  DRAG  cell(122,41)  ──INPUT seq=20────────▶  drawLine((120,40)->(122,41))
  DRAG  cell(125,43)  ──INPUT seq=21────────▶  drawLine((122,41)->(125,43))
                                               automaton step + settleGrid  (tick T)
                                               appliedSeq[2] = 21
                       ◀──DELTA base=T-1 tick=T, appliedSeq=[..,..,21,..], XOR+zstd──
  apply delta to replica; drop predicted seq<=21; rebuild display_
                       ◀── (next tick) ──────
```

Between the PRESS and the DELTA, the guest had already drawn seq 19–21 into `display_` as predicted
segments, so the paint was visible immediately; when the DELTA arrives the predicted segments are
retired and the same cells are on the authoritative grid.

## Appendix C — Example flow: join

```
client                                    host
  ──HELLO v1 "ana" token──▶
                            validate (capacity, version, token)
  ◀──WELCOME id=1 count=2 gridW=640 gridH=360 tickHz=60 startTick=T──
  ◀──KEYFRAME tick=T (zstd(grid), world, roster)──
  ◀──DELTA base=T tick=T+1 ...──
  ◀──DELTA base=T+1 tick=T+2 ...──
  ...
  client applies keyframe, then each delta whose baseTick == lastTick
```
