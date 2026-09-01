// AdcOverloadLogGate — the rate limiter on the chattering HPSDR ADC-overload
// bit (HERMES.md §15.7 / §13 T1-6a). First rising edge logs; further rising
// edges inside the 1 s window are counted, not logged; the next edge after the
// window logs and reports the swallowed count.

#include "core/backends/hl2/AdcOverloadLogGate.h"

#include <cstdio>

using AetherSDR::hl2::AdcOverloadLogGate;

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++failures;
    }
}

} // namespace

int main()
{
    // 1. First-ever call with the bit clear: nothing.
    {
        AdcOverloadLogGate g;
        check(!g.update(false, 0).log, "no rising edge -> no log");
    }

    // 2. First rising edge logs immediately, with zero suppressed.
    {
        AdcOverloadLogGate g;
        const auto d = g.update(true, 0);
        check(d.log && d.suppressedSinceLast == 0, "first rising edge logs");
        // Staying true is not a new edge.
        check(!g.update(true, 100).log, "held-true is not an edge");
        // Falling edge never logs.
        check(!g.update(false, 200).log, "falling edge does not log");
    }

    // 3. Rising edges inside the cooldown are swallowed and counted.
    {
        AdcOverloadLogGate g;
        check(g.update(true, 0).log, "edge at t=0 logs");
        g.update(false, 10);
        check(!g.update(true, 300).log, "edge at t=300 (< 1000) suppressed");
        g.update(false, 310);
        check(!g.update(true, 600).log, "edge at t=600 suppressed");
        g.update(false, 610);
        // t=1000 is exactly kCooldownMs after the last log -> logs, reports 2.
        const auto d = g.update(true, 1000);
        check(d.log && d.suppressedSinceLast == 2,
              "edge at t=1000 logs and reports 2 swallowed");
        // Counter resets after a log.
        g.update(false, 1010);
        check(!g.update(true, 1100).log, "counter reset: next edge suppressed");
    }

    // 4. A long quiet gap then an edge logs (lastLoggedMs age check), not the
    //    edge count.
    {
        AdcOverloadLogGate g;
        check(g.update(true, 0).log, "edge at t=0 logs");
        g.update(false, 10);
        const auto d = g.update(true, 50'000);
        check(d.log && d.suppressedSinceLast == 0,
              "edge after a long quiet gap logs with nothing swallowed");
    }

    // 5. Sustained chatter: 250 rising edges over 2.5 s bounds to 3 logs
    //    (t=0, then one each at t>=1000 and t>=2000).
    {
        AdcOverloadLogGate g;
        int logs = 0;
        for (int i = 0; i < 250; ++i) {
            const int64_t t = i * 10; // 0..2490 ms
            if (g.update(true, t).log) {
                ++logs;
            }
            g.update(false, t + 1);
        }
        check(logs == 3, "250 edges over ~2.5 s -> 3 logs, not 250");
    }

    std::printf("%s\n", failures == 0 ? "ALL PASS" : "SOME FAILED");
    return failures == 0 ? 0 : 1;
}
