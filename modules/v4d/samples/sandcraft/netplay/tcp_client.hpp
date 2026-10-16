// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The connecting half of the TCP transport (specification.md §15.1, roadmap M3).
//
// A client dials the host's endpoint. Because the socket is non-blocking,
// connect() usually returns EINPROGRESS and the connection completes later, so
// this is a tiny state machine: begin() starts the dial and step() is called
// from the poll loop until it reports Connected or Closed. The loop watches the
// socket for POLLOUT and reads SO_ERROR once it is writable (that is the only
// portable way to learn a non-blocking connect's outcome).
//
// On success the fd is handed to the caller with release(), which transfers
// ownership: from then on the net thread's connection record owns it and the
// connector is inert. That keeps exactly one owner of every fd, which is the
// whole reason release() exists rather than a getter plus a flag.
#ifndef MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_TCP_CLIENT_HPP_
#define MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_TCP_CLIENT_HPP_

#include <cstdint>
#include <string>

#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "socket_util.hpp"
#include "transport.hpp" // ConnState

namespace sandcraft {
namespace netplay {

class TcpConnector {
public:
  TcpConnector() = default;
  ~TcpConnector() { close(); }
  TcpConnector(const TcpConnector &) = delete;
  TcpConnector &operator=(const TcpConnector &) = delete;

  // Resolve `host` and start a non-blocking connect. Returns true when the dial
  // is either already complete or in progress (state() tells which); false only
  // when no address could even be attempted, with `err` set.
  bool begin(const std::string &host, uint16_t port, std::string &err) {
    close();
    state_ = ConnState::Connecting;
    struct addrinfo *res = sockutil::resolve(host, port, /*passive=*/false, err);
    if (!res)
      return false;
    int lastErrno = 0;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
      int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
      if (fd < 0) {
        lastErrno = errno;
        continue;
      }
      sockutil::setCloseOnExec(fd);
      sockutil::setNonBlocking(fd);
      const int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
      if (rc == 0) {
        sockutil::setNoDelay(fd);
        fd_ = fd;
        state_ = ConnState::Connected;
        ::freeaddrinfo(res);
        return true;
      }
      if (errno == EINPROGRESS || errno == EINTR) {
        // Commit to this address; the loop's step() will finish or fail it. A
        // second address would need its own socket and its own step, and the
        // v1 ladder (§15.3) dials one endpoint, so there is nothing to gain.
        sockutil::setNoDelay(fd);
        fd_ = fd;
        state_ = ConnState::Connecting;
        ::freeaddrinfo(res);
        return true;
      }
      lastErrno = errno;
      ::close(fd);
    }
    ::freeaddrinfo(res);
    state_ = ConnState::Closed;
    err = "connect failed: " + sockutil::errnoString(lastErrno ? lastErrno : errno);
    return false;
  }

  // Advance an in-progress connect. Safe to call when not Connecting (returns
  // the current state). A writable socket means the attempt finished; SO_ERROR
  // then says whether it succeeded.
  ConnState step(std::string &err) {
    if (state_ != ConnState::Connecting || fd_ < 0)
      return state_;
    struct pollfd p;
    p.fd = fd_;
    p.events = POLLOUT;
    p.revents = 0;
    const int r = ::poll(&p, 1, 0);
    if (r <= 0)
      return state_; // still connecting (or EINTR); caller retries later
    if (p.revents & (POLLOUT | POLLERR | POLLHUP)) {
      int soerr = 0;
      socklen_t l = sizeof(soerr);
      if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &soerr, &l) != 0)
        soerr = errno;
      if (soerr == 0) {
        state_ = ConnState::Connected;
        return state_;
      }
      err = "connect: " + sockutil::errnoString(soerr);
      sockutil::closeFd(fd_);
      state_ = ConnState::Closed;
      return state_;
    }
    return state_;
  }

  // Hand the connected fd to the caller and become inert. Returns -1 if there
  // is nothing to hand over.
  int release() {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }

  void close() {
    sockutil::closeFd(fd_);
    if (state_ != ConnState::Closed)
      state_ = ConnState::Closed;
  }

  ConnState state() const { return state_; }
  bool connected() const { return state_ == ConnState::Connected; }
  int fd() const { return fd_; }

private:
  int fd_ = -1;
  ConnState state_ = ConnState::Closed;
};

} // namespace netplay
} // namespace sandcraft

#endif // MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_TCP_CLIENT_HPP_
