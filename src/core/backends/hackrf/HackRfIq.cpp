#include "HackRfIq.h"

#include <algorithm>
#include <cmath>

namespace AetherSDR::hackrf {

void unpackRxIq(const std::uint8_t* raw, std::size_t byteCount,
                std::vector<std::complex<float>>& out)
{
    const std::size_t numSamples = byteCount / 2;  // an odd trailing byte is dropped
    out.resize(numSamples);
    for (std::size_t i = 0; i < numSamples; ++i) {
        const auto ib = static_cast<std::int8_t>(raw[2 * i]);
        const auto qb = static_cast<std::int8_t>(raw[2 * i + 1]);
        out[i] = std::complex<float>(static_cast<float>(ib) / 128.0f,
                                     static_cast<float>(qb) / 128.0f);
    }
}

void packTxIq(const std::vector<std::complex<float>>& in, std::uint8_t* raw)
{
    for (std::size_t i = 0; i < in.size(); ++i) {
        const float re = std::clamp(in[i].real(), -1.0f, 1.0f);
        const float im = std::clamp(in[i].imag(), -1.0f, 1.0f);
        raw[2 * i]     = static_cast<std::uint8_t>(static_cast<std::int8_t>(std::lround(re * 127.0f)));
        raw[2 * i + 1] = static_cast<std::uint8_t>(static_cast<std::int8_t>(std::lround(im * 127.0f)));
    }
}

namespace {

// Clamps to [0, maxDb], then rounds DOWN to the nearest multiple of stepDb.
int clampStep(int requestedDb, int maxDb, int stepDb)
{
    const int bounded = std::clamp(requestedDb, 0, maxDb);
    return (bounded / stepDb) * stepDb;
}

} // namespace

int clampLnaGainDb(int requestedDb)   { return clampStep(requestedDb, 40, 8); }
int clampVgaGainDb(int requestedDb)   { return clampStep(requestedDb, 62, 2); }
int clampTxVgaGainDb(int requestedDb) { return clampStep(requestedDb, 47, 1); }

} // namespace AetherSDR::hackrf
