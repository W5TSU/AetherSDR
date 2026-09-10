// ExternalControlMigration — the combined one-way flat-key -> nested step.
//
// Runs in its own process: AppSettings is a process-wide singleton.

#include "TestSettingsProfile.h"

#include "core/AppSettings.h"
#include "core/CatSettings.h"
#include "core/DaxSettings.h"
#include "core/ExternalControlMigration.h"
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
    TestSettingsProfile profile(QStringLiteral("ext-control-migration-test"));
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();
    auto& s = AppSettings::instance();

    // ---- no legacy keys: nothing written --------------------------------
    check(!ExternalControlMigration::run(), "no legacy keys: writes nothing");

    // ---- seed legacy CAT + TCI + DAX flat keys --------------------------
    s.setValue(QStringLiteral("CatEnabled"), QStringLiteral("True"));
    s.setValue(QStringLiteral("CatPort_0_Port"), QStringLiteral("4532"));
    s.setValue(QStringLiteral("CatPort_0_Dialect"), QStringLiteral("Rigctld"));
    s.setValue(QStringLiteral("CatPort_0_Enabled"), QStringLiteral("True"));
    s.setValue(QStringLiteral("CatPort_0_VfoA"), QStringLiteral("0"));
    s.setValue(QStringLiteral("CatPort_0_VfoB"), QStringLiteral("-1"));
    for (int i = 1; i < 8; ++i) {
        const QString p = QStringLiteral("CatPort_%1_").arg(i);
        s.setValue(p + QStringLiteral("Port"), QString());
        s.setValue(p + QStringLiteral("Dialect"), QStringLiteral("FlexCAT"));
        s.setValue(p + QStringLiteral("Enabled"), QStringLiteral("False"));
        s.setValue(p + QStringLiteral("VfoA"), QStringLiteral("0"));
        s.setValue(p + QStringLiteral("VfoB"), QStringLiteral("-1"));
    }
    s.setValue(QStringLiteral("AutoStartTCI"), QStringLiteral("True"));
    s.setValue(QStringLiteral("TciPort"), QStringLiteral("50123"));
    s.setValue(QStringLiteral("AutoStartDAX"), QStringLiteral("True"));
    s.save();

    check(ExternalControlMigration::run(), "legacy keys present: migration writes");

    check(CatSettings::enabled(), "CAT master enable migrated");
    check(!CatSettings::ports().isEmpty() && CatSettings::ports().first().port == 4532,
          "CAT listener 0 migrated");
    check(!CatSettings::ports().isEmpty() && CatSettings::ports().first().enabled,
          "CAT listener 0 enabled migrated");
    check(CatSettings::ports().size() == 1, "empty trailing CAT listeners dropped");
    check(TciSettings::enabled() && TciSettings::port() == 50123, "TCI migrated");
    check(DaxSettings::audioEnabled(), "DAX audio autostart migrated");

    check(s.contains(QStringLiteral("CatEnabled"))
              && s.contains(QStringLiteral("AutoStartTCI"))
              && s.contains(QStringLiteral("AutoStartDAX")),
          "legacy keys retained (downgrade safety)");

    check(!ExternalControlMigration::run(), "second run is a no-op");

    if (g_failures == 0) {
        std::printf("external_control_migration_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
