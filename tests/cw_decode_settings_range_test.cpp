// Upstream bb2ba031 (#5921) — the CW decoder's pitch and WPM search ranges are
// one shared, saved setting in the CwDecoder document. A saved range reaches
// the sliders and the decoder only as an ordered band inside the slider span;
// anything else falls back to the default, and saving a range keeps the rest
// of the document. Isolated settings profile; no widgets, no decoder.
#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/CwDecodeSettings.h"

#include <QCoreApplication>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what);
    if (!ok) {
        ++g_failed;
    }
}

bool sameRange(CwDecodeSettings::Range got, int low, int high)
{
    return got.low == low && got.high == high;
}

void store(const char* json)
{
    // CONSTRUCTED blobs: they exercise the read-side validation only.
    AppSettings::instance().setValue("CwDecoder", QString::fromLatin1(json));
}

}  // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("cw-decode-settings-range-test"));
    if (!profile.isValid()) {
        return 1;
    }
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    store(R"({"rx":"True","tx":"False"})");
    check(sameRange(CwDecodeSettings::pitchRange(), 500, 700)
              && sameRange(CwDecodeSettings::speedRange(), 15, 40),
          "missing ranges read as the 500-700 Hz / 15-40 WPM defaults");

    store(R"({"pitchLow":100,"pitchHigh":5000,"speedLow":0,"speedHigh":99})");
    check(sameRange(CwDecodeSettings::pitchRange(), 300, 1200)
              && sameRange(CwDecodeSettings::speedRange(), 5, 60),
          "out-of-span ranges clamp to the slider spans");

    store(R"({"pitchLow":900,"pitchHigh":600,"speedLow":"fast","speedHigh":30})");
    check(sameRange(CwDecodeSettings::pitchRange(), 500, 700)
              && sameRange(CwDecodeSettings::speedRange(), 15, 40),
          "inverted or non-numeric ranges fall back to the defaults");

    store(R"({"rx":"False","tx":"True","fontPx":17})");
    CwDecodeSettings::setPitchRange(651, 954);
    CwDecodeSettings::setSpeedRange(20, 30);
    check(sameRange(CwDecodeSettings::pitchRange(), 651, 954)
              && sameRange(CwDecodeSettings::speedRange(), 20, 30),
          "a saved range reads back");
    check(!CwDecodeSettings::rxEnabled() && CwDecodeSettings::txEnabled()
              && CwDecodeSettings::fontPx() == 17,
          "saving a range keeps the RX/TX toggles and the font size");

    AppSettings::instance().save();
    AppSettings::instance().load();
    check(sameRange(CwDecodeSettings::pitchRange(), 651, 954)
              && sameRange(CwDecodeSettings::speedRange(), 20, 30),
          "the saved ranges survive a reload of the settings store");

    std::printf("%s\n", g_failed == 0 ? "cw_decode_settings_range_test: all checks passed"
                                      : "cw_decode_settings_range_test: FAILED");
    return g_failed == 0 ? 0 : 1;
}
