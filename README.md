# SwitchAirCast

An AirPlay receiver for the Nintendo Switch (homebrew `.nro`). Mirror the screen of an
iPhone, iPad or Mac to the Switch, or play music to it over AirPlay.

- **Screen mirroring:** H.264 video decoded by the Switch's hardware decoder (NVDEC), up to 1080p60.
- **AirPlay video:** tap the AirPlay button in YouTube and the Switch streams the video itself
  (HLS over HTTPS); the phone becomes a remote and can be used for other things or locked.
- **Audio:** AAC-ELD (mirroring), AAC and ALAC (music apps), with the sender's volume control.
- **Now playing:** shows the title, artist, album, cover art and progress when streaming music.
- **Vertical mode:** turns mirrored and cast video 90° so a portrait iPhone fills the screen when
  the console is held upright (by button, setting, or automatically with the motion sensor).
- **Screen off:** turns the screen off while audio keeps playing; also automatically after a few
  minutes of waiting or music (saves battery and protects OLED screens).
- **OLED black theme** (automatic on the OLED model) and **English/Spanish** interface (follows the
  console language).

The AirPlay protocol (mDNS discovery, pairing, FairPlay key exchange, RTSP, encrypted
mirroring and audio streams) comes from [UxPlay](https://github.com/FDH2/UxPlay)'s library,
ported to the Switch. SwitchAirCast replaces UxPlay's GStreamer playback with FFmpeg + SDL2.

## Using it

1. Copy `SwitchAirCast.nro` to `/switch/` on the SD card.
2. Start it from the Homebrew Menu. Launch the menu through a game (hold **R** while starting
   a game), not through the Album: Album mode has too little memory for video decoding.
3. On the iPhone/iPad (same Wi-Fi network): open Control Center → **Screen Mirroring** and
   choose the Switch. On a Mac: Control Center → Screen Mirroring.
4. To send just a video: in YouTube, tap the AirPlay/cast button and choose the Switch. Play,
   pause, seek and stop from the phone, or with the Switch controls below.

| Button | Action |
| --- | --- |
| **+** | Exit |
| **−** | Settings (language, theme, screen-off timer, vertical mode) |
| **Y** | Turn the screen off (any button turns it back on) |
| **R** | While mirroring or playing a video: rotate (off → left → right) |
| **X** | Show/hide stats (resolution, fps, decoder) |
| **B** | Disconnect the current device (stop the video while one is playing) |
| **A** | Pause/play an AirPlay video; retry after a startup error |
| **◀ / ▶** | AirPlay video: back/forward 10 seconds |
| **▲ / ▼** | AirPlay video: forward/back 1 minute |

The screen does not dim or sleep while SwitchAirCast is open.

### Settings

`sdmc:/config/SwitchAirCast/config.ini` is created on first launch. The first four settings below
can be changed in the Settings menu (−), which saves this file; the rest are read at startup.

| Key | Default | Meaning |
| --- | --- | --- |
| `name` | console nickname | Name shown in the Screen Mirroring list |
| `language` | `auto` | `auto` (console language), `en` or `es` |
| `theme` | `auto` | `auto` (black on the OLED model), `dark` or `black` |
| `screen_off_minutes` | `5` | Turn the screen off after this long waiting or playing music (`0` = never) |
| `vertical_mode` | `off` | `off`, `auto` (motion sensor, handheld only), `left` or `right` |
| `width`, `height` | `1920`, `1080` | Resolution requested from the sender |
| `max_fps` | `60` | Frame rate requested from the sender |
| `audio` | `true` | Play sound on the Switch |
| `video_casting` | `true` | Accept AirPlay video from apps such as YouTube |
| `video_language` | `default` | Audio track for multi-language videos: `default` (the video's own default, as on the phone), `system` (console language), or codes such as `es:en` |
| `hw_decode` | `true` | Use NVDEC; `false` forces software decoding |
| `show_stats` | `false` | Start with the stats overlay on |
| `port` | `7000` | AirPlay TCP port |
| `debug_log` | `false` | Verbose protocol logging |

The same folder holds `log.txt` (written on every run) and `device_id` (the receiver's
identity; delete it to appear as a new device).

### Limitations

- Apps that block mirroring of protected video (Netflix, Disney+, Apple TV+ and similar) show a
  black screen. This is a restriction of the sending device.
- AirPlay video works with YouTube, whose streams are unprotected HLS. Apps that send
  FairPlay-encrypted streams (Apple TV+, most paid services) show "This video can't be played";
  Screen Mirroring is not an alternative for those either.
- AirPlay video plays H.264 up to the configured `height` (1080p by default). YouTube's VP9/AV1
  4K versions are skipped. H.265/4K screen mirroring is not supported.
- No PIN or password protection: anyone on the same network can connect.

## Building

Requires [devkitPro](https://devkitpro.org/wiki/Getting_Started) with devkitA64, libnx and these
portlibs: `switch-sdl2 switch-sdl2_ttf switch-ffmpeg switch-mbedtls switch-libsodium`
(`switch-ffmpeg` must be built with `--enable-nvtegra`, which is the case for devkitPro's package).

```sh
export DEVKITPRO=/opt/devkitpro
tools/build-ffmpeg-switch.sh   # once: patched FFmpeg -> build/ffmpeg-switch
make                           # -> SwitchAirCast.nro
```

devkitPro's `switch-ffmpeg` 7.1 has a bug in its NVDEC H.264 decoder: long-term reference
frames are looked up in the wrong list, which crashes on iPhone mirroring streams (they use
long-term references). It also lacks the `https`, `crypto` and `data` protocols that AirPlay
video needs. `tools/build-ffmpeg-switch.sh` rebuilds the same FFmpeg with
`patches/ffmpeg-nvtegra-h264-long-ref.patch` and those protocols, and the Makefile refuses to link
the stock library unless you pass `ALLOW_STOCK_FFMPEG=1`.

### Desktop build for testing

A Linux build runs the same receiver in a window, which makes protocol and UI work possible
without a console. It needs SDL2, FFmpeg (with libx264 for the test sender), freetype2 and
harfbuzz from the system. The first command builds static mbedtls, libsodium and SDL2_ttf into
`build/hostdeps`:

```sh
make -f Makefile.linux hostdeps
make -f Makefile.linux all tests
build/linux/switchcast                      # the receiver
build/linux/fake_sender 127.0.0.1 7000 10 --audio
```

Test tools in `tests/`:

- `fake_youtube` (with `tests/run_youtube_test.sh <hls-dir>`) acts like the YouTube app casting a
  video: reverse event channel, `POST /play` with an `mlhls://` master playlist, FCUP playlist
  requests answered from a local HLS folder, then playback-info polling, pause, seek, stop, a
  resume position and the end of the video.
- `player_test` runs the media player alone against a URL and prints its queues and clock.
- `trim_test` exercises the HLS master/media playlist rewriting used for variant choice and seeking.
- `fake_sender` acts as an AirPlay sender. It runs the full handshake (pair-setup, pair-verify,
  FairPlay, SETUP), then streams encrypted H.264 video (libx264) and optionally AAC audio.
  The sender half of FairPlay is not public, so the sender derives the stream key with the
  receiver's own FairPlay code. That checks the whole receive path, but it does not prove
  compatibility with Apple devices.
- `ui_preview` renders every screen to BMP files (`SDL_VIDEODRIVER=offscreen`).
- `crypto_test` prints deterministic outputs of the crypto layer so they can be diffed against
  UxPlay's OpenSSL implementation.
- `make_icon` regenerates `icon.jpg`.

With `SWITCHCAST_SNAPSHOT=file.bmp`, the desktop build saves a screenshot two seconds after
video starts. `SWITCHCAST_DATA_DIR` changes where it keeps its config and log.

## How it is put together

| Path | Contents |
| --- | --- |
| `source/main.c` | SDL setup, input, frame loop, video texture upload |
| `source/receiver.c` | UxPlay callbacks, connection resets, client watchdog |
| `source/video.c` | FFmpeg decoding on its own thread (NVDEC on Switch, software fallback); newest-frame mode for mirroring, timed frame queue for playback |
| `source/player.c` | AirPlay video player: HLS from in-memory playlists, segment download, A/V sync, seeking |
| `source/audio.c` | AAC-ELD/AAC/ALAC/PCM decoding, resampling, SDL audio queue |
| `source/ui.c` | All screens, drawn with SDL2 and the Switch system font; themes, settings panel, rotation |
| `source/i18n.c` | English, Spanish (Spain) and Latin-American Spanish text |
| `source/platform.c` | libnx services (sockets, network, nickname, font, sleep, backlight, motion sensor) / desktop equivalents |
| `lib/airplay/` | UxPlay's AirPlay library with Switch changes (below) |
| `lib/plist/` | libplist (binary/XML property lists) |

Changes to UxPlay's library:

- `crypto.c` was rewritten on mbedtls and libsodium (UxPlay uses OpenSSL, which devkitPro does
  not ship). Its output matches the OpenSSL version byte for byte (`tests/crypto_test.c`).
- `srp.c` is a stub. SRP is only used for PIN pairing, which SwitchAirCast does not offer.
- The built-in mDNS responder is IPv4-only on the Switch, gets its address from libnx, derives a
  per-console host name, re-announces every 60 s, and keeps announcing if it cannot join the
  multicast group.
- FairPlay setup rejects out-of-range mode bytes, which the original used unchecked as array
  indices (in both FairPlay messages).
- The HTTP server now marks requests on the reverse (PTTH) event channel as client responses
  (`http_request_set_reverse`; the original called the getter), so the phone's replies no longer
  close that channel and break the playlist transfer for AirPlay video.
- `raop_hls_export_playlists()` hands the player the playlists the library would otherwise serve
  on `http://localhost:<port>`, so the Switch does not need loopback networking.

The media player has two workarounds for FFmpeg's HLS demuxer: it disables HTTP keep-alive
(which would hand the in-memory playlist readers to the HTTP layer and trip an assertion), and
it seeks by reopening the stream from playlists trimmed to the target segment, because FFmpeg's
own HLS seek ends the stream when segments are fragmented MP4 (as YouTube's are).

## License

GPL-3.0-or-later (see `LICENSE`), because the UxPlay/RPiPlay code and the FairPlay
implementation it includes are GPL-3. libplist is LGPL-2.1 (`lib/plist/COPYING.LESSER`); llhttp
is MIT. SwitchAirCast is not affiliated with Apple or Nintendo. AirPlay is a trademark of Apple Inc.
