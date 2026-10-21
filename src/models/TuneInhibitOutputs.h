#pragma once

#include <QString>
#include <QStringList>

namespace AetherSDR {

// The per-band TX outputs a TUNE cycle may switch off (Settings -> Inhibit
// during TUNE): ACC TX and the TX1/TX2/TX3 RCA jacks.
struct TuneInhibitOutputs {
    bool accTx{false};
    bool tx1{false};
    bool tx2{false};
    bool tx3{false};

    [[nodiscard]] bool any() const { return accTx || tx1 || tx2 || tx3; }
    [[nodiscard]] QStringList names() const
    {
        QStringList out;
        if (accTx) out << QStringLiteral("ACC TX");
        if (tx1) out << QStringLiteral("TX1");
        if (tx2) out << QStringLiteral("TX2");
        if (tx3) out << QStringLiteral("TX3");
        return out;
    }
};

// What one TUNE cycle disables, and therefore what its restore re-enables
// (upstream 52ea5981): an output the operator asked to inhibit AND that is
// enabled on the band right now. An output already off is not touched, so
// the restore cannot turn it on, and the restore replays this set rather
// than the preferences, so changing a preference mid-TUNE cannot strand or
// invent one.
[[nodiscard]] inline TuneInhibitOutputs tuneInhibitPlan(const TuneInhibitOutputs& wanted,
                                                        const TuneInhibitOutputs& enabledNow)
{
    return {wanted.accTx && enabledNow.accTx, wanted.tx1 && enabledNow.tx1,
            wanted.tx2 && enabledNow.tx2, wanted.tx3 && enabledNow.tx3};
}

} // namespace AetherSDR
