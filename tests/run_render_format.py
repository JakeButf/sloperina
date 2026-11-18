#!/usr/bin/env python3
"""Compile the renderer's format/cache helpers for host tests; no game or NVN needed."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    # The selected helpers have no platform dependencies. Keep their actual bodies;
    # the harness provides only metadata, so allocation/fence changes cannot slip in.
    pattern = r"^static (?:int|bool) " + re.escape(name) + r"\([^)]*\)\n\{.*?^\}"
    match = re.search(pattern, source, re.MULTILINE | re.DOTALL)
    if not match:
        raise ValueError(f"Cannot find renderer helper {name}")
    return match.group()


HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "gfx.h"
#include "nvnapi.h"
static uint32_t drawn_hash[GFX_PANELS][SLOTS], readback_present[GFX_PANELS][SLOTS];
static int drawn_w[GFX_PANELS][SLOTS], drawn_h[GFX_PANELS][SLOTS];
static bool drawn_native[GFX_PANELS][SLOTS];
static nvn_region_t readback_region[GFX_PANELS][SLOTS];
static int swapchain_format, bytes_per_pixel, clean_w, clean_h;
static volatile bool clean_wanted;
static bool clean_valid;
'''
CASES = r'''
static void seed(int format) {
  swapchain_format = format;
  bytes_per_pixel = format_bpp(format);
  clean_w = 2560; clean_h = 1440;
  clean_valid = clean_wanted = true;
  for (int p = 0; p < GFX_PANELS; ++p) for (int s = 0; s < SLOTS; ++s) {
    drawn_hash[p][s] = 0xabcdefu + p + s;
    drawn_w[p][s] = 100; drawn_h[p][s] = 200; drawn_native[p][s] = true;
    readback_region[p][s] = (nvn_region_t){10, 20, 0, 100, 200, 1};
    readback_present[p][s] = 123;
  }
}
static void cached(bool present) {
  assert(clean_valid == present && clean_wanted == present);
  for (int p = 0; p < GFX_PANELS; ++p) for (int s = 0; s < SLOTS; ++s) {
    assert(drawn_hash[p][s] == (present ? 0xabcdefu + p + s : 0));
    assert(drawn_w[p][s] == (present ? 100 : 0));
    assert(drawn_h[p][s] == (present ? 200 : 0));
    assert(drawn_native[p][s] == present);
    nvn_region_t expected = present ? (nvn_region_t){10, 20, 0, 100, 200, 1} : (nvn_region_t){0};
    assert(memcmp(&readback_region[p][s], &expected, sizeof(expected)) == 0);
    assert(readback_present[p][s] == (present ? 123u : 0));
  }
}
int main(void) {
  const int formats[] = {FMT_RGBA8, FMT_RGBX8_SRGB, FMT_RGBA8_SRGB, FMT_RGB10A2, FMT_RGBA16F};
  const int bpp[] = {4, 4, 4, 4, 8};
  for (unsigned a = 0; a < 5; ++a) for (unsigned b = 0; b < 5; ++b) {
    seed(formats[a]);
    bool accepted = bpp[a] == bpp[b];
    assert(overlay_accept_format(formats[b]) == accepted);
    assert(swapchain_format == formats[accepted ? b : a] && bytes_per_pixel == bpp[a]);
    cached(!accepted || a == b);
  }
  const int unsupported[] = {0, -1, 38, 999};
  for (unsigned a = 0; a < 5; ++a) for (unsigned b = 0; b < 4; ++b) {
    seed(formats[a]);
    assert(!overlay_accept_format(unsupported[b]));
    assert(swapchain_format == formats[a] && bytes_per_pixel == bpp[a]);
    cached(true);
  }
  /* A held frame rejects stale pixels before draw_overlay accepts a new format. */
  seed(FMT_RGBA8);
  assert(clean_target_matches(2560, 1440, FMT_RGBA8));
  assert(!clean_target_matches(2560, 1440, FMT_RGB10A2));
  assert(!clean_target_matches(1920, 1440, FMT_RGBA8));
  assert(!clean_target_matches(2560, 1080, FMT_RGBA8));
  assert(overlay_accept_format(FMT_RGB10A2));
  cached(false);
  assert(clean_target_matches(2560, 1440, FMT_RGB10A2));
  assert(!clean_target_matches(2560, 1440, FMT_RGBA8));
  puts("render format: 25 format pairs, 20 unsupported transitions, cache and held-frame guards passed");
  return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    args = parser.parse_args()
    source = (ROOT / "src/ootgz/nvn.c").read_text(encoding="utf-8")
    constants = re.search(r"^enum \{.*?^\};", source, re.MULTILINE | re.DOTALL).group()
    slots = re.search(r"^#define SLOTS\s+\d+", source, re.MULTILINE).group()
    helpers = "\n\n".join(function(source, name) for name in (
        "format_bpp", "overlay_accept_format", "clean_target_matches"))
    out = ROOT / "obj" / "tests"
    out.mkdir(parents=True, exist_ok=True)
    test = out / "render_format.c"
    test.write_text(constants + "\n" + slots + "\n" + HARNESS + helpers + CASES, encoding="utf-8")
    exe = out / ("render_format.exe" if os.name == "nt" else "render_format")
    flags = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}
    env = os.environ.copy()
    if Path(args.cc).is_absolute():
        env["PATH"] = str(Path(args.cc).parent) + os.pathsep + env.get("PATH", "")
    commands = [
        [args.cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-Isrc/ootgz",
         str(test), "-o", str(exe)],
        [str(exe)],
    ]
    for command in commands:
        result = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, **flags)
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        result.check_returncode()


if __name__ == "__main__":
    main()
