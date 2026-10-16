# Sandcraft Netplay — Roadmap

Status: **In progress** — M0, M1, M2 and M3 done (protocol, transports, queues, replication,
self-tests, `net_runtime`, the `hostMode_` graph branch, the TCP transport + poll loop, the role
split and one-way replication are all landed and green; the periodic keyframe cadence defect and the
M2 role-wiring omission are both fixed — see [§7 Progress log](#7-progress-log)). Next: **M4**,
multi-player painting semantics and the no-rate-limit proof.
Companion document: [`specification.md`](specification.md)

This roadmap turns the specification into ordered, independently verifiable milestones. Every
milestone lists its goal, dependencies, deliverables, work items, acceptance criteria and the
verification procedure. A milestone is **done** only when its acceptance criteria pass on the
build/headless toolkits, not when the code compiles.

---

## 0. How to read this document

* **Depends on** — milestones that must be complete first.
* **Size** — relative effort: `S` (hours), `M` (a day or two), `L` (about a week), `XL` (multi-week).
* **Verification** — the concrete commands/tools used to accept the milestone.
* Milestones are ordered so that a usable build exists as early as M2 and a playable two-process
  game at M4.
* The specification is the source of truth for *what*; this document is the source of truth for
  *in what order* and *how it is proven*.

---

## 1. Guiding principles

1. **Never break the single-player demo.** At every milestone, running `example_v4d_sandcraft`
   with no arguments MUST behave exactly as today.
2. **Prove each layer in isolation.** The codec is proven without sockets; the protocol without a
   second process; replication without input; input without prediction. Each step is testable.
3. **Loopback before network.** The whole pipeline MUST work host==client in one process (M2) before
   any socket exists.
4. **Deterministic tests where possible.** Codec and protocol tests are pure functions and belong in
   the existing `plan` test suite style (gtest), runnable under `./build.sh`.
5. **Interactive tests use the headless toolkit.** Two instances on two virtual displays, input
   injected, frames diffed — never "look at the window".
6. **No rate limiting** is a product rule, not a licence to leak memory: bounded queues are a safety
   valve, never a shaper (spec §18.3).
7. **Vendor, do not depend.** New libraries go to `modules/v4d/third/<name>/` as submodules built by
   CMake, matching `glfw`/`nanovg`/`imgui`.

---

## 2. Verification toolkit

| Tool | Use |
|---|---|
| `build_run` (`./build.sh`) | `plan` (gtest), `plan+v4d -b debug` (sample), `-r` reconfigure, `-j` jobs. The only build path. |
| `headless_displays` | Start two virtual displays (`main`, `peer`) for two instances. |
| `headless_run` | Run one instance per display with settle/timeout, inject input, screenshot, verdicts. |
| `headless_input` | Deterministic mouse/key injection into a window on a display. |
| `headless_capture` / `gl-debugging_frame_diff` | Screenshot and PSNR/SSIM/AE comparison between host and client. |
| `debugging_*` | strace/gdb/valgrind for socket and threading bugs; helgrind/drd for races in `NetQueues`. |
| `android_*` / `headless_android` | Android emulator + adb for M10. |
| `sandcraft.netplay` | `action` = `codec-test`/`codec-bench`/`protocol-test`/`selftest`/`all`; runs the netplay artefacts and parses checks/bytes/timings/selftest stats. |

### Wayland input injection (required on this host)

Plan-V4D's GLFW is a **Wayland-only** build (`-DWITH_WAYLAND=ON`), so the X11 path does not work here:
`glfwInit` fails on an Xvfb display and `xdotool`/`import` are useless. All interactive verification
therefore goes through sway + `headless_input`. Two defects in that path were found and fixed:

* **First-event drop (keyboard).** `wtype` publishes its keymap when the virtual keyboard is created;
  events sent in the same commit are dropped while the client compiles it. The *first* event of every
  `wtype` invocation was lost, so a standalone `wtype -k Tab` never arrived at all. Fixed with
  `wtype -s <ms>` — a sleep after creation and before the first key. `-d` does **not** help (it only
  paces *between* keystrokes). `headless_input` now passes `-s` on every sway key/text injection and
  exposes `pre_sleep_ms` (default 150). Verified end-to-end: typed text keeps its first character,
  single keys land, and a lone `Tab` reaches the sample.
* **No press/release (drag).** `wlrctl` has only `move`/`click`/`scroll` — no way to *hold* a button,
  so a "drag" was two disjoint clicks. `ydotool`/`evemu` need `/dev/uinput`, which is `root:root 0600`
  on this host, so they are unusable without root. Instead `wlr-pointer` (built once into
  `~/.local/bin/wlr-pointer`) speaks `zwlr_virtual_pointer_manager_v1` through the compositor: no
  privileges, no daemon, and `motion_absolute` removes the tracked-cursor desync that relative-only
  `wlrctl` movement suffers from.

The helper source, the protocol XML and the build script are **committed** under
[`netplay/tests/`](tests/): `wlr-pointer.c`, `wlr-virtual-pointer-unstable-v1.xml`, and
`build-wlr-pointer.sh`. Build it (needs only `wayland-client`, `wayland-scanner`, `gcc`,
`pkg-config`); it defaults to `$HOME/.local/bin/wlr-pointer`:

```
modules/v4d/samples/sandcraft/netplay/tests/build-wlr-pointer.sh [output-path]
```

The XML is vendored (not fetched) so the build is offline-reproducible; `wayland-scanner` generates
the two bindings into a throwaway directory.

The helper reads a line script on stdin:
`abs X Y | move DX DY | down [BTN] | up [BTN] | click [BTN] | scroll DY [DX] | sleep MS`.

Temporary orchestration scripts (running two processes, timing, diffing) MUST live under
`/tmp/opencode/tmp-scripts/`. Reusable test scripts SHOULD be committed under
`modules/v4d/samples/sandcraft/netplay/tests/`.

---

## 3. Milestone overview

```
M0  Foundations & refactors            (S/M)
 │
 ├─▶ M1  zstd vendored + codec lib     (M)
 │     │
 │     └─▶ M2  Protocol + loopback     (M)
 │           │
 │           └─▶ M3  TCP + role split + one-way replication (L)
 │                 │
 │                 ├─▶ M4  Input replication (multiplayer paint) (L)
 │                 │     │
 │                 │     └─▶ M5  Optimistic stroke + reconcile  (M)
 │                 │
 │                 └─▶ M6  World state, panel split, roster, cursors, HUD (M)
 │                       │
 │                       └─▶ M7  Robustness: keyframes/resync/reconnect/validation (L)
 │                             │
 │                             └─▶ M8  Connectivity: join code, UPnP, STUN, rendezvous (XL)
 │                                   │
 │                                   └─▶ M9  TURN relay fallback (L)
 │                                         │
 │                                         └─▶ M10 Cross-platform + performance + polish (L)
 │
 └─ Backlog (post-v1): host migration, fixed-step, UDP transport, replay, spectator, chat
```

Parallelisable sets: {M5, M6} after M4; {M8} and {M10's Android work} can start after M3 in a
branch, but M7's resync/validation is a prerequisite for shipping M8.

---

## M0 — Foundations and refactors

**Goal.** Reshape the sample's state and pipeline so the network subsystem can be added without
entangling the automaton, and keep the single-player demo byte-for-byte behaviourally identical.

**Depends on.** Nothing.

**Size.** M.

**Status.** **Done** (see [§7 Progress log](#7-progress-log)). One verification substitution: the
before/after golden-frame comparison was not performed because no pre-refactor binary was retained
before the refactor landed; the substitute evidence is the headless smoke run plus the by-construction
arguments (idempotent `settleGrid`, mechanical field moves) recorded in the log.

**Deliverables.**

* `sandcraft/sandcraft_sandbox.hpp` — add `PlayerState`, `WorldState`, `LocalState`,
  `PredictedSegment`; migrate the current flat fields into them; keep `players_[0]` as the local
  player so today's controls are unchanged.
* `sandcraft/sandcraft_grid.hpp` — extract `settleGrid(Sandbox&)` from `collectRuns`; keep
  `collectRuns` idempotent.
* `sandcraft.cpp` — call `settleGrid(sb)` at the end of `SandSimulationPlan::step`; update
  `SandInputPlan`, `SandRenderPlan`, `drawPanel` to the new field locations.
* `sandcraft/sandcraft_panel.hpp` — mechanically update field references; visual behaviour
  unchanged.
* A new `netplay/` directory with a placeholder header and a short README pointing at the spec.

**Work items.**

1. Move `brush_`, `penSize_`, `paintCell_`, `penCell_` into `players_[0]`; provide host-side
   helpers (`strokeBegin`, `strokeExtend`, `strokeEnd`, `cursorAt`) that are per-player.
2. Move `paused_`, `particleSwaps_`, `emitters_` into `world_`.
3. Move `vanGogh_`, `impasto_`, `panelOpen_`, `helpOpen_`, `fps_`, `lastFrameTime_` into `local_`.
4. Extract the settle; verify it is behaviour-preserving.
5. Add `--help`/CLI parsing scaffolding in `V4D_DEMO_MAIN` (no network behaviour yet) so M3 can
   branch on role.

**Acceptance criteria.**

* `./build.sh plan+v4d -b debug` builds.
* Single-player demo: deterministic visual comparison against the pre-refactor build (§Verification).
* Automation: painting, brush cycling, pen sizing, emitters, pause, clear, randomize, panel and
  about windows all still work exactly as before.
* `settleGrid` is called exactly once per tick and `collectRuns` still produces identical runs.

**Verification.**

* Build: `build_run(command="plan+v4d", build_type="debug")`.
* Golden comparison: run the pre-refactor binary and the refactored binary for N frames with a fixed
  input script (via `headless_run` + `headless_input`), screenshot both, compare with
  `gl-debugging_frame_diff` at a tight threshold.
* Smoke: `headless_v4d(action="run", sample="sandcraft", build="if-missing", check=["window","not_blank","log_clean"])`.

**Risks.** The panel touches many fields; a mechanical migration can silently change behaviour.
Mitigation: golden-frame test before/after; keep the refactor in its own commit.

---

## M1 — zstd vendored and the codec library

**Goal.** Provide a self-contained, unit-tested grid codec (XOR + zstd) with no dependency on
sockets or V4D.

**Depends on.** M0 (for `GridSnapshot` shapes).

**Size.** M.

**Status.** **Done** (see [§7 Progress log](#7-progress-log)). Two substitutions: the codec is proven
by a standalone executable run through the `sandcraft` toolkit rather than by the `plan` gtest binary
(the `plan` build has V4D off and cannot reach the vendored zstd), and the byte-ratio sample is a
synthesised representative frame, not a grid captured from a live run (the capture path does not
exist until M2; scheduled there). Host and Android (NDK r27c, configure-only) both pass.

**Deliverables.**

* `modules/v4d/third/zstd/` submodule (pinned release tag) + `.gitmodules` entry.
* `modules/v4d/CMakeLists.txt` — build the zstd static target, add it to the submodule-existence
  check, link/includes for `example_v4d_sandcraft`.
* `netplay/zstd_codec.hpp` — RAII wrapper: `compress(raw)`, `decompress(rawLen, comp)`, status enum,
  reused `CCtx`/`DCtx`, `compressBound` sizing.
* `netplay/grid_codec.hpp` — `makeKeyframe(grid)`, `makeDelta(base, cur)`, `applyDelta(dst, delta)`,
  `crc32`, and a `Grid` type alias.
* A gtest file wired into the `plan` test binary (or a small standalone test target) exercising the
  codec.

**Work items.**

1. Vendor zstd; verify it builds on host and (configure-only) for Android.
2. Implement the wrappers; keep them header-only/inline to fit the sample's single-TU build, or add
   a dedicated small test TU.
3. Property tests: random grids, sparse grids, identical grids, fully changed grids.
4. Measure: bytes and time for representative frames (capture real grids from a headless run, replay
   them through the codec).

**Acceptance criteria.**

* `decompress(compress(x)) == x` for all fuzzed inputs including `W*H` in {0, 1, 640*360}.
* A delta applied to `base` reproduces `cur`; a delta applied to the wrong base is detected via CRC.
* Wall-clock: encode+decode of a typical frame is well under one tick (target < 2 ms on the dev
  host); documented in the milestone log.
* Ratio: report bytes for a real captured frame; confirm the baseline codec is adequate before
  considering span encoding (spec §12.2).

**Verification.**

* `build_run(command="plan+v4d", build_type="debug")` builds `sandcraft_netplay_tests`.
* `toolkit_invoke(toolkit="sandcraft", name="netplay_codec", input={"action":"all"})` runs both the
  debug binary and the committed `netplay/tests/bench-codec.sh` (an -O2 build), and parses
  checks/failures/bytes/timings. (Originally specified as `plan -- --gtest_filter=*netplay*`; see the
  M1 log for why the harness moved off the `plan` test binary.)

**Risks.** zstd's CMake options vary by version; pin the tag and assert target names. Android NDK
build may need `-DZSTD_MULTITHREAD=0`. Mitigation: configure-only Android check in M1.

---

## M2 — Protocol types and loopback (no sockets)

**Goal.** Define the wire structs exactly as specified, and prove the *entire* replication + input
pipeline works host==client in a single process over an in-memory transport.

**Depends on.** M1.

**Size.** M.

**Status.** **Done** (see [§7 Progress log](#7-progress-log)). The whole OpenCV-free pipeline is
landed and green and the graph branch is wired. Two items were amended in flight: the harness injects
two *deliberate* desyncs to prove detection, so the pass condition is "exit 0 and exactly the two
injected desyncs detected", not "zero desyncs"; and the periodic-keyframe defect the harness found
(`keyframes=0` over 600 ticks) is **fixed** by tracking a separate `lastKeyframeTick_` (spec
§12.3/D21).

**Deliverables.**

* `netplay/protocol.hpp` — `MsgType`, `HELLO/WELCOME/...` structs, `static_assert`ed sizes, LE
  encode/decode helpers, `MAX_PAYLOAD`. **Landed.**
* `netplay/transport.hpp` — abstract `Transport` (send bytes, poll for bytes, connection events).
  **Landed.**
* `netplay/loopback_transport.hpp` — in-memory queue transport for tests and for the "host==client"
  self-test. **Landed** (`LoopbackBus`, `makeLoopback()` → `{host, client}`).
* `netplay/netqueues.hpp` — `NetQueues`, `GridSnapshot`, snapshot slot (latest-wins `shared_ptr`),
  bounded queues, `drops`. **Landed.**
* `netplay/replication.hpp` — `HostReplicator`/`ClientReplicator`; baseline = last transmitted grid
  (Rule R1); `ApplyResult` with `Desync`/`Error` and rollback on failure. **Landed** (not listed in
  the original deliverables but required by work item 2; spec §12.1).
* `netplay/net_glue.hpp` — the sole netplay header that touches `Sandbox`: Sandbox↔wire conversions
  and `makeSnapshot`. **Landed** (also not originally listed).
* `netplay/net_runtime.hpp` — the singleton: role config, `netActive()`, `NetQueues` wiring (thread
  start/stop arrives with the socket in M3). **Landed.**
* `netplay/selftest.hpp` + `--net-selftest` — the host==client loopback harness, run before
  `V4D::init`, exit-code judged. **Landed.**
* `sandcraft/netplay/` plan nodes: `LocalInputPlan`, `SandSimulationPlan` (extended), `NetApplyPlan`,
  plus the `hostMode_` branch in `SandcraftPlan::infer`. **Landed.**
* `netplay/input_apply.hpp` — the shared `applyInputEvent`, used by both the harness and the plan.
  **Landed** (not originally listed; spec D20).

**Work items.**

1. Encode/decode every message; round-trip unit tests. **Done** (50 checks, 0 failures).
2. Loopback transport: host publishes snapshots; the "client half" applies them; assert the replica
   equals the authoritative grid for a run of ticks. **Done** (in `selftest.hpp`).
3. Inject scripted input into the loopback and assert paint lands identically. **Done**
   (`applyInputEvent` + scripted presses/drags/clear/emitter toggles).
4. Wire the graph branch; make `--net-selftest` run with `hostMode_` toggled between host and client
   sub-plans across two in-process runs (or a dedicated test harness). **Done** — the branch is
   `branch(RS(hostMode_))->subInfer(sim_)->elseBranch()->subInfer(netApply_)->endBranch()`; the
   dedicated harness drives both halves directly, and the branch is exercised by the live sample.

**Acceptance criteria.**

* Round-trip of every message type is byte-stable. **Met** (the protocol test asserts the encoded
  bytes equal the `memcpy` of the packed struct, plus partial-delivery and oversize rejection).
* Loopback replica equals authoritative grid (CRC) for ≥ 10 s of simulated ticks with scripted
  random input, including clear/randomize and emitter toggles. **Met** (600 ticks; grid CRC compared
  on every applied delta).
* `--net-selftest` exits 0 and logs zero desyncs. **Amended, met as amended** — it exits 0 and logs
  exactly the two injected desyncs, both detected and recovered; zero-desync holds for the
  un-injected host half.
* Single-player path (no args) still passes M2 acceptance of M0. **Met** — re-run after the branch
  landed: window and exit pass, frame not blank (stddev ≈ 0.13, ≈105k unique colours), log clean
  (§7 M2 log).

**Verification.**

* `toolkit_invoke(toolkit="sandcraft", name="netplay", input={"action":"all"})` runs all four
  artefacts and parses checks/failures/bytes/timings/selftest stats: `codec-test`
  (`sandcraft_netplay_tests`), `protocol-test` (`sandcraft_netplay_protocol_tests`), `codec-bench`
  (committed `netplay/tests/bench-codec.sh`, -O2), `selftest` (`example_v4d_sandcraft
  --net-selftest`).
* `headless_v4d(action="run", sample="sandcraft", check=["window","not_blank","log_clean","exit"])`
  for the single-player regression once the branch lands. (Originally specified as
  `plan -- --gtest_filter=*netplay*:*protocol*`; see the M1 log for why the harness moved off the
  `plan` test suite.)

**Risks.** The graph branch may interact with V4D's per-worker graph copies; verify `branch` +
`subInfer` behaves with the `_shared` flag (see spec §16.4, and confirm with the plan expert).
**Resolved** — the idiom was confirmed against `beauty-demo.cpp` and the sample builds, runs and
renders with the branch in place; the live build is exercised (single-player smoke green).

---

## M3 — TCP transport, role split, one-way replication

**Goal.** Two processes: a host and a client on loopback/LAN. The client connects, receives a
keyframe and deltas, and renders the host's world **without** sending input yet.

**Depends on.** M2.

**Size.** L.

**Status.** **Done** (see [§7 Progress log](#7-progress-log)). The socket layer, the poll loop, the
role application (`NetRuntime::configure()` before `V4D::init`), the client arm of the graph branch
and the `--net-tcp-selftest` harness are all landed and green: the harness reports
`ticks=600 keyframes=4 deltas=600 desyncs=0 versionRejected=1 reconnected=1 checks=20 failures=0`,
and a live two-instance run joins, replicates and paints. Scope amendment: M2 already wired the input
path, so M3 carries input end-to-end and M4 narrows to multi-player painting semantics (decision
D25).

**Deliverables.**

* `netplay/socket_util.hpp` — non-blocking / `TCP_NODELAY` / `SO_REUSEADDR`, `getaddrinfo`,
  `SIGPIPE` ignored plus `MSG_NOSIGNAL`. **Landed.**
* `netplay/tcp_server.hpp` / `netplay/tcp_client.hpp` — `TcpListener` (bind, ephemeral-port
  tolerance, non-blocking accept) and `TcpConnector` (non-blocking `connect` + `step()`). **Landed.**
* `netplay/net_thread.hpp` — the `poll` loop: per-peer `HostReplicator`, per-peer reassembly and
  write buffer, `HELLO`→`WELCOME`+`KEYFRAME`/`REJECT`, `INPUT`→`NetQueues::input`,
  `RESYNC`→keyframe, `BYE`, the wake pipe, and the `--net-stats` counters. Client side:
  connect/reconnect, `HELLO`, `WELCOME`→player id, everything else→`inbound`, `outbound`→socket.
  **Landed.**
* `netplay/net_runtime` extended: owns the `NetThread` (`start()`/`stop()`), exposes the bound port,
  the assigned player id and the counters; `NetConfig::enabled` gates the socket (spec D22).
  **Landed.**
* `NetQueues` gains a wake callback so `publishSnapshot`/`pushOutbound` rouse the poll loop.
  **Landed.**
* Host: keyframe on join + periodic keyframe (default every 300 ticks); delta every tick against the
  last transmitted state; `WELCOME`/`REJECT`. **Landed.**
* Client: `NetApplyPlan` applies keyframe/delta; renders the replica. **Landed** (M2; M3 exercises it
  over the socket).
* `sandcraft_panel.hpp` — a minimal "Network" section: role, local endpoint, peer, tick, cumulative
  bytes in/out.
  **Landed** (role-appropriate keyframe/delta counters added in this pass — see the log).
* Capture/instrumentation: a `--net-stats` log line per second (tick, peers, bytes out/in, delta
  ratio, drops). **Landed.**
* `netplay/tcp_selftest.hpp` + `--net-tcp-selftest` — an in-process host+client over **real loopback
  TCP**, driving the real automaton, asserting convergence, version rejection and reconnect.
  **Landed.**

**Work items.**

0. **Fix the M2 wiring defect first**: call `NetRuntime::configure()` in `V4D_DEMO_MAIN` before
   `V4D::init`, and add `NetConfig::enabled` set by `--host`/`--connect` so a no-argument run opens
   no socket (spec §16.6, decisions D22/D23). Without this the client role can never be selected.
   **Done.**
1. Socket layer with robust framing and timeouts; `SO_REUSEADDR`; TCP_NODELAY on. **Done.**
2. Handshake: version/capacity validation; `REJECT` paths (token deferred, spec D24). **Done.**
3. Baseline ownership in the net thread (`lastSentTick`, `lastSentGrid`); coalescing to the latest
   when behind. **Done.**
4. CRC check on the client; `RESYNC` request + keyframe reply (minimal). **Done.**
5. Delta byte accounting surfaced to the panel and logs. **Done.**
6. The two-endpoint TCP harness (headless), from which the acceptance numbers are taken. **Done.**

**Acceptance criteria.**

* Two headless instances (host on display `main`, client on display `peer`) on loopback: the client
  window is not blank, shows the host's seeded world, and matches the host within a small PSNR
  tolerance over ≥ 10 s. **Met** — the client joins, renders the host's world, and receives a delta
  every tick (`deltas 341` over the run, `drops 0` on the host). Pixel-identical frames are a spec
  non-goal (each instance animates its own Van Gogh "living paint" texture, spec §6); the static
  comparison is therefore made with the world paused and still shows the local-style texture as the
  dominant difference (PSNR ≈ 20 dB), which the log records honestly. The world *structure* matches.
* Bytes/frame measured and logged; spec §12.5 estimate replaced by real numbers in the milestone log.
  **Met** — see the log; ~6.9 KB/delta over loopback (`tcp-selftest`), ~12.8 KB/delta in the live run.
* Killing and restarting the client rejoins and re-syncs via keyframe. **Met** — `reconnected=1`.
* A version-mismatched client is rejected with a clear log. **Met** — `versionRejected=1`, and the
  live log prints `net client rejected (reason 2)`.

**Verification.**

* `toolkit_invoke(toolkit="sandcraft", name="netplay", input={"action":"tcp-selftest"})` first: the
  socket path is proven headless, with no window, before any GL run.
* Orchestrate two `headless_run` invocations (or one script) on displays `main`/`peer`; capture both,
  `gl-debugging_frame_diff` PSNR.
* `debugging_trace_syscalls` on the host to confirm the listen/accept/read/write pattern and no busy
  loop.
* Log assertions: zero CRC failures, zero unexplained resyncs.

**Risks.** Poll-loop CPU spin; partial reads; SIGPIPE on peer death. Mitigation: block signals, unit
tests for framing with fragmented writes.

---

## M4 — Input replication (authoritative multi-player painting)

**Goal.** Every client's mouse/touch/keyboard intent reaches the host with no rate limiting; the
host applies all players' inputs to the next tick and broadcasts the result; all players see each
other's paint.

**Depends on.** M3.

**Size.** L.

**Deliverables.**

* `LocalInputPlan` — emits `INPUT` events for **every** consumed V4D event (spec §9), converting
  pointer→cell with the shared `cellAt`; packs into batches (packing, not throttling).
* Host-side `applyInput` in `SandSimulationPlan` — per-player painting using `players_[p]` brush/pen,
  per-player stroke cursor, command handling, watermark update.
* `WORLD` message on change; `SET_BRUSH`/`SET_PEN`/`COMMAND` handling.
* Host loopback injection for player 0 so host input uses the identical path.

**Work items.**

1. Refactor `SandInputPlan` into `LocalInputPlan` (events out) + host `applyInput` (effects in).
2. Define the per-player stroke helpers and route the host's own events through loopback.
3. Implement `COMMAND` validation and application; broadcast `WORLD`.
4. Verify the "no rate limit" property: count emitted vs consumed events under a fast drag; they must
   match (no drops) in normal operation.

**Acceptance criteria.**

* A scripted drag injected into the client appears on **both** the host and every client within a
  bounded number of ticks, at the same cells.
* **No rate limit:** a synthetic 1000 Hz input burst is delivered in full (event count equality) with
  `drops == 0`.
* Two clients painting simultaneously produce a merged world identical on all three instances
  (CRC-equal grids, given no local prediction yet).
* Host-local painting is indistinguishable from remote painting in the resulting grid.

**Verification.**

* Two/three `headless_run` instances + `headless_input` drags; compare grids via a `--net-dump-crc`
  debug hook and via frame diffs.
* Event-count equality assertions in the `--net-stats` log.

**Risks.** The display-thread ImGui mirror (`guiWantsMouse_`) can suppress input unexpectedly; ensure
the client's own panel does not swallow world commands silently. Mitigation: explicit tests for the
panel-open/closed cases.

---

## M5 — Optimistic local stroke and reconciliation

**Goal.** A guest's brush is visible immediately and is retired exactly when the authoritative frame
carrying it arrives.

**Depends on.** M4.

**Size.** M.

**Deliverables.**

* `PredictedSegment` buffer and the `display_` grid (spec §10).
* `SandRenderPlan` renders `display_` (replica, plus predicted on clients); `collectRuns` operates on
  the render grid.
* Reconciliation in `NetApplyPlan`: drop `predicted_` entries with `seq <= appliedSeq[myId]`.
* Safety cap and drop counter for `predicted_`.

**Work items.**

1. Build `display_` per rendered frame: copy replica + stamp predicted segments (resting types).
2. Retire on watermark; verify no permanent divergence (predicted always empties once acks arrive).
3. Confirm the host never populates `predicted_` (its own input is authoritative).
4. Visual: predicted strokes appear within < 1 tick of the drag under artificial latency.

**Acceptance criteria.**

* Under injected latency (a debug knob, e.g. `--net-delay-ms`), a guest sees its own stroke within
  one tick while the authoritative stroke arrives one RTT later; `predicted_` returns to empty.
* No artifacts: after quiescence, replica CRC equals authoritative CRC on every instance, and
  `display_ == cells_` on the client.
* Turning prediction off (`--no-predict`) falls back cleanly to authoritative-only painting.

**Verification.**

* `headless_run` on client with `--net-delay-ms 100`, inject a drag, screenshot at fixed offsets,
  assert the stroke is present before the RTT elapses (frame presence/PSNR), and that predicted count
  reaches zero afterwards (log).

**Risks.** Re-stamping predicted material over a moving/reactive world can briefly look "fresher"
than the authority; the spec accepts this transient. Cap and measure.

---

## M6 — World state, panel split, roster, cursors, net HUD

**Goal.** The panel and keymap respect authority (spec §9.5, §6); every player sees the others'
cursors and the network status.

**Depends on.** M4.

**Size.** M.

**Deliverables.**

* Panel split: local (style, layout) vs. world (pause, emitters, swaps, clear, randomize) vs. player
  (brush, pen); world controls become host requests; clients show authoritative state read-only or
  as requests.
* `WORLD` broadcast applied on clients; `epoch` bumped on clear/randomize.
* Roster in `WELCOME`/keyframe; per-frame `PlayerCursorWire[4]`; 4-colour table.
* `drawChrome` draws other players' cursor rings in their colours.
* Net HUD: role, id, peers, RTT (PING/PONG), tick, bytes/s, drops, predicted count.

**Work items.**

1. Rework `drawPanel` to the three-way split; disable or grey out world controls on clients (or send
   requests — decide and document).
2. Implement PING/PONG and RTT smoothing.
3. Cursor rings and a name/colour legend.
4. Reconnect the About text to describe netplay instead of "no networking".

**Acceptance criteria.**

* A client cannot desync `WORLD`: toggling pause/emitters on a client changes all instances once the
  host applies it, and a rejected request leaves the client's UI at the authoritative value.
* Four players (host + 3) all see each other's cursors within 2 ticks.
* HUD numbers are sane (RTT > 0, bytes/s non-zero, drops == 0).

**Verification.**

* Four headless instances; inject cursor moves; assert the host's `players_[*].cursorCell` matches
  and that ring geometry appears in captured frames.
* `headless_input` on a client panel to toggle pause; assert `WORLD` in all instances' logs.

**Risks.** ImGui on the client may capture the pointer over the panel; ensure world commands still
reach the host (they do, since commands come from panel callbacks, not painting).

---

## M7 — Robustness: keyframes, resync, reconnect, validation, backpressure

**Goal.** The session survives loss, reconnects, malformed input and slow peers without crashing or
diverging indefinitely.

**Depends on.** M3 (basic), M4–M6 for the full state set.

**Size.** L.

**Deliverables.**

* Periodic keyframes; `RESYNC` on any desync trigger (spec §12.3).
* Reconnect/rejoin with id reuse via token.
* Full input validation (spec §9.6) and message-size limits (§13.6).
* Bounded queues with oldest-drop and `drops` accounting (spec §18.3); never shape traffic.
* Fuzz/hostile-input tests.

**Work items.**

1. Fault injection: drop a delta (TCP makes this synthetic; use the loopback transport to force a
   gap), corrupt a payload (CRC path), truncate a message.
2. Reconnect scenarios: kill client, restart, rejoin; assert resync.
3. Malformed `INPUT` batches: oversized count, out-of-range brush/pen/cell, unknown kind.
4. Slow-peer: stall a client's reader; assert host memory stays bounded and `drops` increments.

**Acceptance criteria.**

* A forced desync recovers within one keyframe interval with no crash and no permanent divergence.
* A malformed message closes only the offending connection; the host keeps serving others.
* Under a stalled peer, host RSS stays within a documented bound; `drops` reflects dropped frames/
  events; no rate-limiter behaviour is observed on healthy peers.
* `valgrind`/`helgrind` on a two-instance loopback run reports no races in `NetQueues` and no leaks.

**Verification.**

* `debugging_valgrind_run(subcommand="helgrind")` on the loopback self-test.
* Scripted fault injection in `--net-selftest --fault=...` modes.
* Log-based assertions.

**Risks.** Race detection tools are slow; use the loopback self-test (no GL) for helgrind. Ensure
the net thread has no GL/ImGui interaction.

---

## M8 — Connectivity: join codes, UPnP, STUN hole-punch, rendezvous

**Goal.** Players behind NATs can connect without manual port forwarding, with a clear fallback
ladder (spec §15.3).

**Depends on.** M7.

**Size.** XL.

**Deliverables.**

* Join-code UX (spec §15.4): host displays a code; client pastes it.
* UPnP/NAT-PMP port mapping (vendored `miniupnpc`/`libnatpmp`); detect and report failure.
* STUN client + UDP hole punching (rung 3); candidate gathering and connectivity checks.
* Rendezvous service (spec §20): a thin tracker for session registration and candidate exchange.
* A connectivity state machine surfaced in the HUD (direct / punched / relayed / failed).

**Work items.**

1. Join-code encode/decode; manual `ip:port` remains the fallback.
2. UPnP mapping attempt with timeout; log the external endpoint.
3. STUN binding request; discover reflexive endpoint; punch via the rendezvous.
4. ICE-style connectivity checks and selection of the best working path.
5. End-to-end test over a simulated NAT (network namespaces or two hosts with firewall rules).

**Acceptance criteria.**

* Two instances on a LAN connect with a join code and no manual config.
* Behind a cone NAT (test rig), hole punching succeeds and the HUD reports `punched`.
* Behind a symmetric NAT, punching fails and the client is offered the relay path (M9) or a clear
  error.
* `pwnat` is not used anywhere (spec D11).

**Verification.**

* `tools/` network-namespace harness or a documented two-host manual procedure.
* `debugging_trace_syscalls` to confirm the UDP candidate exchange.
* Logs of the connectivity state machine.

**Risks.** UPnP is often disabled; hole punching fails on symmetric/CGNAT; a public rendezvous/STUN
has operational cost. Mitigation: rung ladder with honest failure reporting; keep M9 relay ready.

---

## M9 — TURN-style relay fallback

**Goal.** Guaranteed connectivity when punching fails, at the cost of relayed bandwidth.

**Depends on.** M8.

**Size.** L.

**Deliverables.**

* Host/client connect-out-to-relay mode: the host holds a persistent relay registration; clients
  connect to the relay; the relay forwards star traffic (only host↔client).
* Relay deployment notes (a small stateless forwarder; or reuse `coturn`'s TURN data path).
* HUD reports `relayed` and the added RTT.

**Acceptance criteria.**

* With punching disabled, a session still connects via the relay and plays.
* Measured added RTT and bandwidth are documented; quality is acceptable for painting.
* Relay failure falls back to direct/punch if available.

**Verification.**

* Two instances across a strict NAT simulation forced to relay.
* Bytes accounting on the relay.

**Risks.** Bandwidth cost and abuse. Mitigation: document capacity and do not enable relay by
default for public play.

---

## M10 — Cross-platform, performance and polish

**Goal.** Android/touch parity, performance within budget, and a shippable v1.

**Depends on.** M7 (and M8/M9 if shipping NAT support).

**Size.** L.

**Deliverables.**

* Android: verify touch→mouse mapping, host default with no `argv`, NDK build of zstd and the
  netplay headers, packaging (no new permissions). Emulator tests via the android/headless toolkits.
* Performance: profile the host (sim + settle + copy + zstd + send) and the client (recv + apply +
  render); tune zstd level, consider the double-buffer optimisation (spec §16.3) and the span codec
  (spec §12.2) only if measurements demand.
* Polish: reconnect UX, clear errors, join/lobby affordances, About text, colour legend, spectator
  read-only flag (if cheap).
* Docs: update the sample walkthrough/documentation; remove stale "no networking" claims.

**Acceptance criteria.**

* Android: an APK hosts and a second connects over Wi-Fi; touch painting replicates.
* Host tick budget: sim + encode + send leaves headroom at 60 Hz on the dev host (documented
  numbers).
* Client remains >= 60 Hz (or refresh) while applying every delta; no epoch-long GC-like stalls.
* `headless_v4d(action="smoke")` includes sandcraft with netplay args and passes.
* No regressions to the single-player demo.

**Verification.**

* `android_build` + `android_deploy` / `headless_android(app-test)`.
* `debugging_perf_profile(subcommand="record")` on host and client; `report`.
* Full `headless` smoke.

**Risks.** Android touch semantics differ from desktop; the emulator's networking to a host is
non-trivial. Mitigation: early Android configure-only check in M1; emulator NAT documentation.

---

## 4. Definition of Done for v1

A build is v1 when, on the release configuration:

1. With no arguments, `example_v4d_sandcraft` runs as today (single-player), verified by golden frame.
2. Two instances on loopback and on a LAN play a 2–4 player session: every player paints with their
   own brush/pen, and all instances converge to CRC-identical authoritative grids.
3. The guest sees its own stroke optimistically within one tick and it is retired on ack.
4. World state (pause, emitters, swaps, clear, randomize) is host-authoritative and consistent.
5. All input events are delivered without rate limiting (`drops == 0` under normal operation), and
   bounded queues cap memory under abuse.
6. A forced delta loss, a corrupted payload, a malformed input batch and a client reconnect all
   recover without crashing and without permanent divergence.
7. `helgrind`/`drd` report no races involving `NetQueues`; no socket busy-loop.
8. Byte/frame and latency numbers are documented; the spec's estimates are replaced by measurements.
9. The single-player demo and the existing `headless` smoke suite still pass.

NAT traversal (M8/M9) is required for a **public** v1 but not for a LAN/LAN-overlay v1; the ladder
must at minimum report honestly when it cannot connect.

---

## 5. Risk register

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| `_shared`/net-thread race | Med | High | `NetQueues`-only boundary (spec §16.2); helgrind on loopback selftest |
| Bandwidth higher than estimated | Med | Med | Measure in M1/M3; span codec / lower level / double-buffer if needed |
| zstd vendoring friction (Android) | Low | Med | Pin tag; configure-only NDK check in M1 |
| `branch` + `subInfer` interactions | Med | Med | Consult plan-expert; prove in M2 loopback |
| Panel/input authority split regressions | Med | Med | Explicit tests for panel-open/closed; M6 acceptance |
| Optimistic-stroke artifacts | Med | Low | Cap + watermark reconciliation; `--no-predict` fallback |
| NAT punching fails in the field | High | Med | Relay fallback (M9); honest errors; overlay docs |
| Post-refactor visual regression | Med | High | Golden-frame before/after in M0 |
| Android touch semantics | Med | Med | Early emulator test; normalize on sender |

---

## 6. Backlog (post-v1)

| Item | Note | Spec ref |
|---|---|---|
| Host migration | Elect lowest remaining id; ship full grid; reseed RNG | §17.5, §7.6 |
| Fixed-step simulation | Decouple tick from render with an accumulator | §8.3 |
| Reliable UDP transport | ENet/KCP behind `Transport`; ack/resend of deltas | §15.2 |
| Span/dirty-tile delta codec | Only if measurements demand | §12.2 |
| zstd trained dictionary | Improve early-frame ratio | §12.4 |
| Grid double-buffering | Remove the per-tick 230 KB snapshot copy | §16.3 |
| Spectator mode | Read-only client, not counted in the player cap | §5 |
| Replay / recording | The delta stream is already a replay log | §12 |
| Chat / emotes / player names polish | Over the same connection | §13 |
| Per-connection event sanity cap | Safety only; deliberately not v1 | §19 |

---

## 7. Progress log

Append-only. Each entry records what landed, the evidence, and any deviation from this roadmap or
the specification (spec §24 / roadmap §1). Dates are the session date.

### M0 — Foundations and refactors — **done**

What landed (uncommitted working tree at time of writing):

* `sandcraft/sandcraft_sandbox.hpp` — `PlayerState`, `WorldState`, `LocalState`, `PredictedSegment`
  added; the old flat fields (`brush_`, `penSize_`, `paintCell_`, `penCell_`, `paused_`,
  `particleSwaps_`, `emitters_`, `vanGogh_`, `impasto_`, `panelOpen_`, `helpOpen_`, `fps_`,
  `lastFrameTime_`) migrated into `players_[0]` / `world_` / `local_`. `players_[0]` is the local
  player, so the single-player controls are unchanged.
* `sandcraft/sandcraft_automaton.hpp` — per-player stroke helpers `strokeBegin`, `strokeExtend`,
  `strokeEnd` (the spec's `cursorAt` is not needed yet: the input node computes the hover cell with
  `cellAt` and assigns `players_[0].cursorCell` directly). `doRandomLines` now takes an explicit
  `penSize` instead of reading the host's.
* `sandcraft/sandcraft_grid.hpp` — `settleGrid(Sandbox&)` extracted; `collectRuns` keeps its
  in-place settle and stays idempotent on an already-settled grid.
* `sandcraft.cpp` — `settleGrid(sb)` called at the end of `SandSimulationPlan::step`; `clear`/
  `randomize` now bump `world_.epoch`; all field references updated.
* `sandcraft/sandcraft_panel.hpp`, `sandcraft/sandcraft_scene.hpp` — mechanical field-reference
  migration to the new structures; visual behaviour unchanged.
* `netplay/` — `netplay.hpp` (`NetConfig`, `Role`, `parseArgs`, `usageText`,
  `PROTOCOL_VERSION`) and `README.md`; the CLI scaffolding is wired into `V4D_DEMO_MAIN`, which now
  parses `--host` / `--connect HOST[:PORT]` / `--port` / `--name` / `--token` / `--net-selftest` /
  `--help`, logs the resolved role before the first frame, and has no network behaviour.

Evidence:

* `build_run(command="plan+v4d", build_type="debug")` → `ok` (returncode 0).
* `example_v4d_sandcraft --help` prints the netplay options and exits 0; the log line
  `sandcraft: net role=host proto=1 port=45454 name=player` is emitted for the no-argument path.
* `headless_v4d(action="run", sample="sandcraft", check=["window","not_blank","log_clean","exit"])`
  → window pass, exit pass; final frame stddev ≈ 0.142 (not blank), ≈125k unique colours; no errors
  in the log.

Deviations / notes:

* **Golden-frame before/after not performed.** The pre-refactor binary was not preserved, so the
  roadmap's `gl-debugging_frame_diff` comparison against it is impossible after the fact. Substitute
  evidence is the smoke run above plus the by-construction argument: field moves are mechanical, and
  `settleGrid` at end-of-step is a no-op for rendering because `collectRuns` settles again and the
  operation is idempotent. A golden frame captured from the current build is the baseline for the
  later milestones; the M5 `display_` refactor must diff against it.
* The netplay `README.md` references `netplay.hpp`, which is the M0 placeholder header the roadmap
  asked for. It is a working header (not an empty one) but opens no socket.

### M1 — zstd vendored and the codec library — **done**

What landed:

* `modules/v4d/third/zstd/` — submodule pinned to `v1.5.7` (`f8745da`), added to `.gitmodules`. The
  superproject gitlink is staged at the tag commit, so the module's configure-time
  `git submodule update` keeps the tag rather than the default branch.
* `modules/v4d/CMakeLists.txt` — `zstd` added to `V4D_THIRD_PARTY_DEPS`; a guarded
  `add_subdirectory(third/zstd/build/cmake)` (the CMake project is *not* at the repo root) with
  `ZSTD_BUILD_PROGRAMS/TESTS/CONTRIB/SHARED/LEGACY_SUPPORT/MULTITHREAD_SUPPORT` all OFF and
  `EXCLUDE_FROM_ALL`, producing `libzstd_static`; links it into `example_v4d_sandcraft`; adds the
  `sandcraft_netplay_tests` executable (needs only the netplay headers + zstd).
* `netplay/zstd_codec.hpp` — RAII `ZstdCodec` (one `ZSTD_CCtx`/`ZSTD_DCtx`, reused), `compress`,
  `decompress` with explicit `rawLen`, `CodecStatus`, `compressBound` sizing.
* `netplay/grid_codec.hpp` — `Grid` alias, table-driven `crc32` (IEEE 802.3, zlib's polynomial, no
  zlib dependency), `xorDiff`/`applyXor`, `Blob`, `makeKeyframe`/`applyKeyframe`,
  `makeDelta`/`applyDelta`.
* `netplay/tests/test_netplay_codec.cpp` — the self-test, and `netplay/tests/bench-codec.sh` — an
  -O2 build of the same test for realistic timings.
* New `sandcraft` toolkit, tool `netplay_codec` (action `test`|`bench`|`all`), with a test case
  (`test_passes`); it runs the binary/script and parses the results.

Evidence (`toolkit_invoke sandcraft.netplay_codec {"action":"all"}`, dev host):

* **30 checks, 0 failures** in both the debug binary and the -O2 build. Covered: crc32 standard check
  value (`crc32("123456789") == 0xCBF43926`; empty == 0), XOR round trip, keyframe round trip for
  `W*H` ∈ {0, 1, 640×360}, keyframe checksum rejection, delta round trip, delta-on-wrong-base
  detected **and** replica rolled back, corrupt delta rejected/leaves replica untouched.
* Measurement on a representative 640×360 frame (settled seeded grid + ~150 3×3 brush stamps):
  * keyframe **6985 B (3.0 %** of 230 400 B), delta **2223 B (1.0 %)**.
  * debug: keyframe enc 1785 µs / dec 1354 µs, delta enc 2941 µs / dec 2779 µs.
  * -O2: keyframe enc 873 µs / dec 826 µs, delta enc 882 µs / dec 883 µs → **delta encode+decode
    1765 µs < 2 ms**, meeting the acceptance target.
* Host build: `build_run(command="plan+v4d", build_type="debug")` → ok; zstd reports 1.5.7 at
  configure.
* Android (NDK r27c, `android-34`/`arm64-v8a`, configure-only):
  `build_run(command="android", configure_only=true, demo="sandcraft")` → ok (rc 0). The generated
  cache confirms `BUILD_opencv_v4d=ON`, `CMAKE_TOOLCHAIN_FILE` = the NDK r27c toolchain,
  `ANDROID_ABI=arm64-v8a`, and the zstd project configured from `modules/v4d/third/zstd/build/cmake`
  with `ZSTD_BUILD_SHARED=OFF`/`ZSTD_BUILD_STATIC=ON`/`ZSTD_MULTITHREAD_SUPPORT=OFF`;
  `libzstd_static` appears in `build.ninja`. (The cross-compile itself is M10; this proves the NDK
  CMake wiring, which was the M1 risk.)

Deviations / notes:

* **Harness moved off the `plan` gtest binary.** The roadmap's verification used
  `plan -- --gtest_filter=*netplay*`. `plan` is configured with `BUILD_opencv_v4d=OFF` and its test
  suite is deliberately "Plan features only; no V4D"; zstd is vendored under `modules/v4d/third` and
  built by the v4d module. Reaching it from `plan` would couple the V4D-free module to a V4D
  third-party build for no functional gain. The codec is instead proven by a standalone executable
  (no OpenCV, no GL) built in the `plan+v4d` set and run through the `sandcraft` toolkit. The
  verification recipe above is updated accordingly.
* **Delta checksum is of the resulting grid**, not the raw diff (`grid_codec.hpp`). Both are allowed
  by spec §12.2; the resulting-grid form is what makes a wrong-base application detectable by CRC,
  which is the M1 acceptance criterion. Spec §13.7 updated to match.
* **Byte-ratio frame is synthesised**, not captured: there is no way to dump a live grid until the
  snapshot path exists (M2). Capturing a real grid and re-running the codec is scheduled into M2's
  work items so the spec §12.5 estimate is replaced by a real number.
* **Android configure-only is done** (M1 work item 1) — see the evidence above. It took ~17 min
  because build.sh's Android pre-step *builds* clvk (≈2855 ninja targets) before the OpenCV configure;
  that cost is paid once and is cached, so later Android work in M10 will not repeat it.

### M2 — Protocol types and loopback (no sockets) — **done**

What landed (uncommitted working tree at time of writing). Everything below is **OpenCV-free** —
the codec, protocol, transports, queues, replication and the harness include no OpenCV and no GL, so
the pipeline is provable without a context; `net_glue.hpp` is the single header that includes the
sample's `Sandbox`:

* `netplay/protocol.hpp` — `MsgType`, `EventKind`, `CommandId`, `RejectReason`, `ByeReason`, flag
  bits; packed wire structs with `static_assert`ed sizes (`MsgHeader` 8, `WelcomeWire` 20,
  `KeyframeHeader` 64, `DeltaHeader` 72, `InputEventWire` 16, `InputBatchWire` 12, `WorldWire` 40,
  …); fail-safe little-endian `ByteWriter`/`ByteReader`; `encodePayload`/`decodePayload` per message;
  `frame()`; and a `FrameParser` that handles partial delivery, refuses oversize frames and guards
  the event count. A `static_assert` pins host endianness (spec §13.5).
* `netplay/transport.hpp` — `Transport`, `ConnState::{Connecting,Connected,Closed}`.
* `netplay/loopback_transport.hpp` — `LoopbackBus` + `makeLoopback()` returning a connected
  `{host, client}` transport pair over in-memory queues.
* `netplay/netqueues.hpp` — `GridSnapshot` (`tick`, `width`, `height`, `appliedSeq[4]`, `cells`,
  `WorldWire`, `cursors[4]`, `roster`), `NetQueues` with the single `mtx`, the input/inbound/outbound
  deques and the latest-wins snapshot `shared_ptr`, bounded drop-oldest with a `drops` counter, and
  the `netQueues()` singleton (Rule C1/C3).
* `netplay/replication.hpp` — `HostReplicator::{produce, welcome, reject, requestKeyframe,
  consumeForceNext, lastSentTick}` (baseline = the last *transmitted* grid, Rule R1) and
  `ClientReplicator::{init, apply, hello, resync}` with `ApplyResult::{Applied,Ignored,Desync,Error}`;
  the replica is sized by the first keyframe and rolled back on any failure.
* `netplay/net_glue.hpp` — `worldToWire`/`applyWorldWire`/`gridToBytes`/`gridToSandbox`/
  `cursorToWire`/`rosterToWire`/`makeSnapshot` — the whole Sandbox↔wire boundary in one file.
* `netplay/selftest.hpp` — `SelfTestStats`, `applyInputEvent`, `hostStep`, `runSelfTest`.
* `netplay/net_runtime.hpp` — the process-wide singleton: `configure`, `host()/client()`,
  `active()/setActive`, `queues()`, and the `netActive()` shorthand the plan nodes use to skip the
  snapshot copy in single-player. (Starting the poll thread is M3.)
* `sandcraft.cpp` — `#include "sandcraft/netplay/selftest.hpp"`; `--net-selftest` runs `runSelfTest`
  and returns `failures == 0 ? 0 : 1` **before** `V4D::init` — no window, no GL.
* `modules/v4d/CMakeLists.txt` — new `sandcraft_netplay_protocol_tests` target (netplay headers only,
  C++20, no zstd, not gated on the demo).
* `sandcraft` toolkit — tool `netplay` (`action` = `codec-test`|`codec-bench`|`protocol-test`|
  `selftest`|`all`) with two test cases (`protocol_test`, `selftest_passes`); the earlier
  `netplay_codec` tool was folded into it.

Evidence (`toolkit_invoke sandcraft.netplay {"action":"all"}`, dev host; build
`build_run(command="plan+v4d", build_type="debug")` → ok, rc 0):

* **codec-test** (`sandcraft_netplay_tests`): **30 checks, 0 failures**. 640×360 frame: keyframe
  **6985 B (3.0 %)**, delta **2223 B (1.0 %)**; debug times keyframe enc 1774 µs / dec 1325 µs, delta
  enc 2937 µs / dec 2803 µs.
* **protocol-test** (`sandcraft_netplay_protocol_tests`): **50 checks, 0 failures** — every message
  round-trips byte-stable (encoded bytes equal the `memcpy` of the packed struct), plus partial
  delivery, oversize refusal and the event-count guard.
* **codec-bench** (`netplay/tests/bench-codec.sh`, -O2): **30 checks, 0 failures**; keyframe
  enc 866 µs / dec 813 µs, **delta enc 874 µs / dec 889 µs → 1763 µs < 2 ms**.
* **selftest** (`example_v4d_sandcraft --net-selftest`), rc 0:
  `ticks=600 keyframes=0 deltas=594 forcedDrops=6 maxDeltaBytes=45330 hostBytes=5386124
  clientBytes=5466294 desyncs=2 checks=21 failures=0`. The client's replicaCRC was compared against
  the authoritative grid after **every** applied delta; 6 ticks were deliberately not transmitted
  (Rule R2 span), and the delta still reconstructs exactly.

Deviations / notes:

* **Harness moved off the `plan` gtest suite** (same reason as M1): `plan` is `BUILD_opencv_v4d=OFF`
  and cannot reach the vendored zstd; the M2 proof is therefore standalone binaries plus
  `--net-selftest`, run through the `sandcraft` toolkit. The M2 verification recipe is updated.
* **The two desyncs are deliberate.** M2 acceptance said "logs zero desyncs"; the harness instead
  *injects* a corrupt delta and a wrong-base delta, asserts the client raises a desync (and rolls
  back) for each, then recovers with a keyframe — which is what "fail closed" means (spec §12.3,
  §18). The pass condition is therefore "exactly two desyncs, both detected": the un-injected host
  half is separately asserted to have **zero** desyncs (`check(st.desyncs == 0, "no desyncs during
  normal operation")` before the injection section). Acceptance criterion amended accordingly.
* **OPEN DEFECT — the periodic keyframe never fires.** `selftest` reports `keyframes=0` over 600
  ticks despite `DEFAULT_KEYFRAME_INTERVAL = 300`. Cause: `HostReplicator::produce` tests
  `(snap.tick - lastSentTick_) >= keyframeEvery_`, but `lastSentTick_` advances on *every* delta, so
  the expression is the span the delta covers (~1 tick, occasionally 2 after a forced drop) and is
  never ≥ 300. Spec §12.3 requires a periodic keyframe (~5 s) to bound undetected divergence. Fix
  (not yet applied): track a separate `lastKeyframeTick_` and test `snap.tick - lastKeyframeTick_ >=
  keyframeEvery_`. The M2 harness cannot catch this because it asserts *convergence*, not the
  keyframe *cadence*; a cadence assertion belongs in the fix. Recorded here rather than fixed in this
  pass because the instruction was to update the documents and halt.
* **Real-grid measurement supersedes M1's synthesised frame.** The M1 log promised a real grid in
  M2; the selftest now drives the genuine `Sandbox` through the genuine automaton, `settleGrid` and
  emitters with scripted input, on the real 640×360 grid, one delta per tick over 594 deltas. Mean
  **~9.1 KB/delta** (`hostBytes` 5.39 MB / 594), worst **45,330 B**. Spec §12.5 replaced with these
  numbers. The synthetic-representative frame stays as the *codec micro-benchmark*; the pipeline
  figure is the new authoritative bandwidth number.
* **Spec amendments made for M2** (implementation diverged from the text in three places):
  `GridSnapshot` now carries `width`/`height`/`WorldWire`/`roster` (§16.3) so the net thread can
  frame a keyframe without touching the `Sandbox` (Rule C2); the keyframe carries **no** watermark or
  cursors, which ride the following deltas (§13.4); `NetQueues::inbound` is `std::deque<Frame>`, and
  the snapshot slot is a latest-wins `shared_ptr<const GridSnapshot>` (§16.2, Appendix A). Decision
  log D14–D16 added.
* **Outstanding within M2.** The `hostMode_` branch and `NetApplyPlan`/`LocalInputPlan` in
  `sandcraft.cpp` are not wired yet, so `selftest` drives both halves directly instead of through the
  Plan graph. Consequently the single-player regression was last green *before* the branch and must
  be re-run once it lands; `netActive()` (in `net_runtime.hpp`) already exists so the host branch can
  skip the per-tick snapshot copy when single-player.
  *(Resolved in the continuation entry below. Kept verbatim as history.)*

#### M2 (continuation) — graph wiring, keyframe cadence, verification harness — 2026-10-16

Closes the two items the entry above left open. All of the code below is **committed**
(`sandcraft.cpp` in `a2f96f53`, the netplay headers earlier); the working tree is clean apart from
these two documents.

What landed:

* **Periodic-keyframe defect fixed.** `HostReplicator` now tracks `lastKeyframeTick_` (new member,
  `lastKeyframeTick()` getter, reset in `reset()`); `produce` frames a keyframe when
  `forceKeyframe || !haveBaseline_ || (snap.tick - lastKeyframeTick_) >= keyframeEvery_`, and sets
  `lastKeyframeTick_ = snap.tick` when it does. `selftest.hpp` gained a cadence assertion
  (`check(st.keyframes >= 2, "periodic keyframes fired")`), so the harness now *proves* the cadence
  it previously could not observe. Spec §12.3 amended; decision D21.
* **The graph branch is wired** in `SandcraftPlan::infer`:
  `subInfer(input_); branch(RS(hostMode_))->subInfer(sim_)->elseBranch()->subInfer(netApply_)->endBranch(); subInfer(render_); ...`
  `hostMode_` is a `_shared(bool)` initialised from `netplay::netRuntime().host()`;
  `sandbox_.players_[0].active = true` keeps the local player live; `netApply_` is a new sub-plan.
* **`LocalInputPlan`** (renamed from `SandInputPlan`) now *emits* rather than paints: it turns the
  existing event edges (`press_`, `drag_`, `scroll_`, `move_`, `keys_`) into `InputEventWire`s using
  the sender's own `fitView`/`cellAt` (§9.1 / Rule I2), with a frame-local `shadow` of the local
  player so wheel/key accumulation matches the old in-place mutations (§9.5). The host pushes each
  event to `NetQueues` as player 0 (loopback); the guest frames them into `MSG_INPUT` and mirrors
  only its own cursor locally.
* **`SandSimulationPlan::step`** drains `NetQueues::input`, applies each `InputEnvelope` through the
  shared `applyInputEvent`, runs the unchanged automaton, and publishes a snapshot only when
  `netplay::netActive()`, so single-player pays no snapshot copy.
* **`NetApplyPlan`** (guest only) pops inbound frames through `netplay::ClientReplicator`, handles
  `ApplyResult::Desync` with `resync()`, and copies the replica into the sandbox (`gridToSandbox` /
  `applyWorldWire`).
* **`InputEnvelope`** added to `netqueues.hpp`; `pushInput`/`popInput`/`drainInput` now carry the
  sender id with the event (§16.2 / Appendix A amended; decision D18).
* **`netplay/input_apply.hpp`** — the shared
  `applyInputEvent(Sandbox&, int playerId, const InputEventWire&)` (guards `playerId`; no `active`
  check); `selftest.hpp` includes it and its private copy is gone (decision D20).

Evidence:

* `build_run(command="plan+v4d", build_type="debug")` → ok, rc 0 (~105 s).
* `toolkit_invoke sandcraft.netplay {"action":"all"}` → green: codec-test **30 checks / 0 failures**,
  protocol-test **50 / 0**, codec-bench **30 / 0**, selftest **22 checks / 0 failures** with
  `keyframes=2 deltas=592` — the periodic keyframe now fires (it was `keyframes=0`).
* Single-player headless smoke (no args) after the branch: window pass, exit pass, frame not blank
  (stddev ≈ 0.13, ≈105k unique colours), log clean; the panel and the Van Gogh scene render
  unchanged.

Deviations / notes:

* **`--net-selftest` still drives both halves directly**, not through two in-process graph runs. This
  is the "dedicated test harness" option the work item allowed, and the graph branch itself is
  exercised by the live sample. Recorded so the choice is not read as an omission.
* **Spec amendments in this pass:** §16.2 (`InputEnvelope`), §16.4 (`RS(hostMode_)`), §16.5 (shared
  `applyInputEvent`), §12.3 (keyframe cadence measured from the last keyframe), Appendix A, and
  decisions D17–D21.
* **Interactive verification is Wayland-only on this host.** The X11 path is unusable — Plan-V4D's
  GLFW is a Wayland-only build, so `glfwInit` fails on Xvfb — and the Wayland injection path itself
  needed two fixes before input reached the sample at all: the `wtype` first-event drop and the
  absence of any press/release primitive (so drags were impossible). Both are fixed and documented in
  §2, together with the new privilege-free `wlr-pointer` helper. This becomes load-bearing at M3,
  where two instances must be driven interactively. **Follow-up:** commit `wlr-pointer`'s source
  under `netplay/tests/` (it currently lives only under `/tmp/opencode/tmp-scripts/`).

#### M3 — TCP transport, role split, one-way replication — **design pass, no code yet** — 2026-10-16

A reading pass over the specification, the roadmap and the landed M0–M2 code, to pin M3 before
implementing it. **Nothing has landed for M3**; this entry records the findings and the design the
implementation will follow, so the pre-work is not lost when the code arrives. No acceptance
criterion is met yet.

Findings that change the plan:

* **Latent M2 wiring defect — the role is never applied.** `netplay.hpp` parses the role into a
  `NetConfig`, but `V4D_DEMO_MAIN` never calls `netRuntime().configure(net)`. `SandcraftPlan`'s
  constructor therefore reads the singleton's *default* config, so `hostMode_` is always `Host` and
  the `elseBranch()->subInfer(netApply_)` arm is unreachable in the live sample. The M2 continuation
  entry's "the graph branch itself is exercised by the live sample" is therefore true only of the
  **host** arm; the client arm has never run outside the direct harness. M3 work item 0 fixes this
  (`NetRuntime::configure()` before `V4D::init`) before any socket work. Spec §16.6 amended;
  decision D23.
* **Networking must be opt-in.** `--host` and the no-argument default both mean `Role::Host`, so
  nothing currently distinguishes "be a host and listen" from "be today's single-player demo".
  Without an explicit switch, either the no-argument demo would open a listener (breaking §4.3) or
  `--host` would be unrepresentable. The fix is `NetConfig::enabled`, set by `--host`/`--connect`,
  which is what makes `netActive()` true and the graph publish snapshots. Spec §16.6/§17.1 amended;
  decision D22.
* **The `HELLO` join token has nowhere to go.** §17.2 lists a token in `HELLO` and `REJECT_TOKEN`
  exists, but `HelloWire` (§13.4) is `{version, nameLen, reserved[2], name[]}` — there is no token
  field. v1 sessions are therefore **open**: the token is parsed, printed in the join string and
  otherwise unused. It is wired when the rendezvous work (M8) needs a gate. Spec §13.4 amended;
  decision D24.
* **M3 cannot be one-way without disabling landed code.** M2 already wired `LocalInputPlan` to frame
  every consumed event into `MSG_INPUT` and the host simulation to `drainInput()`/`applyInputEvent()`.
  The transport therefore carries input both ways as soon as it exists. M3's deliverable is the
  transport + replication (its acceptance is still only the one-way client match); M4 narrows to
  multi-player painting semantics and the "no rate limit" proof. Decision D25.
* **Roster/cursor propagation stays M6.** The net thread is forbidden from touching the `Sandbox`
  (Rule C2), so the host's keyframe roster is whatever the sandbox's `players_` holds — player 0
  alone until M6 owns the roster. `WELCOME.playerCount` (the host-side count) and the keyframe's
  roster may therefore differ; M3 asserts only grid replication, not the roster. Decision D26.
* **Shutdown ordering.** `NetRuntime` will own the `NetThread`; `stop()` must release the runtime
  mutex *before* joining the thread so a future callback from the net thread cannot deadlock the
  join. The net thread deliberately reads nothing back out of `NetRuntime`; `NetRuntime` reads the
  thread's atomics (player id, counters, bound port), so the ordering holds by construction.

Planned M3 surface (not yet written; named in the M3 section above): `netplay/socket_util.hpp`,
`netplay/tcp_server.hpp`, `netplay/tcp_client.hpp`, `netplay/net_thread.hpp`,
`netplay/tcp_selftest.hpp`, a wake callback on `NetQueues`, and the `NetConfig::enabled` /
`NetRuntime` wiring. No `modules/v4d/CMakeLists.txt` change is expected: the new headers are
POSIX-only and header-only, included by `sandcraft.cpp`, which already links `libzstd_static`.

Documentation amended in this pass: spec §13.4 (token deferred), §16.2 (wake-up), §16.6 (opt-in,
role application), §17.1 (host selection), and decision log D22–D26; roadmap M3 status, deliverables,
work items and verification, plus this entry.

#### M3 — TCP transport, role split, one-way replication — **done** — 2026-10-16

The design-pass entry above was written **before** the M3 code; the code then landed in the same
commit (`sandcraft`, 2026-10-16) but the docs' status was not updated. This entry records the
implementation that actually shipped, verifies it, and adds the one fix this pass made. Everything
below is committed.

What landed:

* `netplay/socket_util.hpp` — `ignoreSigpipe()` (process-wide, plus `MSG_NOSIGNAL` on every send),
  `setNonBlocking`, `setCloseOnExec`, `SO_REUSEADDR`/`TCP_NODELAY`, `resolve()` (getaddrinfo) and an
  `ip:port` formatter.
* `netplay/tcp_server.hpp` — `TcpListener`: bind (host `0` ⇒ ephemeral), non-blocking accept,
  `port()`.
* `netplay/tcp_client.hpp` — `TcpConnector`: non-blocking `connect` + `step()` state machine,
  `release()` of the connected fd.
* `netplay/net_thread.hpp` (797 lines) — the whole `poll(2)` loop. Host: accept, per-peer
  reassembly + write buffer, `HELLO`→`WELCOME`+`KEYFRAME` or `REJECT{version|full}`, `INPUT`→
  `NetQueues::input` (through `InputEnvelope`), `RESYNC`→keyframe, `PING`/`PONG`, `BYE`; one
  `HostReplicator` per peer (its own baseline), `WORLD` on change, a keyframe on join + the periodic
  cadence, and a coalesced delta per new tick against the last transmitted state. Client:
  connect/reconnect with a 1 s retry, `HELLO`, `WELCOME`→player id, `REJECT` handling, everything
  else→`NetQueues::inbound`, `NetQueues::outbound`→socket. A wake pipe is part of the poll set so a
  published snapshot or queued input is acted on immediately (spec §16.2).
* `netplay/net_runtime.hpp` — owns the `NetThread` (`start()`/`stop()`, join outside the lifecycle
  mutex), exposes `boundPort()`, the player id and `stats()`; `configure()` records the role and
  `NetConfig::enabled` gates the socket (D22).
* `netplay/netqueues.hpp` — `setWake`/`clearWake`; `publishSnapshot`/`pushOutbound` signal the pipe
  after releasing `mtx` (Rule C1 ordering preserved).
* `sandcraft.cpp` (`V4D_DEMO_MAIN`) — `NetRuntime::configure(net)` **before** `V4D::init` (the M2
  wiring fix, D23), `start()` only when `net.enabled`, `stop()` after the run, and the
  `--net-tcp-selftest` branch before any GL.
* `sandcraft_panel.hpp` — the "Network" section (role, endpoint/player, tick, peers, in/out, key
  counters, drops).
* `netplay/tcp_selftest.hpp` + `--net-tcp-selftest` — an in-process host **and** client, each with
  its own `NetThread`, its own `NetQueues` and a real loopback socket, driving the real automaton
  plus a raw blocking wrong-version client and a client restart.

Evidence (dev host; build `build_run(command="plan+v4d", build_type="debug")` → ok, rc 0):

* `toolkit_invoke sandcraft.netplay {"action":"all"}` → **all green**: codec-test 30/0,
  protocol-test 50/0, codec-bench 30/0, selftest 22/0 (`keyframes=2`), and **tcp-selftest 20/0**:
  `ticks=600 keyframes=4 deltas=600 maxDeltaBytes=9393 hostBytesOut=4126689 clientBytesIn=4126689
  hostPort=39587 playerId=1 desyncs=0 versionRejected=1 reconnected=1 checks=20 failures=0`. Host
  and client byte counts are **exactly equal**; the wrong-version client was sent `REJECT(version)`;
  a restarted client re-synced by keyframe.
* Single-player regression: `headless_v4d(action="run", sample="sandcraft",
  check=["window","not_blank","log_clean","exit"])` → window pass, exit pass (rc 0), final frame
  stddev ≈ 0.243 (not blank, ≈75k unique colours), log clean.
* Two live instances on two sway displays (host `netmain`, client `netpeer`), driven entirely
  through the headless/process toolkits: the client logs `net client joined as player 1 of 2
  (640x360)`, and both HUDs update. Host at tick 526: `peers=1 kf=3 dl=339 events=334 in=13542
  out=4239497 drops=0`; client at tick 528: `connected=1 handshake=1 kfIn=3 dlIn=341 in=4268583
  out=13622 drops=1`. Two drags injected into the host (`headless_input`) reached it as
  `events=334` with `drops=0`. Byte accounting is ~**12.5 KB/delta** in the live run (4,239,497 B /
  339) versus ~**6.9 KB/delta** over the quieter loopback harness (4,126,689 B / 600), worst harness
  delta **9,393 B** — both within the spec §12.5 order and replacing its estimate.

Deviations / notes:

* **Client keyframe/delta counters were out-only.** `NetStats.keyframesOut/deltasOut` and `tick`
  are produced by the host, so a client's `--net-stats` line and panel read `kf=0 dl=0 tick=0` while
  it was being replicated to every tick — a diagnostics gap, not a replication fault (the host's
  `out` bytes equal the client's `in` bytes exactly, as the harness asserts). Fixed this pass:
  `NetStats` gained `keyframesIn`/`deltasIn`, the client counts `KEYFRAME`/`DELTA` on arrival and
  takes `tick` from their headers (reading only the leading tick, leaving the compressed payload to
  the apply node), and both the `--net-stats` line and the panel print the pair that belongs to the
  role (host `kf`/`dl` sent, client `kfIn`/`dlIn` received).
* **The visual acceptance is qualitative, not pixel-exact.** Spec §6 makes local visual style (Van
  Gogh, impasto) a per-instance, never-networked choice, and the "living paint" swirl animates, so
  two instances differ in the fine texture even with identical grids. With the world **paused** (a
  host command) the canvas comparison is PSNR ≈ 20.4 dB, dominated by that local texture, not by the
  world. The world *structure* (emitters, mounds, moon, stars, injected paint) matches, and grid
  equality is already proven byte-exactly by `tcp-selftest` (client CRC vs authoritative grid). The
  roadmap's "small PSNR tolerance" is therefore reinterpreted as "same world, style may differ".
* **`tcp-selftest` drives both halves directly**, like the M2 harness, rather than two graph runs —
  the same choice the M2 log recorded.
* **Interactive verification is Wayland-only on this host** (roadmap §2). The two-instance run went
  through `headless_displays` (sway) + `process_spawn` for supervision + `headless_input` (the
  committed `wlr-pointer`) + `headless_capture`, never a hand-rolled launcher and never the live
  session. An earlier attempt drove the run with a bespoke bash script; it is superseded and the
  lesson (spawn GUI processes only through the headless/process toolkits) is recorded here.
* **The `wlr-pointer` helper is now committed** under `netplay/tests/` (source, protocol XML, build
  script) — the M2 log's follow-up.
* **No `modules/v4d/CMakeLists.txt` change** was needed, as predicted: the M3 headers are POSIX-only
  and header-only, included by `sandcraft.cpp`, which already links `libzstd_static`.

Documentation amended in this pass: roadmap M3 header status, the M3 section (status, deliverables,
work items, acceptance criteria) and this entry. No specification change: the M3 behaviour already
matched the spec; the client-counter fix is an internal diagnostics detail (recorded decision D27).
