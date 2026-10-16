// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The transport seam (specification.md §15.1, §16.1).
//
// The whole point of this interface is that the netplay code above it -- the
// framing, the replication, the session -- never learns whether the bytes went
// over a socket, a pipe, or (as in the M2 self-test) a queue inside the same
// process. TCP is the v1 transport and a reliable-UDP backend is a backlog item
// (spec §15.2); both slot in here without touching the layers above.
//
// The contract a transport must honour, because the layers above rely on it:
//   * it delivers a byte stream in order (framing, not message boundaries, is
//     the protocol's job -- see FrameParser in protocol.hpp);
//   * recv() never blocks (the v1 sockets are non-blocking and the M2 loopback
//     is a queue), so a caller can poll it every tick;
//   * a closed link is reported, never thrown: recv() returns false once the
//     peer is gone and further send() calls fail rather than crash.
#ifndef MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_TRANSPORT_HPP_
#define MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_TRANSPORT_HPP_

#include <cstdint>
#include <vector>

namespace sandcraft {
namespace netplay {

// The connection lifecycle a transport exposes to the session. `Connecting`
// matters only for the connecting end of a socket; the loopback is connected the
// moment it is made.
enum class ConnState { Connecting, Connected, Closed };

class Transport {
public:
  virtual ~Transport() = default;

  // Hand `bytes` to the peer. Returns false if the link is closed (or became so
  // while queueing). A partial write is the transport's problem, not the
  // caller's: either the whole buffer is accepted or the call fails.
  virtual bool send(const std::vector<uint8_t> &bytes) = 0;

  // Append whatever has arrived from the peer to `out`. Returns false when the
  // link is closed; the caller may still have received bytes in `out` on the
  // call that observed the close, so it must drain before tearing down.
  virtual bool recv(std::vector<uint8_t> &out) = 0;

  virtual ConnState state() const = 0;

  bool connected() const { return state() == ConnState::Connected; }

  // Idempotent.
  virtual void close() = 0;
};

} // namespace netplay
} // namespace sandcraft

#endif // MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_TRANSPORT_HPP_
