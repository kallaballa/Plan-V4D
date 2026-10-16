// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// The panel -- Buttons.hpp's agar GUI, as ImGui.
//
// The vertical button strip becomes a material palette, the grey bar along the
// bottom becomes the panel's sections, and the modal "about" window becomes a
// closable ImGui window. It is the only part of the demo that runs on the
// display thread, and the only part that touches ImGui; it reaches the
// simulation exclusively through the one-shot command flags on the Sandbox, so
// it never has to touch the grid itself.
#ifndef MODULES_V4D_SAMPLES_SANDCRAFT_PANEL_HPP_
#define MODULES_V4D_SAMPLES_SANDCRAFT_PANEL_HPP_

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include <opencv2/core.hpp>

#include "imgui.h"

#include "sandcraft_materials.hpp"
#include "sandcraft_sandbox.hpp"
#include "netplay/net_runtime.hpp"

namespace sandcraft {

// The palette is BGRA and ImVec4 is RGBA, so the same colour crosses over with
// its first two components swapped. Both live in the panel's reach, but this is
// where NanoVG stops and ImGui starts, so the conversion belongs next to the
// palette.
inline ImVec4 toImVec(const cv::Scalar &bgra) {
  return ImVec4(float(bgra[2]) / 255.0f, float(bgra[1]) / 255.0f,
                float(bgra[0]) / 255.0f, float(bgra[3]) / 255.0f);
}

// Black lettering on a light swatch, white on a dark one. The palette runs
// from near-black Void (60,60,60) to white Salt and Ice (255,255,255), so a
// single fixed text colour leaves one end of it unreadable.
inline ImVec4 contrastOn(const cv::Scalar &bgra) {
  const double lum = 0.2126 * bgra[2] + 0.7152 * bgra[1] + 0.0722 * bgra[0];
  return lum > 140.0 ? ImVec4(0, 0, 0, 1) : ImVec4(1, 1, 1, 1);
}

// The whole panel: the studio styling, the material palette, the brush
// controls, the emitters, the world readouts, the style section and the about
// window. Registered as the plan's imgui() node, so it runs once per frame on
// the display thread.
inline void drawPanel(Sandbox &sb) {
  using namespace ImGui;
  const Palette pal;
  // The panel drives the local player (always player 0). On a client this writes
  // the replica's player-0 brush/pen, which the net layer forwards as SET_BRUSH/
  // SET_PEN (spec §9.5); on the host it is the authority's own player.
  PlayerState &me = sb.players_[0];

  // ---- the studio: the panel dresses as the painting does -----------
  //
  // Prussian blue ground, chrome-yellow rules, cream lettering, and
  // selection in the yellow of the wheat. The module's own NanoVG
  // context is where the world is painted; the panel is ImGui and
  // therefore styled through the style stack, pushed once for the
  // duration of both windows and popped after the last End().
  const bool styled = sb.local_.vanGogh;
  int pushedColors = 0;
  int pushedVars = 0;
  if (styled) {
    const auto push = [&](ImGuiCol_ idx, ImVec4 c) {
      PushStyleColor(idx, c);
      ++pushedColors;
    };
    push(ImGuiCol_WindowBg, ImVec4(0.055f, 0.075f, 0.145f, 0.955f));
    push(ImGuiCol_Border, ImVec4(0.91f, 0.66f, 0.14f, 0.55f));
    push(ImGuiCol_TitleBg, ImVec4(0.040f, 0.055f, 0.110f, 1.0f));
    push(ImGuiCol_TitleBgActive, ImVec4(0.11f, 0.14f, 0.30f, 1.0f));
    push(ImGuiCol_Text, ImVec4(0.93f, 0.90f, 0.82f, 1.0f));
    push(ImGuiCol_TextDisabled, ImVec4(0.58f, 0.60f, 0.70f, 1.0f));
    push(ImGuiCol_FrameBg, ImVec4(0.09f, 0.11f, 0.22f, 1.0f));
    push(ImGuiCol_FrameBgHovered, ImVec4(0.16f, 0.19f, 0.36f, 1.0f));
    push(ImGuiCol_FrameBgActive, ImVec4(0.22f, 0.26f, 0.46f, 1.0f));
    push(ImGuiCol_Button, ImVec4(0.11f, 0.14f, 0.28f, 1.0f));
    push(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.24f, 0.44f, 1.0f));
    push(ImGuiCol_ButtonActive, ImVec4(0.28f, 0.33f, 0.56f, 1.0f));
    push(ImGuiCol_SliderGrab, ImVec4(0.89f, 0.66f, 0.16f, 1.0f));
    push(ImGuiCol_SliderGrabActive, ImVec4(1.00f, 0.78f, 0.28f, 1.0f));
    push(ImGuiCol_CheckMark, ImVec4(0.91f, 0.66f, 0.14f, 1.0f));
    push(ImGuiCol_Separator, ImVec4(0.45f, 0.42f, 0.30f, 0.60f));
    push(ImGuiCol_Header, ImVec4(0.16f, 0.19f, 0.36f, 1.0f));
    PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
    PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
    PushStyleVar(ImGuiStyleVar_GrabRounding, 3.0f);
    pushedVars += 3; // style vars, popped on their own stack below
  }

  // ---- the material palette: agar's button strip, as swatches ----
  //
  // One window. Two ImGui windows opened with a plain Begin() and no
  // position both land on the default spot and stack on top of each
  // other, and because they are drawn in call order the second one
  // hides the first: the panel's sections and the readouts underneath
  // it are both unusable, and neither can be moved out from under the
  // other. So the readouts are a section of the panel rather than a
  // second window.
  //
  // First-use positioning rather than Always: the panel floats over the
  // world, and a panel that snaps back to the corner every frame can
  // never be dragged out of the way of whatever it is covering.
  SetNextWindowPos(ImVec2(14, 14), ImGuiCond_FirstUseEver);
  if (Begin("Sandcraft", &sb.local_.panelOpen)) {
    Text("Material");
    Separator();
    // The swatch is a Button carrying the material's colour, not a
    // Selectable: Selectable paints its background from the style
    // colours only in its hovered and selected states, so a
    // PushStyleColor around it leaves an unselected, unhovered entry
    // looking exactly like the window background -- a grid of labels
    // with no swatches and no indication of what is selected. A Button
    // fills itself with ImGuiCol_Button unconditionally, so the colour
    // always shows, and a marker below it marks the current brush.
    //
    // Seven columns, because the table sizes to its widest label and
    // the names run to "Water spout": at four or five columns the
    // panel is wide enough to sit on top of the leftmost emitter.
    BeginTable("brushes", 7, ImGuiTableFlags_SizingFixedFit |
                                ImGuiTableFlags_NoSavedSettings);
    for (size_t bi = 0; bi < kBrushes.size(); ++bi) {
      const BrushInfo &b = kBrushes[bi];
      const cv::Scalar c = pal[b.type];
      const bool selected = me.brush == b.type;
      TableNextColumn();
      PushStyleColor(ImGuiCol_Button, toImVec(c));
      PushStyleColor(ImGuiCol_ButtonHovered,
                     toImVec(litFace(c, 0.30f)));
      PushStyleColor(ImGuiCol_ButtonActive, toImVec(litFace(c, 0.55f)));
      char id[32];
      std::snprintf(id, sizeof(id), "##brush%zu", bi);
      if (Button(id, ImVec2(46.0f, 22.0f)))
        me.brush = b.type;
      PopStyleColor(3);
      // The selection marker is drawn by hand rather than through
      // FrameBorderSize/FrameBorderColor: the marker has to survive
      // both the theme (which paints a frame border in a colour that
      // disappears against a light swatch) and the fact that the
      // vendored ImGui has no ImGuiCol_FrameBorder to push at all.
      if (selected) {
        const ImVec2 lo = GetItemRectMin(), hi = GetItemRectMax();
        GetWindowDrawList()->AddRectFilled(
            ImVec2(lo.x, hi.y + 1.0f), ImVec2(hi.x, hi.y + 4.0f),
            IM_COL32(255, 255, 255, 230));
      }
      // The name under the swatch, rather than inside it: the swatches
      // are 22px tall and the names run to "Water spout".
      PushStyleColor(ImGuiCol_Text, contrastOn(c));
      TextUnformatted(b.name);
      PopStyleColor(1);
    }
    EndTable();

    Separator();
    Text("Brush");
    SliderInt("Pen size##pen", &me.penSize, 1, 32);
    Checkbox("Density swaps##swaps", &sb.world_.particleSwaps);
    TextDisabled(
        "  let liquids sink through materials denser than "
        "themselves");
    if (Button("Random strokes"))
      sb.world_.randomizeRequested = true;
    SameLine();
    if (Button("Clear##clear"))
      sb.world_.clearRequested = true;

    Separator();
    Text("Emitters");
    for (size_t i = 0; i < sb.world_.emitters.size(); ++i) {
      Emitter &e = sb.world_.emitters[i];
      const cv::Scalar c = pal[e.type];
      char toggle[64], slider[64];
      std::snprintf(toggle, sizeof(toggle), "%s##emit%zu", e.name, i);
      std::snprintf(slider, sizeof(slider), "##density%zu", i);
      PushStyleColor(ImGuiCol_Button, toImVec(c));
      PushStyleColor(ImGuiCol_ButtonHovered,
                     toImVec(litFace(c, 0.30f)));
      PushStyleColor(ImGuiCol_ButtonActive, toImVec(litFace(c, 0.55f)));
      PushStyleColor(ImGuiCol_Text, contrastOn(c));
      const bool pressed = Button(toggle);
      PopStyleColor(4);
      if (pressed)
        e.on = !e.on;
      SameLine();
      TextColored(e.on ? ImVec4(1, 1, 1, 1) : ImVec4(0.5f, 0.5f, 0.5f, 1),
                 "%s", e.on ? "on" : "off");
      PushItemWidth(130.0f);
      SliderFloat(slider, &e.density, 0.05f, 1.0f, "%.2f");
      PopItemWidth();
    }

    Separator();
    Text("World");
    Text("%.0f fps   %d particles   frame %d", sb.local_.fps,
         sb.particleCount_, sb.simFrames_);
    Text("%dx%d cells   %s, size %d", sb.width_, sb.height_,
         brushName(me.brush), me.penSize);
    Checkbox("Paused##paused", &sb.world_.paused);
    SameLine();
    if (Button("About##about"))
      sb.local_.helpOpen = true;

    Separator();
    Text("Style");
    Checkbox("Van Gogh##style", &sb.local_.vanGogh);
    if (sb.local_.vanGogh) {
      SliderFloat("Impasto##impasto", &sb.local_.impasto, 0.4f, 2.0f);
      TextDisabled("  how thickly the paint is laid on");
      TextDisabled(
          "  on: fields of brush strokes, a night sky with eleven stars,");
      TextDisabled("  glare on the fire and the water, and the weave of the");
      TextDisabled("  canvas under all of it.");
    } else {
      TextDisabled("  the automaton as a diagram: banded cells, one colour");
      TextDisabled("  per material, lit from above");
    }

    // ---- the network: role, endpoint, and what crossed the wire ----
    //
    // The net thread keeps plain atomics for exactly this (spec §16.6); the
    // panel only ever reads them, never blocks on the socket. When the process
    // was not asked to network this section says so rather than pretending.
    Separator();
    Text("Network");
    {
      netplay::NetRuntime &net = netplay::netRuntime();
      if (!net.active()) {
        TextDisabled("  single player: no socket");
        TextDisabled("  start with --host, or --connect HOST to join");
      } else {
        const netplay::NetStats ns = net.stats();
        Text("%s", net.host() ? "host (authoritative)" : "client (replica)");
        if (net.host())
          Text("listening  port %u", unsigned(net.boundPort()));
        else
          Text("player %u  %s", unsigned(ns.playerId),
               !ns.connected ? "reconnecting"
                             : (ns.handshakeDone ? "joined" : "joining"));
        Text("%llu ticks   %d peers", static_cast<unsigned long long>(ns.tick),
             ns.peers);
        Text("in %.1f KiB   out %.1f KiB", double(ns.bytesIn) / 1024.0,
             double(ns.bytesOut) / 1024.0);
        // The host produces keyframes/deltas and the client consumes them; show
        // the pair that belongs to this role (a replica's own counters are zero).
        Text("keyframes %llu   deltas %llu   drops %llu",
             static_cast<unsigned long long>(net.host() ? ns.keyframesOut : ns.keyframesIn),
             static_cast<unsigned long long>(net.host() ? ns.deltasOut : ns.deltasIn),
             static_cast<unsigned long long>(ns.drops));
        if (!net.host())
          Text("frames in %llu", static_cast<unsigned long long>(ns.framesIn));
      }
    }
  }
  End();

  // ---- the about window: the original's modal help dialog ----
  //
  // Begin() is called unconditionally inside the `if`, never as part of
  // it. Written the other way round -- `if (open && Begin(..., &open))`
  // with a separate `if (open) End();` -- the close button clears
  // `open` from inside the window's own contents, so by the time the
  // matching End() is reached the flag is false and the window is left
  // unclosed: ImGui keeps a begun window on its stack forever, and
  // every later window is pushed one level too deep.
  if (sb.local_.helpOpen) {
    SetNextWindowPos(ImVec2(340, 110), ImGuiCond_Always);
    SetNextWindowSize(ImVec2(430.0f, 0.0f), ImGuiCond_Always);
    Begin("About Sandcraft", &sb.local_.helpOpen);
    TextWrapped(
        "A falling-sand sandbox. Materials are laid down with the "
        "brush and then obey the rules of a cellular automaton: sand "
        "piles, water finds a level, fire climbs, salt melts ice, rust "
        "eats iron and plants drink water.");
    Separator();
    Text("Keymap");
    Separator();
    TextDisabled("drag                    paint with the selected brush");
    TextDisabled("wheel, LEFT/RIGHT       previous / next brush");
    TextDisabled("SHIFT + wheel, UP/DOWN  double / halve the pen");
    TextDisabled("CTRL + wheel, -/=       emitter density");
    TextDisabled("ALT + wheel             all emitters on / off");
    TextDisabled("ENTER                  all emitters on / off");
    TextDisabled("BACKSPACE               the eraser");
    TextDisabled("SPACE                   pause the simulation");
    TextDisabled("TAB                    show / hide this panel");
    TextDisabled("R                       a few random strokes");
    TextDisabled("DELETE                  clear the world");
    Separator();
    TextWrapped("Ported from the SDL2 + agar original to Plan-V4D: "
                "NanoVG for the world, ImGui for the panel, and a "
                "host-authoritative netplay layer (TCP) for two to four "
                "players. Paint is applied by the host; clients render "
                "the replica.");
    End();
  }

  // Pop the studio's style now that both windows are closed -- after the
  // last End(), so the About dialog is styled too. Style colours and
  // style vars sit on two separate ImGui stacks, so each is popped with
  // its own count; an imbalance here raises a 'Missing PopStyle*' assert
  // every single frame.
  if (styled) {
    PopStyleColor(pushedColors);
    PopStyleVar(pushedVars);
  }

  // Mirror ImGui's capture flags and modifiers for the worker: the
  // ImGui context only exists on this thread, so the input node cannot
  // ask ImGui itself.
  const ImGuiIO &io = ImGui::GetIO();
  sb.guiWantsMouse_ = io.WantCaptureMouse;
  sb.guiWantsKeyboard_ = io.WantCaptureKeyboard;
  sb.shiftDown_ = io.KeyShift;
  sb.ctrlDown_ = io.KeyCtrl;
  sb.altDown_ = io.KeyAlt;
}

} // namespace sandcraft

#endif // MODULES_V4D_SAMPLES_SANDCRAFT_PANEL_HPP_
