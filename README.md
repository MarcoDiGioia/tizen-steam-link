# tizen-steam-link

A Steam Remote Play (Steam Link) client for Samsung TVs running Tizen 5.5 or later, installed
as a sideloaded web app. The Steam protocol comes from
[ihslib](https://github.com/mariotaku/ihslib); the TV side follows the approach of
[Moonlight for Tizen](https://github.com/OneLiberty/moonlight-chrome-tizen).

This is an unofficial project and is not affiliated with or endorsed by Valve.

## How it works

ihslib handles discovery, PIN pairing, the streaming request and the session itself. On top
of it sit:

- `src/core`: code shared by the TV app and the desktop tools. It wraps the ihslib client and
  session, keeps the device identity, maps keyboard codes, and provides virtual gamepads.
  Steam receives every controller as a generic gamepad, using the same 48-byte report as
  ihslib's SDL backend.
- `src/platform`: thread and UDP backends for ihslib. They replace its SDL thread backend, and
  its POSIX UDP code, which calls `fcntl()` on sockets. Samsung's socket layer aborts on that
  call.
- `src/tizen`: the WebAssembly module for the TV. Video goes to the TV's hardware decoder
  through Samsung's ElementaryMediaStreamSource. Audio arrives as Opus, gets decoded to PCM,
  and goes through the same player. Gamepads are read from the browser Gamepad API.
- `web`: the TV interface (computer list, pairing, settings, stream view) and `config.xml`.
  It looks like Steam's Big Picture mode: a shelf of computer tiles as in the Steam Link
  app, a white focus glow, and a hint bar whose glyphs follow the last input, controller
  (A, B, X) or remote (OK, Back, red A). The colors were sampled from a Big Picture stream.
  It uses no Valve logos, artwork or fonts; Figtree stands in for Motiva Sans.
- `src/desktop`: `tsl-cli`, a Linux command line client that talks to Steam the same way and
  writes the stream to files.

Dependencies (ihslib, mbedTLS 3.6, protobuf-c, Opus) are downloaded by CMake at pinned
versions and checked by hash. Nothing is installed system-wide. ihslib is pinned to the head
of its pull request #4, which adds the key exchange current Steam requires for pairing (an
X25519 key masked with the PIN). The key Steam and the device agree on is per host, so the
app stores one for each paired computer. `patches/ihslib` holds ten patches applied to
ihslib at download time: two threading fixes, a thread function signature fix the TV build
needs, three fixes that let a stream get past lost packets (on Wi-Fi a single one used to
end the session or freeze the video for good), stream statistics, forward error correction
for video, the quality and bitrate settings, and the codecs and picture size the client asks
for. Each patch starts with a description of what it does.

Video uses Steam's forward error correction: every video message comes with one or two
parity packets, and the app rebuilds a message that lost up to that many packets instead of
waiting for a keyframe. The format isn't documented; `session/fec.h` in the FEC patch
describes it as worked out from captured traffic.

## Building

Desktop CLI and unit tests (CMake, Ninja and a C/C++ compiler):

```sh
scripts/build-desktop.sh
```

TV app (Docker):

```sh
scripts/build-tizen.sh
```

The first run builds a toolchain image of about 2 GB with Tizen Studio 5.6 (CLI only) and
Samsung's Emscripten 1.39.4.7. The result is `out/TizenSteamLink.wgt`, signed with a throwaway
Tizen author certificate made inside the image.

## Installing on the TV

The app runs on Samsung TVs with Tizen 5.5 or later. Samsung Smart Monitors run Tizen too,
and Apps2Samsung lists them as supported. The same steps apply if the monitor's Apps panel
offers Developer Mode.

1. Find the TV's IP address under Settings, Network, Network Status, IP Settings.
2. Open Apps on the TV and type `1 2 3 4 5` with the remote (on remotes without number keys,
   open the on-screen keypad first). Turn Developer mode on and enter this computer's IP
   address as the host.
3. Restart the TV fully by holding the power button until it switches off and comes back on.
   A short press only puts it in standby. The Apps screen then shows Developer Mode.
4. On the computer, install [Apps2Samsung](https://github.com/Apps2Samsung/Apps2Samsung)
   (on Arch: `apps2samsung` in the AUR) and start it. It finds the TV, or you type its IP.
5. Keep the default certificate. Pick "Custom WGT File", select `out/TizenSteamLink.wgt`, and
   install. TVs on Tizen 7 or later only run packages signed for that TV, so Apps2Samsung
   asks you to sign in with a Samsung account and re-signs the package.
6. Open "Tizen Steam Link" from Apps. Select the computer, type the PIN the TV shows into
   the dialog Steam opens on the computer, then select the computer again to stream.

A firmware update can turn Developer Mode off. If the app stops opening after one, repeat
steps 2, 3 and 5.

To see the app's log on the TV (Developer Mode on, with this computer's IP):

```sh
docker run --rm -it --network host tizen-steam-link-toolchain bash
sdb connect <tv-ip>
sdb shell 0 debug TSLinkTV01.TizenSteamLink   # prints a port
```

Then open `chrome://inspect` in Chrome, add `<tv-ip>:<port>` under Configure, and inspect
the app.

## The computer

Steam must be running and logged in, with Remote Play enabled, on the same network as the
TV. A firewall on the computer has to let the TV in: UDP 27031 to 27036 and TCP 27036 to
27037. With ufw:

```sh
sudo ufw allow from 192.168.0.0/24 to any port 27031:27036 proto udp
sudo ufw allow from 192.168.0.0/24 to any port 27036:27037 proto tcp
```

Steam chooses the codec. It streams HEVC only when it can encode it in hardware, and falls
back to H.264 otherwise, even with HEVC selected in the settings.

## Using it

- Computers that answer on the network appear on their own. "Add by IP address" covers
  networks where they don't.
- Selecting a computer that isn't paired shows a four-digit PIN. Steam on that computer opens
  a dialog asking for it.
- Selecting a paired computer starts the stream.
- During a stream, holding Back leaves it, and so does pressing LB + RB + View + Menu on a
  controller. A short press of Back sends Escape to the computer. The red (A) key shows or
  hides the stream statistics: resolution, frame rate and bitrate, packet loss (over the last
  second and overall), messages rebuilt by error correction and lost anyway, keyframe
  requests, and the round trip to the computer.
- On the computer list, the red (A) key forgets the selected computer.
- Keyboards and mice plugged into the TV are passed through. Click the picture to capture
  the mouse.
- The app reloads its page after each stream, which resets the video player.

Settings cover the maximum resolution (720p to 4K), H.264 or HEVC, Steam's quality
preference (Automatic, Fast, Balanced, Beautiful), a bandwidth limit (Automatic, or 5 to
100 Mbit/s; 30 by default), audio, and whether the stream opens on the desktop or in Big
Picture. On a busy Wi-Fi network a lower bandwidth limit means fewer packets to lose.

Steam sends HEVC only when the computer has a hardware encoder that can produce it; otherwise
it falls back to H.264. For 4K, pick HEVC where the computer supports it, and a bandwidth
limit of 50 Mbit/s or more.

## tsl-cli

```
tsl-cli discover                 list computers that answer a broadcast
tsl-cli probe <ip>               ask one computer directly
tsl-cli pair <ip>                pair, typing the printed PIN into Steam on that computer
tsl-cli stream <ip> --seconds 20 --out /tmp/stream
                                 stream, then write video.h264 (or .hevc) and audio.wav
```

`pair` also takes `--pin NNNN` and `--wait SECONDS` (120 by default). `stream` also takes
`--hevc`, `--no-audio`, `--max 1920x1080`, `--quality auto|fast|balanced|beautiful`,
`--bitrate KBPS` (-1 lets Steam decide), `--no-fec`, and `--input-test`, which plugs in a
virtual gamepad, presses A and D-pad down, and sends Escape, so the effect shows in the
recording. At the end it prints the frame count and a network summary: packets lost,
messages rebuilt, keyframe requests and the round trip.
The device identity and the paired computers, with their keys, are kept in
`~/.config/tizen-steam-link`. The CLI has its own identity, so it is paired separately
from the TV.

To see how a stream copes with Wi-Fi-like packet loss on a wired network, set
`TSL_TEST_RECV_LOSS` to a percentage, for example `TSL_TEST_RECV_LOSS=0.5 tsl-cli stream ...`.
The desktop build then drops that share of incoming datagrams of 1000 bytes or more and logs
each drop, each retransmission, and each dropped packet that arrives again. The TV build
doesn't have this.

## Trying the interface in a browser

```sh
python3 -m http.server -d web 8000
```

Open `http://localhost:8000/index.html?mock` at 1920x1080. `?mock` swaps the WASM module for
`web/dev/mock.js`, which fakes two computers, pairing and a stream. Arrow keys, Enter and
Escape stand in for the remote.

## License

GPL-3.0 (see `LICENSE`). ihslib is LGPL-3.0, mbedTLS is Apache-2.0, Opus and protobuf-c are
BSD licensed. The Figtree font in `web/fonts` is under the SIL Open Font License
(`web/fonts/OFL.txt`).
