#pragma once

#include <composer/contracts/InstrumentPatch.h>

#include <string>
#include <utility>
#include <vector>

namespace composer::tests
{

/** The named patches in tests/contracts/fixtures/patches.json, decoded by the contract. */
std::vector<std::pair<std::string, contracts::InstrumentPatch>> loadPatchFixtures();

/** A valid patch whose continuous values have no exact float representation. */
contracts::InstrumentPatch awkwardPatch();

} // namespace composer::tests
