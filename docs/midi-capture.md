# Original MIDI capture

`MidiCaptureBuffer` retains MIDI 1 channel messages alongside the recording and
playback engine. It does not schedule notes, quantize timing, interpret note
lengths, or implement a sequencer. Its output is suitable for a project document
after the caller supplies the patch and duration and validates that document.

Create the buffer with a capacity from 1 through `maxProjectEvents` on the control
thread before attaching MIDI callbacks. Invalid capacity throws
`std::invalid_argument`; storage allocation can throw `std::bad_alloc`. The same
control thread calls `start()` and `finish()`. Never call these methods from an
audio or MIDI callback. Only `submit()` supports multiple concurrent threads.
Detach and join all callbacks before destroying the buffer.

Call `submit(relativeTimeSeconds, message)` with the complete status byte and its
data bytes. The buffer accepts note off, note on (including velocity zero),
polyphonic pressure, controller, program change, channel pressure, and pitch bend
on every channel. It preserves the original bytes, including release velocity.
Running status, malformed message lengths, and data bytes above 127 are rejected.
Timestamps must be finite, nonnegative, and no larger than the project duration
limit. The caller must establish a consistent relative clock and preserve device
or within-block timing before submitting an event.

Take association is determined by the epoch read inside `submit()`, not when an
outer callback receives an event or computes its timestamp. The epoch guard
cannot protect that earlier work. To change the timestamp origin between takes,
detach the recording callback and wait for its running invocations to finish,
stop the old take, establish the new origin and start the next take on the control
thread, then reattach the callback. An invocation first entering `submit()` after
a new `start()` belongs to the new take, even if its timestamp was computed
earlier. The caller must prevent that mismatch by quiescing callbacks around
take and origin changes.

System messages beginning with `0xf0` through `0xff` are counted and ignored;
their payloads are not interpreted or stored. They do not make a channel-message
capture incomplete. Timestamp validation precedes message classification, so an
invalid timestamp is reported even for a system message. Invalid channel data,
invalid timestamps, and capacity overflow each have a separate result and count.
Rejected or ignored events do not consume event slots. A caller must inspect
`CapturedMidi::isComplete()` and report incomplete takes instead of silently
presenting them as successful recordings. Accepted event count is `events.size()`.

Stopping a take prevents new submissions to that take, drains already admitted
writers, then creates and sorts the result on the control thread. Events are
ordered by their exact timestamp; equal timestamps retain the order in which
accepted submissions reserved slots. That reservation is the ordering point for
concurrent producers. Stopping an inactive buffer returns `nullopt`. A second
`start()` while a take is active is rejected without changing it. If allocating
the finished result fails, the stopped take remains available for another
`finish()` attempt; `start()` refuses to discard it.

## Callback and synchronization contract

`submit()` performs bounded validation of at most three channel bytes and a
bounded number of lock-free atomic operations. It does not allocate, lock, wait,
retry a compare-and-exchange operation, perform I/O, or create project objects.
The build requires always-lock-free 64-bit atomics. Capacity and storage never
change during the object's lifetime. As with the other counters, the number of
submissions to one take must fit in an unsigned 64-bit integer.

All synchronization atomics use sequential consistency. A producer first reads
the active epoch, joins the active-writer count, then checks the epoch again. An
epoch is never reused. A producer paused before joining therefore cannot write
after the take stops or into a subsequent take. A producer that passes the second
check reserves a unique slot and releases its writer count after writing it.
`finish()` sets the epoch inactive and waits outside processing until the writer
count is zero. The atomic release/acquire order publishes all completed slot
writes before they are read. A producer joining after that zero observation
fails the epoch check and cannot touch slots or take counters. The writer count
is never reset, including across takes.

This component's tests cover raw byte and timestamp preservation, channel
validation, bounded overflow, concurrent producers, and repeated stop/start
boundaries. They do not establish hardware capture, the source clock's accuracy,
latency compensation, sample-accurate engine playback, or complete recording
acceptance.
