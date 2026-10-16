// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level
// directory of this distribution and at http://opencv.org/license.html.
// Copyright Amir Hassan (kallaballa) <amir@viel-zu.org>
//
// Committed source for the headless-interaction helper described in
// roadmap.md §2. It is a test tool, not part of the sample: build it with
// build-wlr-pointer.sh (which generates the protocol bindings from the
// committed wlr-virtual-pointer-unstable-v1.xml). The protocol XML is
// Copyright © 2019 Josef Gajdusek, MIT-licensed (see its own header).
/*
 * wlr-pointer -- minimal Wayland virtual-pointer driver for headless UI tests.
 *
 * wlrctl can move/click a cursor but cannot *hold* a button, so it cannot drag.
 * Xdotool needs X11 (Plan-V4D's GLFW is a Wayland-only build) and ydotool needs
 * /dev/uinput which is root-only.  This helper speaks the wlroots
 * zwlr_virtual_pointer_manager_v1 protocol directly through the compositor, so
 * it needs no privileges, no daemon and no uinput device.
 *
 * It reads a small command script on stdin and drives ONE virtual pointer for
 * the whole script (avoids per-invocation races and is far faster):
 *
 *   abs X Y              absolute cursor move (uses the real output extents)
 *   move DX DY           relative cursor move
 *   down [BTN]           press a button (default left)
 *   up   [BTN]           release a button (default left)
 *   click [BTN]          press+release
 *   scroll DY [DX]       wheel scroll
 *   sleep MS             pause between events
 *
 * every command is terminated with frame() and flushed, so the compositor
 * sees complete, well-formed event sequences.
 */
#include <linux/input-event-codes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

static struct zwlr_virtual_pointer_manager_v1 *manager = NULL;
static struct wl_seat *seat = NULL;

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t version) {
  (void)data;
  if (!strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name)) {
    uint32_t v = version < 2 ? version : 2;
    manager = wl_registry_bind(reg, name,
                               &zwlr_virtual_pointer_manager_v1_interface, v);
  } else if (!strcmp(iface, wl_seat_interface.name)) {
    seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
  }
}
static void registry_global_remove(void *data, struct wl_registry *reg,
                                   uint32_t name) {
  (void)data;
  (void)reg;
  (void)name;
}
static const struct wl_registry_listener registry_listener = {
    registry_global, registry_global_remove};

static uint32_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static uint32_t button_code(const char *name) {
  if (!name || !*name) return BTN_LEFT;
  if (!strcmp(name, "left")) return BTN_LEFT;
  if (!strcmp(name, "right")) return BTN_RIGHT;
  if (!strcmp(name, "middle")) return BTN_MIDDLE;
  if (!strcmp(name, "side")) return BTN_SIDE;
  if (!strcmp(name, "extra")) return BTN_EXTRA;
  return (uint32_t)strtoul(name, NULL, 0);
}

int main(int argc, char **argv) {
  int extent_w = 1920, extent_h = 1080;
  int settle_ms = 10;   /* pause after each command */
  int startup_ms = 60;  /* pause after the virtual pointer is created */

  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--extent") && i + 2 < argc) {
      extent_w = atoi(argv[++i]);
      extent_h = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--settle") && i + 1 < argc) {
      settle_ms = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--startup") && i + 1 < argc) {
      startup_ms = atoi(argv[++i]);
    } else {
      fprintf(stderr,
              "usage: wlr-pointer [--extent W H] [--settle MS] [--startup MS]\n"
              "commands on stdin: abs X Y | move DX DY | down [BTN] | up [BTN] |\n"
              "                   click [BTN] | scroll DY [DX] | sleep MS\n");
      return 2;
    }
  }

  struct wl_display *dpy = wl_display_connect(NULL);
  if (!dpy) {
    fprintf(stderr, "wlr-pointer: cannot connect to the Wayland display\n");
    return 1;
  }
  struct wl_registry *reg = wl_display_get_registry(dpy);
  wl_registry_add_listener(reg, &registry_listener, NULL);
  if (wl_display_roundtrip(dpy) < 0 || !manager) {
    fprintf(stderr,
            "wlr-pointer: compositor lacks zwlr_virtual_pointer_manager_v1\n");
    return 3;
  }
  struct zwlr_virtual_pointer_v1 *vp =
      zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager, seat);
  wl_display_flush(dpy);
  if (startup_ms > 0) usleep((useconds_t)startup_ms * 1000);

  char line[256];
  while (fgets(line, sizeof(line), stdin)) {
    char cmd[32] = {0};
    double a = 0, b = 0;
    char who[32] = {0};
    if (sscanf(line, "%31s", cmd) != 1) continue;

    if (!strcmp(cmd, "abs")) {
      sscanf(line, "%*s %lf %lf", &a, &b);
      long x = (long)a, y = (long)b;
      if (x < 0) x = 0;
      if (y < 0) y = 0;
      if (x > extent_w - 1) x = extent_w - 1;
      if (y > extent_h - 1) y = extent_h - 1;
      zwlr_virtual_pointer_v1_motion_absolute(
          vp, now_ms(), (uint32_t)x, (uint32_t)y, (uint32_t)extent_w,
          (uint32_t)extent_h);
    } else if (!strcmp(cmd, "move")) {
      sscanf(line, "%*s %lf %lf", &a, &b);
      zwlr_virtual_pointer_v1_motion(vp, now_ms(), wl_fixed_from_double(a),
                                     wl_fixed_from_double(b));
    } else if (!strcmp(cmd, "down") || !strcmp(cmd, "up") ||
               !strcmp(cmd, "click")) {
      sscanf(line, "%*s %31s", who);
      uint32_t btn = button_code(who);
      uint32_t st = strcmp(cmd, "up") ? WL_POINTER_BUTTON_STATE_PRESSED
                                      : WL_POINTER_BUTTON_STATE_RELEASED;
      zwlr_virtual_pointer_v1_button(vp, now_ms(), btn, st);
      if (!strcmp(cmd, "click")) {
        zwlr_virtual_pointer_v1_frame(vp);
        wl_display_flush(dpy);
        usleep(40000);
        zwlr_virtual_pointer_v1_button(vp, now_ms(), btn,
                                       WL_POINTER_BUTTON_STATE_RELEASED);
      }
    } else if (!strcmp(cmd, "scroll")) {
      sscanf(line, "%*s %lf %lf", &a, &b);
      zwlr_virtual_pointer_v1_axis_source(vp, WL_POINTER_AXIS_SOURCE_WHEEL);
      if (a != 0)
        zwlr_virtual_pointer_v1_axis_discrete(
            vp, now_ms(), WL_POINTER_AXIS_VERTICAL_SCROLL,
            wl_fixed_from_double(a), (int)a);
      if (b != 0)
        zwlr_virtual_pointer_v1_axis_discrete(
            vp, now_ms(), WL_POINTER_AXIS_HORIZONTAL_SCROLL,
            wl_fixed_from_double(b), (int)b);
    } else if (!strcmp(cmd, "sleep")) {
      sscanf(line, "%*s %lf", &a);
      if (a > 0) usleep((useconds_t)a * 1000);
      continue; /* nothing to frame */
    } else {
      fprintf(stderr, "wlr-pointer: unknown command '%s'\n", cmd);
      continue;
    }

    zwlr_virtual_pointer_v1_frame(vp);
    wl_display_flush(dpy);
    if (settle_ms > 0) usleep((useconds_t)settle_ms * 1000);
  }

  wl_display_flush(dpy);
  zwlr_virtual_pointer_v1_destroy(vp);
  zwlr_virtual_pointer_manager_v1_destroy(manager);
  if (seat) wl_seat_destroy(seat);
  wl_display_disconnect(dpy);
  return 0;
}
