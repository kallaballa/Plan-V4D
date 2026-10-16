// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// Small POSIX-socket helpers shared by the TCP transport (specification.md
// §15.1, roadmap M3).
//
// Nothing here knows about the protocol, the queues or the session: it is the
// socket vocabulary the listener and the connector are built from -- make a
// non-blocking socket, ask for the options the transport relies on, turn an
// address into a printable "ip:port", and resolve a name.
//
// Two details are deliberate and easy to get wrong:
//
//   * SIGPIPE is ignored process-wide. A TCP write to a peer that has closed
//     its end raises SIGPIPE and, unhandled, terminates the process; a sandbox
//     that dies because one client closed its window is not acceptable (spec
//     §18.1). Every send() additionally passes MSG_NOSIGNAL, so the ignore is
//     belt-and-braces rather than the only guard.
//   * Every socket is non-blocking. The whole net thread is a poll loop (spec
//     §15.1), so a socket that can block would stall every peer behind it.
//     MSG_DONTWAIT is not used: the fd itself carries O_NONBLOCK, so accept(),
//     connect(), recv() and send() all return EAGAIN instead of blocking.
#ifndef MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_SOCKET_UTIL_HPP_
#define MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_SOCKET_UTIL_HPP_

#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace sandcraft {
namespace netplay {
namespace sockutil {

inline std::string errnoString(int e) { return std::string(std::strerror(e)); }

// Ignore SIGPIPE exactly once per process. Safe to call from every start().
inline void ignoreSigpipe() {
  static std::once_flag once;
  std::call_once(once, [] {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    ::sigaction(SIGPIPE, &sa, nullptr);
  });
}

inline bool setNonBlocking(int fd) {
  const int fl = ::fcntl(fd, F_GETFL, 0);
  if (fl < 0)
    return false;
  return ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

inline bool setCloseOnExec(int fd) {
  const int fl = ::fcntl(fd, F_GETFD, 0);
  if (fl < 0)
    return false;
  return ::fcntl(fd, F_SETFD, fl | FD_CLOEXEC) == 0;
}

// Allow a restart to bind the same port immediately rather than waiting out
// TIME_WAIT -- the normal case while iterating on the demo.
inline bool setReuseAddr(int fd) {
  int one = 1;
  return ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0;
}

// Deltas are small and latency matters far more than packet count (spec §15.1),
// so Nagle is off: waiting to coalesce a 40-byte delta would add a round trip
// for nothing.
inline bool setNoDelay(int fd) {
  int one = 1;
  return ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0;
}

inline bool wouldBlock(int e) {
  return e == EAGAIN || e == EWOULDBLOCK || e == EINTR;
}

inline void closeFd(int &fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

// The port from a bound or connected address, in host order.
inline uint16_t portOf(const struct sockaddr *sa) {
  if (!sa)
    return 0;
  if (sa->sa_family == AF_INET)
    return ntohs(reinterpret_cast<const struct sockaddr_in *>(sa)->sin_port);
  if (sa->sa_family == AF_INET6)
    return ntohs(reinterpret_cast<const struct sockaddr_in6 *>(sa)->sin6_port);
  return 0;
}

// "ip:port", numeric. The peer string is for logs and the panel only, so it is
// never reverse-resolved: a DNS lookup in the net thread would block the loop.
inline std::string numericAddress(const struct sockaddr *sa, socklen_t len) {
  char host[INET6_ADDRSTRLEN] = {0};
  const uint16_t port = portOf(sa);
  if (sa->sa_family == AF_INET) {
    ::inet_ntop(AF_INET, &reinterpret_cast<const struct sockaddr_in *>(sa)->sin_addr,
                host, sizeof(host));
  } else if (sa->sa_family == AF_INET6) {
    ::inet_ntop(AF_INET6, &reinterpret_cast<const struct sockaddr_in6 *>(sa)->sin6_addr,
                host, sizeof(host));
  } else {
    ::inet_ntop(sa->sa_family, sa, host, sizeof(host));
  }
  (void)len;
  return std::string(host) + ":" + std::to_string(port);
}

// Resolve a listen or connect address. `passive` selects the bind semantics
// (AI_PASSIVE, so an empty host means "any"). Returns a getaddrinfo list the
// caller must freeaddrinfo(); nullptr means the name did not resolve, with
// `err` filled in.
inline struct addrinfo *resolve(const std::string &host, uint16_t port,
                                bool passive, std::string &err) {
  const std::string portStr = std::to_string(port);
  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (passive)
    hints.ai_flags = AI_PASSIVE;
  struct addrinfo *res = nullptr;
  const char *node = host.empty() ? nullptr : host.c_str();
  const int rc = ::getaddrinfo(node, portStr.c_str(), &hints, &res);
  if (rc != 0) {
    err = std::string("getaddrinfo(") + (host.empty() ? "*" : host) + "): " +
          ::gai_strerror(rc);
    return nullptr;
  }
  return res;
}

} // namespace sockutil
} // namespace netplay
} // namespace sandcraft

#endif // MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_SOCKET_UTIL_HPP_
