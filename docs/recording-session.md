# Recording a first MIDI performance

At startup, Composer opens the default or previously saved audio output and the
enabled MIDI inputs, and stores device preferences in its own settings. Choose
an audio output, refresh the MIDI input list and select a keyboard. Adjust
the instrument controls while stopped, then press Record. Wait for the recording
status before playing; preparation needs a running audio clock. Stop keeps the
take. Play auditions it, and Save writes the original MIDI and instrument patch
to a `.composer` project. Open restores that performance and patch.

Capture retains the supported MIDI 1.0 channel messages described in the
[project format](project-format.md). Playback is derived from those originals;
the [application project](application-project.md) documents its equal-time,
unmatched-note and very-short-note limitations.

This first workspace holds one instrument and one performance. Save the current
project before using New for another take. Discard take cancels an active or
pending recording. Undo patch and Redo patch affect instrument edits only. The
event table shows the retained MIDI bytes and times without changing them.

The recording controller joins the project owner, original MIDI capture and live
monitoring into one message-thread operation. It accepts one already opened,
stopped JUCE input and one isolated virtual input from the engine's current device
list. Device selection and physical input opening happen before this operation.

The virtual input must be enabled, monitor input, use no physical source list and
not receive all MIDI inputs. The controller routes it to the project's instrument
track and removes competing destinations from that edit. The existing project
owner admits only a first performance: an existing performance must be saved and
a new project created before another take.

## Preparing and recording

`start` retains the project's recording ticket and enters `preparing`. A regular
message-thread timer calls `poll`; it must keep running while preparing, recording
or stopping. The default preparation limit is five seconds and the capture's
freshness limit is one second. A delayed owner timer can therefore interrupt a
take even if MIDI continues arriving.

Before admitting MIDI, preparation requires a playing graph and a current input
instance. Pending device updates can leave the same playback-context object with
an unallocated graph. Preparation may rebuild that graph and start a fresh clock
calibration. Calibration observes two advancing graph synchronization points,
then waits until the input clock's block-start anchor has passed the fixed take
origin by one millisecond plus one sample. The controller also requires a later
completed graph block before input starts.

Once recording starts, the mapping and take origin are fixed. Context, routing,
rate, tempo, transport and input changes interrupt the take. An admitted take is
never restarted or recalibrated. Delayed driver timestamps that precede the origin
retain their bytes and initially map to zero with an explicit pre-origin count,
then undergo the same ordering adjustment as other events. Backwards timing
corrections raise a later event's time to its predecessor's time, preserving MIDI
message order. These adjustments do not imply lost events. Completion reports
when timing was adjusted; the saved project contains those mapped, adjusted times.

## Stopping and completing

Stopping closes callback admission, joins native input delivery and seals capture
validity while its publisher is alive. It then retains a coherent clock snapshot
before removing the publisher, witness or graph. A transient snapshot collision
can be retried for a bounded interval. Expiry retains an unavailable-endpoint
error; a later retry cannot substitute a newer endpoint.

Completion uses the greater of the mapped endpoint and the last retained event.
This preserves silence at the end and covers valid asynchronous events beyond the
last completed block. It is a completed-block endpoint, not a sample-exact measure
of the Stop button press. No event-only fallback repairs an invalid clock.

Only a complete take can be committed with its exact original recording ticket.
An interrupted take, lost event or invalid time leaves the project unchanged and
retains the captured data. Collection or project-construction failures also keep
the same ticket, original capture and frozen endpoint for `retryCompletion`.
Retries do not lengthen the recording. `discardPending` explicitly cancels that
ticket and releases the retained data after callback shutdown succeeds.

## Ownership

Keep the project and engine alive until the controller is destroyed. While the
controller is busy, it exclusively owns its transport, graph changes and global
output-processor slot. Resolve or explicitly discard a pending take before
destroying the controller. Its destructor joins callbacks as a safety fallback;
it does not save or commit the take. Retention in memory is not crash recovery.

Project and patch controls stay disabled while capture or completion is pending.
Original events remain read only in the event table. Patch controls submit the
same validated, undoable project commands used by other editing clients, and an
asynchronous file choice is rejected if its project identity, revision or editing
state changed while the chooser was open.

## Device selection

Selection temporarily disables and closes only the chosen physical input in the
engine. A dedicated virtual input receives monitoring from the raw MIDI wrapper,
preventing the selected keyboard from entering the same track twice. Inputs with
ambiguous identifiers or shared settings names, ports assigned to external
control and engines with another active project are refused.

Changing the selection or closing the workspace removes its virtual input and
restores the selected port's prior enablement. If disabling that port caused an
automatic default-input fallback, cleanup restores the original default only
while the observed setting still matches that fallback. Independent setting
changes are preserved. An unavailable port can prevent reopening; explicit
release reports that failure and retains its restoration information for retry.
Destruction performs best-effort cleanup without terminating because a device
setting could not be restored.

## Checking the workflow

The Windows verification commands in [Building](building.md) include a synthetic
one-minute recording workflow. It sends 540 MIDI messages through the selected
input and recording controller, checks one monitoring delivery per message,
restores device settings, and compares the saved performance and playback after
reopening it in a fresh engine. Every input and project in this test is synthetic.
After building, run that case alone with:

```powershell
ctest --test-dir out/windows -C Release -R "A selected native lease records a full synthetic minute through the controller" --output-on-failure
```

The test advances hosted audio time and does not measure physical-driver latency
or sustained real-time performance. For a manual device check, use a scratch
project, record a minute after the recording status appears, save it, close and
reopen it, then compare playback and the displayed MIDI events. Check that changing
the selected input or closing the workspace restores its previous input settings.
