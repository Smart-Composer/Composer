// Renders a MIDI performance through a VST 3 instrument with Steinberg's hosting library, so tests
// can compare the plug-in's output in a host other than JUCE with the in-process instrument.
//
//   composer_vst3_render <bundle> <state> <events> <sample rate> <block size> <samples> <output>
//
// <state> is passed to the component's setState unchanged. <events> holds one event per line:
// sample time, then three MIDI bytes, all decimal. Notes become note events; controllers go through
// the controller's MIDI mapping as parameter changes, as a VST 3 host sends them. <output> receives
// the first output bus's left channel and then its right channel, as 32-bit floats.

#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/processdata.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace
{

struct ScriptEvent
{
    int sample = 0;
    int status = 0;
    int first = 0;
    int second = 0;
};

int fail(const std::string& message)
{
    std::cerr << "composer_vst3_render: " << message << '\n';
    return 1;
}

// The SDK's module loader takes UTF-8 paths.
std::string toUtf8(const std::filesystem::path& path)
{
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

bool readEvents(const std::filesystem::path& path, std::vector<ScriptEvent>& events)
{
    std::ifstream file(path);
    if (! file)
        return false;

    ScriptEvent event;
    while (file >> event.sample >> event.status >> event.first >> event.second)
        events.push_back(event);

    return file.eof();
}

bool readBytes(const std::filesystem::path& path, std::vector<char>& bytes)
{
    std::ifstream file(path, std::ios::binary);
    if (! file)
        return false;

    bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

} // namespace

// Wide arguments, so that paths outside the system code page arrive intact.
int wmain(int argc, wchar_t* argv[])
{
    if (argc != 8)
        return fail("usage: composer_vst3_render <bundle> <state> <events> <sample rate> <block size> <samples> <output>");

    const std::string bundle = toUtf8(argv[1]);
    const double sampleRate = std::wcstod(argv[4], nullptr);
    const int blockSize = static_cast<int>(std::wcstol(argv[5], nullptr, 10));
    const int totalSamples = static_cast<int>(std::wcstol(argv[6], nullptr, 10));

    std::vector<char> state;
    std::vector<ScriptEvent> script;

    if (! readBytes(argv[2], state))
        return fail("cannot read the state file");

    if (! readEvents(argv[3], script))
        return fail("cannot read the events file");

    if (sampleRate <= 0.0 || blockSize <= 0 || totalSamples <= 0)
        return fail("invalid sample rate, block size or length");

    std::string error;
    VST3::Hosting::Module::Ptr module;

    try
    {
        module = VST3::Hosting::Module::create(bundle, error);
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
    }

    if (! module)
        return fail("cannot load " + bundle + ": " + error);

    const auto& factory = module->getFactory();
    VST3::Hosting::ClassInfo effect;
    bool found = false;

    for (const auto& info : factory.classInfos())
    {
        if (info.category() == kVstAudioEffectClass)
        {
            effect = info;
            found = true;
            break;
        }
    }

    if (! found)
        return fail("the module has no audio processor class");

    auto* hostContext = new HostApplication();
    PluginContextFactory::instance().setPluginContext(hostContext);

    int result = 0;

    {
        PlugProvider provider(factory, effect, true);

        if (! provider.initialize())
            return fail("cannot initialise the plug-in");

        auto component = provider.getComponentPtr();
        auto controller = provider.getControllerPtr();
        FUnknownPtr<IAudioProcessor> processor(component);

        if (! processor)
            return fail("the component is not an audio processor");

        SpeakerArrangement stereo = SpeakerArr::kStereo;
        if (processor->setBusArrangements(nullptr, 0, &stereo, 1) != kResultOk)
            return fail("the plug-in does not accept a stereo output");

        component->activateBus(kAudio, kOutput, 0, true);
        if (component->getBusCount(kEvent, kInput) > 0)
            component->activateBus(kEvent, kInput, 0, true);

        MemoryStream componentState(state.data(), static_cast<TSize>(state.size()));
        if (component->setState(&componentState) != kResultOk)
            return fail("the plug-in rejected the state");

        if (controller)
        {
            componentState.seek(0, IBStream::kIBSeekSet, nullptr);
            controller->setComponentState(&componentState);
        }

        ProcessSetup setup { kRealtime, kSample32, blockSize, sampleRate };
        if (processor->setupProcessing(setup) != kResultOk)
            return fail("the plug-in rejected the processing setup");

        if (component->setActive(true) != kResultOk)
            return fail("the plug-in cannot be activated");

        processor->setProcessing(true);

        FUnknownPtr<IMidiMapping> midiMapping(controller);
        HostProcessData data;
        data.prepare(*component, blockSize, kSample32);

        EventList events(512);
        ParameterChanges parameterChanges(64);
        data.inputEvents = &events;
        data.inputParameterChanges = &parameterChanges;

        std::vector<float> left;
        std::vector<float> right;
        left.reserve(static_cast<std::size_t>(totalSamples));
        right.reserve(static_cast<std::size_t>(totalSamples));
        std::size_t next = 0;

        for (int start = 0; start < totalSamples; start += blockSize)
        {
            const int length = std::min(blockSize, totalSamples - start);
            events.clear();
            parameterChanges.clearQueue();

            for (; next < script.size() && script[next].sample < start + length; ++next)
            {
                const auto& scripted = script[next];
                const auto offset = static_cast<int32>(scripted.sample - start);
                const auto channel = static_cast<int16>(scripted.status & 0x0F);
                const int kind = scripted.status & 0xF0;

                if (kind == 0x90 || kind == 0x80)
                {
                    Event event {};
                    event.busIndex = 0;
                    event.sampleOffset = offset;

                    if (kind == 0x90 && scripted.second > 0)
                    {
                        event.type = Event::kNoteOnEvent;
                        event.noteOn = { channel, static_cast<int16>(scripted.first),
                                         0.0f, static_cast<float>(scripted.second) / 127.0f, 0, -1 };
                    }
                    else
                    {
                        event.type = Event::kNoteOffEvent;
                        event.noteOff = { channel, static_cast<int16>(scripted.first),
                                          static_cast<float>(scripted.second) / 127.0f, -1, 0.0f };
                    }

                    events.addEvent(event);
                }
                else if (kind == 0xB0 && midiMapping)
                {
                    ParamID parameter = 0;
                    if (midiMapping->getMidiControllerAssignment(0, channel, static_cast<CtrlNumber>(scripted.first), parameter)
                        != kResultOk)
                        continue;

                    int32 queueIndex = 0;
                    int32 pointIndex = 0;
                    if (auto* queue = parameterChanges.addParameterData(parameter, queueIndex))
                        queue->addPoint(offset, static_cast<ParamValue>(scripted.second) / 127.0, pointIndex);
                }
            }

            data.numSamples = length;
            if (processor->process(data) != kResultOk)
            {
                result = fail("processing failed");
                break;
            }

            auto* channels = data.outputs[0].channelBuffers32;
            left.insert(left.end(), channels[0], channels[0] + length);
            right.insert(right.end(), channels[1], channels[1] + length);
        }

        processor->setProcessing(false);
        component->setActive(false);
        data.unprepare();

        if (result == 0)
        {
            std::ofstream output(std::filesystem::path(argv[7]), std::ios::binary);
            output.write(reinterpret_cast<const char*>(left.data()), static_cast<std::streamsize>(left.size() * sizeof(float)));
            output.write(reinterpret_cast<const char*>(right.data()), static_cast<std::streamsize>(right.size() * sizeof(float)));

            if (! output)
                result = fail("cannot write the output file");
        }
    }

    PluginContextFactory::instance().setPluginContext(nullptr);
    hostContext->release();
    return result;
}
