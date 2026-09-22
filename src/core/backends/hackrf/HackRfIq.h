#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

// Pure, dependency-light IQ format conversion and gain-range clamping for
// the HackRF backend (#42) — split out from HackRfWorker (the hackrf_device*
// owner) so it can be unit tested without linking libhackrf or Qt, mirroring
// N1MMSpotParser's split from N1MMSpotClient.
//
// HackRF's wire format is interleaved SIGNED 8-bit I/Q. hackrf.h types the
// transfer buffer as uint8_t* (a generic byte buffer), but the values
// themselves are two's-complement: hackrf.h's own TX example reinterprets
// the buffer as int8_t* and writes `128 * creal(phasor)`. This is NOT
// RtlSdrWorker's unsigned/127.5-offset format — RTL-SDR's ADC genuinely
// outputs unsigned samples, while HackRF is symmetric around exact zero.
namespace AetherSDR::hackrf {

// Unpacks interleaved signed-8-bit [I,Q,I,Q,...] at `raw` into `out`,
// resizing it to byteCount/2 (an odd trailing byte, which should never
// happen on a real transfer, is silently dropped rather than read
// out-of-bounds). Scale is 1/128 so the full signed range [-128,127] maps
// to (-1.0, +0.9921875], matching hackrf.h's own `128 * creal(phasor)` TX
// convention in reverse.
void unpackRxIq(const std::uint8_t* raw, std::size_t byteCount,
                std::vector<std::complex<float>>& out);

// Packs `in` into `raw`, which must have room for 2*in.size() bytes. Each
// component is clamped to [-1.0, 1.0] before scaling by 127 (not 128, so a
// full-scale +1.0 sample cannot round up past int8_t's +127 max) and
// rounded to the nearest integer — a WDSP overshoot clips instead of
// wrapping around into a wildly wrong sample.
void packTxIq(const std::vector<std::complex<float>>& in, std::uint8_t* raw);

// HackRF's RX IF ("LNA") gain: 0-40 dB in 8 dB steps. Out-of-range or
// off-step requests round DOWN to the nearest valid setting — silently
// exceeding a requested ceiling is the worse failure of the two.
int clampLnaGainDb(int requestedDb);

// RX baseband ("VGA") gain: 0-62 dB in 2 dB steps.
int clampVgaGainDb(int requestedDb);

// TX IF ("VGA") gain: 0-47 dB in 1 dB steps (i.e. a plain range clamp).
int clampTxVgaGainDb(int requestedDb);

} // namespace AetherSDR::hackrf
