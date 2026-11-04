// The AGC-T auto sweep stops when no audio level reaches this computer. Ported
// from upstream's slice_apf_agc_off_seam_test section 7; the fork always sweeps
// down from 100, so the applied sequences differ. Socket-free, nothing keyed.
#include "core/AgcTCalibrator.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>

#include <cstdio>
#include <vector>

using namespace AetherSDR;

namespace {

int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("    %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++failures;
}

void testSweepStopsWithoutAudio()
{
    SliceModel s(0);
    s.setAgcMode(QStringLiteral("med"));
    s.setAgcThreshold(47);
    AgcTCalibrator cal;
    cal.setSlice(&s);
    cal.setSettleMs(1);
    std::vector<int> applied;
    int audibleAbove = 1000;   // levels arrive only for values over this
    QObject::connect(&s, &SliceModel::agcThresholdChanged, &cal, [&](int value) {
        applied.push_back(value);
        if (value > audibleAbove) {
            for (int i = 0; i < 40; ++i)
                cal.onAudioLevel(0.001f);
        }
    });
    // As the dialog wires it: a value that settles outside a sweep is recorded
    // as a manual point, 120 ms later.
    QObject::connect(&s, &SliceModel::agcThresholdChanged, &cal, &AgcTCalibrator::onValueChanged);
    const auto nothingRecordedOnceSettled = [&] {
        QTest::qWait(400);
        return cal.curve().isEmpty() && !cal.hasRecommendation();
    };
    QSignalSpy finished(&cal, &AgcTCalibrator::finished);
    QSignalSpy noAudio(&cal, &AgcTCalibrator::noAudio);

    cal.startAutoSweep();
    check(finished.wait(5000), "with no audio at all the sweep ends");
    check(noAudio.size() == 1 && !finished.constFirst().constFirst().toBool(),
          "and says it stopped for no audio");
    check(applied == std::vector<int>{100, 47},
          "at its first step, putting AGC-T back");
    check(!cal.isRunning() && !cal.hasRecommendation(), "with nothing to recommend");
    check(nothingRecordedOnceSettled(), "and the put-back value is not recorded as a point");

    // Audio that stops part-way: levels for 100, 96 and 92, none from 88.
    applied.clear();
    finished.clear();
    noAudio.clear();
    audibleAbove = 90;
    cal.startAutoSweep();
    check(finished.wait(5000) && noAudio.size() == 1, "audio that stops mid-sweep stops it");
    check(applied == std::vector<int>{100, 96, 92, 88, 47},
          "at the first silent step, putting AGC-T back");
    check(!cal.hasRecommendation(), "with no recommendation from a part curve");
    check(nothingRecordedOnceSettled(),
          "nor once the put-back value settles: the part curve is dropped");

    // A level from before the sweep, none during it: stale, not absent.
    applied.clear();
    finished.clear();
    noAudio.clear();
    audibleAbove = 1000;
    cal.onAudioLevel(0.2f);
    cal.startAutoSweep();
    check(finished.wait(5000) && noAudio.size() == 1,
          "a level from before the sweep does not count for its first step");
    check(applied == std::vector<int>{100, 47}, "so the sweep stops at 100");
    check(nothingRecordedOnceSettled(), "recording nothing once AGC-T settles");
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testSweepStopsWithoutAudio();
    std::printf("\n  %s — %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
