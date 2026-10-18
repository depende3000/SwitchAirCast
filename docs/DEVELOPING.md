# Developing SwitchAirCast

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
the stock library unless you pass `ALLOW_STOCK_FFMPEG=1`. FFmpeg is configured with
`--enable-gpl` and without `--enable-nonfree`, so the resulting `.nro` can be distributed under
the GPL.

## Desktop build for testing

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

`SWITCHCAST_DATA_DIR` changes where the desktop build keeps its config and log. With
`SWITCHCAST_SNAPSHOT=file.bmp` it saves a screenshot two seconds after video starts.

### Test tools (`tests/`)

| Tool | What it checks |
| --- | --- |
| `fake_sender` | Acts as an iPhone mirroring: full handshake (pair-setup, pair-verify, FairPlay, SETUP), then encrypted H.264 (libx264) and optionally AAC audio. The sender half of FairPlay is not public, so it derives the stream key with the receiver's own FairPlay code: this checks the whole receive path but does not prove compatibility with Apple devices. |
| `fake_youtube` + `run_youtube_test.sh <hls-dir>` | Acts like the YouTube app casting a video: reverse event channel, `POST /play` with an `mlhls://` master playlist, FCUP playlist requests answered from a local HLS folder, playback-info polling, pause, seek, stop, a resume position and the end of the video. |
| `fake_mac_pair <ip> <port> <log.txt> [--wrong-pin]` | Acts like a Mac pairing with the on-screen code: `pair-pin-start`, then the three `pair-setup-pin` steps (SRP-6a, then ed25519 keys under AES-GCM). Reads the code from the receiver's log. |
| `srp_server` + `srp_test.py` | The SRP verifier against an independent Python client (pyatv's formulas); also checks the group constant is the RFC 5054 safe prime. |
| `mirror_decrypt_test` | The mirroring stream decryptor against one continuous AES-CTR keystream cut into random packet sizes, including packets shorter than a block. Run it under AddressSanitizer to catch overruns. |
| `player_test <url>` | The media player alone against a URL, printing its queues and clock. `PLAYER_SAFARI=1` or `2` replays Safari's pause/play/seek requests around the start of playback. |
| `trim_test` | The HLS master/media playlist rewriting used for variant choice and seeking. |
| `ui_preview <dir> [w h] [black] [es\|es-419]` | Renders every screen to BMP files (`SDL_VIDEODRIVER=offscreen`); `bmp2png.py` converts them. |
| `crypto_test` | Deterministic outputs of the crypto layer, to diff against UxPlay's OpenSSL implementation. |
| `make_icon` | Regenerates `icon.jpg`. |

## How it is put together

| Path | Contents |
| --- | --- |
| `source/main.c` | SDL setup, input, frame loop, video texture upload |
| `source/receiver.c` | UxPlay callbacks, connection resets, client watchdog, pairing code |
| `source/video.c` | FFmpeg decoding on its own thread (NVDEC on Switch, software fallback); newest-frame mode for mirroring, timed frame queue for playback |
| `source/player.c` | AirPlay video player: HLS from in-memory or remote playlists, segment download, A/V sync, seeking |
| `source/audio.c` | AAC-ELD/AAC/ALAC/PCM decoding, resampling, SDL audio queue |
| `source/ui.c` | All screens, drawn with SDL2 and the Switch system font; themes, settings panel, rotation, banners |
| `source/i18n.c` | English, Spanish (Spain) and Latin-American Spanish text |
| `source/config.c` | `config.ini` loading and saving |
| `source/platform.c` | libnx services (sockets, network, nickname, font, sleep, backlight, motion sensor), logs, data folder / desktop equivalents |
| `lib/airplay/` | UxPlay's AirPlay library with Switch changes (below) |
| `lib/plist/` | libplist (binary/XML property lists) |

### Changes to UxPlay's library

- `crypto.c` is rewritten on mbedtls and libsodium (UxPlay uses OpenSSL, which devkitPro does
  not ship). Its output matches the OpenSSL version byte for byte (`tests/crypto_test.c`).
- `srp.c` is a new SRP-6a verifier on mbedtls bignums (Apple's variant: SHA-1, the RFC 5054
  2048-bit group, a 40-byte session key), used when a Mac asks for a code before sending video.
  Feature bit 27 (legacy pairing) must stay on, or the Mac never shows its code prompt.
- The mirroring decryptor no longer overruns its output when a packet is shorter than the
  keystream left from the previous one (tiny frames of a static screen corrupted the heap).
  Empty or absurdly large mirroring packets are skipped or end the connection.
- Joining a thread that could not be created is a no-op instead of a crash, and the HTTP server
  reports a failed thread start.
- `pair-setup-pin` never reads past what the client sent, and the pairing code is kept for two
  minutes so a Mac's repeated `pair-pin-start` requests show the same code.
- The built-in mDNS responder is IPv4-only on the Switch, gets its address from libnx, derives a
  per-console host name, re-announces every 60 s, and keeps announcing if it cannot join the
  multicast group.
- FairPlay setup rejects out-of-range mode bytes, which the original used unchecked as array
  indices (in both FairPlay messages).
- The HTTP server marks requests on the reverse (PTTH) event channel as client responses
  (`http_request_set_reverse`; the original called the getter), so the phone's replies no longer
  close that channel and break the playlist transfer for AirPlay video.
- `raop_hls_export_playlists()` hands the player the playlists the library would otherwise serve
  on `http://localhost:<port>`, so the Switch does not need loopback networking.

### Media player notes

- Master playlists are reduced to one variant (the best H.264 that fits the configured height)
  before FFmpeg sees them, both for the YouTube app's in-memory playlists and for the remote
  playlist URLs Safari sends. Otherwise FFmpeg opens every quality's playlist and probes its
  stream before playing, which took several seconds over HTTPS on the console.
- HTTP keep-alive is disabled, because it would hand the in-memory playlist readers to the HTTP
  layer and trip an assertion.
- Seeking in the YouTube app's streams reopens them from playlists trimmed to the target segment,
  because FFmpeg's own HLS seek ends the stream when segments are fragmented MP4.

## Logs and crash reports

The app writes `log.txt` (this run) and `log-previous.txt` (the run before) to
`sdmc:/config/SwitchAirCast/`. The first line says whether it runs in application or applet
(Album) mode and how much memory it has. Older builds, named SwitchCast, used
`/config/SwitchCast/`; the current one copies `config.ini` and `device_id` from there on first
start. Atmosphère writes crash reports to `/atmosphere/crash_reports/`; addresses shown as
`SwitchAirCast + 0x…` can be resolved with
`aarch64-none-elf-addr2line -f -C -e SwitchAirCast.elf 0x…` against the same build.
