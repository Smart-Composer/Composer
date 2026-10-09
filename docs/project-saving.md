# Saving and opening a project

Project file operations run outside audio processing. They use the shared project
encoder and decoder, so saving and opening enforce the same document and patch
rules as in-memory operations. Opening returns a complete document or an error;
the caller replaces its active session only after successful loading.

## Preparing a save

`PreparedProjectSave::prepare(target, document)` validates and encodes the entire
document before creating files. The destination must have an existing parent
directory and must be absent or a regular file. Directories, links, Windows device
names and alternate data streams are rejected. Existing target bytes are captured
for later change detection. Files larger than `maxProjectJsonBytes` are rejected
before allocation; loading also rejects missing and empty files.

Preparation reserves a unique sibling file, writes all encoded bytes with checked
unbuffered writes, explicitly flushes it, closes it and verifies the bytes by
reading it back. It does not change the destination. `stagedFile()` exposes the
prepared path for diagnostics and file-sharing tests; callers must not edit it.
Destroying an unused prepared save attempts to delete only its own staging file.

Temporary and backup siblings use 16 random hexadecimal characters plus a
four-character extension. Preparation checks that the target and both sibling
paths fit the ordinary Windows path limit, including the terminating NUL, before
creating any file. A directory too long for those siblings is rejected even for
the first save, so a new project cannot appear to save successfully while every
subsequent replacement fails. Choose a shorter directory in that case. Loading
an existing project does not require room for save siblings.

## Committing and recovering

`commit()` is one-shot, including when it fails. Moving a prepared save transfers
its ownership; the moved-from object cannot commit. `saveProjectFile` provides
the prepare-and-commit convenience operation.

Before committing, the store checks the prepared bytes and compares the current
destination with the captured bytes or captured absence. A destination that was
changed, created or deleted causes an error. These checks detect ordinary external
changes; they are not a lock against adversarial or simultaneous filesystem
writers between checking and replacement.

On Windows, an existing file is replaced with `ReplaceFileW`, supplying a unique
sibling backup filename and flags zero. A new file uses `MoveFileExW` with
`MOVEFILE_WRITE_THROUGH`, without overwrite or cross-volume-copy flags. The store
does not delete a destination before moving and does not use JUCE's unchecked
text/data replacement shortcuts.

A successful save verifies the installed bytes and the previous-version backup
before removing that backup. If backup cleanup fails,
`SaveReceipt::retainedPreviousVersion` identifies
the file left behind. The caller may mark the document saved only after receiving
a receipt; an error must leave its unsaved state intact.

Replacement failure can leave the previous version under the supplied backup
name instead of the original destination. The store preserves that backup and
any remaining staged file rather than attempting a destructive restore.
`ProjectFileError::recoveryFile` identifies the retained backup when present, or
the staged file otherwise. A staged file contains the proposed new version; a
backup contains the previous version. Treat a verification error as requiring
inspection of the retained file. A failed save must not be presented as successful
just because one of these files exists. After a failed commit, prepare a new save
attempt or recover to another filename.

File flushes and native same-directory replacement reduce exposure to incomplete
writes. They do not establish unconditional crash atomicity or durability across
power loss, filesystem faults or every Windows replacement failure. Other
platforms currently return an explicit unsupported-platform error.

## Focused verification

File-store tests use uniquely created synthetic scratch directories and remove
only their direct owned entries. They cover exact project round trips, replacement,
invalid documents and inputs, file-size bounds, post-prepare destination changes,
tampered staging bytes, moves, abandoned saves and repeated commit attempts.
Long-path cases cover first saves and replacements through the sibling-path
boundary, and rejection before creating staging files beyond that boundary.

The replacement failure test opens the prepared temporary file without delete
sharing while leaving the destination unlocked. A failed replacement must retain
the original bytes; a subsequent newly prepared unlocked save must succeed. This
is a real Windows sharing failure, not a simulated success result. It tests that
specific failure mode, not crash recovery or every possible storage failure.
