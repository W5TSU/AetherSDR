// DaxSettings — owned config object ("DaxServer" root key, Principle V/XIV).
//
// Runs in its own process: AppSettings is a process-wide singleton.

#include "TestSettingsProfile.h"

#include "core/AppSettings.h"
#include "core/DaxSettings.h"

#include <QCoreApplication>

#include <cstdio>

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("dax-settings-test"));
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    // ---- defaults --------------------------------------------------------
    check(!DaxSettings::audioEnabled(), "default: DAX audio disabled");
    check(DaxSettings::iqChannelRatesHz() == QVector<int>({48000, 48000, 48000, 48000}),
          "default: four IQ channels at 48k");
    check(DaxSettings::iqChannelEnabled() == QVector<bool>({false, false, false, false}),
          "default: no IQ channels enabled");

    // ---- round trip ----------------------------------------------------------
    DaxSettings::setAudioEnabled(true);
    DaxSettings::setIqChannelRatesHz({192000, 96000, 48000, 24000});
    DaxSettings::setIqChannelEnabled({true, false, true, false});
    check(DaxSettings::audioEnabled(), "round trip: audio enabled");
    check(DaxSettings::iqChannelRatesHz() == QVector<int>({192000, 96000, 48000, 24000}),
          "round trip: IQ rates");
    check(DaxSettings::iqChannelEnabled() == QVector<bool>({true, false, true, false}),
          "round trip: IQ enables");

    // ---- illegal rate snaps to nearest legal ------------------------------
    DaxSettings::setIqChannelRatesHz({12345, 48000, 48000, 48000});
    check(DaxSettings::iqChannelRatesHz().first() == 24000,
          "illegal IQ rate 12345 snaps to 24000");

    // ---- one-way migration ---------------------------------------------------
    {
        auto& s = AppSettings::instance();
        s.remove(QStringLiteral("DaxServer"));
        s.setValue(QStringLiteral("AutoStartDAX"), QStringLiteral("True"));
        s.setValue(QStringLiteral("DaxIqRate1"), QStringLiteral("96000"));
        s.setValue(QStringLiteral("DaxIqEnabled1"), QStringLiteral("True"));
        s.save();
    }
    check(DaxSettings::migrate(), "migration: writes when legacy present");
    check(DaxSettings::audioEnabled(), "migration: DAX audio autostart carried over");
    check(DaxSettings::iqChannelRatesHz().first() == 96000, "migration: IQ rate 1 carried over");
    check(DaxSettings::iqChannelEnabled().first(), "migration: IQ enable 1 carried over");
    check(AppSettings::instance().contains(QStringLiteral("AutoStartDAX")),
          "migration: legacy keys retained");
    check(!DaxSettings::migrate(), "migration: second run is a no-op");

    if (g_failures == 0) {
        std::printf("dax_settings_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
