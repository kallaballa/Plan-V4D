# Sandcraft netplay

Host-authoritative networking for the `example_v4d_sandcraft` sample: one instance runs the
cellular automaton, up to three clients replicate it and send their input back.

* [`specification.md`](specification.md) — the authoritative design (roles, state, tick model,
  input model, delta codec, wire protocol, concurrency contract, connectivity, acceptance).
* [`roadmap.md`](roadmap.md) — the ordered milestones, their deliverables, tests and risks, and
  the milestone log recording what is actually done.

`netplay.hpp` is the seam: it turns the command line into a `NetConfig` (role, endpoint, name,
token). M0 provides only that scaffolding; the transports, the net thread and the graph nodes
land in later milestones behind the same config.

Nothing here opens a socket yet. With no arguments the sample still runs exactly as it did
before this directory existed (specification §4.3).
