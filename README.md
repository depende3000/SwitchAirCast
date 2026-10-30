<p align="center">
  <img src="docs/logo.svg" width="140" alt="SwitchAirCast logo">
</p>

<h1 align="center">SwitchAirCast</h1>

<p align="center">
  Turn your Nintendo Switch into an AirPlay receiver. Mirror your iPhone, iPad or Mac to the
  Switch, send YouTube videos to it, or play music through it — the same way you would with an
  Apple TV. Android phones and Windows PCs can send videos and music to it too, over DLNA.
</p>

<p align="center">
  <a href="../../releases/latest"><b>Download the latest release</b></a>
</p>

## What it can do

- **Screen mirroring** from iPhone, iPad and Mac, up to 1080p at 60 fps, decoded by the Switch's
  hardware video decoder. On a weak Wi-Fi, switch to 720p or 30 fps in the Settings menu.
- **Send videos instead of mirroring.** Tap the AirPlay button in the YouTube app (iPhone/iPad)
  or in Safari on a Mac and the Switch plays the video by itself. Your device becomes the remote,
  so you can lock it or use other apps meanwhile.
- **Music over AirPlay**, with the song title, artist, album, cover art and progress on screen,
  and volume control from your device.
- **Videos and music from Android and Windows** (DLNA): send them with an app such as
  BubbleUPnP or Web Video Caster, or with **Cast to Device** in Windows. The Switch plays the
  file itself; pause, seek, volume and playlists work from the app.
- **Vertical mode** for portrait apps: turn the picture 90° and hold the console upright. It can
  also follow the console's motion sensor automatically.
- **Screen off** while music keeps playing (button Y, or automatically after a few minutes).
- **Black theme** for the OLED model (chosen automatically) and an **English / Spanish**
  interface that follows the console's language.

## What you need

- A Switch that can run homebrew (Atmosphère with the Homebrew Menu).
- The Switch and your iPhone, iPad, Mac, Android device or PC on the same Wi-Fi network.

## Install

1. Download `SwitchAirCast.nro` from the [Releases](../../releases) page.
2. Copy it to the `/switch/` folder on the SD card.
3. Open the Homebrew Menu **through a game**: hold **R** while starting any game. From the
   Album the Switch gives homebrew too little memory to play video, so SwitchAirCast only shows
   how to open it properly.
4. Start **SwitchAirCast**. It shows the name it uses on the network (your console's nickname).

## Using it

**Mirror your screen**

- iPhone / iPad: open Control Center → **Screen Mirroring** → choose your Switch.
- Mac: Control Center → **Screen Mirroring** → choose your Switch.

**Send a video**

- iPhone / iPad: in the YouTube app, tap the cast/AirPlay button → **AirPlay & Bluetooth
  devices** → choose your Switch.
- Mac: in **Safari**, tap the AirPlay button on the YouTube video and choose your Switch.
  The first time, the Mac asks for a code: type the 4 digits shown on the Switch. After that it
  connects without asking. (Chrome has no AirPlay; use Safari.)

Pause, seek and stop from your device, or with the Switch buttons below.

**Play music:** choose the Switch as the AirPlay speaker in Music, Spotify, Control Center, etc.

**From Android**

Android has no AirPlay, and the Switch can't appear under the **Cast** (Chromecast) button:
Google only lets devices it has certified receive Google Cast. Use a DLNA app instead:

- **BubbleUPnP**, **LocalCast** or **Web Video Caster**: choose the Switch as the "renderer" or
  "player", then pick a video, song or web video in the app. Playlists play one after another.
- Other media apps with a "Play to" or UPnP player option (Kodi, for example) work the same way.

**From Windows:** right-click a video or music file → **Cast to Device** (on Windows 11 under
**Show more options**) → choose the Switch.

**From a Mac or Linux PC:** any DLNA / UPnP control point app can send to the Switch.

### Buttons

| Button | What it does |
| --- | --- |
| **−** | Settings: language, theme, screen-off timer, vertical mode, resolution, frame rate |
| **+** | Exit |
| **Y** | Turn the screen off (any button turns it back on) |
| **R** | While mirroring or playing a video: turn the picture (off → left → right) |
| **B** | Disconnect the device, or stop the video that is playing |
| **A** | Pause / play a video |
| **◀ / ▶** | Video: back / forward 10 seconds |
| **▲ / ▼** | Video: forward / back 1 minute |
| **X** | Show or hide technical info (resolution, frame rate, decoder) |

Touching any button during a cast shows a reminder that **−** opens the settings. Any button
also hides an AirPlay code you don't need.

The Switch does not dim or go to sleep while SwitchAirCast is open.

## Settings

The Settings menu (**−**) changes the most common options and saves them right away; the line
under the list explains the selected one. Resolution and frame rate apply the next time a device
connects (press **B** to disconnect, then choose the Switch again).

All options live in `/config/SwitchAirCast/config.ini` on the SD card; you can edit that file on
a computer (changes there apply the next time you start the app).

| Option | Default | Meaning |
| --- | --- | --- |
| `name` | console nickname | Name shown in the Screen Mirroring list |
| `language` | `auto` | `auto` (console language), `en` or `es` |
| `theme` | `auto` | `auto` (black on the OLED model), `dark` or `black` |
| `screen_off_minutes` | `5` | Turn the screen off after this many minutes of waiting or music (`0` = never) |
| `vertical_mode` | `off` | `off`, `auto` (motion sensor, handheld only), `left` or `right` |
| `resolution` | `1080p` | Picture size asked from your device: `1080p`, `720p` (lighter on a weak Wi-Fi), or `custom` (uses `width` and `height`). Also in the Settings menu |
| `width`, `height` | `1920`, `1080` | Picture size used with `resolution = custom` |
| `max_fps` | `60` | Frame rate asked from your device (`60` or `30` in the Settings menu) |
| `audio` | `true` | Play sound on the Switch |
| `video_casting` | `true` | Accept videos sent from YouTube / Safari |
| `dlna` | `true` | Accept videos and music from Android and Windows apps (DLNA). Uses UDP port 1900 and TCP port 49152 |
| `video_language` | `default` | Audio track for videos with several languages: `default` (the video's own), `system` (console language), or language codes such as `es:en` |
| `hw_decode` | `true` | Use the hardware video decoder (turn off only for troubleshooting) |
| `show_stats` | `false` | Start with the technical info visible |
| `port` | `7000` | Network port |
| `debug_log` | `false` | Write a very detailed log (for bug reports), and keep the last 10 runs in `logs/` |

## Troubleshooting

**The Switch doesn't appear in the list.** Make sure both are on the same Wi-Fi network (not a
guest network, which usually keeps devices apart). Wait a few seconds after starting the app, or
restart it. Some routers block devices from finding each other; look for "AP isolation" or
"client isolation" in the router settings and turn it off.

**The Switch isn't under the Cast (Chromecast) button on Android or in Chrome.** That is
expected: Google Cast only works with devices that Google has certified, and the Switch cannot
be one. Use a DLNA app (see [From Android](#using-it)) or Windows' Cast to Device. YouTube's own
app can't cast to DLNA devices; open the video in Web Video Caster instead.

**An Android app or Windows doesn't find the Switch.** Same Wi-Fi network and router checks as
above. Wait up to 30 seconds after starting SwitchAirCast: it announces itself on the network
every 30 seconds. Some apps also let you refresh the device list.

**"Open SwitchAirCast through a game" appears.** It was started from the Album. Press **+**,
then open the Homebrew Menu by holding **R** while starting any game; see [Install](#install).

**The picture stutters.** Move closer to the router, or open Settings (**−**) and choose
**Resolution** 1280 × 720 or **Frame rate** 30 fps, then reconnect. Press **X** while
mirroring to see the share of frames that reach the screen (over the last 5 seconds).

**Black screen in Netflix, Disney+, Apple TV+ and similar apps.** These apps block mirroring of
their videos, and their AirPlay streams are encrypted (FairPlay), so the Switch cannot play
them. YouTube works.

**A video file from QuickTime Player on a Mac doesn't play.** If you choose the Switch with
QuickTime's AirPlay button for a movie stored on the Mac, nothing plays on the Switch and
QuickTime goes back to playing the movie on the Mac. For files on the Mac, macOS
itself serves the movie to the receiver, and on macOS 27.0 that fails inside macOS: its log
shows `Stream file … error: EPERM` from `AirPlayXPCHelper` (the Mac's AirPlay service is not
allowed to open the file). Moving the file or removing its quarantine flag doesn't help.
Workaround: mirror the Mac instead.

1. Open Control Center → **Screen Mirroring** and choose your Switch.
2. Play the movie in QuickTime Player and press **⌃⌘F** for full screen.

Sound goes to the Switch too. Videos from the internet sent with Safari's AirPlay button are
not affected and play normally.

**A Mac asks for a code.** Type the 4 digits shown on the Switch. The code disappears by itself
after a minute, or press any button to hide it.

**Reporting a bug.** Please attach `log.txt` and `log-previous.txt` from
`/config/SwitchAirCast/` (the second one is the run before, useful after a crash) and, if the
app crashed, the newest file in `/atmosphere/crash_reports/`. Setting `debug_log = true` makes
the log much more detailed and also keeps the last 10 runs as
`/config/SwitchAirCast/logs/log-<date>_<time>.txt`. While mirroring or playing a video, the log
gets a line every 5 seconds with the frame rate and how many frames were skipped.

## Limitations

- Videos are played up to 1080p in H.264. YouTube's 4K (VP9/AV1) versions are skipped, and 4K
  (H.265) mirroring is not supported.
- No Google Cast (Chromecast): it requires a Google-certified device. Android and Windows send
  over DLNA instead, which plays files and web videos but can't mirror the screen.
- Movie files sent from QuickTime Player on a Mac don't play (see
  [Troubleshooting](#troubleshooting)); mirror the Mac's screen instead.
- Anyone on your network can mirror to the Switch, or send it media over DLNA, while the app is
  open. Macs need the one-time code before sending videos; iPhones, iPads and DLNA apps connect
  without one. Set `dlna = false` to turn DLNA off.

## For developers

Building, the desktop test setup and how the code is organised are described in
[docs/DEVELOPING.md](docs/DEVELOPING.md).

## License and credits

GPL-3.0-or-later (see [LICENSE](LICENSE)). The AirPlay protocol support comes from
[UxPlay](https://github.com/FDH2/UxPlay)'s library (with RPiPlay and the FairPlay
implementation it includes), ported to the Switch; the DLNA renderer is SwitchAirCast's own; video
and audio playback use FFmpeg and SDL2.
libplist is LGPL-2.1 and llhttp is MIT.

SwitchAirCast is not affiliated with or endorsed by Apple or Nintendo. AirPlay, iPhone, iPad,
Mac and Safari are trademarks of Apple Inc. Nintendo Switch is a trademark of Nintendo. Google Cast
and Chromecast are trademarks of Google LLC. DLNA is a trademark of the Digital Living Network
Alliance.

## Made with AI assistance

AI was used as a tool in developing SwitchAirCast. Much of the code, the tests and the
documentation were written with [Claude Code](https://claude.com/claude-code), Anthropic's AI
coding assistant, under the direction of the author, who tested it on real Switch consoles
with iPhone and Mac.
