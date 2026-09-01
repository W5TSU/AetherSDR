#pragma once

#include <cstdint>

namespace AetherSDR::hl2 {

// The HPSDR ADC-overload bit (`0x00[24]`) is not a level — it flips true/false
// frame to frame on a band that is only marginally hot. piHPSDR and
// HERMES.md §15.7 measured ~133 rising edges per second on 40 m MW, and the
// existing edge-gated `qWarning` fired on every one, flushing the log ring
// ~130x/s so nothing else in it survived.
//
// This gate keeps the edge detection but rate-limits the LOG: the first rising
// edge after a quiet period logs immediately; further rising edges inside the
// cooldown window are counted, not logged; when the window has elapsed the
// next rising edge logs again and reports how many it swallowed.
//
// Pure and header-only (the `*Policy.h` pattern) so every transition is a
// unit-test row — see tests/hl2_adc_overload_ratelimit_test.cpp. The monotonic
// clock is injected as milliseconds so the struct carries no Qt types.
struct AdcOverloadLogGate {
    // At most one warning per this window, however hard the bit chatters.
    static constexpr int kCooldownMs = 1000;

    bool overloaded{false};     // last-seen bit value, for edge detection
    int64_t lastLoggedMs{-1};   // -1 = nothing logged yet
    int suppressed{0};          // rising edges swallowed since the last log

    struct Decision {
        bool log{false};             // emit the warning now
        int suppressedSinceLast{0};  // append to the message when log is true
    };

    // Feed the current bit and a monotonic millisecond clock on every
    // telemetry frame that carries an overload value.
    Decision update(bool nowOverloaded, int64_t nowMs)
    {
        const bool risingEdge = nowOverloaded && !overloaded;
        overloaded = nowOverloaded;
        if (!risingEdge) {
            return {};
        }
        if (lastLoggedMs < 0 || nowMs - lastLoggedMs >= kCooldownMs) {
            const Decision decision{true, suppressed};
            lastLoggedMs = nowMs;
            suppressed = 0;
            return decision;
        }
        ++suppressed;
        return {};
    }
};

} // namespace AetherSDR::hl2
