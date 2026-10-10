#include "ReferencePatches.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>

namespace composer::tests
{

std::vector<SavedV1Patch> loadSavedV1Patches()
{
    std::ifstream file(std::string(COMPOSER_SOURCE_DIR) + "/tests/instrument/reference/v1/fixtures/patches-v1.json");

    if (! file)
        throw std::runtime_error("the saved v1 patches are unavailable");

    // The file keeps each patch's members in the order the v1 encoder wrote them, so the compact
    // form of an entry is the saved text itself.
    const auto document = nlohmann::ordered_json::parse(file);
    std::vector<SavedV1Patch> patches;

    for (const auto& [name, value] : document.items())
        patches.push_back({ name, value.dump() });

    return patches;
}

} // namespace composer::tests
