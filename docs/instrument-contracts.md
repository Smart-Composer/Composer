# Instrument and patch-edit contracts

This version describes one instrument slot shared by the application and its
VST3 wrapper. It includes a patch value and one project editing operation.
Samples, modulation graphs, accompaniment and language-worker protocols are
outside this version.

## InstrumentPatch

[The patch schema](contracts/instrument-patch.schema.json) defines the complete
JSON object. Every field is required; loading never supplies missing defaults.
Creating a new patch explicitly uses the values below. Unknown fields and
unsupported versions are errors.

| Field | Unit or values | Inclusive bounds | New-patch value |
| --- | --- | --- | --- |
| `schema_version` | Integer | `1` | `1` |
| `waveform` | `sine`, `saw`, `square` | Exact lowercase spelling | `sine` |
| `gain_db` | Output gain in decibels | -60 to 0 | -12 |
| `attack_seconds` | Seconds | 0 to 10 | 0.01 |
| `decay_seconds` | Seconds | 0 to 10 | 0.1 |
| `sustain_level` | Envelope amplitude fraction | 0 to 1 | 0.7 |
| `release_seconds` | Seconds | 0 to 10 | 0.2 |
| `cutoff_hz` | Low-pass cutoff in Hz | 20 to 20000 | 8000 |
| `resonance_q` | Low-pass resonance Q | 0.1 to 10 | 0.7071067811865476 |

Zero envelope durations mean immediate stage transitions and must not cause a
division by zero. The renderer derives a Nyquist-safe effective cutoff from the
sample rate without changing the stored patch. Render smoothing is separate
from patch validation; out-of-range input is rejected rather than silently
clamped. Zero decibels means unity gain; sustain zero is valid and makes the
sustain stage silent.

All numeric values must be finite. Booleans, numeric strings and null are not
numbers. Version and revision fields require integer JSON tokens: a decimal point
or exponent is invalid even when its value would be integral. In particular,
`1.0`, `1e0` and a fraction that rounds to an integer are rejected. The schemas
express value constraints; the decoder also enforces this token rule. Object
member order and whitespace do not change meaning. Duplicate members, trailing
content, malformed JSON and numeric overflow are errors. Loading either succeeds
with a complete validated patch or leaves the existing instrument unchanged.

Serialization emits the complete object in lexicographic key order, using UTF-8,
locale-independent round-trip numbers and no nonfinite extensions. Normalize
negative zero to zero. Compatibility is measured by decoded values, not incidental
float formatting or input member order. Repeated encoding by the same encoder
must be stable. Version 1 does not migrate unknown older or newer formats.

## ProjectCommand

[The command schema](contracts/project-command.schema.json) defines the only
operation: `replace_instrument_patch`. It supplies a complete patch, an expected
project revision and a project-instance identifier. There is one instrument slot;
track targeting and partial parameter operations are later extensions.

`project_instance_id` is an opaque ASCII token of 1 to 128 characters drawn from
`A-Z`, `a-z`, `0-9`, `_` and `-`. The application creates a fresh token on every
new/open/reload operation, including reopening the same saved project. The token
is transient and not restored from
the project file. A VST3 state blob stores the patch, not this application token.

Revisions are integers from zero to 9007199254740991. A newly opened project
starts at zero with a fresh token. Every effective edit, undo and redo increments
the current revision; undo does not restore the old revision. Every project edit
that changes the context of an asynchronous request must use this same revision.
At the maximum revision, any operation that would advance it is rejected without
changing state. Never wrap the counter.

The executor validates the entire command, checks its project token and expected
revision, then changes the patch and history as one transaction. Invalid, stale
or differently scoped commands leave patch, revision, undo and redo unchanged.
Validate scope and revision even if the requested patch equals the current one.
A valid equal-value replacement is a no-op: it preserves revision and both history
stacks, including when the revision counter is exhausted. Outcome precedence is
invalid command, wrong project, stale revision, equal-value no-op, exhausted
revision, then applied. A successful new edit clears redo and records the
before/after patches.

Undo and redo are application history operations, not additional serialized
command types. Each restores the recorded patch values while advancing the
current revision. Empty-history operations are no-ops, including at the maximum
revision. A worker never receives authority to rewind the counter or replay a
command against another instance.

## Other project context changes

`ProjectPatchSession::advanceContext()` lets the owning editing thread commit an
effective non-patch change, such as a completed recording, using the same project
revision. It increments that revision once and returns `applied`, preserving the
instance token, patch, undo and redo. At the revision limit it fails without
mutation. This trusted application method has no serialized command or payload;
the caller identifies no-ops before calling it. It is not a worker or audio
callback entry point.

Prepare and validate the replacement, allocate its storage and preserve any
previous performance before advancing. Recheck that prepared work belongs to the
active instance and revision. Then call `advanceContext()` and install the prepared
state with a proven nonthrowing, allocation-free swap in the same editing-thread
transaction, without yielding or notifying observers between these steps. An
error or exception from the call leaves the session unchanged; leave the active
project unchanged too. The method cannot roll back later caller work, so file
operations, engine preparation and other potentially failing work must precede it.

Advance only for an effective accepted context change, not for arming, transport
controls, monitoring, cancelled or rejected capture, saving or failed saves. A
previously prepared patch command becomes stale after the change even when its
patch equals the current patch. Undo and redo remain patch-only and preserve the
performance; label those controls accordingly. Take preservation and any recording
history remain the application's responsibility. Reopening still creates a fresh
instance at revision zero rather than advancing the previous session.

## Diagnostics

Error `code` and `field` identify the failure; exact English message wording is
not part of the contract. Messages are nonempty except for the allocation fallback
described below. Structural validation proceeds in this order: parse JSON, require
an object, check its version, require known members, reject extra members, then
validate member values. Missing patch members are checked in descriptor order;
command members are checked in `type`, `project_instance_id`, `expected_revision`,
`patch` order. No additional priority is promised between multiple invalid
parameter values.

| Failure | Code | Field |
| --- | --- | --- |
| Malformed, duplicate-member, oversized, or excessively nested JSON; leading BOM | `invalidJson` | empty |
| Non-object root | `invalidPatch` or `invalidCommand` | empty |
| Missing or invalid version token | enclosing invalid code | `schema_version` |
| Representable nonnegative integer version other than 1 | `unsupportedVersion` | `schema_version` |
| Missing required member | enclosing invalid code | missing member ID |
| Extra member on an otherwise complete object | enclosing invalid code | empty |
| Invalid known member type or value | enclosing invalid code | member ID |

Version recognition covers unsigned 64-bit integers before comparison, including
values above the 32-bit range. Negative, floating, exponent, boolean, string, and
null version tokens are invalid. Integers beyond the unsigned 64-bit range that
the parser represents as floating point are also invalid. An unsupported version
takes priority over version-1 member requirements.

A command's nested patch errors use `patch.<field>`, or exactly `patch` when the
patch error has no field. They use `invalidCommand`, while an unsupported nested
patch version preserves `unsupportedVersion` with field `patch.schema_version`.

## Native boundary

The shared value and validation interfaces must be independent of the wrappers.
The patch is an allocation-free plain aggregate. A constexpr descriptor table
provides stable parameter IDs, bounds and defaults for editable numeric fields.
Serialization uses nlohmann/json without a JUCE dependency.
The application and VST3 wrapper use the same patch decoder, validator and encoder.
The application owns project-instance identity and undo history. The plugin
wrapper owns host automation and host state callbacks.

Decode, validate and prepare replacement state outside audio processing. Publish
an already prepared snapshot at a safe rendering boundary; never deserialize,
construct patches or perform project/history operations in the audio callback.
The native integration chooses the scheduling and storage mechanism without
changing these value semantics.

## Fixtures

`tests/contracts/fixtures/patches.json` contains synthetic patches with defaults,
both inclusive bounds and a useful changed patch. The remaining fixture files
define value-validation cases, raw parsing failures and state/history sequences.
Fixture metadata and fixture-only operations are not public command types.

The native contract tests consume these same values for patch round trips,
atomic failure and revision/history behavior. They are included in both Windows
verification configurations, run from the repository root:

```powershell
pwsh -NoProfile -File scripts/verify.ps1 -Configuration Release
pwsh -NoProfile -File scripts/verify.ps1 -Configuration Debug
```

See [the build instructions](building.md) for the native toolchain requirements.
Schema validation checks the fixture corpus; it does not establish native parser,
audio, plugin or project correctness.

With Python 3.12 and the packages in `tests/contracts/requirements.txt` already
available, run the standalone corpus check from the repository root:

```powershell
python -B tests/contracts/test_fixture_consistency.py --root . -v
```

This command checks fixture consistency, including the decoder's integer-token
rule and expected history transitions. It is not native validation or a substitute
for the native build and focused tests.

## Input and allocation bounds

Patch and command JSON input is limited to 65,536 bytes, including whitespace.
Larger input and a leading UTF-8 byte-order mark are rejected as `invalidJson`.
Parsing permits at most two nested containers: a root command object and its
flat patch object. A third container or a parser callback depth above two is
rejected before schema validation. These bounds do not change the wire schema.

The four encode/decode functions catch recoverable `std::bad_alloc` failures
across parsing, validation, serialization, and construction or copying of error
diagnostics. The fallback has empty `field` and `message` strings. Its code is
`invalidJson` during parsing, or `invalidPatch`/`invalidCommand` at the later
codec stage. Callers must handle an error even when its diagnostic strings are
empty. The standalone validation and editing-session methods have no added
allocation-failure guarantee.

These catches provide best-effort recovery, not general memory-exhaustion
containment. The pinned JSON library allocates during DOM destruction; failure
of that allocation can terminate the process before a codec catch can run.
