# Dependencies

Composer fetches every third-party library at configure time from a pinned source archive,
either a GitHub archive of an exact commit or a release asset, and verifies the archive's
SHA-256. The pins live in [`cmake/ComposerDependencies.cmake`](../cmake/ComposerDependencies.cmake).
Nothing is vendored in this repository, and no binary is downloaded.

| Library | Version | Upstream commit | Licence used | Role |
|---|---|---|---|---|
| [JUCE](https://github.com/juce-framework/JUCE) | 9.0.3 (release tag) | `be29c81492b6151c8ea8d14c840e1311963b3a83` | AGPL-3.0 | Application framework, audio and MIDI devices, plugin client and hosting |
| [Tracktion Engine](https://github.com/Tracktion/tracktion_engine) | `develop`, 2026-10-08 | `e42d82f06e553771477eabd1dd0d561fe2cbe573` | GPL-3.0-or-later | Sequencing, editing, recording and offline rendering |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 (release asset `json.tar.xz` of tag `v3.12.0`) | `55f93686c01528224f448c19128836e7df245f72` | MIT | JSON library available to Composer's libraries |
| [Catch2](https://github.com/catchorg/Catch2) | 3.16.1 (release tag) | `08092139210881f01a34e60c5f2fd6afa0ea4024` | BSL-1.0 | Test framework; tests only, never shipped |

Archive digests (SHA-256):

- JUCE: `f846e74503e5b959b1de514c772b05ab1cb05478b7513b19d6294cb2b3d2906a`
- Tracktion Engine: `cd8571a1b0ea5e70e342c3315272adc282b505eefa39fc7e7acebd94484dd5c5`
- nlohmann/json: `42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa`
- Catch2: `c66daf9f31712f673ebdffb074c90b5d7e61ea50adc6a1d2db95e798af027940`

## Why these revisions

- **JUCE 9.0.3** is the newest JUCE release. It descends directly from the JUCE revision that
  the Tracktion Engine pin records for its own builds, and Tracktion Engine's continuous
  integration builds that pin against JUCE's development branch, which is 9.0.3 plus later
  fixes, on Windows, macOS and Linux.
- **Tracktion Engine**'s newest tag, v3.2.0, records a JUCE development revision from February
  2025 (JUCE 8.0.6) and does not compile against JUCE 8.0.9 or later. The pin is instead the
  head of its `develop` branch on the date above, 452 commits later; its own Windows build and
  JUCE compatibility jobs passed for that commit.
- Tracktion Engine's archive does not contain its JUCE submodule; only its `modules/` directory
  is configured, against the JUCE pin above, so a single JUCE revision is used throughout.

## Known upstream issues

- Destroying a `MidiInput` after its device is unplugged blocks for about seven seconds on
  Windows ([JUCE#1756](https://github.com/juce-framework/JUCE/issues/1756)). The regression is new
  in 9.0.3 and fixed on JUCE's development branch; move to JUCE 9.0.4 once it is released.
- With the default WinMM MIDI backend, a device that is removed and quickly re-added sends no
  notification, and an input opened on it stays silent
  ([JUCE#1758](https://github.com/juce-framework/JUCE/issues/1758)). WinMM cannot report this
  case; JUCE handles it only through its Windows MIDI Services backend, which Composer does not
  enable yet.

## Licence notes

These notes record what the pinned sources declare. They are not a legal review, and the
distribution obligations they imply remain to be confirmed before the first release.

- JUCE's modules are dual-licensed; Composer uses them under the AGPL-3.0.
- Tracktion Engine is dual-licensed; Composer uses it under the GPL-3.0-or-later. Section 13 of
  the GPL-3.0 and of the AGPL-3.0 permits combining the two in one work, each part keeping its
  own licence.
- JUCE's modules carry third-party code, listed with versions and licences in the software bill
  of materials at the root of the pinned archive, `JUCE.spdx.json`. With the formats and
  features Composer enables on Windows, the products compile in:
  - zlib (Zlib)
  - HarfBuzz, LunaSVG and PlutoVG (MIT)
  - SheenBidi (Apache-2.0)
  - libpng (libpng-2.0)
  - the IJG JPEG library (IJG)
  - libwebp, FLAC, libogg, libvorbis, Opus, opusfile and libopusenc (BSD-3-Clause)
  - the VST3 SDK 3.8.0 (MIT)
- Tracktion Engine compiles in third-party code from its source tree:
  - From `modules/3rd_party`: CHOC (ISC), crill (BSL-1.0), libsamplerate (BSD-2-Clause),
    magic_enum (MIT), rigtorp MPMCQueue (MIT), rpmalloc (public domain) and tl::expected
    (CC0-1.0).
  - From `modules/tracktion_graph/3rd_party`: moodycamel ConcurrentQueue (simplified BSD), its
    lightweight semaphore (simplified BSD, with a part under the zlib licence) and farbot (MIT).
  - SoundTouch's BPM detection, peak finder and sample FIFO (LGPL-2.1-or-later). These are
    compiled even though the SoundTouch time-stretcher itself is disabled.
- Tracktion Engine's optional components stay at their defaults, all disabled. These include the
  AirWindows effects, the Signalsmith, SoundTouch, Rubber Band and Elastique time-stretchers,
  ARA, Ableton Link, FFmpeg, LAME, Cmajor and ReWire.
- JUCE's ASIO support (`JUCE_ASIO`) stays at its default, off, so the ASIO SDK headers JUCE
  carries are not compiled. Enabling ASIO needs its own licence review.
- nlohmann/json and Catch2 are permissive; Catch2 is used only to build tests.
- The notices of all compiled-in third-party code must ship with any binary distribution.

## Changing a pin

1. Pick the new upstream commit or release asset and download its archive from the same URL
   pattern.
2. Replace the URL and the SHA-256 together in `cmake/ComposerDependencies.cmake` and in the
   tables above, and record why the revision changed.
3. Run `scripts/verify.ps1 -Clean` for Release and Debug, and let the hosted checks pass, before
   anything depends on the new revision.
