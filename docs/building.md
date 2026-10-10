# Building Composer on Windows

## Requirements

- Windows 10 or 11, x64.
- Visual Studio 2026 or Visual Studio Build Tools 2026 with the **Desktop development with C++**
  workload, including the MSVC x64 tools, a Windows 11 SDK and **C++ CMake tools for Windows**
  (CMake 3.28 or newer and Ninja). Composer has been built and tested with Visual Studio 18.1
  (MSVC 14.50) and, in continuous integration, with the toolchain of GitHub's
  `windows-2025-vs2026` image (MSVC 14.51 when this was written).
- Git.
- PowerShell 7 (`pwsh`), or Windows PowerShell 5.1.
- Network access for the first configure, which downloads about 68 MB of pinned sources, 13 MB
  of them the VST 3 SDK used only by the tests. See [dependencies.md](dependencies.md).
- A short checkout path of plain ASCII characters, such as `C:\src\Composer`. The deepest build
  outputs sit about 180 characters below the checkout, and the MSVC tools fail on paths longer
  than 260 characters. The VST 3 SDK's module loader, used by the validator and the render host,
  cannot open a plug-in whose path contains other characters.

## Build and test

From the repository root:

```powershell
pwsh -NoProfile -File scripts/verify.ps1 -Configuration Release
```

With Windows PowerShell 5.1, whose default execution policy blocks local scripts, run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\verify.ps1 -Configuration Release
```

The script finds Visual Studio with `vswhere`, loads its x64 developer environment for its own
process, then configures, builds and runs every test through the `windows-release` CMake workflow
preset. Use `-Configuration Debug` for a debug build and `-Clean` to delete the binary directory
first. It exits with a non-zero code when any step fails.

From a shell that already has the **x64** MSVC environment, such as the *x64 Native Tools
Command Prompt for VS* or a developer PowerShell entered with `-Arch amd64`, the presets can be
used directly:

```powershell
cmake --workflow --preset windows-release
cmake --workflow --preset windows-debug
```

Configuring with a 32-bit compiler stops with an error; Composer builds for x64 only. Both
configurations share one binary directory, `out/windows`, generated with Ninja Multi-Config.
The tests need a Ninja generator; with any other, configure with `-DCOMPOSER_BUILD_TESTS=OFF`.
Compiler warnings in Composer's own sources are errors in these presets.

## Outputs

| Product | Location under `out/windows` |
|---|---|
| Composer application | `src/app/Composer_artefacts/<Config>/Composer.exe` |
| Composer Instrument VST3 | `src/plugin/ComposerInstrument_artefacts/<Config>/VST3/Composer Instrument.vst3` |

Both link the C++ runtime statically, so neither needs a Visual C++ redistributable.

When tests are built, `out/windows/v3/bin` also holds Steinberg's `validator.exe` and
`composer_vst3_render.exe`, built from the pinned VST 3 SDK as a separate, always optimised
project (`tests/vst3host`). They are test tools and are never shipped. To validate a build by
hand, run `out\windows\v3\bin\validator.exe -e` with the path of the `Composer Instrument.vst3`
folder.

The instrument is described in [instrument.md](instrument.md). The build never installs the
plugin. To try it in another host, copy the whole `Composer Instrument.vst3` folder into that
host's VST3 search path, for example `C:\Program Files\Common Files\VST3`.

## Tests

CTest runs six groups:

- **Contracts**: the versioned instrument patch and project command formats, independent of
  JUCE. They decode, validate and re-encode the shared fixtures in `tests/contracts/fixtures`.
- **Project**: the versioned project document, bounded MIDI capture and checked file storage,
  with synthetic performances in temporary directories.
- **Synthesiser core**: the instrument's signal processing, independent of JUCE: parameter
  curves, band-limiting and aliasing, filter response, envelope timing, smoothing, voice
  allocation and fades, click limits, patch changes reaching the sound (sustain, cutoff,
  resonance and release on a playing note, attack and decay on the next one), the order of patch
  publication and host edits under every interleaving of their steps in several small
  scenarios, and output that is identical whatever the block sizes or denormal mode.
- **Instrument**: the processor shared by the application and the plugin: host parameters and
  their notification, exact patch application, state, panic, the stop controllers, bypass,
  block-size independence, and that processing never calls operator new or delete (in Debug
  builds, never allocates through the C runtime either). Output buffers start filled with NaN,
  so a sample the processor fails to write fails the test. A frozen copy of the first version of
  the instrument, with patches it saved, is compiled into the same test from
  `tests/instrument/reference/v1`. Each saved patch must render bit for bit the same through it
  and through the current instrument, with host automation, bypass, patch changes, state
  restores, a reset and a second prepare, at four sample rates. The two synthesiser cores'
  double-precision voice and filter state must also match after every sample. These comparisons
  cover the inputs they use; the reference's README describes what they cannot see. Every v1
  host parameter keeps its place, identity, steps and displayed text, recorded in a snapshot
  that must describe the frozen copy exactly; its host values, text parsing and normalisation
  curves must match the frozen copy over a dense grid of settings; and no VST3 parameter ID may
  collide with those the plugin framework reserves.
- **Integration**: the VST3 bundle built in the same configuration, in three hosts.
  - A JUCE host: the VST3 must render every fixture patch bit for bit like the processor compiled
    into the test after a state restore, through a performance that includes all-notes-off and
    all-sound-off. Host automation is compared bit for bit for one patch; a VST3 prepared again
    at a new sample rate must match a fresh one; the stop controllers, a host reset and bypass
    are checked on the VST3 alone. The VST3 must keep its component class ID, and the wrapper's
    own bypass and MIDI controller parameters must use the IDs the patch parameters avoid.
  - Steinberg's validator runs its extensive suite on the bundle.
  - A render host built on the VST 3 SDK's own hosting library, not on JUCE, plays the same
    performance through the VST3, sending controllers as a VST3 host does, through the
    plug-in's MIDI mapping; its output must match the processor bit for bit.

  The sequencing engine renders a MIDI clip offline without opening any audio device.
- **Application**: the built `Composer.exe` starts with `--startup-check`, which creates the
  sequencing engine without devices, creates an edit, runs the instrument for one block and
  exits with a non-zero code if any step fails.

In Debug builds, any failed JUCE or Tracktion Engine assertion, including a leak report at
shutdown, fails the test executable that raised it. The VST3 module carries its own copy of JUCE,
so an assertion inside it is not seen by the test executables, the validator or the render host;
those check the plug-in's behaviour instead. Tests write only to temporary directories,
which they remove.

The hosted workflow also checks the contract fixtures against the published JSON schemas in
`docs/contracts`, independently of the native decoder. To run that check locally with Python
3.12:

```powershell
python -m pip install -r tests/contracts/requirements.txt
python tests/contracts/test_fixture_consistency.py
```

## Source layout

| Path | Contents |
|---|---|
| `include/composer/` | Public headers of Composer's libraries |
| `src/contracts/` | The versioned patch and command contracts, independent of JUCE |
| `src/instrument/` | The instrument processor shared by the application and the plugin |
| `src/instrument/synth/` | The synthesiser core, independent of JUCE |
| `src/engine/` | The JUCE and Tracktion Engine library behind the application, and engine setup |
| `src/plugin/` | The VST3 wrapper |
| `src/app/` | The Composer application |
| `tests/` | Test executables and their support code |
| `cmake/` | Dependency pins and build helpers |
| `scripts/` | The verification entry point used locally and in continuous integration |

Continuous integration runs the same script on GitHub's `windows-2025-vs2026` image, in Release
and Debug, for every pull request, merge-queue entry and push to `main`; the Release run also
performs the contract fixture check.
