# Nuvio PS5

[![Buy Me a Coffee](https://img.shields.io/badge/Buy_Me_a_Coffee-Support_this_port-FFDD00?style=for-the-badge&logo=buymeacoffee&logoColor=black)](https://buymeacoffee.com/theghostonline)

Nuvio, at home on the PlayStation 5: the full Nuvio TV interface as a PS5 app,
with a native 4K HDR player built for the console's own video hardware.

Browse, search, pick a source and press play exactly as in Nuvio. The stream
then opens in a native player that looks like Nuvio's - same controls, same
panels, same typography - but decodes on the PS5 itself: 4K HDR10, Dolby
Vision (HDR10 base layer), HEVC and H.264 in hardware, lossless TrueHD and
DTS-HD audio in 7.1, every subtitle format, and a deep read-ahead buffer so
internet streams play without stalls. When the film ends, or you press back,
you are returned to exactly where you were in Nuvio: progress saved, Trakt
scrobbled, next episode ready.

> Unofficial. This project is not affiliated with, endorsed by or supported by
> the Nuvio team. Nuvio's own interface and services are used as they are
> published; everything listed under "Built for this project" is new.

---

## Features

**Playback**
- Hardware HEVC (8 and 10-bit) and H.264 decoding up to 3840x2160, measured
  at ~1 ms per 4K frame
- HDR10 output with automatic switching of the console's display mode
- Dolby Vision profile 8 plays its HDR10 base layer; profile 5 (no HDR10
  layer) is flagged on screen
- UHD Blu-ray remuxes encoded in tiles, and AV1, decode in software on all
  CPU cores - 4K tiled HEVC at real time with no late frames
- Lossless audio: TrueHD / Atmos, DTS-HD MA, E-AC-3, AC-3, AAC, FLAC, PCM,
  output as 32-bit float 7.1 so 24-bit tracks keep their full resolution;
  the best track for your language is chosen automatically (never commentary)
- 30 seconds of read-ahead for internet streams, kept in one preallocated
  block of memory; a clean pause-and-refill when the network falls behind
  instead of stuttering
- Large files over HTTP are fetched by six connections at once into a 128 MB
  read-ahead cache, because one connection is usually the limit rather than
  the line: Wi-Fi caps a single stream and debrid hosts cap per connection.
  Measured on a PS5 at 118-120 Mbit/s on a 15.7 GB remux. Small files,
  playlists and hosts without byte ranges use the single reader as before
- Instant seeking (0.4-0.7 s to resume on 4K), resume from Continue Watching
- Automatic recovery: a source the hardware decoder rejects is reopened on
  the software decoder at the same point

**Interface**
- Nuvio-style on-screen controls drawn natively over the video: title,
  episode line, quality line (resolution, HDR, codec, audio), progress with
  buffered range, clock and "ends at", pause screen
- Subtitles panel (languages, tracks, size, position, colour, background),
  audio panel, sources panel to switch streams mid-film, episodes panel
- Next-episode card, skip intro, error screen with "try another source"
- Subtitles from the stream (ASS/SSA with full styling, SRT, WebVTT, PGS)
  and from your Nuvio subtitle addons, fetched natively
- Seamless hand-off: the web player never flashes up - the screen stays black
  until the native player appears, and Back returns one step, to the source
  list or the title page

---

## Built for this project

Nuvio's web interface runs unchanged apart from a small PS5 integration patch.
Everything else here was written for the PS5:

| Part | What it is |
| --- | --- |
| `service/` | The PS5 payload. Serves the Nuvio interface on the console, installs and updates the Nuvio app, carries play requests and results between the page and the player, keeps the page's settings, and offers a LAN debug interface for development. |
| `app/src/` | The Nuvio app (title PPSA99176): hosts the Nuvio page in the system browser, then plays natively. The player session, the native on-screen interface and its renderer (canvas, text shaping with FreeType/HarfBuzz/FriBidi, icons, images), subtitles (libass and bitmap), input, screenshots and the bridge to the page. |
| `app/engine/` | The playback engine: demuxing, hardware and software decoding, audio output, A/V sync and GPU presentation, extended here with the read-ahead buffer, packet ring, rebuffering, HEVC stream cleaning, decoder recovery and the subtitle and audio hooks the player needs. |
| `app/runtime/` | The process allocator, tuned so FFmpeg's aligned allocations come from pooled memory. |
| `web/nuvio-ps5.patch` | The PS5 platform layer for Nuvio's web app: native-player hand-off, return navigation, subtitle requests, input and service clients. |
| `toolkit/` | Tools that turn the linked program into a signed PS5 application. |

### The engineering behind it

Getting a stable 4K player out of the PS5 took a long series of
hardware-verified fixes. A summary of the harder ones:

- **Memory.** FFmpeg asks for 64-byte-aligned memory for every packet and
  buffer reference. The process allocator served each such request as its own
  memory mapping, so a lossless audio track (1,200 packets a second) exhausted
  the app's memory within seconds of deep buffering. The allocator now serves
  small aligned blocks from pools, and the packet queues copy data into one
  preallocated ring per stream - memory stays flat for hours of playback.
- **Buffering.** Queue limits measured in packets meant a TrueHD track held
  video read-ahead to under two seconds. Queues are now budgeted in seconds
  and bytes; underruns trigger a proper rebuffer that pauses audio and video
  together and resumes in sync.
- **The PS5 video decoder.** UHD Blu-ray remuxes repeat their parameter sets
  in-band and sometimes change them; the decoder refused keyframes that
  carried both the container's copy and a changed in-band copy, which showed
  as green and scrambled blocks. Access units are now cleaned - Dolby Vision
  RPU and enhancement-layer units removed, superseded parameter sets dropped -
  before they reach the hardware. A refused keyframe holds back the pictures
  that depend on it instead of decoding garbage, and seeks rebuild the decoder
  rather than reset it.
- **Tiled HEVC.** Some remuxes switch to a 4x4 tile grid the hardware decoder
  will not take. These are detected from the stream and decoded in software,
  which needed a rebuilt FFmpeg/dav1d toolchain to run 4K at real time.
- **Deadlocks.** Audio waiting for video, video waiting for the demuxer, the
  demuxer waiting for room in the audio queue - each such cycle found on
  hardware is broken by bounded overshoot and starvation checks.
- **Return to Nuvio.** The page's route, profile and playback context survive
  the round trip through the native player, even across a restart of the
  service, so Back lands one screen back and never on the profile picker.

---

## Install

Requirements: a jailbroken PS5 that can load ELF payloads. Tested on
firmware 13.60 with the Relapse exploit, kstuff-lite, ShadowMount+ and
etaHEN; any firmware with a working ELF loader should do.

1. Download `nuvio-ps5.elf` from the latest release.
2. Send it to your ELF loader (port 9021) after your usual payloads. The
   service starts, installs the Nuvio tile and shows "Nuvio ready".
3. Open Nuvio from the home screen and sign in as usual.

Tip: add `nuvio-ps5.elf` to the end of your payload autoload list so it starts
with the jailbreak - PS5 jailbreaks are tethered, so it needs loading again
after every reboot. Sending a newer `nuvio-ps5.elf` updates the app the next
time Nuvio is closed.

---

## Troubleshooting

- **"No streams found", or an addon shown in red.** If the addon only
  returns torrents (Torrentio without a debrid service, for example), Nuvio
  PS5 says how many torrent links it found: the PS5 cannot stream torrents
  directly. Add a debrid service (Real-Debrid, AllDebrid, TorBox, ...) in the
  addon's own configuration and its links play. If an addon shows red
  everywhere, your internet provider may block its site: set the PS5's DNS
  to 1.1.1.1 and 1.0.0.1 (Settings > Network > Settings > Set Up Internet
  Connection, your network, Advanced Settings, DNS Settings: Manual).
- **A white screen on launch, a blank sign-in frame, or every addon red.**
  Nuvio needs ordinary DNS lookups to work. Blocking Sony's own servers is
  not itself the problem: a console set up that way plays media here normally.
  The resolver used to do the blocking can be, though, and the symptoms are a
  white screen for up to a minute on launch, a sign-in frame with no code, or
  every addon in red. Which of the two it is has not been pinned down, so the
  thing to do is test it: point the console at 1.1.1.1 and 1.0.0.1 (Settings >
  Network > Settings > Set Up Internet Connection, your network, Advanced
  Settings, DNS Settings: Manual) and see whether it clears. If it does, the
  resolver was the cause; if it does not, please open an issue. Changing DNS
  does not affect a jailbreak that is already running; set your own resolver
  back before the next time you jailbreak if your entry point needs it.
- **The sign-in QR code does not appear.** The frame says when no code could
  be fetched, and Nuvio keeps retrying on its own. A PS5 whose date and time
  are badly wrong cannot make secure connections: set them under Settings >
  System > Date and Time. It can also mean Nuvio's sign-in service is down
  for a while.
- **Controller.** Move with the D-pad, or with the left stick as a cursor.
  After a D-pad press, Cross always acts on the highlighted item, wherever
  the cursor rests; moving the stick switches back to the cursor. Circle goes
  back, also from the player to Nuvio.
- **A stream fails or the app closes.** Please open an issue with your
  firmware, the addon, and the file name or format (MKV/MP4, codec, HDR).

---

## Support

Nuvio PS5 is an unofficial port: it isn't made or funded by the Nuvio team,
and donations here go to this project, not to them. It was built and tested
on real hardware, one stream, codec and console quirk at a time. If it's
become how you watch on your PS5, a coffee helps cover the cost of building
it and keeps the updates coming.

<a href="https://buymeacoffee.com/theghostonline"><img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png" alt="Buy Me a Coffee" height="50"></a>

Not able to donate? A star on the repository and a bug report with the
source type, codec and HDR format help just as much.

---

## Build from source

Prerequisites: the PS5 Payload SDK with its homebrew sysroot (FFmpeg 7.1.1
built with dav1d, libass, HarfBuzz, FriBidi, FreeType, libpng, libjpeg,
libwebp), LLVM with lld, Node.js, Python 3, GNU bash 4+, make, meson, ninja,
nasm.

```sh
# 1. Web interface: upstream NuvioTVSmart at commit 48a94b3 plus the PS5 patch
git clone <NuvioTVSmart repository> web/NuvioTVSmart
git -C web/NuvioTVSmart checkout 48a94b347837965e2e052f82707820c0dd7c8028
git -C web/NuvioTVSmart apply ../nuvio-ps5.patch
(cd web/NuvioTVSmart && npm ci && npm run build)

# 2. Nuvio's public runtime config, taken from an official Nuvio TV release
cp <official release>/nuvio.env.js web/nuvio.env.js

# 3. The app
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
app/scripts/build.sh

# 4. The payload (embeds the web build and the app)
make -C service
```

The result is `service/nuvio-ps5.elf`.

---

## Known limitations

- The PS5 cannot output Dolby Vision, so it is shown through its HDR10 base
  layer. Profile 5 has no such layer: it plays with wrong colours and is
  flagged on screen, so pick an HDR10 version of those. Profile 7
  enhancement layers are not used.
- Torrents are not streamed directly: an addon's torrent links play through
  a debrid service (see Troubleshooting).
- Dolby Atmos and DTS:X play their 7.1 bed: the PS5 gives apps no way to pass
  the original bitstream to a receiver, so height and object information is
  not available.
- Tiled 4K HEVC and 4K AV1 decode on the CPU: smooth at 24 fps, but seeking
  in them takes about two seconds.
- The PS5 web browser alone (without the app) falls back to its own player.

---

## Not here yet

Each of these is already working in [PS5VR](https://github.com/theghostonline/PS5VR),
the sibling project, on the same console and the same decoder. None of it is
VR-specific, so it is a port rather than new ground.

**Picture**
- **8K decoding.** Nuvio stops at 3840x2160. The hardware decoder takes HEVC
  up to 7680x3840, which PS5VR plays.
- **Frame generation.** Where the decoder cannot keep up (8K60 arrives at
  30 fps), motion estimation on the GPU builds the frames in between. PS5VR
  does ~55-60 pictures a second from a 30 fps decode.
- **FSR 1 upscaling and sharpening** for sources below the panel's
  resolution.
- **HDR in float buffers**, so highlights stay above SDR white instead of
  being tone-mapped.

**Sound**
- **First-order ambisonics (AmbiX)**.
- **Automatic audio delay correction** when a decode runs slow, instead of
  letting the two drift.

**Sources**
- **Local files**: internal storage, USB drives and extended storage. Nuvio
  plays only what an addon hands it, so a file already on the console cannot
  be opened.
- **DLNA / UPnP servers** (Plex, Jellyfin, Emby). This is the shortest route
  to the Jellyfin and Plex libraries people have asked for.
- **RSS / Atom feeds** with video enclosures.

**Formats**
- **Spatial video (MV-HEVC)** and **3D Blu-ray (H.264 MVC)**. On a TV these
  would play as one eye, which is the honest reason this is low priority
  here and high priority there.

---

## License

Copyright (C) 2026 Husam Osman. Nuvio PS5 is free software under the GNU
General Public License v3.0 or later - see `LICENSE`. Forks and derivatives
must stay open source under the same license and keep these notices. Third-party components and their licenses are listed
in `THIRD_PARTY_NOTICES.md`; original copyright notices are kept in the
source files.
