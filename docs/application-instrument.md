# Application instrument

`InstrumentAdapter` connects a Tracktion instrument position to the shared
`InstrumentProcessor`. Register its built-in type with the engine's plugin manager
before creating an edit. It accepts MIDI and produces stereo audio, sharing the
synthesizer and patch format used by the VST3 processor. It preserves the event
order supplied by Tracktion. Simultaneous notes and controllers can therefore
sound different when a VST3 host supplies them in another order.

The editing-thread owner applies the validated project patch before playback and
keeps the project document as the persistent source of truth. The adapter does not
save projects or implement undo. Its `currentPatch` accessor belongs to the editing
thread. Preparation, release and final destruction require the rendering graph to
be quiescent. Detach the graph before dropping its last plugin reference.
Reset the adapter before disabling it. Tracktion skips disabled built-ins, so a
note-off received while disabled cannot release a previously held voice. A user
interface that exposes disabling must enforce this owner action.

Rendering uses a preallocated bridge with 1,024-sample slices and space for 4,096
incoming messages per render context. Each supported channel message retains its
bytes and order at its sample position. Context timestamps include Tracktion's MIDI
offset, round to the nearest sample, and clamp late messages to the first sample.
Messages at or beyond the context end are left for another context. System messages
are ignored, but count toward the input limit. Incoming Tracktion MIDI is never
consumed or modified.

Only the requested destination range is written. Stereo output uses the processor's
two channels; mono output averages them, and additional channels are cleared.
Rendering without a destination still advances voices. A zero-length context may
request all-notes-off without consuming an event. The adapter reports the shared
processor's tail length.

`midiPanic` and Tracktion's all-notes-off flag request the instrument's short fade.
`reset` requests immediate silence at the next processing boundary. Invalid render
contexts, malformed channel messages and excess MIDI input latch a diagnostic,
silence a valid destination span, and request a hard reset before processing resumes.
The editing thread can retrieve and clear accumulated flags with
`consumeRenderFaults`. An invalid destination range is never written.

Preparation reserves MIDI storage. Events are sorted by sample and original order,
then copied into the pinned JUCE MIDI buffer's public packed storage. This avoids
searching the whole buffer for each insertion. Tests compare the packed bytes with
JUCE's insertion API, including mixed message sizes and simultaneous events.
Rendering performs no patch validation, file
access or project operations. The allocation test observes the Windows debug
runtime during full-capacity rendering, overflow and unprepared processing, with a
positive control for its allocation hook. Other tests compare actual processor
samples across slices and rates, check panic/reset and destination boundaries, and
render a clip through the headless Tracktion graph.
