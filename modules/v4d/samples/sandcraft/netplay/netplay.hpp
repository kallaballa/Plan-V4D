// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// Sandcraft netplay -- the entry point of the networked side of the demo.
//
// The design is in specification.md and the order it is built in is in
// roadmap.md, both next to this header. This file is the seam the pipeline
// keys on: the role the process runs as, and where a client is told to connect.
// Everything a sample needs before its first frame comes from here, and here
// takes it from argv (spec §16.6) -- or, on Android, where there is no argv,
// defaults to host (spec §21).
//
// M0 (roadmap) is scaffolding only: the role is parsed and reported but nothing
// opens a socket and the simulation still runs exactly as it does with no
// netplay code at all. The framing, transports and the net thread land in M1..M3
// behind this same NetConfig, so the sample's argument surface does not change
// again.
#ifndef MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_NETPLAY_HPP_
#define MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_NETPLAY_HPP_

#include <cstdint>
#include <cstring>
#include <string>

namespace sandcraft {
namespace netplay {

// Bumped on any wire-incompatible change (spec §23). A mismatch is a REJECT.
inline constexpr uint8_t PROTOCOL_VERSION = 1;

// The default listen port. Arbitrary, unassigned and easy to type; the host
// prints the endpoint it binds so a client can dial it (spec §15.4).
inline constexpr uint16_t DEFAULT_PORT = 45454;

enum class Role {
  Host,   // authoritative simulation, player 0 (spec §5)
  Client, // display/replica (spec §5)
};

// Everything the process needs to decide what it is before it draws a frame.
struct NetConfig {
  Role role = Role::Host;   // no network args => the demo as it always was
  std::string connect;      // client: "host" or "host:port"
  uint16_t port = DEFAULT_PORT; // host: bind port
  std::string name = "player";  // shown in the roster (spec §13.4)
  std::string token;        // optional join token; empty => open session
  bool selfTest = false;    // --net-selftest: in-process host==client (M2)
  bool help = false;        // --help: print usage and leave
};

namespace detail {

// Split "host[:port]" into its halves. A missing port keeps the default; a
// present one is parsed with std::stoi and clamped to the uint16 range. A
// malformed port is treated as absent rather than fatal -- the client will fail
// to connect with a clear "cannot resolve" log, which is a better error than a
// parse abort in main().
inline void splitEndpoint(const std::string &s, std::string &host, uint16_t &port) {
  const size_t colon = s.rfind(':');
  if (colon == std::string::npos) {
    host = s;
    return;
  }
  host = s.substr(0, colon);
  const std::string p = s.substr(colon + 1);
  if (p.empty())
    return;
  try {
    const int v = std::stoi(p);
    if (v > 0 && v <= 65535)
      port = uint16_t(v);
  } catch (...) {
    // leave port at its default
  }
}

} // namespace detail

// Parse the netplay half of a sample's command line. Unrecognised arguments are
// ignored here rather than rejected: this is a sample, and other subsystems may
// grow their own flags. Anything that begins with "--net" or is "help" is ours.
inline NetConfig parseArgs(int argc, char **argv) {
  NetConfig cfg;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i] ? argv[i] : "";
    auto next = [&](const char *what) -> std::string {
      return (i + 1 < argc && argv[i + 1]) ? std::string(argv[++i]) : std::string(what);
    };
    if (a == "--host") {
      cfg.role = Role::Host;
    } else if (a == "--serve" || a == "--connect") {
      cfg.role = Role::Client;
      cfg.connect = next("");
    } else if (a == "--port") {
      const std::string v = next("");
      try {
        const int p = std::stoi(v);
        if (p > 0 && p <= 65535)
          cfg.port = uint16_t(p);
      } catch (...) {
      }
    } else if (a == "--name") {
      cfg.name = next("player");
    } else if (a == "--token") {
      cfg.token = next("");
    } else if (a == "--net-selftest") {
      cfg.selfTest = true;
    } else if (a == "--help" || a == "-h") {
      cfg.help = true;
    }
  }
  // A --connect implies client even if it followed a --host; the explicit
  // connect string is the stronger statement of intent.
  if (!cfg.connect.empty())
    cfg.role = Role::Client;
  return cfg;
}

inline std::string endpointHost(const NetConfig &cfg) {
  std::string host;
  uint16_t port = cfg.port;
  detail::splitEndpoint(cfg.connect, host, port);
  return host;
}

inline uint16_t endpointPort(const NetConfig &cfg) {
  std::string host;
  uint16_t port = cfg.port;
  detail::splitEndpoint(cfg.connect, host, port);
  return port;
}

inline const char *usageText() {
  return "Sandcraft netplay options:\n"
         "  --host                 run as the authoritative host (default)\n"
         "  --connect HOST[:PORT]  run as a client and dial HOST\n"
         "  --port N               host listen port (default 45454)\n"
         "  --name NAME            player name shown in the roster\n"
         "  --token TOKEN          shared join token (empty => open)\n"
         "  --net-selftest         in-process host==client self test (M2)\n"
         "  -h, --help             this message\n";
}

} // namespace netplay
} // namespace sandcraft

#endif // MODULES_V4D_SAMPLES_SANDCRAFT_NETPLAY_NETPLAY_HPP_
