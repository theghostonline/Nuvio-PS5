# Third-party notices

Nuvio PS5 - Copyright (C) 2026 Husam Osman - is distributed under the GNU
General Public License v3.0 or later. It builds
on, links or bundles the following components, each under its own license.

## Source in this repository

| Component | Where | License |
| --- | --- | --- |
| Nuvio TV web app (NuvioTVSmart) - modified by `web/nuvio-ps5.patch` | `web/` | GPL-3.0 |
| PS5 media engine and app packaging toolkit, from an open-source GPL-3.0 PS5 media player; modified for this project in 2026 (see the history of `app/engine/` and `toolkit/`) | `app/engine/`, `toolkit/` | GPL-3.0 (original notices kept in the files and in `app/engine/LICENSE`) |
| hbldr / elfldr payload loader, Copyright (C) 2024 John Törnblom | `service/src/hbldr/` | GPL-3.0-or-later |
| cJSON | `app/engine/addons/src/cJSON.c` | MIT |
| NanoSVG | `app/third_party/` | zlib |
| Inter typeface | `app/assets/` | SIL Open Font License 1.1 |
| Roboto typeface | `app/assets/` | Apache-2.0 |
| Noto Naskh Arabic, Noto Emoji | `app/assets/` | SIL Open Font License 1.1 |
| Mozilla CA certificate bundle | `service/assets/cacert.pem` | MPL-2.0 |

## Linked libraries (from the PS5 SDK sysroot)

| Component | License |
| --- | --- |
| FFmpeg 7.1.1 (libavformat, libavcodec, libavutil, libswresample, libswscale) | LGPL-2.1+ / GPL as configured |
| dav1d | BSD-2-Clause |
| x264 | GPL-2.0+ |
| libass | ISC |
| FreeType | FreeType License |
| HarfBuzz | MIT |
| FriBidi | LGPL-2.1+ |
| Fontconfig, Expat | MIT |
| libpng | libpng License |
| libjpeg-turbo | IJG / BSD-3-Clause |
| libwebp, libsharpyuv | BSD-3-Clause |
| libxml2 | MIT |
| zlib | zlib |
| bzip2 | bzip2 License |
| xz (liblzma) | 0BSD / public domain |
| zstd | BSD-3-Clause |
| libiconv | LGPL-2.1+ |
| OpenSSL 3 | Apache-2.0 |
| libcurl, libpsl | curl License / MIT |
| GNU libmicrohttpd | LGPL-2.1+ |
| Jansson | MIT |
| libarchive | BSD-2-Clause |
| PS5 Payload SDK runtime | see the SDK's own license |
