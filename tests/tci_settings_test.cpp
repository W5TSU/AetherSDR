// TciSettings — owned config object ("TciServer" root key, Principle V/XIV).
//
// Runs in its own process: AppSettings is a process-wide singleton.

#include "TestSettingsProfile.h"

#include "core/AppSettings.h"
#include "core/TciSettings.h"

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
    TestSettingsProfile profile(QStringLiteral("tci-settings-test"));
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    // ---- defaults --------------------------------------------------------
    check(!TciSettings::enabled(), "default: disabled");
    check(TciSettings::port() == 50001, "default: port 50001");

    // ---- round trip ----------------------------------------------------------
    TciSettings::setEnabled(true);
    TciSettings::setPort(50123);
    check(TciSettings::enabled(), "round trip: enabled");
    check(TciSettings::port() == 50123, "round trip: port");

    // ---- invalid port falls back --------------------------------------------
    TciSettings::setPort(80);
    check(TciSettings::port() == 50001, "privileged port falls back to 50001");

    // ---- corrupt store falls back to defaults -----------------------------
    AppSettings::instance().setValue(QStringLiteral("TciServer"),
                                     QStringLiteral("not json"));
    check(!TciSettings::enabled() && TciSettings::port() == 50001,
          "corrupt store: defaults, no crash");

    // ---- one-way migration ---------------------------------------------------
    {
        auto& s = AppSettings::instance();
        s.remove(QStringLiteral("TciServer"));
        s.setValue(QStringLiteral("AutoStartTCI"), QStringLiteral("True"));
        s.setValue(QStringLiteral("TciPort"), QStringLiteral("50123"));
        s.save();
    }
    check(TciSettings::migrate(), "migration: writes when legacy present");
    check(TciSettings::enabled() && TciSettings::port() == 50123,
          "migration: enable + port carried over");
    check(AppSettings::instance().contains(QStringLiteral("AutoStartTCI")),
          "migration: legacy keys retained");
    check(!TciSettings::migrate(), "migration: second run is a no-op");

    if (g_failures == 0) {
        std::printf("tci_settings_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
