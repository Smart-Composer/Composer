# Frozen v1 instrument

This directory holds an exact copy of the first version of the Composer Instrument: the patch
contract it decodes, the synthesiser core and the JUCE processor around it, with patches saved by
that version. The instrument tests compile it beside the live instrument and compare the two, so
that a later addition to the instrument that changes how a saved v1 patch renders fails a test.

The comparison runs in one test executable, on one CPU, rather than against stored audio or
digests: the C runtime chooses between implementations of some maths functions when a process
starts, according to the CPU, so the same build can produce different bits on another machine.

## Source

Copied from commit `39befa3657f2b61fc2b068fe61953aac2a2ad360`:

| Copy | Original |
|---|---|
| `include/composer_v1/contracts/ContractResult.h` | `include/composer/contracts/ContractResult.h` |
| `include/composer_v1/contracts/InstrumentPatch.h` | `include/composer/contracts/InstrumentPatch.h` |
| `include/composer_v1/instrument/InstrumentProcessor.h` | `include/composer/instrument/InstrumentProcessor.h` |
| `include/composer_v1/instrument/synth/ParameterCurves.h` | `include/composer/instrument/synth/ParameterCurves.h` |
| `include/composer_v1/instrument/synth/PatchState.h` | `include/composer/instrument/synth/PatchState.h` |
| `include/composer_v1/instrument/synth/Synth.h` | `include/composer/instrument/synth/Synth.h` |
| `src/contracts/InstrumentPatch.cpp` | `src/contracts/InstrumentPatch.cpp` |
| `src/contracts/JsonContract.h` | `src/contracts/JsonContract.h` |
| `src/instrument/InstrumentProcessor.cpp` | `src/instrument/InstrumentProcessor.cpp` |
| `src/instrument/PatchParameter.cpp` | `src/instrument/PatchParameter.cpp` |
| `src/instrument/PatchParameter.h` | `src/instrument/PatchParameter.h` |
| `src/instrument/synth/*` | `src/instrument/synth/*`: `Envelope.h`, `LinearSmoother.h`, `ParameterCurves.cpp`, `PatchState.cpp`, `StateVariableFilter.h`, `Synth.cpp`, `Wavetables.cpp`, `Wavetables.h` |

## Transformation

The copies differ from the originals only by two textual replacements, applied to every file,
so that the copy links into the same executable as the live code:

1. `composer::` becomes `composer_v1::`. This renames the top-level namespace in every
   `namespace composer::...` declaration and its closing comment. The originals contain no other
   `composer::` qualifier.
2. `#include <composer/` becomes `#include <composer_v1/`, so includes reach the copies under
   `include/composer_v1`. Quoted includes are unchanged; they resolve beside the copies as they did
   beside the originals.

No other change was needed to build. Line endings are LF.

## Saved patches

`fixtures/patches-v1.json` holds five patches as v1 saved them: the four contract fixture patches
of that commit and a patch whose values have no exact float representation. Each entry, written
compactly with its members in the order given, is the exact state text the v1 encoder produced,
and a test checks that the v1 encoder still reproduces it. The comparisons feed the same text to
both instruments, through each one's own decoder and state restore, so they also check that the
live instrument still accepts what v1 saved.

## Build

`tests/instrument/CMakeLists.txt` builds the copy as the live instrument was built at that commit:
the contract and the synthesiser core as static libraries with the target settings of
`composer_contracts` and `composer_instrument_synth` (C++20, and `/fp:precise` for the synthesiser
core), and the processor and its parameters inside the instrument test executable beside the live
processor, under the same JUCE configuration. The copies are held to the same warning policy as
Composer's own sources, with no exemptions.

Only those target settings are frozen. The compiler, JUCE and global compiler flags are shared by
both instruments, so a change there would change both alike and pass the comparison. Configuring
fails instead when the global flags, the directory's options or either synthesiser core's options
select an instruction set, contract or relax floating-point arithmetic, or enable whole-program
optimisation.

## Checks

- **Sources.** `SOURCES.sha256` lists the SHA-256 of every copy and of the saved patches,
  computed with line endings read as LF. A test recomputes them, and fails naming any file that
  differs or any file added here. The files are never edited; a later version of the instrument
  is frozen in a directory of its own.
- **Output.** Every saved patch renders through both processors at four sample rates and block
  sizes, in three performances: plain MIDI; host automation of every parameter, including writes
  of each parameter's own value and a panic; and bypass, a patch applied and a state restored
  mid-note, a reset and a second prepare at another rate. The float output must be identical bit
  for bit.
- **State.** The float output can hide a change in the last bit of a double behind it, so the two
  synthesiser cores are also driven directly, sample by sample, through the plain and automation
  performances. After every sample their voice counts, effective cutoff and every voice's stage,
  note, envelope level, start order and waveform must be identical bit for bit.
- **Parameters.** Host values after restoring each saved patch, parameter text, text parsing and
  the normalisation curves must match v1 bit for bit over a dense grid of settings.

The comparisons are only as wide as their inputs. A last-bit change in a double that neither the
float output nor the compared state shows, such as one inside the oscillator, the filter or the
gain stage that rounds away before it reaches the output, is not detected; nor is a change that
only inputs outside these patches and performances reach.

Setting `COMPOSER_REFERENCE_RENDER_HASHES` to a file path when running the output comparison
appends the SHA-256 of each render to that file, for comparing machines. Nothing checks those
digests.
