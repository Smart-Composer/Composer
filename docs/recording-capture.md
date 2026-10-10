# Original MIDI capture

Recording preserves MIDI channel messages before Tracktion's monitoring transformations.
`RecordingCapture` writes their original bytes and mapped timestamps to a bounded, preallocated
buffer. Collection retains accepted submission order and reports invalid messages, invalid
timestamps and overflow. If a changing timestamp correction moves an event backwards, collection
raises its time to the preceding event's time instead of rearranging the MIDI messages. This
preserves order-dependent note and controller sequences; it is not musical quantization.
`PendingTake::arrivalTimeAdjustments` counts those timing adjustments. Valid events mapped before
the origin retain their bytes at time zero, counted separately by `preOriginEvents`. These are
informational diagnostics; the stored times of those events have been adjusted.

## Clock admission

`CaptureClockWitness` observes completed graph processing. `CaptureClock` runs in the engine's
global output processor slot, after the graph, and publishes the completed stream endpoint and
the input's timestamp correction and the stream sample that anchored it at block start.
Use the enabled dedicated virtual input that receives monitoring, rather than the disabled
physical wrapper. A generation cannot begin unless that input is a current engine device;
disabling it faults the publisher. Audio and MIDI buffers pass through unchanged.
The publisher does not allocate, wait, read the wall clock or perform project operations.

Each recording uses a fresh generation. Message-thread `CaptureCalibration` requires two
advancing playback sync points, each followed by a later completed publisher and graph witness.
The playback context must remain allocated, playing, straight and at the same sample rate.
After the two-point proof, calibration waits for the correction anchor to pass the fixed take
origin by one millisecond plus one sample. This accounts for the block-start correction and
whole-millisecond native input timestamps. The context's current offset is checked again before
the mapping is returned. Starting the transport alone does not establish a valid capture origin.

The MIDI callback reads the actual publisher signals, brackets its correction read with generation
checks, and maps a timestamp to a sample before converting it to take-relative seconds. Timestamp
zero is literal; there is no wall-clock fallback. Callback capture performs no allocation, locks,
waiting, file access or live engine calls.

The owner polls publisher freshness on the message thread. Generation changes, invalid correction,
missing or regressing publication and late owner service close admission and mark the take
incomplete. A later healthy clock cannot reopen or rehabilitate that take. Device, transport,
playback-context, sample-rate and origin changes require explicit invalidation by the owner.

The physical audio callback lock serializes publisher lifecycle changes. Hosted test callers
must serialize their explicit processing calls. Install the publisher after its graph witness
exists. The publisher retains the witness's storage if it is removed from the graph, but must
still be removed before destroying its edit or engine. Input storage is also retained. Manager
membership can change without clearing an old input's enabled flag, so the owner must check exact
current membership during the take and invalidate before an owned topology change.

## Input ownership and stopping

`RecordingInput` takes ownership of an already opened, stopped JUCE input. The caller supplies a
dedicated enabled Tracktion virtual input with no other physical sources. The router captures
the original first and forwards it once for monitoring. Its constructor registers the callback,
opens capture admission and starts input last. It does not enumerate or open a physical device.

Monitoring can apply Tracktion's normal filtering and timing transformations. The completed-block
correction can also differ briefly from the correction at a new block boundary. Neither the raw
archive nor these components promise identical physical monitoring timing or latency compensation.
Only the capture portion of the callback has the bounded allocation-free guarantee.

Stopping closes admission, stops the JUCE input and removes its callback before sealing the take.
Sealing checks final clock validity even when no MIDI arrived after an interruption. The publisher
must stay alive through this check. The input is released before callback dependencies; an
abandoned router marks its take incomplete. Explicit stop retains ownership if detachment throws,
so destruction cannot silently leave a registered callback pointing to freed memory.

After producers have joined and sealing succeeds, `collect()` creates a retained `PendingTake`.
Allocation failure leaves the originals available for retry. Repeated successful collection
returns the same object. Collection and destruction after sealing do not read the publisher,
so the retained originals survive clock teardown.

The application must retain its recording ticket, obtain a coherent final endpoint while the
publisher is live, and stop and detach the clock before replacing playback. Duration uses the
mapped stream endpoint and must also cover any later retained raw event. The graph endpoint is
a different time domain and is not a substitute. Interrupted takes remain recoverable data and
must not be passed to project completion as successful recordings.

## Verification and scope

Synthetic tests exercise the actual hosted Tracktion graph, publisher, calibration, raw capture
callback and virtual monitor. Mixed message lengths across all 16 MIDI channels retain their
bytes, mapped sample positions and submission order. That fixture explicitly sends each event
to capture and the virtual monitor; it does not exercise the native `RecordingInput` wrapper.
Rate changes and publisher faults preserve the originals while rejecting successful completion.
Other tests cover concurrent producers, numerical limits, freshness, overflow and allocation-free
capture in Debug.

These tests do not open physical devices or prove physical callback registration, driver timing
or hardware latency. Device selection, the application recording controller and its interactive
controls are separate integration work.
