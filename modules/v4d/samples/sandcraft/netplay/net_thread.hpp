// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The network thread (specification.md §15.1, §16.1, roadmap M3).
//
// One std::thread runs a poll(2) loop over the listen socket, the client
// sockets (or the single host socket) and a wake pipe. It owns the sockets, the
// per-peer reassembly buffers and the per-peer last-transmitted baselines. It
// MUST NOT touch the Sandbox or the plan (Rule C2): everything crosses through
// the NetQueues handed to it at start().
//
// Host and client are the same loop with two different frame handlers:
//
//   host   -- accepts peers, answers HELLO with WELCOME + KEYFRAME, folds the
//             latest published snapshot into a DELTA (or periodic KEYFRAME) per
//             peer through that peer's HostReplicator, folds a peer's INPUT
//             batch into NetQueues::input demultiplexed by player id, and
//             answers RESYNC with a fresh keyframe.
//   client -- dials the host (a non-blocking connect finished by the loop),
//             sends HELLO, records WELCOME/REJECT, forwards everything else to
//             NetQueues::inbound (the apply node in NetApplyPlan owns the
//             ClientReplicator), takes NetQueues::outbound (INPUT batches the
//             input node framed) and writes it to the socket.
//
// Two properties are worth stating because they are easy to break:
//
//   * The replicator is per peer. Two clients join at different ticks, so each
//     needs its own baseline: HostReplicator advances its baseline only on what
//     it actually transmitted (spec Rule R1), which is what makes a late joiner
//     get a keyframe and an established peer get a delta from the same snapshot.
//   * The queues are only ever touched through their locking accessors and the
//     wake callback is invoked by the queues after they release their lock, so
//     the lock order stays V4D-mutex -> queues mtx (spec Rule C1) with the net
//     thread taking only the latter.
#ifndef MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_NET_THREAD_HPP_
#define MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_NET_THREAD_HPP_

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "netqueues.hpp"
#include "netplay.hpp"
#include "protocol.hpp"
#include "replication.hpp"
#include "socket_util.hpp"
#include "tcp_client.hpp"
#include "tcp_server.hpp"

namespace sandcraft {
namespace netplay {

// Host + at most three clients (spec §15.1, §5).
inline constexpr int MAX_PLAYERS = 4;

// poll() timeout. The wake pipe bounds latency, so this only has to be short
// enough to notice a non-blocking connect completing and to notice a stop flag
// promptly; it is not the delivery path.
inline constexpr int POLL_TIMEOUT_MS = 50;

// Read chunk. Large enough to pull a keyframe in a few reads, small enough to
// keep the loop responsive.
inline constexpr size_t RECV_CHUNK = 64 * 1024;

// A per-connection write buffer cap. TCP backpressure propagates from the
// socket so this should never bind; if it does, the peer is not draining and
// the connection is dropped rather than growing without bound (spec §18.3).
inline constexpr size_t OUT_BUFFER_CAP = 16u * 1024 * 1024;

// A snapshot of the net thread's counters, readable without a lock. The HUD and
// the --net-stats log line read these; the net thread writes them.
struct NetStats {
  uint64_t bytesIn = 0, bytesOut = 0;
  uint64_t framesIn = 0, framesOut = 0;
  // Keyframes and deltas are produced in one direction only: the host sends
  // them, the client receives them. The *Out counters are therefore the host's
  // contribution and the *In counters the client's, and a `--net-stats` line
  // prints whichever pair belongs to the role so a guest does not read as
  // `kf=0 dl=0` while it is being replicated to every tick (they used to: the
  // counters were out-only).
  uint64_t keyframesOut = 0, deltasOut = 0;
  uint64_t keyframesIn = 0, deltasIn = 0;
  uint64_t inputEventsIn = 0;
  uint64_t drops = 0;
  uint64_t tick = 0;
  int peers = 0;
  uint8_t playerId = 0;
  bool connected = false;
  bool handshakeDone = false;
};

class NetThread {
public:
  NetThread() = default;
  ~NetThread() { stop(); }
  NetThread(const NetThread &) = delete;
  NetThread &operator=(const NetThread &) = delete;

  // Bind (host) or start dialling (client) on the calling thread so the caller
  // can read boundPort() and any error before the loop runs, then spawn the
  // loop. `queues` defaults to the process-wide singleton but the self-test
  // passes its own so two endpoints can run in one process.
  bool start(const NetConfig &cfg, NetQueues *queues, std::string &err) {
    if (running_.load()) {
      err = "net thread already running";
      return false;
    }
    cfg_ = cfg;
    q_ = queues ? queues : &netQueues();
    isHost_ = (cfg_.role == Role::Host);
    sockutil::ignoreSigpipe();

    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
      err = "pipe: " + sockutil::errnoString(errno);
      return false;
    }
    wakeR_ = fds[0];
    wakeW_ = fds[1];
    sockutil::setNonBlocking(wakeR_);
    sockutil::setNonBlocking(wakeW_);
    sockutil::setCloseOnExec(wakeR_);
    sockutil::setCloseOnExec(wakeW_);

    if (isHost_) {
      if (!listener_.open(/*host=*/"", cfg_.port, err)) {
        cleanupFds();
        return false;
      }
      boundPort_ = listener_.port();
    } else {
      boundPort_ = 0;
      if (!beginClientDial(err)) {
        cleanupFds();
        return false;
      }
    }

    // Register the wake callback. It runs from the plan thread(s) after the
    // queues' lock is released; it only writes one byte to the pipe.
    const int wakeFd = wakeW_;
    q_->setWake([wakeFd] {
      const uint8_t one = 1;
      const ssize_t n = ::write(wakeFd, &one, 1);
      (void)n;
    });

    stop_.store(false);
    running_.store(true);
    thread_ = std::thread([this] { run(); });
    return true;
  }

  void stop() {
    if (!running_.exchange(false)) {
      // Either never started or already stopped.
      return;
    }
    stop_.store(true);
    signalWake();
    if (thread_.joinable())
      thread_.join();
    if (q_)
      q_->clearWake();
    listener_.close();
    connector_.close();
    for (auto &p : peers_)
      if (p)
        sockutil::closeFd(p->fd);
    peers_.clear();
    sockutil::closeFd(hostConn_.fd);
    cleanupFds();
    peersCount_.store(0);
    connected_.store(false);
    handshakeDone_.store(false);
  }

  bool running() const { return running_.load(); }
  bool isHost() const { return isHost_; }

  uint16_t boundPort() const { return boundPort_; }
  uint8_t playerId() const { return playerId_.load(); }
  bool connected() const { return connected_.load(); }
  bool handshakeDone() const { return handshakeDone_.load(); }
  bool rejected() const { return rejected_.load(); }

  NetStats stats() const {
    NetStats s;
    s.bytesIn = bytesIn_.load();
    s.bytesOut = bytesOut_.load();
    s.framesIn = framesIn_.load();
    s.framesOut = framesOut_.load();
    s.keyframesOut = keyframesOut_.load();
    s.deltasOut = deltasOut_.load();
    s.keyframesIn = keyframesIn_.load();
    s.deltasIn = deltasIn_.load();
    s.inputEventsIn = inputEventsIn_.load();
    s.drops = q_ ? q_->drops.load() : 0;
    s.tick = tick_.load();
    s.peers = peersCount_.load();
    s.playerId = playerId_.load();
    s.connected = connected_.load();
    s.handshakeDone = handshakeDone_.load();
    return s;
  }

private:
  // A live connection. `out` is the write buffer with `outOff` bytes already
  // written to the socket; `parser` reassembles the byte stream into frames.
  struct Conn {
    int fd = -1;
    std::string addr;
    FrameParser parser;
    std::vector<uint8_t> out;
    size_t outOff = 0;
    bool closed = false;
  };

  // A host-side peer carries the handshake state and its own replicator (its own
  // baseline). The client's single host connection is a plain Conn.
  struct HostPeer : Conn {
    std::unique_ptr<HostReplicator> replicator = std::make_unique<HostReplicator>();
    uint8_t playerId = 0;
    bool helloOk = false;
    bool welcomed = false;
  };

  // --- lifecycle helpers -----------------------------------------------------

  void cleanupFds() {
    sockutil::closeFd(wakeR_);
    sockutil::closeFd(wakeW_);
  }

  void signalWake() {
    if (wakeW_ >= 0) {
      const uint8_t one = 1;
      const ssize_t n = ::write(wakeW_, &one, 1);
      (void)n;
    }
  }

  void drainWake() {
    uint8_t buf[256];
    for (;;) {
      const ssize_t n = ::read(wakeR_, buf, sizeof(buf));
      if (n <= 0)
        break;
    }
  }

  // --- the loop --------------------------------------------------------------

  void run() {
    using clock = std::chrono::steady_clock;
    auto lastStats = clock::now();
    std::vector<struct pollfd> pfds;
    while (!stop_.load()) {
      pfds.clear();
      auto watch = [&](int fd, short ev) {
        if (fd < 0)
          return;
        struct pollfd p;
        p.fd = fd;
        p.events = ev;
        p.revents = 0;
        pfds.push_back(p);
      };
      watch(wakeR_, POLLIN);
      if (isHost_) {
        watch(listener_.fd(), POLLIN);
        for (auto &pp : peers_) {
          if (!pp || pp->fd < 0)
            continue;
          short ev = POLLIN;
          if (pp->outOff < pp->out.size())
            ev |= POLLOUT;
          watch(pp->fd, ev);
        }
      } else if (hostConn_.fd >= 0) {
        short ev = POLLIN;
        if (hostConn_.outOff < hostConn_.out.size())
          ev |= POLLOUT;
        watch(hostConn_.fd, ev);
      }

      if (!pfds.empty())
        ::poll(pfds.data(), pfds.size(), POLL_TIMEOUT_MS);
      for (const struct pollfd &p : pfds)
        if (p.revents)
          dispatch(p.fd, p.revents);

      if (isHost_)
        stepHost();
      else
        stepClient();

      if (cfg_.netStats) {
        const auto now = clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastStats).count() >= 1000) {
          logStats();
          lastStats = now;
        }
      }
    }
  }

  void dispatch(int fd, short revents) {
    if (fd == wakeR_) {
      drainWake();
      return;
    }
    if (isHost_) {
      if (fd == listener_.fd()) {
        acceptNew();
        return;
      }
      for (auto &pp : peers_) {
        if (!pp || pp->fd != fd)
          continue;
        HostPeer &p = *pp;
        if (revents & POLLIN) {
          if (!readConn(p, [this, &p](Frame &f) { hostOnFrame(p, f); }))
            closePeer(p);
        }
        if (!p.closed && p.fd >= 0 && (revents & (POLLOUT | POLLERR | POLLHUP))) {
          if (!flushConn(p))
            closePeer(p);
        }
        return;
      }
    } else if (fd == hostConn_.fd) {
      if (revents & POLLIN) {
        if (!readConn(hostConn_, [this](Frame &f) { clientOnFrame(f); }))
          closeClientConn();
      }
      if (hostConn_.fd >= 0 && (revents & (POLLOUT | POLLERR | POLLHUP))) {
        if (!flushConn(hostConn_))
          closeClientConn();
      }
    }
  }

  // --- connection I/O --------------------------------------------------------

  bool readConn(Conn &c, const std::function<void(Frame &)> &onFrame) {
    uint8_t buf[RECV_CHUNK];
    for (;;) {
      const ssize_t n = ::recv(c.fd, buf, sizeof(buf), 0);
      if (n > 0) {
        bytesIn_.fetch_add(uint64_t(n));
        c.parser.push(buf, size_t(n));
        Frame f;
        while (c.parser.next(f)) {
          // A length over MAX_PAYLOAD (oversize) or a header shorter than
          // MsgHeader (invalid) is a structural failure: close (spec §13.6).
          if (f.oversize || !f.valid)
            return false;
          framesIn_.fetch_add(1);
          onFrame(f);
          if (c.fd < 0)
            return false;
        }
      } else if (n == 0) {
        return false; // orderly close by the peer
      } else {
        if (sockutil::wouldBlock(errno))
          break;
        return false;
      }
    }
    return true;
  }

  bool flushConn(Conn &c) {
    if (c.fd < 0)
      return false;
    while (c.outOff < c.out.size()) {
      const ssize_t n = ::send(c.fd, c.out.data() + c.outOff, c.out.size() - c.outOff,
                              MSG_NOSIGNAL);
      if (n > 0) {
        c.outOff += size_t(n);
        bytesOut_.fetch_add(uint64_t(n));
      } else if (n < 0 && sockutil::wouldBlock(errno)) {
        return true; // socket buffer full; the loop will finish it on POLLOUT
      } else {
        return false;
      }
    }
    c.out.clear();
    c.outOff = 0;
    return true;
  }

  void sendFrame(Conn &c, const std::vector<uint8_t> &bytes) {
    if (c.fd < 0 || c.closed || bytes.empty())
      return;
    if (c.out.size() - c.outOff + bytes.size() > OUT_BUFFER_CAP) {
      c.closed = true;
      sockutil::closeFd(c.fd);
      return;
    }
    framesOut_.fetch_add(1);
    c.out.insert(c.out.end(), bytes.begin(), bytes.end());
    if (!flushConn(c)) {
      c.closed = true;
      sockutil::closeFd(c.fd);
    }
  }

  // Send a small control frame (REJECT) and give it a brief chance to drain
  // before the fd dies, so the peer sees the reason rather than a reset.
  void sendThenClose(Conn &c, const std::vector<uint8_t> &bytes) {
    sendFrame(c, bytes);
    for (int i = 0; i < 20 && c.fd >= 0 && c.outOff < c.out.size(); ++i) {
      struct pollfd p;
      p.fd = c.fd;
      p.events = POLLOUT;
      p.revents = 0;
      ::poll(&p, 1, 10);
      if (!flushConn(c))
        break;
    }
    c.closed = true;
    sockutil::closeFd(c.fd);
  }

  void broadcast(const std::vector<uint8_t> &bytes) {
    for (auto &pp : peers_)
      if (pp && pp->welcomed && !pp->closed)
        sendFrame(*pp, bytes);
  }

  // --- host ------------------------------------------------------------------

  int freePlayerId() const {
    for (int id = 1; id < MAX_PLAYERS; ++id)
      if (!idUsed_[id])
        return id;
    return -1;
  }

  int activePlayerCount() const {
    int n = 1; // the host is player 0
    for (const auto &pp : peers_)
      if (pp && pp->helloOk && !pp->closed)
        ++n;
    return n;
  }

  void acceptNew() {
    for (;;) {
      std::string peer, err;
      const int fd = listener_.acceptPeer(peer, err);
      if (fd < 0)
        return; // EAGAIN or a transient error; either way, nothing more to take
      auto p = std::make_unique<HostPeer>();
      p->fd = fd;
      p->addr = peer;
      std::cerr << "sandcraft: net host accepted connection from " << peer << "\n";
      peers_.push_back(std::move(p));
    }
  }

  void closePeer(HostPeer &p) {
    if (p.playerId != 0 && p.playerId < MAX_PLAYERS)
      idUsed_[p.playerId] = false;
    p.closed = true;
    sockutil::closeFd(p.fd);
    peersCount_.store(int(std::count_if(peers_.begin(), peers_.end(),
                                        [](const std::unique_ptr<HostPeer> &q) {
                                          return q && !q->closed && q->helloOk;
                                        })));
  }

  void hostOnFrame(HostPeer &p, Frame &f) {
    switch (f.type) {
    case MSG_HELLO:
      handleHello(p, f);
      break;
    case MSG_INPUT:
      handleInput(p, f);
      break;
    case MSG_RESYNC: {
      ResyncMsg r;
      if (decodePayload(f.payload, r) && p.replicator)
        p.replicator->requestKeyframe();
      break;
    }
    case MSG_PING: {
      PingPongMsg in;
      if (decodePayload(f.payload, in)) {
        PingPongMsg out;
        out.p.senderTimeMs = nowMillis();
        out.p.peerTimeMs = in.p.senderTimeMs;
        sendFrame(p, frame(MSG_PONG, ++hostMsgSeq_, encodePayload(out)));
      }
      break;
    }
    case MSG_BYE:
      closePeer(p);
      break;
    default:
      break;
    }
  }

  void handleHello(HostPeer &p, Frame &f) {
    if (p.helloOk)
      return; // a second HELLO is ignored
    if (f.version != PROTOCOL_VERSION) {
      sendThenClose(p, p.replicator->reject(REJECT_VERSION));
      return;
    }
    HelloMsg h;
    if (!decodePayload(f.payload, h)) {
      p.closed = true;
      sockutil::closeFd(p.fd);
      return;
    }
    if (h.h.version != PROTOCOL_VERSION) {
      sendThenClose(p, p.replicator->reject(REJECT_VERSION));
      return;
    }
    // The join token is parsed but not enforced: v1 sessions are open (spec D24).
    const int id = freePlayerId();
    if (id < 0) {
      sendThenClose(p, p.replicator->reject(REJECT_FULL));
      return;
    }
    p.playerId = uint8_t(id);
    idUsed_[id] = true;
    p.helloOk = true;
    std::cerr << "sandcraft: net host accepted " << (h.name.empty() ? "peer" : h.name)
              << " (" << p.addr << ") as player " << id << "\n";
  }

  void handleInput(HostPeer &p, Frame &f) {
    InputBatchMsg b;
    if (!decodePayload(f.payload, b)) {
      p.closed = true;
      sockutil::closeFd(p.fd);
      return;
    }
    for (const InputEventWire &e : b.events) {
      q_->pushInput({p.playerId, e});
      inputEventsIn_.fetch_add(1);
    }
  }

  static bool sameWorld(const WorldWire &a, const WorldWire &b) {
    if (a.paused != b.paused || a.particleSwaps != b.particleSwaps || a.epoch != b.epoch)
      return false;
    for (int i = 0; i < 4; ++i) {
      if (a.emitters[i].type != b.emitters[i].type ||
          a.emitters[i].on != b.emitters[i].on ||
          a.emitters[i].density != b.emitters[i].density)
        return false;
    }
    return true;
  }

  void stepHost() {
    // Drop peers that closed since the last pass.
    peers_.erase(std::remove_if(peers_.begin(), peers_.end(),
                                [](const std::unique_ptr<HostPeer> &p) {
                                  return !p || p->closed;
                                }),
                 peers_.end());

    std::shared_ptr<const GridSnapshot> snap = q_->takeSnapshot();
    if (snap && !snap->cells.empty()) {
      const bool newTick = (snap->tick != lastHandledTick_);
      if (!haveWorld_) {
        lastWorld_ = snap->world;
        haveWorld_ = true;
      } else if (!sameWorld(lastWorld_, snap->world)) {
        WorldMsg wm;
        wm.w = snap->world;
        broadcast(frame(MSG_WORLD, ++hostMsgSeq_, encodePayload(wm)));
        lastWorld_ = snap->world;
      }

      for (auto &pp : peers_) {
        HostPeer &p = *pp;
        if (!p.helloOk || p.closed)
          continue;
        if (!p.welcomed) {
          const uint8_t pc = uint8_t(activePlayerCount());
          sendFrame(p, p.replicator->welcome(p.playerId, pc, snap->width, snap->height,
                                             snap->tick));
          std::vector<uint8_t> kf = p.replicator->produce(*snap, /*forceKeyframe=*/true);
          countSent(kf);
          sendFrame(p, kf);
          p.welcomed = true;
          continue;
        }
        if (newTick) {
          const bool force = p.replicator->consumeForceNext();
          std::vector<uint8_t> fr = p.replicator->produce(*snap, force);
          countSent(fr);
          sendFrame(p, fr);
        }
      }
      if (newTick) {
        lastHandledTick_ = snap->tick;
        tick_.store(snap->tick);
      }
    }

    // Nothing on the host pushes outbound today (its own input goes through
    // NetQueues::input), but drain it so the queue cannot grow if it ever does.
    std::vector<uint8_t> m;
    while (q_->popOutbound(m))
      broadcast(m);

    peersCount_.store(int(std::count_if(peers_.begin(), peers_.end(),
                                        [](const std::unique_ptr<HostPeer> &p) {
                                          return p && !p->closed && p->helloOk;
                                        })));
  }

  void countSent(const std::vector<uint8_t> &framed) {
    if (framed.size() > 4) {
      if (framed[4] == MSG_KEYFRAME)
        keyframesOut_.fetch_add(1);
      else if (framed[4] == MSG_DELTA)
        deltasOut_.fetch_add(1);
    }
  }

  // --- client ----------------------------------------------------------------

  bool beginClientDial(std::string &err) {
    hostAddress_ = endpointHost(cfg_);
    hostPort_ = endpointPort(cfg_);
    if (hostPort_ == 0)
      hostPort_ = DEFAULT_PORT;
    if (!connector_.begin(hostAddress_, hostPort_, err))
      return false;
    if (connector_.state() == ConnState::Connected)
      adoptClientConn();
    return true;
  }

  void adoptClientConn() {
    hostConn_.fd = connector_.release();
    hostConn_.addr = hostAddress_ + ":" + std::to_string(hostPort_);
    hostConn_.closed = false;
    hostConn_.out.clear();
    hostConn_.outOff = 0;
    hostConn_.parser = FrameParser{};
    connected_.store(true);
    handshakeDone_.store(false);
    rejected_.store(false);
    sendHello();
  }

  void sendHello() {
    HelloMsg h;
    h.h.version = PROTOCOL_VERSION;
    h.name = cfg_.name;
    h.h.nameLen = uint8_t(h.name.size());
    sendFrame(hostConn_, frame(MSG_HELLO, ++clientMsgSeq_, encodePayload(h)));
  }

  void closeClientConn() {
    sockutil::closeFd(hostConn_.fd);
    hostConn_.closed = false;
    hostConn_.out.clear();
    hostConn_.outOff = 0;
    hostConn_.parser = FrameParser{};
    connected_.store(false);
    handshakeDone_.store(false);
    connector_.close();
    reconnectAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  }

  void clientOnFrame(Frame &f) {
    switch (f.type) {
    case MSG_WELCOME: {
      WelcomeMsg w;
      if (decodePayload(f.payload, w)) {
        playerId_.store(w.w.playerId);
        handshakeDone_.store(true);
        std::cerr << "sandcraft: net client joined as player " << int(w.w.playerId)
                  << " of " << int(w.w.playerCount) << " (" << w.w.gridW << "x" << w.w.gridH
                  << ")" << std::endl;
      }
      break;
    }
    case MSG_REJECT: {
      RejectMsg r;
      if (decodePayload(f.payload, r))
        std::cerr << "sandcraft: net client rejected (reason " << int(r.r.reason) << ")"
                  << std::endl;
      rejected_.store(true);
      closeClientConn();
      break;
    }
    case MSG_KEYFRAME: {
      // Read only the header's leading tick, so the stats/HUD have a tick to
      // show; the compressed grid is left untouched for the apply node, which
      // owns the ClientReplicator (the net thread only moves bytes, spec §16.2).
      ByteReader r(f.payload);
      const uint64_t t = r.u64();
      if (r.ok()) {
        keyframesIn_.fetch_add(1);
        tick_.store(t);
      }
      q_->pushInbound(f);
      break;
    }
    case MSG_DELTA: {
      ByteReader r(f.payload);
      r.u64(); // baseTick
      const uint64_t t = r.u64();
      if (r.ok()) {
        deltasIn_.fetch_add(1);
        tick_.store(t);
      }
      q_->pushInbound(f);
      break;
    }
    default:
      // WORLD/PING/BYE go to the apply node, which owns the ClientReplicator:
      // the net thread only moves bytes.
      q_->pushInbound(f);
      break;
    }
  }

  void stepClient() {
    if (hostConn_.fd < 0) {
      if (connector_.state() == ConnState::Connecting) {
        std::string err;
        if (connector_.step(err) == ConnState::Connected)
          adoptClientConn();
        else if (connector_.state() == ConnState::Closed) {
          std::cerr << "sandcraft: net client connect failed: " << err << std::endl;
          reconnectAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        }
      } else if (std::chrono::steady_clock::now() >= reconnectAt_) {
        std::string err;
        if (!beginClientDial(err)) {
          std::cerr << "sandcraft: net client connect failed: " << err << std::endl;
          reconnectAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        }
      }
      return;
    }
    std::vector<uint8_t> m;
    while (q_->popOutbound(m))
      sendFrame(hostConn_, m);
  }

  // --- diagnostics -----------------------------------------------------------

  void logStats() const {
    const NetStats s = stats();
    std::cerr << "net-stats: role=" << (isHost_ ? "host" : "client")
              << " tick=" << s.tick << " peers=" << s.peers
              << " player=" << int(s.playerId)
              << " connected=" << (s.connected ? 1 : 0)
              << " handshake=" << (s.handshakeDone ? 1 : 0)
              // The host produces keyframes/deltas and the client consumes them,
              // so print the pair that belongs to the role: a host reads `kf`/
              // `dl` (sent) and a client `kfIn`/`dlIn` (received). A single
              // `kf=`/`dl=` pair that was out-only made every client look idle.
              << (isHost_ ? " kf=" : " kfIn=") << (isHost_ ? s.keyframesOut : s.keyframesIn)
              << (isHost_ ? " dl=" : " dlIn=") << (isHost_ ? s.deltasOut : s.deltasIn)
              << " events=" << s.inputEventsIn
              << " in=" << s.bytesIn << " out=" << s.bytesOut
              << " frames=" << s.framesIn << "/" << s.framesOut
              << " drops=" << s.drops << std::endl;
  }

  // --- state -----------------------------------------------------------------
  NetConfig cfg_{};
  NetQueues *q_ = nullptr;
  bool isHost_ = true;

  TcpListener listener_;
  TcpConnector connector_;
  Conn hostConn_;                          // client: the host connection
  std::vector<std::unique_ptr<HostPeer>> peers_; // host: accepted peers
  bool idUsed_[MAX_PLAYERS] = {false, false, false, false};

  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};
  int wakeR_ = -1;
  int wakeW_ = -1;

  uint16_t boundPort_ = 0;
  std::string hostAddress_;
  uint16_t hostPort_ = 0;
  std::chrono::steady_clock::time_point reconnectAt_{};

  uint64_t lastHandledTick_ = 0;
  WorldWire lastWorld_{};
  bool haveWorld_ = false;
  uint32_t hostMsgSeq_ = 0;
  uint32_t clientMsgSeq_ = 0;

  std::atomic<uint64_t> bytesIn_{0}, bytesOut_{0};
  std::atomic<uint64_t> framesIn_{0}, framesOut_{0};
  std::atomic<uint64_t> keyframesOut_{0}, deltasOut_{0};
  std::atomic<uint64_t> keyframesIn_{0}, deltasIn_{0};
  std::atomic<uint64_t> inputEventsIn_{0};
  std::atomic<uint64_t> tick_{0};
  std::atomic<int> peersCount_{0};
  std::atomic<uint8_t> playerId_{0};
  std::atomic<bool> connected_{false};
  std::atomic<bool> handshakeDone_{false};
  std::atomic<bool> rejected_{false};
};

} // namespace netplay
} // namespace sandcraft

#endif // MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_NET_THREAD_HPP_
