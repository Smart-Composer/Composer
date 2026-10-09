# Project sessions

ProjectSession owns the persistent ProjectDocument and its instrument patch
history. One editing thread owns a session; none of its methods belongs in an
audio or MIDI callback. Const references and history spans should be used only
before the next session mutation. Copy the required document and context before
asynchronous work. A moved-from session may only be destroyed or assigned.

## Changes and original performances

Patch apply, undo and redo update the document's instrument patch and the same
project revision. They never change original MIDI bytes, event ordering,
timestamps or duration. History is explicitly patch-only. Effective changes mark
the document dirty; an equal-value command does not. Undoing back to saved values
remains dirty until a current save succeeds. Commands with an obsolete revision
are rejected even when their patch equals the current patch.

beginRecording returns an owned ticket and freezes patch edits and project
replacement until completion or explicit cancellation. The owner retains that
ticket; destroying it does not cancel recording. The ticket's attempt number
distinguishes cancelled and restarted recordings at the same project revision.
It is not another project revision and never wraps.

Before commitRecording, the owner must stop and quiesce archival callbacks,
finish the capture, and check its completeness diagnostics. A ProjectDocument
does not prove that all incoming events were captured or that timestamps were
aligned correctly. Retain the finished capture until the transaction succeeds;
failed completion leaves the caller's document and the pending ticket intact.

Completion validates the ticket, document and frozen patch, then prepares a copy
before advancing the shared project revision. Installation uses nonthrowing
member swaps. Notify observers only after success; prepare any fallible engine
or playback work before committing the session. A successful nonempty take
advances once without changing the patch-history stacks. An identical empty
completion ends recording without revision or dirty changes. A positive silent
duration is an effective performance. At the maximum revision, effective
completion fails and stays pending for cancellation or recovery.

The first recording workflow refuses to record over existing events or nonzero
duration, even after saving. Save the performance and create a new project before
recording again. There is no implicit take replacement or discard-unsaved action.

## Opening and saving

create accepts an already accepted clean starting document. New projects normally
use the empty document and revision zero. Open and new require a clean idle
session and a globally fresh project-instance token. The session rejects reuse
of its current token but does not retain a history of earlier tokens; the owner
must generate globally fresh values. Successful open resets revision to zero and
clears both patch-history stacks.

reopen accepts either encoded project text or a ProjectDocument, such as the
value returned by loadProjectFile. Both inputs are validated before installation.
Typed reopen prepares its own copy and preserves the caller's document on failure
or success. File-load failures must be reported without calling reopen. Failed
open leaves the existing session intact.

completeSave is trusted application bookkeeping, not a file operation or a
language command. Capture a document and its ProjectContext together on the
editing thread. Pass true only after the file store returns a successful receipt
for that exact document, and pass the context captured with it. Never substitute
the current context when an older asynchronous receipt arrives. A stale or
wrong-project receipt cannot clear dirty state. A failed save leaves all live
state unchanged. Each PreparedProjectSave can commit only once; retry by preparing
a new save.

Saving while recording acknowledges only the persistent pre-take document. An
effective later take completion advances the revision and marks it dirty again.
Cancellation or empty completion does not change the persistent document.

## Error and allocation limits

Command, document and session lifecycle errors retain separate result domains.
Operations validate and allocate before changing persistent state, and allocation
failure diagnostics use a best-effort resourceLimit result with an empty message.
The API does not promise unconditional no-throw or persistent memory-exhaustion
containment. Argument copies before function entry and context() string copying
can throw. Checked standard-library implementations may allocate even in
noexcept constructors, including an empty-string error fallback. No callback
safety or memory-exhaustion recovery follows from that fallback.
