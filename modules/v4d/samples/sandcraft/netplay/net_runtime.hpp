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
// M3 gives it ownership of the network thread. configure() records the role and
// decides whether a session is live at all (networking is opt-in, spec §16.6);
// start() binds/dials and spawns the loop; stop() joins it. The join happens
// *outside* the lifecycle mutex (see stop()) so a callback from the net thread
// can never deadlock the shutdown.
#ifndef MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_NET_RUNTIME_HPP_
#define MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_NET_RUNTIME_HPP_

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "net_thread.hpp"
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

  ~NetRuntime() { stop(); }

  // Called once from the demo entry point, before V4D::init() constructs any
  // plan instance. A role configured here is what makes the §16.4 branch pick
  // the client arm; without the call the singleton keeps its default (Host) and
  // the client arm is unreachable (spec §16.6).
  void configure(const NetConfig &cfg) {
    cfg_ = cfg;
    active_.store(cfg.enabled);
  }

  const NetConfig &config() const { return cfg_; }
  bool host() const { return cfg_.role == Role::Host; }
  bool client() const { return cfg_.role == Role::Client; }

  // A live session. True only when the process was asked to network -- an
  // instance with no network arguments runs exactly today's single-player demo
  // and publishes no per-tick snapshot (§4.3, §16.6).
  bool active() const { return active_.load(); }
  void setActive(bool a) { active_.store(a); }

  // Bind (host) or dial (client) and spawn the net thread. Idempotent: calling
  // it twice starts one thread. Returns false with `err` set only on a hard
  // failure (bad address, bind refused); a client whose connect is still in
  // progress returns true (the loop finishes it).
  bool start(std::string &err) {
    std::lock_guard<std::mutex> lk(lifecycleMtx_);
    if (!cfg_.enabled)
      return true; // nothing to do; the single-player path
    if (thread_ && thread_->running())
      return true;
    auto t = std::make_unique<NetThread>();
    if (!t->start(cfg_, &netQueues(), err))
      return false;
    thread_ = std::move(t);
    return true;
  }

  // Join the net thread. The unique_ptr is moved out under the lock, then the
  // join runs without it, so the net thread can never block on the mutex this
  // method holds (the net thread does not take it, but the ordering is a
  // property we want regardless).
  void stop() {
    std::unique_ptr<NetThread> t;
    {
      std::lock_guard<std::mutex> lk(lifecycleMtx_);
      t = std::move(thread_);
    }
    if (t)
      t->stop();
  }

  NetThread *thread() { return thread_.get(); }

  uint16_t boundPort() const { return thread_ ? thread_->boundPort() : cfg_.port; }

  NetStats stats() const { return thread_ ? thread_->stats() : NetStats{}; }

  NetQueues &queues() { return netQueues(); }

private:
  NetRuntime() = default;

  std::mutex lifecycleMtx_;
  std::unique_ptr<NetThread> thread_;
  NetConfig cfg_{};
  std::atomic<bool> active_{false};
};

inline NetRuntime &netRuntime() { return NetRuntime::instance(); }

// Shorthand the plan nodes use to decide whether to publish / drain.
inline bool netActive() { return netRuntime().active(); }

} // namespace netplay
} // namespace sandcraft

#endif // MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_NET_RUNTIME_HPP_
