#pragma once

#include "core/backends/hackrf/HackRfTuning.h"

namespace AetherSDR {
class RadioSettingsScope;
}

namespace AetherSDR::hackrf {

// dBFS -> dBm for the HackRF: the spectrum, the waterfall and the S-meter all
// add offsetDb() to their raw readings, so they agree with each other and a
// gain change moves none of them (the same rule Hl2DbReference keeps).
//
//   dBm = dBFS - gain(board, lna, vga, amp) + kFullScaleDbm + operator offset
//
// THE GAIN TABLE IS MEASURED, not the nominal labels. Measured 2026-10-15 on a
// HackRF Pro with hackrf_transfer at 8 MS/s, from the power of two FM
// broadcast carriers at 101.9 and 102.7 MHz (constant envelope, so the level
// is steady whatever the modulation):
//   * LNA: the Pro's IF chip has three steps, not five. Codes 0 and 8 measure
//     the same, as do 16 and 24, and 32 and 40: +18.0 and +35.5 dB over code 0.
//     The HackRF One's MAX2837 takes the documented 8 dB steps.
//   * VGA: within ~1 dB of the commanded dB over 10-30 dB.
//   * RF amp: +12.3 dB at 100 MHz on the Pro. The docs give ~11 dB for the
//     One, falling at higher frequencies.
//
// kFullScaleDbm IS AN ESTIMATE, which is why the operator offset exists. It
// is the level at the antenna that would read 0 dBFS with zero table gain.
// Derived from the receiver's own noise: at 1.415 GHz (the protected 1400-1427
// MHz band, so no signals), amp off, LNA 32/40 and VGA 40, the Pro read -95.0
// dBFS/Hz after removing the 0.8 dB the post-LNA stages add (from the LNA
// 16 -> 32 step, which raised the noise 7.5 dB for 17.4 dB of gain). A 10 dB
// noise figure puts that noise at -164 dBm/Hz at the antenna: K = -164 + 95.0
// - (35.5 + 40) = +6.5 dBm. Each dB the real noise figure differs from 10 is a
// dB of error, and the HackRF's response varies by several dB across 1 MHz -
// 6 GHz, so this gets readings into the right range, not onto a calibrated
// figure. The One uses the same figure, unmeasured.
class HackRfDbReference {
public:
    static constexpr double kFullScaleDbm = 6.5;
    static constexpr double kProAmpGainDb = 12.3;
    static constexpr double kOneAmpGainDb = 11.0;
    // Hl2Spectrum divides each bin by HALF the window sum, the convention for a
    // real signal. The HackRF's IQ is complex, so a carrier reads 6.02 dB above
    // its power there while the S-meter reads its power. The spectrum adds
    // this as well, so the trace and the meter agree on a carrier.
    static constexpr double kSpectrumCorrectionDb = -6.0206;
    // The operator offset's range: wide enough for any real unit and band.
    static constexpr double kMaxOffsetDb = 40.0;

    // The receive gain ahead of the ADC that the commanded settings apply.
    static double gainDb(HackRfBoard board, int lnaDb, int vgaDb, bool ampOn);

    // What to add to a dBFS reading to get dBm at the antenna.
    static double offsetDb(HackRfBoard board, int lnaDb, int vgaDb, bool ampOn,
                           double operatorOffsetDb);

    static double clampOffset(double db);

    // The operator's correction for one physical HackRF, keyed by its serial.
    // 0 is "uncalibrated" and is stored as no document.
    static double loadOffsetDb(const RadioSettingsScope& scope);
    static void saveOffsetDb(const RadioSettingsScope& scope, double db);

    static constexpr const char* kFeature = "LevelCalibration";
    static constexpr const char* kFieldOffsetDb = "offsetDb";
    static constexpr int kSchemaVersion = 1;
};

} // namespace AetherSDR::hackrf
