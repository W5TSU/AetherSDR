// The HL2 span change re-spans the radio: crossing a decimation-rate boundary
// rebuilds EVERY receiver's WDSP channel at the new input rate, because the DDC
// rate register is radio-wide. rebuildReceiversForRate() is the pure sequence
// behind that — reconfigure each receiver to the new rate in order, and on the
// first failure walk the already-converted ones back to the previous rate so
// the set is never left split across two rates.
//
// This tests the externally observable behaviour: the exact (index, rate) calls
// made through the configure callable, and the outcome. No Qt, no threads, no
// WDSP — the caller owns all of that.

#include "core/backends/hl2/Hl2SpanRebuild.h"

#include <cstdio>
#include <functional>
#include <utility>
#include <vector>

using AetherSDR::hl2::rebuildReceiversForRate;
using AetherSDR::hl2::SpanRebuildOutcome;

namespace {

int g_failures = 0;
void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

constexpr int kOldRate = 48000;
constexpr int kNewRate = 192000;

// Records every (index, rate) the sequence asks for, and can be told to fail a
// chosen new-rate index and/or a chosen previous-rate (rollback) index.
struct FakeConfigure {
    std::vector<std::pair<std::size_t, int>> calls;
    long failNewRateIndex  = -1;   // fail configure(idx, kNewRate) for this idx
    long failPrevRateIndex = -1;   // fail configure(idx, kOldRate) for this idx

    bool operator()(std::size_t index, int rateHz)
    {
        calls.emplace_back(index, rateHz);
        if (rateHz == kNewRate && static_cast<long>(index) == failNewRateIndex)
            return false;
        if (rateHz == kOldRate && static_cast<long>(index) == failPrevRateIndex)
            return false;
        return true;
    }

    int countAt(int rateHz) const
    {
        int n = 0;
        for (const auto& c : calls)
            if (c.second == rateHz) ++n;
        return n;
    }
    bool sawNewRateFor(std::size_t index) const
    {
        for (const auto& c : calls)
            if (c.first == index && c.second == kNewRate) return true;
        return false;
    }
    bool sawPrevRateFor(std::size_t index) const
    {
        for (const auto& c : calls)
            if (c.first == index && c.second == kOldRate) return true;
        return false;
    }
};

}  // namespace

int main()
{
    // ---- happy path: every receiver takes the new rate ----------------------
    {
        FakeConfigure fake;
        const auto outcome = rebuildReceiversForRate(4, kNewRate, kOldRate, std::ref(fake));
        check(outcome == SpanRebuildOutcome::Ok, "all reconfigure -> Ok");
        check(fake.countAt(kNewRate) == 4, "Ok path calls new-rate configure once per receiver");
        check(fake.countAt(kOldRate) == 0, "Ok path never touches the previous rate");
    }

    // ---- no receivers: nothing to do --------------------------------------
    {
        FakeConfigure fake;
        const auto outcome = rebuildReceiversForRate(0, kNewRate, kOldRate, std::ref(fake));
        check(outcome == SpanRebuildOutcome::Ok, "zero receivers -> Ok");
        check(fake.calls.empty(), "zero receivers makes no configure calls");
    }

    // ---- receiver 2 of 4 fails: roll 0 and 1 back, stop at 2 --------------
    {
        FakeConfigure fake;
        fake.failNewRateIndex = 2;
        const auto outcome = rebuildReceiversForRate(4, kNewRate, kOldRate, std::ref(fake));
        check(outcome == SpanRebuildOutcome::RolledBack, "a mid-set failure -> RolledBack");
        check(fake.sawNewRateFor(0) && fake.sawNewRateFor(1),
              "receivers before the failure were taken to the new rate");
        check(fake.sawNewRateFor(2), "the failing receiver was attempted at the new rate");
        check(!fake.sawNewRateFor(3),
              "no receiver past the failure is opened at the new rate");
        check(fake.sawPrevRateFor(0) && fake.sawPrevRateFor(1),
              "the converted receivers are walked back to the previous rate");
        check(!fake.sawPrevRateFor(2) && !fake.sawPrevRateFor(3),
              "the failing receiver and those past it are not rolled back");
    }

    // ---- receiver 0 fails: nothing converted, nothing to roll back -------
    {
        FakeConfigure fake;
        fake.failNewRateIndex = 0;
        const auto outcome = rebuildReceiversForRate(3, kNewRate, kOldRate, std::ref(fake));
        check(outcome == SpanRebuildOutcome::RolledBack, "first receiver fails -> RolledBack");
        check(fake.countAt(kOldRate) == 0, "nothing to roll back when the first receiver fails");
        check(!fake.sawNewRateFor(1) && !fake.sawNewRateFor(2),
              "no further receivers are opened after the first fails");
    }

    // ---- rollback itself fails: the set is inconsistent ------------------
    {
        FakeConfigure fake;
        fake.failNewRateIndex  = 3;   // receiver 3 fails at the new rate
        fake.failPrevRateIndex = 1;   // and receiver 1 will not go back
        const auto outcome = rebuildReceiversForRate(4, kNewRate, kOldRate, std::ref(fake));
        check(outcome == SpanRebuildOutcome::Inconsistent,
              "a failed rollback -> Inconsistent");
        check(fake.sawPrevRateFor(0) && fake.sawPrevRateFor(2),
              "rollback is best-effort: receivers past the failed restore are still attempted");
    }

    if (g_failures == 0)
        std::fprintf(stderr, "hl2_span_rebuild_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
