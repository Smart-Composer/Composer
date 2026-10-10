#pragma once

#include <string>
#include <vector>

namespace composer::tests
{

/** A patch as the first version of the instrument saved it: the exact text of its state. */
struct SavedV1Patch
{
    std::string name;
    std::string text;
};

/** The saved v1 patches in tests/instrument/reference/v1/fixtures/patches-v1.json, in file order:
    the contract's fixture patches and a patch whose values have no exact float representation.
    Each text is what the v1 encoder wrote for that patch. */
std::vector<SavedV1Patch> loadSavedV1Patches();

} // namespace composer::tests
