# Performance notes

This fork is designed for machines where a full-screen 30fps video is a
significant part of the CPU budget. The first implementation worked but decoded
in `draw()` on the main thread; the optimized version moves decoding to a
worker, decodes fewer pixels, and removes redundant full-surface renders.

## Test setup

- **CPU**: Intel Core i5-7200U (2 cores / 4 threads, Kaby Lake)
- **GPU**: Intel HD Graphics 620 (Mesa iris)
- **Output**: 1920×1080, single headless Sway output (GPU renderer, not pixman)
- **Media**: `butterfly.mp4`, HEVC Main, 1864×1080, 30fps, 19.67s, ~313 kb/s
- **Config**: the full desktop config (background video, clock, date, user
  label, input field, shadows)

The headless compositor uses the same EGL/GLES driver as the real desktop, but
it is still a synthetic environment. Treat the numbers as relative, not as a
guarantee for your machine.

## Results

Measured with `time` around a `timeout` run of the built binary, plus frame
counts from `hyprlock -v` (`frame N` for compositor frames, `media: ... frame N`
for uploaded video frames).

### Single output

| Build | Wall | User CPU | CPU per wall second | Video uploads | Average fps |
|---|---:|---:|---:|---:|---:|
| First working version (sync decode, fixed 33ms timer) | 12.0s | 8.9s | 0.74 core | 275 / 11.6s ≈ 23.7fps | 28.9 |
| Optimized (async decode, dynamic timer, 1 decode thread) | 10.0s | 4.6s | **0.46 core** | 295 / 9.8s ≈ 30.1fps | 28–30 |

The optimized build does more useful work (it no longer drops frames) with
roughly **38% less CPU**. The important part is not only the total: decoding no
longer runs on the main thread. In a per-thread sample the main thread was
**~9% CPU**, the media worker ~12%, and the decoder threads the rest.

### Blurred background

`blur_passes = 3`, `blur_size = 8`, same 1080p30 source:

| Wall | User CPU | Uploads | Average fps |
|---:|---:|---:|---:|
| 10.0s | 5.15s | 277 ≈ 28fps | 32.6 |

The source is decoded at 932×540 and blurred at that size, then upscaled. That
is why adding three blur passes costs little compared with a full-resolution
blur per frame.

### Dual output

Two 1920×1080 outputs, same config on both:

| Metric | Before epoch/port targeting | After |
|---|---:|---:|
| Rendered frames per output in 10s | 500 | 316 |
| Video uploads (both players) | 574 | 588 (≈294 each) |
| User CPU / 10s | 12.6s | 12.9s |

The render count dropped because both players now share one wall-clock epoch
(the same frame is due on both outputs at once) and the manager renders only the
outputs that are due. CPU is still dominated by **two independent software
decoders**; this is the next known limitation.

## What changed

1. **Decode thread + frame ring**
   - The decoder and `sws_scale` run in a worker thread.
   - Three decoded RGBA frames are buffered; the renderer takes the newest frame
     whose timestamp is due and drops older ones.
   - A seek is issued if the consumer jumps more than ~1.5s ahead, so a stall
     does not cause minutes of catch-up decoding.

2. **Decode at display size**
   - Background: output size, or half the viewport with blur.
   - Image: 2× the widget's `size`.
   - `sws_scale` converts and downscales in one step; no full-size RGBA buffer
     for a small widget.

3. **Cheaper background path**
   - No blur and no output transform: texture straight to the screen.
   - Blur: render into a half-resolution FBO, blur there, upscale.
   - `CRenderer::blurFB` reuses two mirror framebuffers instead of allocating
     them on every call.

4. **One timer, precise deadlines**
   - All players share a `CMediaManager` timer.
   - The timer is scheduled from the next frame's PTS (`nextFrameDelayMs`), with
     a 2ms wake-up margin. A fixed 33ms timer against 33.333ms frames drifts and
     drops a frame every few seconds.
   - When the ring is empty the manager waits ~15ms instead of polling every
     1–4ms.

5. **One epoch, targeted redraws**
   - Players start from the same `steady_clock` point, so two outputs do not
     interleave their deadlines (which previously caused two full-surface
     renders per frame).
   - `CMediaManager` calls `renderOutput(port)` for the outputs that are due
     instead of `renderAllOutputs()`.

6. **Decoder thread count**
   - Based on source pixel rate: 1 thread for 1080p30, 2 for 1080p60, 3–4 for
     4K/high frame rate. `thread_count = 0` (ffmpeg auto) can starve the
     compositor on a 4-thread laptop.

## Tuning

If the lock screen is still heavy:

1. `media_fps = 24` — 20% less decode/render work than 30.
2. `media_scale = 0.75` — 44% fewer pixels; software scaling cost is usually
   smaller than the saved conversion/upload/blur work at larger ratios.
3. `blur_passes = 1` or `2` with `blur_size = 4..8` — the background is already
   soft; the pipeline also decodes at half size.
4. Split monitors into separate blocks and give the secondary output a lower
   `media_fps` (e.g. 15).
5. Use a smaller/lower-bitrate source: a 720p or 1080p H.264 clip is much
   cheaper to decode than 4K HEVC, and at 1080p output a 4K source has no
   visible advantage after blur.

## Known limits

- **Software decode.** VAAPI/VA-API is not implemented. On Intel, hardware HEVC
  decode needs `intel-media-driver` and a dmabuf/EGL import path; `av_hwframe_transfer_data`
  plus `sws_scale` was tested locally but was not faster for this low-bitrate
  clip, so it was left out.
- **Per-output decode.** Two outputs showing the same file decode it twice. A
  shared stream keyed by `(path, maxWidth, maxHeight, loop)` is the next step.
- **8-bit output.** 10-bit/HDR sources are converted to RGBA8 (SDR).
- **No audio.** Audio streams are ignored by design.
- **Media is still a full-surface redraw.** Every video frame redraws the lock
  surface; labels, shadows and the input field are composited again. Shadow
  culling was measured and did not change the frame rate on the HD 620, so it is
  not implemented.

## Reproducing

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

/usr/bin/time -v timeout 10 ./build/hyprlock -c your.conf -v --grace 0 2>run.log
grep -c '\[.*\] frame' run.log              # compositor frames
grep -c 'media: .* frame' run.log           # uploaded video frames
grep 'Current fps' run.log | tail
```

For a fair comparison, use the same source file and config, and ignore the first
~1.5–2s (resource gathering and fade-in).
