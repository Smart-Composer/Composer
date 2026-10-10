# Applies Composer's repairs to the pinned JUCE 9.0.3 sources. FetchContent runs this script in the
# extracted source directory after verifying the archive; docs/dependencies.md lists each repair and
# its upstream status. Each file must be either the pinned original or already repaired, so a
# changed pin stops here instead of silently losing or misapplying a repair.

# A Windows MIDI input whose midiInOpen or midiInStart fails is destroyed before its buffer thread
# starts, and its destructor joins that thread unconditionally, which ends the process. The repair
# guards the join, as the same file already does for its device watcher thread.
set(file "modules/juce_audio_devices/native/juce_Midi_windows.cpp")
set(original "4ad86a75b29cade2c26c093c984204a2c2e24a3b6e2f34ed09623da979fbc803")
set(repaired "9c93a415b88d70415bf425a65e8245e8fb14c3b2195c8521d2958e7870b39c71")

file(SHA256 "${file}" current)

if(current STREQUAL original)
    # The repair stays on one line: file(READ) and file(WRITE) translate line endings on Windows.
    file(READ "${file}" text)
    string(REPLACE "blockQueueThread.join();"
        "if (blockQueueThread.joinable()) blockQueueThread.join();" text "${text}")
    file(WRITE "${file}" "${text}")
    file(SHA256 "${file}" current)
endif()

if(NOT current STREQUAL repaired)
    message(FATAL_ERROR "${file} is neither the pinned JUCE 9.0.3 source nor its repaired form "
        "(SHA-256 ${current}); review Composer's JUCE repairs before changing the pin")
endif()
