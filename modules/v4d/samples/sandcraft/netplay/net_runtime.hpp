// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// NetRuntime -- the process-wide netplay identity (specification.md §16.6).
//
// The Plan is constructed more than once per process (once per compute worker
// and once for the display thread), so anything that must be a single decision
// -- which role this process is, whether a session is live, which NetQueues the
// world talks to -- cannot live on a plan member. It lives here, behind a
// function-local static, and is configured once from argv before the first
// frame.
//
// M2 owns the configuration and the "is the network live" query; the listening
// socket and the poll loop arrive in M3 behind this same object, so the graph
// keeps asking the same questions.
#ifndef MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_NET_RUNTIME_HPP_
#define MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_NET_RUNTIME_HPP_

#include <atomic>

#include "netplay.hpp"
#include "netqueues.hpp"

namespace sandcraft {
namespace netplay {

class NetRuntime {
public:
  static NetRuntime &instance() {
    static NetRuntime rt;
    return rt;
  }

  NetRuntime(const NetRuntime &) = delete;
  NetRuntime &operator=(const NetRuntime &) = delete;

  void configure(const NetConfig &cfg) { cfg_ = cfg; }

  const NetConfig &config() const { return cfg_; }
  bool host() const { return cfg_.role == Role::Host; }
  bool client() const { return cfg_.role == Role::Client; }

  // A live session. In M2 this is only true once a client role is configured
  // (the socket that actually makes it true lands in M3). While it is false the
  // graph runs exactly as the single-player demo always has: the host branch,
  // and no per-tick snapshot copy.
  bool active() const { return active_; }
  void setActive(bool a) { active_ = a; }

  NetQueues &queues() { return netQueues(); }

private:
  NetRuntime() = default;
  NetConfig cfg_{};
  std::atomic<bool> active_{false};
};

inline NetRuntime &netRuntime() { return NetRuntime::instance(); }

// Shorthand the plan nodes use to decide whether to publish / drain.
inline bool netActive() { return netRuntime().active(); }

} // namespace netplay
} // namespace sandcraft

#endif // MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_NET_RUNTIME_HPP_
