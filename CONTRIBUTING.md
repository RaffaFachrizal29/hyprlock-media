# Contributing

Thanks for trying hyprlock-media. This is a small fork; keeping the diff
reviewable matters more than matching upstream process.

## Before you start

- For upstream hyprlock behavior (widgets, PAM, session lock, config syntax),
  read the [hyprlock wiki](https://wiki.hyprland.org/Hypr-Ecosystem/hyprlock/).
- For the media pipeline, read `docs/PERFORMANCE.md` and
  `src/renderer/MediaPlayer.hpp` first.
- Do not test a lock screen against your live Wayland session. Use a nested or
  headless compositor (Sway with `WLR_BACKENDS=headless` works; see the test
  commands below). Locking your real session mid-development is a bad time.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DHYPRLOCK_BUILD_TESTS=ON
cmake --build build -j$(nproc)
```

The only extra dependency compared with upstream is ffmpeg (libav* + swscale).

## Tests

```sh
# decoder / scaling / async playback / loop / speed / orientation / bad file
./build/media_test /path/to/video.mp4 /path/to/animation.gif /path/to/still.webp

# config schema
./build/config_test ./test.conf
```

Both harnesses are opt-in (`HYPRLOCK_BUILD_TESTS=ON`) and are never installed.
`media_test` uses EGL surfaceless; it does not need a compositor.

For an end-to-end check, run the binary in a headless Sway session and capture
with `grim`:

```sh
WLR_BACKENDS=headless WLR_RENDERER=gles2 WLR_HEADLESS_OUTPUTS=1 \
  sway -c /tmp/sway.conf

WAYLAND_DISPLAY=wayland-1 ./build/hyprlock -c ./lock.conf -v
WAYLAND_DISPLAY=wayland-1 grim -o HEADLESS-1 shot.png
```

Then pixel-sample the capture to prove frames changed. The `media_test` harness
covers timing/orientation; screenshots cover the real widget/FBO path.

## Code expectations

- Keep the media path off the main thread. `draw()` may upload a frame, but it
  must not decode or call `sws_scale`.
- Never `break` out of the catch-up loop for an FPS cap: that uploads one frame
  per render and the playhead drifts. Throttle at tick entry instead.
- Reuse framebuffers; per-frame `alloc`/`destroy` is visible on integrated GPUs.
- Any new config key needs all three places: `addSpecialConfigValue`, the widget
  props map in `getWidgetConfigs()`, and the `std::any_cast` in `configure()`.
- `Hyprlang::STRING` is `const char*`; wrap it in `std::string` before comparing.
- Keep the binary name and existing config behavior stable. This fork should be
  a drop-in replacement.

## Pull requests

- Describe the use case and the measured effect (before/after CPU or frame
  counts, not just "faster").
- Include the source characteristics (resolution, codec, fps) for performance
  claims.
- If you change decode scheduling, update `docs/PERFORMANCE.md`.

## License

By contributing you agree that your changes are licensed under the repository's
BSD 3-Clause License.
