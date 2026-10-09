#include "PatchFixtures.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>
#include <variant>

namespace composer::tests
{

std::vector<std::pair<std::string, contracts::InstrumentPatch>> loadPatchFixtures()
{
    std::ifstream file(std::string(COMPOSER_SOURCE_DIR) + "/tests/contracts/fixtures/patches.json");

    if (! file)
        throw std::runtime_error("the patch fixtures are unavailable");

    const auto document = nlohmann::json::parse(file);
    std::vector<std::pair<std::string, contracts::InstrumentPatch>> patches;

    for (const auto& [name, value] : document.items())
    {
        const auto decoded = contracts::decodePatch(value.dump());

        if (const auto* patch = std::get_if<contracts::InstrumentPatch>(&decoded))
            patches.emplace_back(name, *patch);
        else
            throw std::runtime_error("fixture " + name + " does not decode");
    }

    return patches;
}

contracts::InstrumentPatch awkwardPatch()
{
    contracts::InstrumentPatch patch;
    patch.waveform = contracts::Waveform::saw;
    patch.gainDb = -7.123456789012345;
    patch.attackSeconds = 0.012345678901234567;
    patch.decaySeconds = 1.2345678901234567;
    patch.sustainLevel = 0.3333333333333333;
    patch.releaseSeconds = 0.7777777777777777;
    patch.cutoffHz = 1234.5678901234567;
    patch.resonanceQ = 3.3333333333333335;
    return patch;
}

} // namespace composer::tests
