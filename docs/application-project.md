# Application project ownership

`ApplicationProject` owns a validated `ProjectSession` and the Tracktion edit that plays it.
The engine outlives the owner. All owner operations run on the message thread, outside audio
and MIDI callbacks. The current component supports one instrument and one original performance.

The project document remains the persistent source of truth: the instrument patch, performance
duration and ordered original MIDI channel messages. Tracktion clips are disposable playback
derived from that document. Saving writes the original document, never an export of those clips.

## Editing and recovery

Patch commands carry the current project instance token and revision. Obsolete results are
rejected. Undo and redo affect the patch while preserving the performance. Open and new require
a clean, stopped session and create a fresh instance token. They cannot silently replace unsaved
work. A successful open resets patch history and starts at revision zero.

An edit constructs its replacement playback before changing the session. Once session validation
and history changes succeed, a pointer swap publishes the prepared playback. Reported construction,
validation or file-loading failures retain the current document and playback. Callers must release
borrowed edit, track and clip handles before any successful mutation that replaces playback.
Allocation recovery covers exceptions that reach the owner. Some JUCE construction APIs are
`noexcept` despite allocating, so this boundary cannot contain every out-of-memory failure.

Saves are synchronous on the owning thread. The checked file store's actual receipt acknowledges
the exact context saved; a failure leaves the dirty state intact. There is no asynchronous save
queue in which an older disk commit can overwrite a newer acknowledged version. File failures
include any recovery-file path returned by the store.

## Recording completion

Beginning a take returns a ticket for the capture owner to retain and freezes patch edits, replacement, saving and
ordinary playback. A second take cannot overwrite an existing performance. Capture storage and
device setup must be prepared before input is admitted.

Before completion, the input owner closes and joins every producer and verifies that its clock
remained valid. `commitRecording` additionally rejects the capture buffer's invalid-message,
timestamp and overflow diagnostics. It constructs and validates the new document and playback
before advancing the session revision. Completion and cancellation require the original ticket's
project context and attempt identity. A retained result from a cancelled take or another project
cannot be substituted for a newer take. A failure retains the pending attempt and leaves the caller's
captured events untouched for recovery or retry. Cancellation releases the pending attempt without
changing the original document. An empty zero-duration completion leaves the revision unchanged.

The capture callback, device selection and clock admission are separate integration work. Calling
`beginRecording` alone does not register a device or start capture.

## Derived playback

Conversion uses fixed 120 BPM with one clip per MIDI channel. It retains note release velocity,
disables quantization, MPE and implicit bank changes, and adds one sample to the derived clip
extent so inclusive endpoint controllers can play. Rebuild the playback when the audio sample
rate changes; changing that rate does not edit the persistent document or its revision.

The converter reports repeated-pitch closures, terminal closures for held notes, orphan note-offs,
velocity-zero note-offs and suppressed short notes. Repeated notes close the earlier note;
unmatched held notes close at the original duration; orphan note-offs and notes of at most
0.00001 beats are omitted from the clips. The original events remain unchanged.

Tracktion remains the playback scheduler. Its equal-time order can differ from the original
event order, and the pinned graph can move a terminal note-off one sample earlier even with the
extra clip extent. The converter does not claim a bit-preserving MIDI replay for those cases.

## Verification

Tests drive the actual hosted Tracktion graph through the shared instrument. A synthetic minute
of MIDI retains its original bytes, order, timestamps, duration and nondefault patch after save
and reopen in a fresh engine, with identical rendered audio before and after. Other cases cover
patch undo/redo, obsolete commands, incomplete capture, cancelled takes, file failures and
sample-rate changes. Separate graph tests exercise conversion limits and inclusive endpoints.

These tests use synthetic input and scratch projects. They do not open physical MIDI or audio
devices, establish hardware latency, or complete the interactive recording workspace.
