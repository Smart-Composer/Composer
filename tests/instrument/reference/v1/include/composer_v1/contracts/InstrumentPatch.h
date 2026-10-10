#pragma once

#include "ContractResult.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>

namespace composer_v1::contracts
{
inline constexpr std::uint32_t patchSchemaVersion = 1;

enum class Waveform : std::uint8_t { sine = 0, saw = 1, square = 2 };

struct InstrumentPatch
{
    Waveform waveform = Waveform::sine;
    double gainDb = -12.0;
    double attackSeconds = 0.01;
    double decaySeconds = 0.1;
    double sustainLevel = 0.7;
    double releaseSeconds = 0.2;
    double cutoffHz = 8000.0;
    double resonanceQ = 0.7071067811865476;

    bool operator==(const InstrumentPatch&) const = default;
};

static_assert(std::is_trivially_copyable_v<InstrumentPatch>);
static_assert(std::is_aggregate_v<InstrumentPatch>);

enum class ParameterKind { choice, continuous };

struct ParameterDescriptor
{
    std::string_view id;
    std::string_view displayName;
    std::string_view unit;
    ParameterKind kind;
    double minimum;
    double maximum;
    double defaultValue;
    double InstrumentPatch::* member;
};

struct WaveformDescriptor
{
    Waveform value;
    std::string_view id;
    std::string_view displayName;
};

inline constexpr std::array<WaveformDescriptor, 3> waveformDescriptors {{
    {Waveform::sine, "sine", "Sine"},
    {Waveform::saw, "saw", "Saw"},
    {Waveform::square, "square", "Square"}
}};

// IDs and choice indices are persistent automation and state identifiers.
inline constexpr std::array<ParameterDescriptor, 8> parameterDescriptors {{
    {"waveform", "Waveform", "", ParameterKind::choice, 0, 2, 0, nullptr},
    {"gain_db", "Gain", "dB", ParameterKind::continuous, -60, 0, InstrumentPatch{}.gainDb, &InstrumentPatch::gainDb},
    {"attack_seconds", "Attack", "s", ParameterKind::continuous, 0, 10, InstrumentPatch{}.attackSeconds, &InstrumentPatch::attackSeconds},
    {"decay_seconds", "Decay", "s", ParameterKind::continuous, 0, 10, InstrumentPatch{}.decaySeconds, &InstrumentPatch::decaySeconds},
    {"sustain_level", "Sustain", "", ParameterKind::continuous, 0, 1, InstrumentPatch{}.sustainLevel, &InstrumentPatch::sustainLevel},
    {"release_seconds", "Release", "s", ParameterKind::continuous, 0, 10, InstrumentPatch{}.releaseSeconds, &InstrumentPatch::releaseSeconds},
    {"cutoff_hz", "Cutoff", "Hz", ParameterKind::continuous, 20, 20000, InstrumentPatch{}.cutoffHz, &InstrumentPatch::cutoffHz},
    {"resonance_q", "Resonance", "Q", ParameterKind::continuous, 0.1, 10, InstrumentPatch{}.resonanceQ, &InstrumentPatch::resonanceQ}
}};

std::optional<ContractError> validatePatch(const InstrumentPatch& patch);
Result<InstrumentPatch> decodePatch(std::string_view source);
Result<std::string> encodePatch(const InstrumentPatch& patch);
}
