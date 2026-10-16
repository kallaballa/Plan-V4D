// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The listening half of the TCP transport (specification.md §15.1, roadmap M3).
//
// The host owns one TcpListener for the life of the session. It is separate
// from the connector because the two halves have nothing in common besides the
// socket options: a listener binds, listens and accepts; a connector dials. The
// poll loop that drives both lives a layer up, in net_thread.hpp.
//
// Port 0 means "let the kernel choose" and the chosen port is read back with
// getsockname(). That is what lets the self-test bind an ephemeral port on the
// loopback and hand it to the client without a fixed port colliding with a
// running demo (spec §15.3 rung 0, "loopback before network").
#ifndef MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_TCP_SERVER_HPP_
#define MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_TCP_SERVER_HPP_

#include <cstdint>
#include <cstring>
#include <string>

#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "socket_util.hpp"

namespace sandcraft {
namespace netplay {

// The v1 star is host + at most three clients (spec §15.1); the backlog only
// needs to absorb the burst between accept() calls, so a small constant is
// right rather than SOMAXCONN.
inline constexpr int LISTEN_BACKLOG = 8;

class TcpListener {
public:
  TcpListener() = default;
  ~TcpListener() { close(); }
  TcpListener(const TcpListener &) = delete;
  TcpListener &operator=(const TcpListener &) = delete;

  // Bind and listen. `host` empty means every interface (AI_PASSIVE). `port`
  // may be 0 to take an ephemeral one. Returns false with `err` set on any
  // failure; on success the listener is ready to accept and port() is the
  // actual bound port.
  bool open(const std::string &host, uint16_t port, std::string &err) {
    close();
    struct addrinfo *res = sockutil::resolve(host, port, /*passive=*/true, err);
    if (!res)
      return false;
    int lastErrno = 0;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
      int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
      if (fd < 0) {
        lastErrno = errno;
        continue;
      }
      sockutil::setReuseAddr(fd);
      sockutil::setCloseOnExec(fd);
      if (::bind(fd, ai->ai_addr, ai->ai_addrlen) != 0 || ::listen(fd, LISTEN_BACKLOG) != 0) {
        lastErrno = errno;
        ::close(fd);
        continue;
      }
      if (!sockutil::setNonBlocking(fd)) {
        lastErrno = errno;
        ::close(fd);
        continue;
      }
      fd_ = fd;
      boundPort_ = localPort(fd, port);
      ::freeaddrinfo(res);
      return true;
    }
    ::freeaddrinfo(res);
    err = "bind/listen failed: " + sockutil::errnoString(lastErrno ? lastErrno : errno);
    return false;
  }

  // Accept one pending connection. Returns the new fd (already non-blocking,
  // TCP_NODELAY, close-on-exec) or -1. An -1 with wouldBlock() set in `err` is
  // the ordinary "nothing waiting" case, not an error.
  int acceptPeer(std::string &peer, std::string &err) {
    if (fd_ < 0)
      return -1;
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    const int fd = ::accept(fd_, reinterpret_cast<struct sockaddr *>(&ss), &len);
    if (fd < 0) {
      err = sockutil::errnoString(errno);
      return -1;
    }
    if (!sockutil::setNonBlocking(fd)) {
      err = "set non-blocking: " + sockutil::errnoString(errno);
      ::close(fd);
      return -1;
    }
    sockutil::setNoDelay(fd);
    sockutil::setCloseOnExec(fd);
    peer = sockutil::numericAddress(reinterpret_cast<struct sockaddr *>(&ss), len);
    return fd;
  }

  void close() { sockutil::closeFd(fd_); }

  int fd() const { return fd_; }
  bool open() const { return fd_ >= 0; }
  uint16_t port() const { return boundPort_; }

private:
  static uint16_t localPort(int fd, uint16_t fallback) {
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (::getsockname(fd, reinterpret_cast<struct sockaddr *>(&ss), &len) != 0)
      return fallback;
    return sockutil::portOf(reinterpret_cast<struct sockaddr *>(&ss));
  }

  int fd_ = -1;
  uint16_t boundPort_ = 0;
};

} // namespace netplay
} // namespace sandcraft

#endif // MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_TCP_SERVER_HPP_
