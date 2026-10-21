// A TUNE cycle restores only the outputs it inhibited (upstream 52ea5981).
// RadioModel::applyTuneInhibit() disables tuneInhibitPlan(wanted, enabledNow)
// and restoreTuneInhibit() re-enables that same snapshot. Pure: no radio.

#include "models/TuneInhibitOutputs.h"

#include <cstdio>

using namespace AetherSDR;

static int g_failed = 0;
static void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok)
        ++g_failed;
}

int main()
{
    const TuneInhibitOutputs all{true, true, true, true};

    // ACC enabled, TX1 already off, TX2 on, TX3 on; the operator inhibits all.
    const TuneInhibitOutputs band{true, false, true, true};
    const TuneInhibitOutputs plan = tuneInhibitPlan(all, band);
    check(plan.accTx && plan.tx2 && plan.tx3, "enabled outputs the operator inhibits are disabled");
    check(!plan.tx1, "an output already off is not touched, so the restore cannot turn it on");

    // The restore replays the snapshot, not the preferences: a preference
    // cleared mid-TUNE still gets its output back...
    const TuneInhibitOutputs restore = plan;
    check(restore.accTx, "a preference cleared during TUNE does not strand ACC TX off");
    // ...and one set mid-TUNE does not invent a restore of something never disabled.
    check(!restore.tx1, "a preference set during TUNE invents no restore");

    check(!tuneInhibitPlan(TuneInhibitOutputs{}, band).any(),
          "no inhibit preference, nothing disabled");
    check(!tuneInhibitPlan(all, TuneInhibitOutputs{}).any(),
          "every output already off, nothing disabled and nothing to restore");
    check(plan.names().join(QStringLiteral(",")) == QStringLiteral("ACC TX,TX2,TX3"),
          "the log names exactly the outputs this cycle owns");

    if (g_failed) {
        std::printf("tune_inhibit_restore_test: %d failure(s)\n", g_failed);
        return 1;
    }
    std::printf("tune_inhibit_restore_test: all checks passed\n");
    return 0;
}
