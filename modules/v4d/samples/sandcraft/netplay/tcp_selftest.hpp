// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The `--net-tcp-selftest` harness (roadmap M3): a host and a client, each with
// its own real network thread and its own NetQueues, talking over **real
// loopback TCP**, driving the real automaton.
//
// This is the milestone's proof and it is deliberately the same shape as the
// M2 loopback harness (selftest.hpp) with the in-memory bus replaced by a
// listening socket. What it adds over M2 is everything the socket brings:
//
//   * framing across a real byte stream (TCP coalesces and splits at will),
//   * the HELLO/WELCOME/KEYFRAME handshake as an actual exchange of frames
//     between two threads, not two function calls,
//   * a version-mismatched peer actually being sent a REJECT over the wire,
//   * a client that dies and a new one that joins and re-syncs by keyframe.
//
// The host side of the loop is a stand-in for SandSimulationPlan: it drains
// NetQueues::input, applies each envelope through the shared applyInputEvent,
// steps the automaton and publishes a snapshot. The client side is a stand-in
// for NetApplyPlan: it pops NetQueues::inbound, applies each frame through a
// ClientReplicator and copies the replica into a Sandbox. The two halves are in
// one process but on two sockets and two threads, so a bug that only appears
// when the protocol is spoken -- a short read, a replayed seq, a keyframe split
// across two segments -- has somewhere to show up.
#ifndef MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_TCP_SELFTEST_HPP_
#define MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_TCP_SELFTEST_HPP_

#include <chrono>
#include <cstdint>
#include <ostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../sandcraft_automaton.hpp"
#include "net_glue.hpp"
#include "net_thread.hpp"
#include "netplay.hpp"
#include "protocol.hpp"
#include "replication.hpp"
#include "selftest.hpp" // hostStep() and seedScene(), shared with M2
#include "socket_util.hpp"

namespace sandcraft {
namespace netplay {

struct TcpSelfTestStats {
  int checks = 0;
  int failures = 0;
  uint64_t ticks = 0;
  size_t keyframes = 0;
  size_t deltas = 0;
  uint64_t hostBytesOut = 0;
  uint64_t clientBytesIn = 0;
  uint32_t maxDeltaBytes = 0;
  uint32_t clientDesyncs = 0;
  uint16_t hostPort = 0;
  uint8_t clientPlayerId = 0;
  bool versionRejected = false;
  bool reconnected = false;
};

namespace tcpselftest_detail {

using Clock = std::chrono::steady_clock;

inline bool waitFor(const Clock::time_point &deadline, const std::function<bool()> &pred) {
  while (Clock::now() < deadline) {
    if (pred())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}

// Apply every frame the net thread has queued for the client. Mirrors
// NetApplyPlan::apply. Records what it saw and asks for a RESYNC on a desync.
inline void pumpClient(NetQueues &cq, ClientReplicator &rep, Sandbox &clientSb,
                       TcpSelfTestStats &st) {
  Frame f;
  while (cq.popInbound(f)) {
    if (f.oversize || !f.valid)
      continue;
    if (f.type == MSG_KEYFRAME)
      ++st.keyframes;
    else if (f.type == MSG_DELTA) {
      ++st.deltas;
      st.maxDeltaBytes = std::max(st.maxDeltaBytes, uint32_t(f.payload.size()));
    }
    std::vector<std::vector<uint8_t>> replies;
    const ApplyResult r = rep.apply(f, &replies);
    if (r == ApplyResult::Desync) {
      ++st.clientDesyncs;
      replies.push_back(rep.resync());
    }
    for (std::vector<uint8_t> &reply : replies)
      cq.pushOutbound(std::move(reply));
  }
  if (rep.haveState())
    gridToSandbox(rep.grid(), clientSb);
}

// A raw blocking connect, used only by the version-rejection check (which must
// speak a deliberately wrong protocol, so it cannot use NetThread).
inline int connectBlocking(const std::string &host, uint16_t port, std::string &err) {
  struct addrinfo *res = sockutil::resolve(host, port, /*passive=*/false, err);
  if (!res)
    return -1;
  int fd = -1;
  for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    const int s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s < 0)
      continue;
    if (::connect(s, ai->ai_addr, ai->ai_addrlen) == 0) {
      fd = s;
      break;
    }
    ::close(s);
  }
  ::freeaddrinfo(res);
  if (fd < 0)
    err = "connect failed";
  return fd;
}

// Read one frame, waiting at most `timeoutMs` in total.
inline bool readFrameBlocking(int fd, Frame &out, int timeoutMs) {
  FrameParser parser;
  const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
  while (Clock::now() < deadline) {
    struct pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    const int r = ::poll(&p, 1, 20);
    if (r > 0 && (p.revents & POLLIN)) {
      uint8_t tmp[4096];
      const ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
      if (n <= 0)
        return false;
      parser.push(tmp, size_t(n));
      if (parser.next(out))
        return true;
    }
  }
  return false;
}

} // namespace tcpselftest_detail

// Run the whole harness. Returns stats; the caller turns failures into the exit
// code. `log` receives the summary line, exactly like runSelfTest().
inline TcpSelfTestStats runTcpSelfTest(NetConfig cfg, std::ostream &log) {
  using namespace tcpselftest_detail;

  TcpSelfTestStats st;
  auto check = [&](bool cond, const char *what) {
    ++st.checks;
    if (!cond) {
      ++st.failures;
      log << "net-tcp-selftest: FAIL " << what << "\n";
    }
  };

  Sandbox hostSb;
  hostSb.allocate(640, 360);
  selftest_detail::seedScene(hostSb);
  hostSb.players_[0].active = true;
  hostSb.players_[1].active = true; // the guest, player 1
  hostSb.players_[1].colorIdx = 1;

  NetQueues hq; // the host's queues
  NetQueues cq; // the client's queues

  // --- host: bind an ephemeral loopback port ---------------------------------
  NetConfig hcfg;
  hcfg.role = Role::Host;
  hcfg.enabled = true;
  hcfg.port = 0; // ephemeral
  hcfg.name = "host";
  std::string err;
  NetThread hostNet;
  check(hostNet.start(hcfg, &hq, err), "host net thread started");
  if (st.failures) {
    log << "net-tcp-selftest: host start failed: " << err << "\n";
    return st;
  }
  st.hostPort = hostNet.boundPort();
  check(st.hostPort != 0, "host bound an ephemeral port");

  // --- client: dial it -------------------------------------------------------
  NetConfig ccfg;
  ccfg.role = Role::Client;
  ccfg.enabled = true;
  ccfg.connect = "127.0.0.1";
  ccfg.port = st.hostPort;
  ccfg.name = "guest";
  NetThread clientNet;
  check(clientNet.start(ccfg, &cq, err), "client net thread started");
  if (st.failures) {
    log << "net-tcp-selftest: client start failed: " << err << "\n";
    return st;
  }

  ClientReplicator clientRep;
  Sandbox clientSb;
  clientSb.allocate(640, 360);

  // Publish a first snapshot so the host can welcome as soon as HELLO lands.
  hq.publishSnapshot(std::make_shared<GridSnapshot>(makeSnapshot(hostSb, 1)));
  const bool joined = waitFor(Clock::now() + std::chrono::seconds(10), [&] {
    hq.publishSnapshot(std::make_shared<GridSnapshot>(makeSnapshot(hostSb, 1)));
    return clientNet.handshakeDone();
  });
  check(joined, "client completed the handshake over TCP");
  st.clientPlayerId = clientNet.playerId();

  // --- 600 ticks of scripted play (~10 s at 60 Hz) ---------------------------
  std::mt19937 rng(20240607);
  auto cellX = [&] { return int(rng() % unsigned(hostSb.width_)); };
  auto cellY = [&] { return int(rng() % unsigned(hostSb.height_ - 8)); };
  int16_t paintX = -1, paintY = -1;

  const uint64_t TICKS = 600;
  uint64_t lastTick = 1;
  for (uint64_t i = 0; i < TICKS; ++i) {
    const uint64_t tick = 2 + i;
    // --- the client frames its input and hands it to its net thread ---
    std::vector<InputEventWire> events;
    auto push = [&](EventKind k, int x, int y, uint8_t brush, uint8_t pen) {
      InputEventWire e{};
      e.kind = uint8_t(k);
      e.x = int16_t(x);
      e.y = int16_t(y);
      e.brush = brush;
      e.penSize = pen;
      e.seq = uint32_t(events.size() + 1);
      events.push_back(e);
    };
    const int mx = cellX(), my = cellY();
    push(EV_MOVE, mx, my, 0, 0);
    if (paintX < 0 && (tick % 5 == 0)) {
      paintX = int16_t(mx);
      paintY = int16_t(my);
      push(EV_PRESS, mx, my, uint8_t(2 + rng() % 6), uint8_t(1 + rng() % 6));
    } else if (paintX >= 0) {
      push(EV_DRAG, mx, my, uint8_t(2 + rng() % 6), uint8_t(1 + rng() % 6));
      if (tick % 7 == 0) {
        push(EV_RELEASE, mx, my, 0, 0);
        paintX = paintY = -1;
      }
    }
    InputBatchMsg batch;
    batch.b.batchTimeMs = nowMillis();
    batch.b.firstSeq = events.front().seq;
    batch.b.count = uint16_t(events.size());
    batch.events = events;
    cq.pushOutbound(frame(MSG_INPUT, uint32_t(tick), encodePayload(batch)));

    // --- the host drains input, steps, publishes ---
    std::vector<InputEnvelope> in;
    hq.drainInput(in);
    for (const InputEnvelope &env : in)
      applyInputEvent(hostSb, env.player, env.event);
    hostStep(hostSb);
    hq.publishSnapshot(std::make_shared<GridSnapshot>(makeSnapshot(hostSb, tick)));
    lastTick = tick;

    // --- let the two net threads move bytes, then apply on the client ---
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    pumpClient(cq, clientRep, clientSb, st);
  }
  st.ticks = TICKS;

  // Settle. The transport is asynchronous, and the host coalesces snapshots to
  // the latest (Rule R2), so the client is *not* required to be caught up on
  // any particular tick -- only to reach the authoritative grid once the stream
  // drains. Publishing fresh ticks (the grid is unchanged, so the deltas are
  // tiny) keeps the host producing while the replica catches up; convergence is
  // bounded so a stall cannot hang the test.
  bool converged = false;
  for (int i = 0; i < 500 && !converged; ++i) {
    hq.publishSnapshot(std::make_shared<GridSnapshot>(makeSnapshot(hostSb, ++lastTick)));
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    pumpClient(cq, clientRep, clientSb, st);
    if (clientRep.haveState() && crc32(clientRep.grid()) == crc32(gridToBytes(hostSb)))
      converged = true;
  }

  check(clientRep.haveState(), "client reached a replica over TCP");
  check(converged, "replica CRC equals authoritative grid after the run");
  check(st.keyframes >= 1, "at least one keyframe crossed the wire");
  check(st.deltas > 0, "deltas crossed the wire");
  check(st.clientDesyncs == 0, "no desyncs across the TCP run");
  {
    const NetStats hs = hostNet.stats();
    const NetStats cs = clientNet.stats();
    st.hostBytesOut = hs.bytesOut;
    st.clientBytesIn = cs.bytesIn;
    check(hs.peers == 1, "host reports one live peer");
    check(cs.inputEventsIn == 0, "client reports no inbound input (it is the guest)");
    check(hs.inputEventsIn > 0, "host applied input that arrived over TCP");
  }

  // --- a wrong-version client is rejected ------------------------------------
  {
    int fd = connectBlocking("127.0.0.1", st.hostPort, err);
    check(fd >= 0, "raw client connected for the version check");
    if (fd >= 0) {
      // A HELLO whose header version (and hello version) is wrong.
      HelloMsg hm;
      hm.h.version = uint8_t(PROTOCOL_VERSION + 1);
      hm.name = "old";
      hm.h.nameLen = 3;
      const std::vector<uint8_t> payload = encodePayload(hm);
      ByteWriter w;
      w.u32(uint32_t(sizeof(MsgHeader) + payload.size()));
      w.u8(MSG_HELLO);
      w.u8(uint8_t(PROTOCOL_VERSION + 1));
      w.u16(0);
      w.u32(1);
      w.blob(payload);
      const std::vector<uint8_t> bad = w.take();
      const ssize_t wrote = ::send(fd, bad.data(), bad.size(), MSG_NOSIGNAL);
      check(wrote == ssize_t(bad.size()), "sent the wrong-version HELLO");
      Frame reply;
      check(readFrameBlocking(fd, reply, 3000), "received a reply to the bad HELLO");
      check(reply.type == MSG_REJECT, "wrong-version client was rejected");
      if (reply.type == MSG_REJECT) {
        RejectMsg r;
        check(decodePayload(reply.payload, r) && r.r.reason == REJECT_VERSION,
              "reject reason was REJECT_VERSION");
        st.versionRejected = true;
      }
      ::close(fd);
    }
  }

  // --- the client dies and a new one rejoins by keyframe ---------------------
  {
    clientNet.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    NetConfig c2 = ccfg;
    NetThread client2;
    NetQueues cq2;
    check(client2.start(c2, &cq2, err), "second client net thread started");
    const bool rejoined = waitFor(Clock::now() + std::chrono::seconds(10), [&] {
      hq.publishSnapshot(std::make_shared<GridSnapshot>(makeSnapshot(hostSb, ++lastTick)));
      return client2.handshakeDone();
    });
    check(rejoined, "second client completed the handshake");
    if (rejoined) {
      ClientReplicator rep2;
      Sandbox sb2;
      sb2.allocate(640, 360);
      bool converged2 = false;
      for (int i = 0; i < 500 && !converged2; ++i) {
        hq.publishSnapshot(std::make_shared<GridSnapshot>(makeSnapshot(hostSb, ++lastTick)));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        pumpClient(cq2, rep2, sb2, st);
        if (rep2.haveState() && crc32(rep2.grid()) == crc32(gridToBytes(hostSb)))
          converged2 = true;
      }
      check(converged2, "rejoined client re-synced to the authoritative grid");
      st.reconnected = converged2;
    }
    client2.stop();
  }

  hostNet.stop();

  log << "net-tcp-selftest: ticks=" << st.ticks << " keyframes=" << st.keyframes
      << " deltas=" << st.deltas << " maxDeltaBytes=" << st.maxDeltaBytes
      << " hostBytesOut=" << st.hostBytesOut << " clientBytesIn=" << st.clientBytesIn
      << " hostPort=" << st.hostPort << " playerId=" << int(st.clientPlayerId)
      << " desyncs=" << st.clientDesyncs
      << " versionRejected=" << (st.versionRejected ? 1 : 0)
      << " reconnected=" << (st.reconnected ? 1 : 0)
      << " checks=" << st.checks << " failures=" << st.failures << "\n";
  return st;
}

} // namespace netplay
} // namespace sandcraft

#endif // MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_TCP_SELFTEST_HPP_
