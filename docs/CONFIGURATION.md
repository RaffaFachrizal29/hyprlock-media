# Configuration

`hyprlock-media` uses the same config file, syntax and widget system as
hyprlock. If you already have a working hyprlock config, only the `path` values
(and optionally the new `media_*` keys) need to change.

The default location is `~/.config/hypr/hyprlock.conf`. Pass a different file
with `hyprlock --config /path/to/file.conf`.

Because this is hyprlang, blocks are anonymous unless they have a name, and
values can reference variables:

```ini
$accent = rgb(a9c7ff)

background {
    monitor =
    path = ~/wallpapers/butterfly.mp4
}
```

## Animated media

Set `path` to a supported file. The widget switches to the media player
automatically; no extra block or flag is needed.

```ini
background {
    monitor =
    path = ~/wallpapers/butterfly.mp4
    media_loop = true
    media_fps = 30
}
```

The same keys work on `image`:

```ini
image {
    monitor =
    path = ~/wallpapers/spinner.gif
    size = 180
    position = 0, -220
    halign = center
    valign = center
    media_loop = true
    media_fps = 15
}
```

### Media keys

These four keys exist on both `background` and `image`:

| Key | Type | Default | Description |
|---|---|---|---|
| `media_loop` | bool | `true` | Restart at the end. With `false`, playback stops on the last frame and keeps showing it. |
| `media_fps` | int | `0` | Maximum display rate. `0` follows the source. A lower value (24, 15, 10) reduces decode and render cost. |
| `media_speed` | float | `1.0` | Playback speed. `0.5` is half speed, `2.0` is double. Values ≤ 0 are treated as `1.0`. |
| `media_scale` | float | `0` | Multiplier for the decoded size after the automatic cap. `0` or `1` = automatic. `0.75` decodes at 75% of the capped size; values do not upscale. |

### Automatic decode size

The player avoids decoding pixels that are never shown:

- **Background**: the frame is capped to the output size. If `blur_passes > 0`,
  it is capped to half the output size because the blur hides the detail.
- **Image**: the frame is capped to twice the widget's `size` (the longest
  side). A 4K video in a 200px badge is decoded at 400px.
- Aspect ratio is preserved. The source is never upscaled.

`media_scale` multiplies this automatic target. Examples:

```ini
# 1080p output, no blur: decode at most 1920x1080
background {
    path = ~/4k-wallpaper.mp4
}

# 1080p output, blur: decode at 960x540 and blur at that size
background {
    path = ~/4k-wallpaper.mp4
    blur_passes = 3
}

# force 75% of the automatic target (e.g. 1440x810)
background {
    path = ~/4k-wallpaper.mp4
    media_scale = 0.75
}
```

### Supported files

The media path is selected by file extension:

- Animated images: `.gif`, `.gifv`, `.webp`, `.apng`, `.avif`, `.jxl`
- Video: `.mp4`, `.m4v`, `.mov`, `.mkv`, `.webm`, `.avi`, `.wmv`, `.flv`,
  `.mpg`, `.mpeg`, `.ts`, `.m2ts`, `.ogv`, `.3gp`

The actual codec support comes from your ffmpeg build. Common H.264, HEVC,
VP8/VP9, AV1, GIF, animated WebP and APNG files work. Audio and subtitle streams
are ignored. Cover art (`AV_DISPOSITION_ATTACHED_PIC`) is not treated as video.

A file with any other extension (`.png`, `.jpg`, `.jpeg`, ...) uses hyprlock's
original still-image loader. If a media file fails to open or decode, the widget
also falls back to the still loader, so a broken GIF does not leave a blank
background.

`path = screenshot` still means "use the pre-lock screenshot" and is never
treated as media.

## Background options

| Key | Type | Default | Description |
|---|---|---|---|
| `monitor` | string | `""` | Output name/description; empty applies to every output. |
| `path` | string | `""` | Still image, `screenshot`, GIF/WebP/APNG/AVIF or video. |
| `color` | color | `0xFF111111` | Fallback/background color behind the image. |
| `blur_passes` | int | `0` | Number of blur passes. `0` disables blur. |
| `blur_size` | int | `8` | Blur radius. With media + blur, the pipeline works at half resolution. |
| `noise` | float | `0.0117` | Blur noise. |
| `contrast` | float | `0.8917` | Blur contrast. |
| `brightness` | float | `0.8172` | Blur brightness. |
| `vibrancy` | float | `0.1686` | Blur vibrancy. |
| `vibrancy_darkness` | float | `0.05` | Blur vibrancy in dark areas. |
| `zindex` | int | `-1` | Stacking order. |
| `reload_time` | int | `-1` | Seconds between reload checks. `-1` disables. `0` checks hourly. |
| `reload_cmd` | string | `""` | Command whose stdout is the new path. |
| `crossfade_time` | float | `-1` | Crossfade for still-image reloads. Not used for media frames. |
| `media_loop` | bool | `true` | See above. |
| `media_fps` | int | `0` | See above. |
| `media_speed` | float | `1.0` | See above. |
| `media_scale` | float | `0` | See above. |

## Image options

| Key | Type | Default | Description |
|---|---|---|---|
| `monitor` | string | `""` | Output name/description; empty applies to every output. |
| `path` | string | `""` | Still image, GIF/WebP/APNG/AVIF or video. |
| `size` | int | `150` | Longest side in pixels. The image is scaled to fit. |
| `rounding` | int | `-1` | Corner radius; `-1` = fully rounded. |
| `border_size` | int | `4` | Border thickness. |
| `border_color` | gradient | `0xFFDDDDDD` | Border color/gradient. |
| `position` | layout | `0,0` | Position or percentage offset. |
| `halign` | string | `center` | `left`, `center`, `right`, `none`. |
| `valign` | string | `center` | `top`, `center`, `bottom`, `none`. |
| `rotate` | float | `0` | Rotation in degrees. |
| `reload_time` | int | `-1` | Seconds between reload checks. |
| `reload_cmd` | string | `""` | Command whose stdout is the new path. |
| `zindex` | int | `0` | Stacking order. |
| `shadow_*` | | | Standard hyprlock shadow keys. |
| `onclick` | string | `""` | Command to run on click. |
| `media_loop` | bool | `true` | See above. |
| `media_fps` | int | `0` | See above. |
| `media_speed` | float | `1.0` | See above. |
| `media_scale` | float | `0` | See above. |

## Examples

### Video wallpaper with a soft, cheap blur

```ini
background {
    monitor =
    path = ~/wallpapers/butterfly.mp4
    blur_passes = 2
    blur_size = 6
    brightness = 0.9
    contrast = 0.85
    media_fps = 30
}
```

The video is decoded at half the output resolution and blurred there. This is
much cheaper than a full-resolution blur per frame and looks the same after the
blur.

### Muted, slow background

```ini
background {
    path = ~/wallpapers/clouds.webm
    media_fps = 24
    media_speed = 0.6
    media_scale = 0.75
}
```

### Animated logo next to the clock

```ini
image {
    path = ~/wallpapers/logo.gif
    size = 96
    position = -220, 150
    halign = center
    valign = center
    media_loop = true
    media_fps = 12
}
```

### Play once and hold the last frame

```ini
image {
    path = ~/wallpapers/intro.mp4
    size = 320
    media_loop = false
}
```

### Different media per monitor

Use one block per output. `monitor` accepts the port name (`eDP-1`, `DP-1`) or
a description prefix (`desc:...`), same as upstream hyprlock.

```ini
background {
    monitor = eDP-1
    path = ~/wallpapers/laptop.mp4
    media_fps = 24
}

background {
    monitor = DP-1
    path = ~/wallpapers/desktop.mp4
    media_fps = 30
}
```

### Reload when a file changes

`reload_time` and `reload_cmd` still work. For media the file is reopened when
the path or mtime changes.

```ini
background {
    path = /tmp/current-wallpaper.mp4
    reload_time = 5
}

image {
    path = /tmp/generated.gif
    reload_cmd = echo /tmp/generated.gif
    reload_time = 2
}
```

## Performance presets

| Preset | Settings | Typical use |
|---|---|---|
| Quality | `media_fps = 0`, `media_scale = 0` | Desktop GPU, 1080p/1440p, short clips |
| Balanced | `media_fps = 30` | Laptops, 1080p video |
| Low CPU | `media_fps = 24`, `media_scale = 0.75` | Dual-core laptops, battery |
| Minimal | `media_fps = 1`, `media_scale = 0.5` or `media_loop = false` | Almost static background with a small motion hint |
| Blur | `blur_passes = 1..3`, `media_fps = 24` | Hides compression artifacts and lowers GPU cost |

See [PERFORMANCE.md](PERFORMANCE.md) for the internals and benchmarks.

## Troubleshooting

### The background is black or the fallback color

1. Run `hyprlock -v` and look for `media: playing ...` / `media: failed to ...`.
2. Check that ffmpeg can decode it:
   `ffmpeg -v error -i file.mp4 -f null -`
3. Make sure the file is readable by your user and the path is absolute or uses
   `~` (hyprlock expands `~`).
4. Check the extension is in the supported list; otherwise it goes to the still
   loader, which may not support video.
5. HEVC/AV1 need the corresponding decoder in your ffmpeg build. A minimal
   ffmpeg package may lack some codecs.

### It plays but stutters / the whole desktop lags

- Set `media_fps = 24` or `30`.
- Set `media_scale = 0.75` (or lower) on the background.
- Enable `blur_passes` only if you want the look; it adds GPU work.
- On dual monitors with the same file, lower `media_fps` on both blocks; each
  output currently decodes its own copy.
- Check `hyprlock -v` for decode errors; a corrupt file can cause repeated
  retries.

### It shows one frame and stops

- A still image opened through the media path (`animated` is false) is shown as
  one frame by design.
- Check `media_loop`; with `false` the last frame is held.
- If the file is a video with a single frame, it behaves like a still image.

### Audio is missing

Audio is intentionally ignored. The lock screen has no audio output.

### The clock/input field updates look slow while video runs

The media timer only redraws outputs with a due frame; other widgets render with
the same pass. If the whole UI is slow, reduce `media_fps`/`media_scale` first.
On very weak GPUs, `blur_passes` is the next thing to reduce.

### Where are the logs?

`hyprlock -v` enables TRACE logs, including `media: opened ...` (source and
decoded size), `media: playing ...`, and per-frame uploads. Without `-v`, only
warnings/errors are printed.

## Upstream behavior

All existing hyprlock keys, animations, labels, shapes, input-field options,
PAM/fingerprint auth and CLI arguments are unchanged. The only additions are the
four `media_*` keys and media dispatch on `path`. For the full upstream
reference, see the
[hyprlock wiki](https://wiki.hyprland.org/Hypr-Ecosystem/hyprlock/).
