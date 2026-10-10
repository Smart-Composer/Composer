#pragma once

#include <composer/project/ProjectDocument.h>
#include <tracktion_engine/tracktion_engine.h>

#include <cstddef>
#include <vector>

namespace composer::app
{
struct PlaybackConversion
{
    std::vector<tracktion::MidiClip*> clips; // Borrowed from the target track.
    std::size_t retriggerClosures = 0;
    std::size_t terminalClosures = 0;
    std::size_t orphanNoteOffs = 0;
    std::size_t velocityZeroNoteOffs = 0;
    std::size_t suppressedShortNotes = 0;
};

// Message thread only, on an empty track in a stopped, quiescent disposable edit
// at fixed 120 BPM. Throws on invalid input or construction failure; the caller
// must discard the disposable edit after any failure, including partial builds.
// The source document is never modified. Tracktion remains the playback scheduler.
//
// Each used channel gets a clip with one sample of extra extent at sampleRate.
// This admits inclusive endpoint controllers, including zero-duration documents.
// It does not guarantee sample-exact endpoint note-offs: the pinned graph can move
// them one sample earlier. Equal-time playback order can also differ from the raw
// document. Repeated pitches close the prior note, unmatched held notes close at
// the original duration, orphan offs are omitted, and very short notes are omitted.
// These changes are reported and affect only derived playback. Rebuild for a new rate.
PlaybackConversion derivePlayback(const project::ProjectDocument& document,
                                  tracktion::AudioTrack& track, double sampleRate);
}
