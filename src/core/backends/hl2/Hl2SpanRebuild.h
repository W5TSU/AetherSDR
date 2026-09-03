#pragma once

#include <cstddef>
#include <utility>

namespace AetherSDR::hl2 {

// The outcome of re-spanning the radio across a decimation-rate boundary.
//
// Ok           — every receiver's WDSP channel reopened at the new input rate.
//                The caller commands the hardware to the new rate and persists it.
// RolledBack   — a receiver failed to reopen at the new rate; every receiver
//                that had already converted was restored to the previous rate.
//                The caller commands the hardware back to the previous rate,
//                does not persist, and the zoom reads as "not taken".
// Inconsistent — a receiver failed AND at least one converted receiver could not
//                be restored, so the set is split across two rates. The caller
//                commands the previous rate and surfaces a connection error.
enum class SpanRebuildOutcome {
    Ok,
    RolledBack,
    Inconsistent,
};

// Reconfigure receivers [0, count) to newRateHz in order. On the FIRST failure,
// walk every receiver that already converted back to prevRateHz (best-effort:
// every one is attempted even if an earlier restore fails) and stop — receivers
// past the failure are never opened at the new rate.
//
// `configure(index, rateHz)` returns true on success. It is called SERIALLY from
// one thread: WDSP's FFTW planner is not re-entrant and the spectrum instance
// plans in its constructor, so two of these running at once corrupt the
// planner's process-global state. This function does no locking and starts no
// threads — the caller owns the thread and any cross-thread hop to the DSP.
//
// The failing receiver itself is not restored: configure(i, newRateHz) failing
// leaves receiver i in whatever state it left behind, exactly as the previous
// blocking implementation did. Only [0, i) is walked back.
template <typename ConfigureFn>
SpanRebuildOutcome rebuildReceiversForRate(std::size_t count,
                                           int newRateHz,
                                           int prevRateHz,
                                           ConfigureFn configure)
{
    for (std::size_t i = 0; i < count; ++i) {
        if (configure(i, newRateHz))
            continue;

        bool restoredAll = true;
        for (std::size_t k = 0; k < i; ++k) {
            if (!configure(k, prevRateHz))
                restoredAll = false;
        }
        return restoredAll ? SpanRebuildOutcome::RolledBack
                           : SpanRebuildOutcome::Inconsistent;
    }
    return SpanRebuildOutcome::Ok;
}

}  // namespace AetherSDR::hl2
