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
- Network access for the first configure, which downloads about 55 MB of pinned sources. See
  [dependencies.md](dependencies.md).

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
Compiler warnings in Composer's own sources are errors in these presets.

## Outputs

| Product | Location under `out/windows` |
|---|---|
| Composer application | `src/app/Composer_artefacts/<Config>/Composer.exe` |
| Composer Instrument VST3 | `src/plugin/ComposerInstrument_artefacts/<Config>/VST3/Composer Instrument.vst3` |

Both link the C++ runtime statically, so neither needs a Visual C++ redistributable.

The instrument loads and accepts MIDI but produces no sound yet; its synth voice is still to
come. The build never installs the plugin. To try it in another host, copy the whole
`Composer Instrument.vst3` folder into that host's VST3 search path, for example
`C:\Program Files\Common Files\VST3`.

## Tests

CTest runs three groups:

- **Instrument**: the shared instrument processor's buses, MIDI handling and sample-rate
  changes, compiled into a test executable. Output buffers start filled with NaN, so a sample
  the processor fails to write fails the test.
- **Integration**: a JUCE host scans and instantiates the VST3 bundle built in the same
  configuration and plays MIDI through it; the sequencing engine renders a MIDI clip offline
  without opening any audio device.
- **Application**: the built `Composer.exe` starts with `--startup-check`, which creates the
  sequencing engine without devices, creates an edit, runs the instrument for one block and
  exits with a non-zero code if any step fails.

In Debug builds, any failed JUCE or Tracktion Engine assertion, including a leak report at
shutdown, fails the test executable that raised it. Tests write only to temporary directories,
which they remove.

## Source layout

| Path | Contents |
|---|---|
| `include/composer/` | Public headers of Composer's libraries |
| `src/instrument/` | The instrument shared by the application and the plugin |
| `src/engine/` | The JUCE and Tracktion Engine library behind the application, and engine setup |
| `src/plugin/` | The VST3 wrapper |
| `src/app/` | The Composer application |
| `tests/` | Test executables and their support code |
| `cmake/` | Dependency pins and build helpers |
| `scripts/` | The verification entry point used locally and in continuous integration |

Continuous integration runs the same script on GitHub's `windows-2025-vs2026` image, in Release
and Debug, for every pull request, merge-queue entry and push to `main`.
