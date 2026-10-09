# Project format

A version 1 project stores one instrument patch and one ordered MIDI performance. All
project parsing, validation and serialization allocate memory and run outside audio
processing. The document API is independent of JUCE and does not read or write files.

The document is UTF-8 JSON with exactly these fields:

| Field | Value |
| --- | --- |
| `schema_version` | Integer `1`. Floating-point tokens such as `1.0` and `1e0` are invalid. |
| `tempo_bpm` | Number `120`. This checkpoint has a fixed tempo. |
| `patch` | Complete versioned instrument patch accepted by the shared instrument contracts. |
| `duration_seconds` | Finite number from `0` through `86400`, inclusive. |
| `events` | Array of MIDI events in nondecreasing timestamp order. |

Each event has exactly two fields: `time_seconds`, a finite number between zero and the
document duration inclusive, and `bytes`, an array containing a complete MIDI 1.0 channel
message. Timestamps are seconds from the start of the performance, stored as C++ `double`
values. Encoding and decoding preserve those values without quantization. JSON numeric
spelling is not preserved.

The first byte must be a status from `0x80` through `0xEF`. Program change (`0xC0` through
`0xCF`) and channel pressure (`0xD0` through `0xDF`) require two bytes; all other channel
messages require three. Each following data byte is an integer from `0` through `127`.
All bytes must use integer JSON tokens. Running status, system messages, SysEx and MIDI 2.0
messages are outside this format.

The events array preserves raw bytes and its original ordering, including equal-time
messages, velocity-zero note-ons, note-off velocity, controller changes, pitch bend and
both kinds of aftertouch. No note pairing, interpretation of controller state or sorting
is performed. Empty performances are valid. A zero-duration performance may contain
events at time zero.

For example, this performance changes program, plays a note and releases it:

```json
{
  "schema_version": 1,
  "tempo_bpm": 120,
  "patch": {
    "schema_version": 1,
    "waveform": "sine",
    "gain_db": -12,
    "attack_seconds": 0.01,
    "decay_seconds": 0.1,
    "sustain_level": 0.7,
    "release_seconds": 0.2,
    "cutoff_hz": 8000,
    "resonance_q": 0.7071067811865476
  },
  "duration_seconds": 1,
  "events": [
    {"time_seconds": 0, "bytes": [192, 7]},
    {"time_seconds": 0, "bytes": [144, 60, 100]},
    {"time_seconds": 0.75, "bytes": [128, 60, 64]}
  ]
}
```

`validateProject` checks in-memory values; `encodeProject` validates before producing
JSON; `decodeProject` returns either a complete validated document or a `ProjectError`.
The decoder rejects unsupported versions, extra or missing fields, duplicate fields
(including escaped spellings of the same key), malformed JSON, literal NUL, trailing
non-whitespace data, invalid numeric types, nonfinite values and out-of-range values.
It also rejects structures nested beyond the four container levels needed by this format.
Invalid values are never clamped, reordered or replaced with defaults. The nested patch
uses the same validation and codec as the instrument.

A document may contain at most 1,000,000 events and at most 128 MiB (134,217,728 bytes) of
JSON, including whitespace. Duration is limited to 24 hours. These are resource bounds,
not musical timing tolerances. Size checks at the file-storage boundary should reject
oversized files before reading them into memory.

The format does not store project-instance tokens, command revisions, filenames, plugin
references, XML or external assets. Opening a project must establish a fresh live editing
context. File replacement, dirty state, recording, playback and previous-take retention
belong to the application and file-storage layers.
