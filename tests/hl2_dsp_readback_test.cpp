// The Hl2RxDsp read accessors that back the bridge's `dsp.get` extension
// (`get_state model=dsp selector=backend`). HERMES.md §8.1: a readback of the
// live WDSP config "would have caught gaps 3, 4 and 5 immediately" — the
// recurring failure in this backend is model/DSP divergence, and these
// accessors are what lets an assertion see the DSP side.
//
// Pinned here, at the Hl2RxDsp level, for the same reason hl2_noise_blanker_test
// works directly with the chain: the JSON-assembly half in
// Hl2Backend::invokeExtension mirrors the already-proven nb.get iteration and
// the beginDspSetup() BlockingQueuedConnection read pattern, and — like
// nb.get — is exercised end to end through the automation bridge on hardware.

#include "core/backends/hl2/Hl2RxDsp.h"
#include "core/dsp/WdspChannel.h"

#include <QCoreApplication>

#include <cstdio>

using namespace AetherSDR::hl2;

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

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    Hl2RxDsp dsp;

    // Unconfigured: channelConfig() is value-initialised, not a crash.
    {
        const WdspChannel::Config c = dsp.channelConfig();
        check(!dsp.isConfigured() && c.inputSampleRate == 48000,
              "channelConfig() before configure() is the default Config");
    }

    Hl2RxDsp::Config cfg;
    cfg.inputSampleRateHz = 96000;
    cfg.audioSampleRateHz = 24000;
    cfg.mode = WdspChannel::Mode::Lsb;
    cfg.filterLowHz = -2700.0;
    cfg.filterHighHz = -300.0;
    cfg.agcMode = 2;
    cfg.maximumAgcGainDb = 90.0;

    std::string err;
    const bool ok = dsp.configure(cfg, &err);
    check(ok, err.empty() ? "configure() succeeded" : err.c_str());

    const WdspChannel::Config c = dsp.channelConfig();
    check(c.inputSampleRate == 96000, "input rate echoes configure()");
    check(c.dspSampleRate == 48000, "dsp rate is fixed at 48000 (HERMES.md §5)");
    check(c.outputSampleRate == 24000, "output rate is the audio rate");
    check(c.mode == WdspChannel::Mode::Lsb, "mode echoes configure()");
    check(c.filterLowHz == -2700.0 && c.filterHighHz == -300.0,
          "passband edges echo configure()");
    check(c.agcMode == 2 && c.maximumAgcGainDb == 90.0,
          "AGC mode and ceiling echo configure()");
    check(c.filterTaps == Hl2RxDsp::kRxFilterTapsShort,
          "filter taps: the low-latency length with no notch placed (Plan 4.1)");

    // shiftHz() tracks the runtime slice offset.
    check(dsp.shiftHz() == 0.0, "shift starts at 0");
    dsp.setShift(1234.0);
    check(dsp.shiftHz() == 1234.0, "shiftHz() reports the set shift");

    // notchesEnabled() tracks the global run flag.
    check(dsp.notchesEnabled(), "notches run by default");
    dsp.setNotchesEnabled(false);
    check(!dsp.notchesEnabled(), "notchesEnabled() follows setNotchesEnabled(false)");
    dsp.setNotchesEnabled(true);
    check(dsp.notchesEnabled(), "notchesEnabled() follows setNotchesEnabled(true)");

    // notchCount() reflects the mirrored set.
    check(dsp.notchCount() == 0, "no notches initially");
    dsp.addNotch(0, 14'100'000.0, 100.0, true);
    check(dsp.notchCount() == 1, "notchCount() reflects an added notch");

    std::printf("%s\n", failures == 0 ? "ALL PASS" : "SOME FAILED");
    return failures == 0 ? 0 : 1;
}
