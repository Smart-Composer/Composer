#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace composer::tests
{

/** The SHA-256 digest of a byte sequence, as 64 lowercase hexadecimal digits (FIPS 180-4). */
std::string sha256Hex(const void* data, std::size_t size);

inline std::string sha256Hex(std::string_view text)
{
    return sha256Hex(text.data(), text.size());
}

} // namespace composer::tests
