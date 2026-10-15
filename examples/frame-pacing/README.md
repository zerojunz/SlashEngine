# Frame pacing

V-Sync, the frame cap and frame pacing, read and changed while a game runs. [main.cpp](main.cpp)
registers the commands `slash-cli run --serve` adds and calls them as an agent would.

- `display.list` lists the displays with their refresh rates, primary first, and the game's
  V-Sync mode and frame cap. Headless runs have no displays.
- `display.set` changes `vsync_mode` (`disabled`, `enabled`, `adaptive` or `mailbox`) or
  `max_fps` from the next frame and returns the settings in effect: a mode the GPU lacks falls
  back to `enabled`, and the reply says so. Out-of-range values are refused with their JSON
  Pointer, and nothing changes.
- `perf.stats` reports fps, frame times (average, fastest and slowest over the last 120 frames)
  and catch-up frames: frames that ran more than one tick because the one before was late. On a
  machine with a display, those show whether a game keeps to its frame cap.
- Headless frames run one tick each and never wait for the frame cap, so headless runs stay fast
  and repeatable; the cap only paces windowed runs.
- A game sets the same things in `project.json` (`max_fps` and `window.vsync_mode`), and its own
  code calls `RenderDevice::set_vsync` and `Loop::set_max_fps`, e.g. from an options menu.
