# Composer Instrument

The Composer Instrument is the subtractive synthesiser shared by the Composer application and the
Composer Instrument VST3. Both embed the same instrument processor, built from the same compiled
signal-processing code, so given the same events a patch renders the same in either, sample for
sample. Its patch format is described in [instrument-contracts.md](instrument-contracts.md).

## Signal path

Each voice runs an oscillator into a two-pole resonant low-pass filter, then an envelope and the
note's velocity gain. The voices are summed and the output gain is applied. The result is mono,
written to both channels of a stereo output; the instrument takes no audio input.

- **Oscillator**: sine, saw or square, read from band-limited single-cycle tables. Each note uses
  the richest table whose highest harmonic stays below the Nyquist frequency, so aliasing is at
  least 80 dB below the fundamental across the keyboard. Every note starts at a zero crossing.
- **Filter**: a topology-preserving state-variable low-pass. Its gain is 1 at DC and equals Q at
  the cutoff. The cutoff used for rendering is limited to 0.49 of the sample rate; the stored patch
  value is never changed.
- **Envelope**: a linear attack to full level, an exponential decay to the sustain level, and an
  exponential release to silence. Decay and release times are the time a full fall from 1 to 0
  takes, and each stage ends exactly, so a released note becomes exactly silent. A time of zero
  makes its stage immediate.
- **Velocity**: the note's gain is (velocity / 127) squared.
- **Gain**: the patch's output gain in decibels. There is no limiter.

## MIDI

| Message | Response |
|---|---|
| Note on, velocity 1-127 | Starts or retriggers the note on that channel |
| Note on, velocity 0, or note off | Releases the note |
| Controller 123 (all notes off) | Releases every held note on that channel, as its note-offs would |
| Controller 120 (all sound off) | Fades out every note on that channel over 3 ms, with no release |
| Anything else | Ignored |

Every channel is played, and the same note on two channels sounds twice. Pitch bend, the sustain
pedal and other controllers have no effect in this version.

Events at the same sample are handled in the order they arrive. In the VST3, hosts deliver
controllers as parameter changes, which reach the instrument before the notes of the same sample,
so a controller 123 there cannot release a note that starts at that sample.

## Voices

Sixteen notes sound at once. A repeated note retriggers its own voice from its current level
and counts as the newest note. If the waveform has changed since that note started, or the attack
is shorter than 3 ms and so could not rise from the current level without a click, the old voice
fades out over 3 ms and the note starts again from silence. When sixteen notes are sounding, a new
note takes the quietest releasing note, or else the oldest held one, which fades out over 3 ms
instead of stopping abruptly.

The instrument has 32 voices: the sixteen sounding notes and room for notes fading out. A note
taken in the same sample it started, before it has made any sound, needs no fade. A note-on that
finds no free voice is ignored, so an overload never cuts a sound off. That happens when a note
would need a fade while sixteen others are already fading, or in the 3 ms after an all-sound-off,
panic or bypass that follows other fades, when more than sixteen notes can be fading.

## Parameters

Hosts see one automatable parameter per patch field. Each parameter's ID is the patch field
name; its VST3 parameter ID is derived from it. Hosts store automation and sessions against these
IDs and against the normalised curves below, so neither changes once released.

| ID | Name | Unit | Range | Default | Curve | VST3 ID |
|---|---|---|---|---|---|---|
| `waveform` | Waveform | | Sine, Saw, Square | Sine | choice | 604207933 |
| `gain_db` | Gain | dB | -60 to 0 | -12 | linear | 1948451134 |
| `attack_seconds` | Attack | s | 0 to 10 | 0.01 | cubic | 42801032 |
| `decay_seconds` | Decay | s | 0 to 10 | 0.1 | cubic | 1860120314 |
| `sustain_level` | Sustain | | 0 to 1 | 0.7 | linear | 1770322536 |
| `release_seconds` | Release | s | 0 to 10 | 0.2 | cubic | 1899307591 |
| `cutoff_hz` | Cutoff | Hz | 20 to 20000 | 8000 | logarithmic | 199910052 |
| `resonance_q` | Resonance | Q | 0.1 to 10 | 0.7071 | logarithmic | 987871424 |

A cubic curve maps a normalised value n to minimum + (maximum - minimum) * n^3; a logarithmic
curve maps it to minimum * (maximum / minimum)^n.

A patch applied through the processor is kept exactly, value for value. A host setting a
parameter replaces that one value with the value its normalised setting represents; a setting
equal to that of the applied value keeps the applied value exactly. Gain, sustain, cutoff and
resonance changes glide over 20 ms, in decibels, level, log-frequency and log-Q, so they never
click. Envelope time changes apply at once to every sounding note. A waveform change applies to
notes started afterwards.

When a patch is applied while a host automates the instrument, each host edit takes effect
either before the patch, which then replaces it, or after it; never both. An edit never
overwrites a later one, edits made one after another keep their order, and the patch the
instrument plays includes every edit already completed. The host is told the new value of every
parameter the patch changes, including one whose host edit it replaced, unless a later host
edit has already replaced the patch's value. Host edits and the host's reads of a parameter take
a bounded number of steps; an edit that loses eight times in a row to simultaneous edits of the
same parameter gives way to them.

## State

The processor's saved state is exactly its patch in the contract's JSON form, with no wrapper.
Restoring a state that does not decode as a valid patch, or one larger than 64 KiB, leaves the
patch unchanged.

In the VST3, the state a host stores is that JSON followed by a short block that the plugin
framework adds to remember bypass. The plugin removes the block when the state is restored, and
it can restore bypass even when the patch part is ignored.

## Stopping and bypass

- **Panic** from the embedding code, from any thread, fades every note on every channel out over
  3 ms with no release tail. Controller 120 does the same for one channel. The patch is unchanged.
- **A host reset**, which hosts send when they stop processing, silences every voice at once.
- **Bypass** fades every note out over 3 ms and ignores MIDI, so no note is left hanging when
  bypass is switched off.
- The reported tail is 10.003 seconds: the longest release plus a fade.

## Embedding the instrument

- `applyPatch` and `currentPatch` belong to the message thread. `applyPatch` validates the patch
  and returns the validation error, changing nothing, if it is invalid; a valid patch plays from
  the next processed block. A prepared instrument starts from the current patch with no glide.
- `panic` may be called from any thread.
- Processing never allocates memory, takes a lock or validates a patch. Preparation must not
  overlap processing.
- Patch changes and host automation take effect at the start of a processed block: normally the
  next one, or a later one when the block's read of the patch overlaps another change. With a
  fixed patch, output depends only on the sample rate and the sample timing of MIDI events, not on
  how the host divides audio into blocks or on the processor's denormal mode.
