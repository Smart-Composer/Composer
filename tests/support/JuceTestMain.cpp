#include "TestProcess.h"

#include <catch2/catch_session.hpp>
#include <juce_events/juce_events.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace
{

constexpr int assertionExitCode = 3;

/** Turns JUCE assertions into test failures.

    Debug builds define JUCE_LOG_ASSERTIONS, so every failed jassert in the code under test is
    written to the current logger. Assertions during the test session fail the process when the
    session ends; leak reports, which JUCE raises during static destruction after main has
    returned, end the process immediately with a failing exit code.
*/
class AssertionLogger final : public juce::Logger
{
public:
    void logMessage(const juce::String& message) override
    {
        std::fprintf(stderr, "%s\n", message.toRawUTF8());
        std::fflush(stderr);

        if (! message.startsWith("JUCE Assertion failure"))
            return;

        if (sessionEnded.load())
            std::_Exit(assertionExitCode);

        assertions.fetch_add(1);
    }

    std::atomic<int> assertions { 0 };
    std::atomic<bool> sessionEnded { false };
};

} // namespace

int main(int argc, char* argv[])
{
    composer::tests::reportFailuresWithoutDialogs();

    // Static storage that is never destroyed: leak reports arrive during static destruction,
    // and the logger must still be there to receive them.
    alignas(AssertionLogger) static unsigned char loggerStorage[sizeof(AssertionLogger)];
    auto* const logger = new (loggerStorage) AssertionLogger();
    juce::Logger::setCurrentLogger(logger);

    int result = 0;

    {
        // Plugin hosting and the sequencing engine expect a message thread; the test
        // runner's main thread takes that role for the whole session.
        const juce::ScopedJuceInitialiser_GUI juceInitialiser;
        result = Catch::Session().run(argc, argv);
    }

    logger->sessionEnded.store(true);

    if (const auto failed = logger->assertions.load(); failed > 0)
    {
        std::fprintf(stderr, "%d JUCE assertion(s) failed during the test session.\n", failed);
        return result != 0 ? result : assertionExitCode;
    }

    return result;
}
