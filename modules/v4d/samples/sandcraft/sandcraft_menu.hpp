// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The startup dialog -- the first thing the sample shows.
//
// Upstream Sandcraft paired two players over a websocket signalling server; the
// port dropped the networking and grew a host-authoritative TCP layer instead
// (netplay/). Until now the role was decided entirely on the command line: with
// no argument the demo was single-player, and a host or a join needed --host or
// --connect. This dialog is the interactive front for that decision, so a bare
// `example_v4d_sandcraft` asks instead of silently choosing.
//
// It is an ImGui modal popup (the shadertoy-editor file dialog is the in-tree
// precedent) drawn from the display thread inside the plan's gui()/imgui()
// transaction, so it appears from the first frame. The choice is applied by
// reconfiguring the process-wide NetRuntime and starting its thread; because the
// host/client graph branch reads a flag on the *shared* Sandbox (not a
// per-plan-instance member -- the display thread and the compute worker are
// different instances), the worker picks the new arm up on the next frame.
//
// The dialog is only shown when NetConfig::menu is set. An explicit role
// (--host, --connect, --single-player, --no-menu, or either self-test) clears
// it, so scripts and the netplay self-tests are unaffected.
#ifndef MODULES_V4D_SAMPLES_SANDCRAFT_MENU_HPP_
#define MODULES_V4D_SAMPLES_SANDCRAFT_MENU_HPP_

#include <cstring>
#include <iostream>
#include <string>

#include <opencv2/core.hpp>

#include "imgui.h"

#include "sandcraft_sandbox.hpp"
#include "netplay/net_runtime.hpp"
#include "netplay/netplay.hpp"

namespace sandcraft {

// The popup's ImGui id. A plain name, not a "label##id", so OpenPopup() and
// BeginPopupModal() cannot disagree about what they are addressing.
inline constexpr const char *kStartupPopup = "Sandcraft Startup";

// Configure the net runtime for @p cfg and start its thread, then point the
// shared host/client flag at the role that took hold. On a hard start failure
// the runtime is rolled back to the offline single-player defaults, so the
// graph never switches to the client arm with no session behind it; the error
// is surfaced in the dialog and the menu stays up.
//
// Called from the display thread inside the imgui() transaction, so the write
// to hostMode_ is already under the sandbox mutex. start() is idempotent and
// guarded by its own lifecycle mutex, so a single commit is all that is needed.
inline bool commitChoice(Sandbox &sb, netplay::NetConfig cfg) {
  cfg.menu = false; // the choice has been made; never show the dialog again
  netplay::netRuntime().configure(cfg);

  std::string err;
  if (!netplay::netRuntime().start(err)) {
    netplay::NetConfig offline;
    offline.role = netplay::Role::Host;
    offline.enabled = false;
    offline.menu = false;
    netplay::netRuntime().configure(offline);
    sb.hostMode_ = true;
    sb.menu_.error = "could not start: " + err;
    std::cerr << "sandcraft: startup choice failed: " << err << std::endl;
    return false;
  }

  sb.hostMode_ = netplay::netRuntime().host();
  sb.menu_.error.clear();
  sb.menu_.open = false;
  // One machine-readable line mirroring main()'s "net role=" line, so a harness
  // can tell what the dialog settled on (the parseArgs line only says "startup
  // menu", it predates the choice).
  std::cerr << "sandcraft: startup choice role="
            << (sb.hostMode_ ? "host" : "client")
            << (cfg.enabled ? "" : " (single player)");
  if (cfg.enabled) {
    if (sb.hostMode_)
      std::cerr << " listening=" << netplay::netRuntime().boundPort();
    else
      std::cerr << " connect=" << cfg.connect;
  }
  std::cerr << std::endl;
  return true;
}

// Draw the startup modal while it is open. No-op once a choice was committed
// (or when the process was given an explicit role). Must be called from the
// display thread's ImGui pass.
inline void drawStartupMenu(Sandbox &sb) {
  using namespace ImGui;

  // An explicit role on the command line disables the dialog entirely.
  if (!netplay::netRuntime().config().menu) {
    sb.menu_.open = false;
    return;
  }
  if (!sb.menu_.open)
    return;

  // OpenPopup() has to precede the matching BeginPopupModal() exactly once;
  // afterwards the popup stays open across frames until CloseCurrentPopup().
  if (sb.menu_.openPopupRequested) {
    OpenPopup(kStartupPopup);
    sb.menu_.openPopupRequested = false;
  }

  SetNextWindowPos(GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                   ImVec2(0.5f, 0.5f));
  // No close button: the player has to make a choice (single-player is one
  // button away, so nothing is forced). NoEscape likewise: letting Escape
  // dismiss the popup would leave menu_.open set with no popup on screen, and
  // the process stuck in "menu" mode with no role and no way back.
  if (!BeginPopupModal(kStartupPopup, nullptr,
                       ImGuiWindowFlags_AlwaysAutoResize |
                           ImGuiWindowFlags_NoEscape))
    return;

  if (sb.menu_.page == 0) {
    TextUnformatted("Welcome to Sandcraft");
    TextDisabled("A falling-sand sandbox. Play alone, host a game, or join one.");
    Separator();

    if (Button("Single player", ImVec2(230.0f, 0.0f))) {
      netplay::NetConfig cfg = netplay::netRuntime().config();
      cfg.role = netplay::Role::Host;
      cfg.enabled = false;
      if (commitChoice(sb, cfg))
        CloseCurrentPopup();
    }
    if (Button("Multiplayer", ImVec2(230.0f, 0.0f))) {
      sb.menu_.page = 1;
      sb.menu_.focusHost = true;
    }
  } else {
    TextUnformatted("Multiplayer");
    Separator();

    if (Button("Host a game", ImVec2(230.0f, 0.0f))) {
      // Listen on the configured port (default 45454); the panel shows the
      // bound endpoint once the socket is up.
      netplay::NetConfig cfg = netplay::netRuntime().config();
      cfg.role = netplay::Role::Host;
      cfg.enabled = true;
      if (commitChoice(sb, cfg))
        CloseCurrentPopup();
    }

    Separator();
    TextUnformatted("Join a game");
    TextDisabled("Host name or address (optionally host:port)");
    if (sb.menu_.focusHost) {
      // The host field is where the pointer should be when the page appears;
      // this has to sit immediately before the InputText it applies to.
      SetKeyboardFocusHere();
      sb.menu_.focusHost = false;
    }
    const bool connectEntered =
        InputText("##host", sb.menu_.host, sizeof(sb.menu_.host),
                  ImGuiInputTextFlags_EnterReturnsTrue);
    if (Button("Connect", ImVec2(230.0f, 0.0f)) || connectEntered) {
      if (sb.menu_.host[0] == '\0') {
        sb.menu_.error = "enter a host name";
      } else {
        netplay::NetConfig cfg = netplay::netRuntime().config();
        cfg.role = netplay::Role::Client;
        cfg.enabled = true;
        cfg.connect = sb.menu_.host;
        if (commitChoice(sb, cfg))
          CloseCurrentPopup();
      }
    }
    if (Button("Back", ImVec2(230.0f, 0.0f)))
      sb.menu_.page = 0;
  }

  if (!sb.menu_.error.empty()) {
    Separator();
    TextColored(ImVec4(0.94f, 0.40f, 0.40f, 1.0f), "%s",
                sb.menu_.error.c_str());
  }

  EndPopup();
}

} // namespace sandcraft

#endif // MODULES_V4D_SAMPLES_SANDCRAFT_MENU_HPP_
