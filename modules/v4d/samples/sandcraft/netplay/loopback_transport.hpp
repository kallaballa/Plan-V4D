// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// An in-memory Transport (specification.md §15.3 rung 0, roadmap M2).
//
// Two endpoints share a bus; each one's send() lands in the other one's recv().
// There is no thread and no blocking: the two halves are pumped by whoever owns
// them, which for the M2 self-test is the test loop itself. That is deliberate --
// it means the entire replication and input pipeline can be proven host==client
// in one process, deterministically, before a socket exists (roadmap guiding
// principle 3, "loopback before network").
//
// The mutex is not for concurrency here (the self-test is single-threaded): it
// is there so that the same class can be handed to a real net thread in M3
// without a rewrite, and so the memory model is already the one the socket
// transport will need.
#ifndef MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_LOOPBACK_TRANSPORT_HPP_
#define MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_LOOPBACK_TRANSPORT_HPP_

#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "transport.hpp"

namespace sandcraft {
namespace netplay {

// The shared state behind a loopback link. Public so makeLoopback() can build
// the pair; the endpoints only ever hold a shared_ptr to it.
struct LoopbackBus {
  std::mutex mtx;
  std::vector<uint8_t> hostToClient;
  std::vector<uint8_t> clientToHost;
  bool hostClosed = false;
  bool clientClosed = false;

  // Bytes queued but not yet read, for the self-test's "did the link drain"
  // assertion. Lock the mutex before calling.
  size_t pendingLocked() const {
    return hostToClient.size() + clientToHost.size();
  }
};

class LoopbackTransport : public Transport {
public:
  LoopbackTransport(std::shared_ptr<LoopbackBus> bus, bool host)
      : bus_(std::move(bus)), host_(host) {}

  bool send(const std::vector<uint8_t> &bytes) override {
    std::lock_guard<std::mutex> lk(bus_->mtx);
    if (host_ ? bus_->hostClosed : bus_->clientClosed)
      return false;
    std::vector<uint8_t> &q = host_ ? bus_->hostToClient : bus_->clientToHost;
    q.insert(q.end(), bytes.begin(), bytes.end());
    return true;
  }

  bool recv(std::vector<uint8_t> &out) override {
    std::lock_guard<std::mutex> lk(bus_->mtx);
    std::vector<uint8_t> &q = host_ ? bus_->clientToHost : bus_->hostToClient;
    if (!q.empty()) {
      out.insert(out.end(), q.begin(), q.end());
      q.clear();
    }
    // Report a closed peer even on the call that drained the last bytes, so the
    // caller's loop terminates; it must process `out` before acting on it.
    const bool peerClosed = host_ ? bus_->clientClosed : bus_->hostClosed;
    return !peerClosed;
  }

  ConnState state() const override {
    std::lock_guard<std::mutex> lk(bus_->mtx);
    const bool me = host_ ? bus_->hostClosed : bus_->clientClosed;
    const bool peer = host_ ? bus_->clientClosed : bus_->hostClosed;
    if (me || peer)
      return ConnState::Closed;
    return ConnState::Connected;
  }

  void close() override {
    std::lock_guard<std::mutex> lk(bus_->mtx);
    if (host_)
      bus_->hostClosed = true;
    else
      bus_->clientClosed = true;
  }

private:
  std::shared_ptr<LoopbackBus> bus_;
  bool host_;
};

// Build a connected pair: {host end, client end}.
inline std::pair<std::unique_ptr<LoopbackTransport>, std::unique_ptr<LoopbackTransport>>
makeLoopback() {
  auto bus = std::make_shared<LoopbackBus>();
  return {std::make_unique<LoopbackTransport>(bus, true),
          std::make_unique<LoopbackTransport>(bus, false)};
}

} // namespace netplay
} // namespace sandcraft

#endif // MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_LOOPBACK_TRANSPORT_HPP_
