// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// NetQueues -- the only thing the network thread and the Plan graph may share
// (specification.md §16.2, §16.3).
//
// The contract (spec Rule C1/C2/C3) is narrow on purpose:
//   * every field is reached through an accessor that takes `mtx`, so the net
//     thread (which holds only `mtx`) and a Plan node (which holds the V4D
//     per-variable mutex and *then* `mtx`) can never deadlock: the ordering is
//     fixed to V4D-mutex -> queues mtx and must not be inverted;
//   * the net thread never sees a Sandbox, only bytes and the plain value
//     structs below, so a foreign writer can never race the automaton;
//   * the queues are bounded and drop the *oldest* item on overflow, counting
//     the drop. This is a memory-safety valve and never a rate limiter (spec
//     Rule B1): under normal operation `drops` stays zero.
//
// The types here are the wire types only (protocol.hpp), so the whole subsystem
// can be exercised without OpenCV or a GL context -- the Sandbox lives one layer
// up, in the sample's plan nodes.
#ifndef MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_NETQUEUES_HPP_
#define MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_NETQUEUES_HPP_

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "protocol.hpp"

namespace sandcraft {
namespace netplay {

// The host's simulation -> network hand-off (spec §16.3). A single latest-wins
// slot, not a queue: the net thread always wants the freshest state, and a
// skipped tick is corrected by taking the next delta against the last state
// actually *transmitted* (spec Rule R1/R2), not against this slot.
struct GridSnapshot {
  uint64_t tick = 0;
  uint16_t width = 0, height = 0;        // the session's fixed dimensions
  uint32_t appliedSeq[4] = {0, 0, 0, 0}; // host watermark, folded into this frame
  std::vector<uint8_t> cells;            // the settled grid, W*H bytes
  WorldWire world{};                     // authoritative world state
  PlayerCursorWire cursors[4]{};         // every player's hover cell + colour
  std::vector<PlayerWire> roster;        // active players, for KEYFRAME
};

// Bounds for the safety valve (spec §18.3). Defaults are the ones the spec
// names; they are template-free constants so a test can reason about them.
inline constexpr size_t INPUT_CAP = 65536;
inline constexpr size_t OUTBOUND_CAP = 8192;
inline constexpr size_t INBOUND_CAP = 8;

// A queued input event together with the player it came from. The host must
// apply a peer's event to *that* player's state (spec Rule A2), and the events
// arrive over one shared queue, so the sender id has to travel with the event.
// (The spec sketched `input` as a bare `InputEventWire[]`; that cannot demux
// players, so it is amended to carry the envelope -- see the M2 log.)
struct InputEnvelope {
  uint8_t player = 0; // 0..3, the session player id
  InputEventWire event{};
};

struct NetQueues {
  std::mutex mtx;

  // host: network + loopback -> simulation
  std::deque<InputEnvelope> input;
  // client: network -> apply node
  std::deque<Frame> inbound;
  // both: input/producer node -> network thread
  std::deque<std::vector<uint8_t>> outbound;
  // host: simulation -> network thread (latest wins)
  std::shared_ptr<const GridSnapshot> snapshot;

  // Dropped-item counter. Atomic so it can be read (HUD, logs) without `mtx`.
  std::atomic<uint64_t> drops{0};

  // --- host input path -------------------------------------------------------
  void pushInput(const InputEnvelope &e) {
    std::lock_guard<std::mutex> lk(mtx);
    if (input.size() >= INPUT_CAP) {
      input.pop_front();
      drops.fetch_add(1, std::memory_order_relaxed);
    }
    input.push_back(e);
  }

  bool popInput(InputEnvelope &out) {
    std::lock_guard<std::mutex> lk(mtx);
    if (input.empty())
      return false;
    out = input.front();
    input.pop_front();
    return true;
  }

  // Drain everything queued into `out` (host tick: consume the whole batch at
  // once, in arrival order, per spec §11.1).
  void drainInput(std::vector<InputEnvelope> &out) {
    std::lock_guard<std::mutex> lk(mtx);
    out.insert(out.end(), input.begin(), input.end());
    input.clear();
  }

  // --- client inbound path ---------------------------------------------------
  void pushInbound(const Frame &f) {
    std::lock_guard<std::mutex> lk(mtx);
    // A keyframe MUST be applied in order, so it is never dropped here; only
    // non-keyframe frames are shed when the apply node lags (spec §18.3).
    if (inbound.size() >= INBOUND_CAP) {
      // Drop the oldest frame that is not a keyframe; if the queue is all
      // keyframes (pathological), drop the oldest anyway.
      auto victim = inbound.begin();
      for (auto it = inbound.begin(); it != inbound.end(); ++it) {
        if (it->type != MSG_KEYFRAME) {
          victim = it;
          break;
        }
      }
      inbound.erase(victim);
      drops.fetch_add(1, std::memory_order_relaxed);
    }
    inbound.push_back(f);
  }

  bool popInbound(Frame &out) {
    std::lock_guard<std::mutex> lk(mtx);
    if (inbound.empty())
      return false;
    out = std::move(inbound.front());
    inbound.pop_front();
    return true;
  }

  // --- outbound path ---------------------------------------------------------
  void pushOutbound(std::vector<uint8_t> bytes) {
    std::lock_guard<std::mutex> lk(mtx);
    if (outbound.size() >= OUTBOUND_CAP) {
      outbound.pop_front();
      drops.fetch_add(1, std::memory_order_relaxed);
    }
    outbound.push_back(std::move(bytes));
  }

  bool popOutbound(std::vector<uint8_t> &out) {
    std::lock_guard<std::mutex> lk(mtx);
    if (outbound.empty())
      return false;
    out = std::move(outbound.front());
    outbound.pop_front();
    return true;
  }

  // --- snapshot slot ---------------------------------------------------------
  void publishSnapshot(std::shared_ptr<const GridSnapshot> snap) {
    std::lock_guard<std::mutex> lk(mtx);
    snapshot = std::move(snap);
  }

  std::shared_ptr<const GridSnapshot> takeSnapshot() {
    std::lock_guard<std::mutex> lk(mtx);
    return snapshot;
  }

  // For diagnostics/tests. Locks mtx (so they cannot be const: the mutex is
  // the object's, and a const method may not take it).
  size_t inputSize() {
    std::lock_guard<std::mutex> lk(mtx);
    return input.size();
  }
  size_t inboundSize() {
    std::lock_guard<std::mutex> lk(mtx);
    return inbound.size();
  }
  size_t outboundSize() {
    std::lock_guard<std::mutex> lk(mtx);
    return outbound.size();
  }
};

// One per process (spec Rule C3): the plan is built once per worker *and* once
// for the display thread, and the queues must be the same object for all of them
// -- and for the net thread. A function-local static is the thread-safe way to
// say that (C++11 magic statics).
inline NetQueues &netQueues() {
  static NetQueues q;
  return q;
}

} // namespace netplay
} // namespace sandcraft

#endif // MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_NETQUEUES_HPP_
