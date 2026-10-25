# Changelog

## 0.9.6-media.1 — 2026-10-25

First public release of the hyprlock-media fork, based on hyprlock v0.9.6.

### Added

- Animated media playback for `background` and `image` widgets through ffmpeg
  (libavformat/libavcodec/libavutil/libswscale).
- Support for GIF, animated WebP, APNG, AVIF/JXL, MP4, M4V, MOV, MKV, WebM, AVI,
  WMV, FLV, MPEG, TS/M2TS, OGV and 3GP containers, subject to the ffmpeg build.
- `media_loop`, `media_fps`, `media_speed` and `media_scale` config keys on both
  widget types.
- Async decode worker with a three-frame ring, frame dropping, loop, playback
  speed and seek-on-stall.
- Automatic decode-size capping (output size; half viewport with blur; 2× widget
  size for images).
- Half-resolution blur pipeline for animated backgrounds.
- Standalone `media_test` and `config_test` harnesses
  (`-DHYPRLOCK_BUILD_TESTS=ON`).
- Documentation: configuration reference and performance notes.

### Performance

- Decoding no longer runs on the lock surface's main thread.
- Reduced per-frame CPU by ~38% on the development laptop (HEVC 1080p30).
- Removed a per-frame full-viewport framebuffer pass for backgrounds without
  blur or output transform.
- Reused blur mirror framebuffers instead of allocating them on every call.
- Shared wall-clock epoch and per-output redraw targeting avoid duplicated
  renders with multiple monitors.
- Timer is scheduled from the next frame's PTS instead of a fixed interval,
  eliminating periodic dropped frames.
- Decoder thread count follows the source pixel rate to leave CPU for the
  compositor.

### Notes

- Audio/subtitle streams are ignored.
- Software decode only; no VAAPI yet.
- Each output decodes its own copy of the same media file.
- Binary name, config format and all upstream options remain compatible.
