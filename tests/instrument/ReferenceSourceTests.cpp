#include "Sha256.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>

namespace
{

using namespace composer::tests;

const std::filesystem::path referenceDirectory =
    std::filesystem::path(COMPOSER_SOURCE_DIR) / "tests" / "instrument" / "reference" / "v1";

/** A file's bytes with every CRLF line ending read as LF, so a checkout's line endings do not
    change its digest. */
std::string readNormalised(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.good());
    const std::string bytes { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
    std::string text;
    text.reserve(bytes.size());

    for (std::size_t index = 0; index < bytes.size(); ++index)
        if (! (bytes[index] == '\r' && index + 1 < bytes.size() && bytes[index + 1] == '\n'))
            text.push_back(bytes[index]);

    return text;
}

/** The digest list: one line per file, its digest, two spaces and its path relative to the
    reference directory. */
std::map<std::string, std::string> recordedDigests()
{
    std::istringstream lines(readNormalised(referenceDirectory / "SOURCES.sha256"));
    std::map<std::string, std::string> digests;
    std::string line;

    while (std::getline(lines, line))
    {
        if (line.empty())
            continue;

        INFO(line);
        REQUIRE(line.size() > 66);
        REQUIRE(line.substr(64, 2) == "  ");
        REQUIRE(digests.emplace(line.substr(66), line.substr(0, 64)).second);
    }

    return digests;
}

} // namespace

TEST_CASE("Source digests match the SHA-256 standard examples")
{
    CHECK(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
          == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(sha256Hex(std::string(1000, 'a')) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

TEST_CASE("The frozen v1 instrument sources are unchanged")
{
    const auto digests = recordedDigests();
    CHECK(digests.size() == 20);

    for (const auto& [path, digest] : digests)
    {
        INFO("tests/instrument/reference/v1/" << path);
        const auto file = referenceDirectory / std::filesystem::path(path);
        REQUIRE(std::filesystem::is_regular_file(file));
        CHECK(sha256Hex(readNormalised(file)) == digest);
    }

    // A file added beside the listed ones could change what the reference compiles.
    for (const auto& entry : std::filesystem::recursive_directory_iterator(referenceDirectory))
    {
        if (! entry.is_regular_file())
            continue;

        const auto path = entry.path().lexically_relative(referenceDirectory).generic_string();
        INFO("tests/instrument/reference/v1/" << path);
        CHECK((path == "README.md" || path == "SOURCES.sha256" || digests.contains(path)));
    }
}
