#include "DerivedPlayback.h"

#include <array>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace composer::app
{
PlaybackConversion derivePlayback(const project::ProjectDocument& document,
                                  tracktion::AudioTrack& track, double sampleRate)
{
    const auto* messages = juce::MessageManager::getInstanceWithoutCreating();
    if (messages == nullptr || !messages->isThisTheMessageThread())
        throw std::invalid_argument("Playback conversion requires the message thread");
    if (const auto error = project::validateProject(document))
        throw std::invalid_argument(error->field + ": " + error->message);
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        throw std::invalid_argument("Playback conversion requires a finite positive sample rate");
    if (!track.getClips().isEmpty())
        throw std::invalid_argument("Playback conversion requires an empty disposable track");
    const auto& transport = track.edit.getTransport();
    if (transport.isPlaying() || transport.isStopping() || transport.isRecordingStopping())
        throw std::invalid_argument("Stop the transport before converting playback");
    if (track.edit.tempoSequence.getNumTempos() != 1
        || track.edit.tempoSequence.getTempo(0)->getBpm() != project::projectTempoBpm)
        throw std::invalid_argument("Playback conversion requires fixed 120 BPM");

    PlaybackConversion result;
    if (document.events.empty())
        return result;
    const double clipEnd = document.durationSeconds + 1.0 / sampleRate;
    if (!std::isfinite(clipEnd) || clipEnd <= document.durationSeconds)
        throw std::invalid_argument("One sample cannot extend this playback duration at the supplied rate");

    std::array<juce::MidiMessageSequence, 16> channels;
    for (const auto& event : document.events)
    {
        juce::MidiMessage message(event.bytes.data(), static_cast<int>(event.bytes.size()), event.timeSeconds);
        channels[static_cast<std::size_t>(message.getChannel() - 1)].addEvent(message);
        if ((event.bytes[0] & 0xf0) == 0x90 && event.bytes[2] == 0)
            ++result.velocityZeroNoteOffs;
    }

    result.clips.reserve(channels.size());
    for (std::size_t channel = 0; channel < channels.size(); ++channel)
    {
        auto& sequence = channels[channel];
        if (sequence.getNumEvents() == 0)
            continue;
        const int originalCount = sequence.getNumEvents();
        // JUCE pairs notes and closes an earlier repeated pitch at its next note-on.
        sequence.updateMatchedPairs();
        result.retriggerClosures += static_cast<std::size_t>(sequence.getNumEvents() - originalCount);
        std::vector<int> openPitches;
        for (const auto* event : sequence)
            if (event->message.isNoteOn() && event->noteOffObject == nullptr)
                openPitches.push_back(event->message.getNoteNumber());
        for (const auto pitch : openPitches)
            sequence.addEvent(juce::MidiMessage::noteOff(static_cast<int>(channel + 1), pitch), document.durationSeconds);
        result.terminalClosures += openPitches.size();
        sequence.updateMatchedPairs();

        std::unordered_set<const juce::MidiMessageSequence::MidiEventHolder*> matchedOffs;
        for (const auto* event : sequence)
            if (event->noteOffObject != nullptr)
                matchedOffs.insert(event->noteOffObject);
        for (const auto* event : sequence)
            if (event->message.isNoteOff() && !matchedOffs.contains(event))
                ++result.orphanNoteOffs;

        auto clip = track.insertMIDIClip({ tracktion::TimePosition(),
                                          tracktion::TimePosition::fromSeconds(clipEnd) }, nullptr);
        if (clip == nullptr)
            throw std::runtime_error("Could not construct derived MIDI clip");
        clip->setMidiChannel(tracktion::MidiChannel(static_cast<int>(channel + 1)));
        // The ordinary proxy route nudges note-offs in its seconds export. This
        // flag selects Tracktion's beatsRaw graph route instead.
        clip->setUsesProxy(false);
        clip->setMPEMode(false);
        clip->setSendingBankChanges(false);
        clip->getQuantisation().setType("(none)");
        clip->setVolumeDb(0.0f);
        auto& list = clip->getSequence();
        juce::MidiMessageSequence controllers;
        for (const auto* event : sequence)
        {
            const auto& message = event->message;
            if (message.isNoteOn())
            {
                if (event->noteOffObject == nullptr)
                    throw std::runtime_error("Could not pair a derived MIDI note");
                const auto& release = event->noteOffObject->message;
                const auto start = track.edit.tempoSequence.toBeats(
                    tracktion::TimePosition::fromSeconds(message.getTimeStamp()));
                const auto end = track.edit.tempoSequence.toBeats(
                    tracktion::TimePosition::fromSeconds(release.getTimeStamp()));
                if ((end - start).inBeats() <= 0.00001)
                {
                    ++result.suppressedShortNotes;
                    continue;
                }
                // The sequence importer omits release velocity; use the note API.
                auto* note = list.addNote(message.getNoteNumber(), start, end - start,
                                          message.getVelocity(), 0, nullptr);
                if (note == nullptr)
                    throw std::runtime_error("Could not construct a derived MIDI note");
                note->setNoteOffVelocity(release.getVelocity(), nullptr);
            }
            else if (!message.isNoteOff())
            {
                controllers.addEvent(message);
            }
        }
        list.importMidiSequence(controllers, &track.edit, tracktion::TimePosition(), nullptr);
        result.clips.push_back(clip.get());
    }
    return result;
}
}
