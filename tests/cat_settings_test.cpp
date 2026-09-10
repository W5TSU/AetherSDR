// CatSettings — owned config object ("CatServer" root key, Principle V/XIV).
//
// The load-bearing assertions: defaults on an empty store, JSON round-trip,
// the "which listeners run" rule (master enable AND per-port enable AND
// port >= 1024), the 8-port cap, one-way migration from the legacy
// CatEnabled / CatPort_<n>_* flat keys (which must be left in place), and
// "a corrupt store never crashes and falls back to defaults".
//
// Runs in its own process: AppSettings is a process-wide singleton.

#include "TestSettingsProfile.h"

#include "core/AppSettings.h"
#include "core/CatSettings.h"

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
    TestSettingsProfile profile(QStringLiteral("cat-settings-test"));
    QCoreApplication app(argc, argv);
    AppSettings::instance().load();

    // ---- defaults on an empty store --------------------------------------
    check(!CatSettings::enabled(), "default: CAT server disabled");
    {
        const QVector<CatPortSpec> d = CatSettings::ports();
        check(d.size() == 1, "default: one listener");
        check(d.first().port == 4532, "default: port 4532");
        check(d.first().dialect == QStringLiteral("Rigctld"), "default: rigctld dialect");
        check(!d.first().enabled, "default: listener disabled");
    }
    check(CatSettings::activePortCount() == 0, "default: nothing active");

    // ---- round trip ----------------------------------------------------------
    {
        QVector<CatPortSpec> p;
        p.append({4532, QStringLiteral("Rigctld"), true, 0, -1});
        p.append({5001, QStringLiteral("FlexCAT"), true, 0, 1});
        CatSettings::setEnabled(true);
        CatSettings::setPorts(p);
    }
    check(CatSettings::enabled(), "round trip: enabled");
    {
        const QVector<CatPortSpec> back = CatSettings::ports();
        check(back.size() == 2, "round trip: two listeners");
        check(back.at(1).port == 5001, "round trip: second port preserved");
        check(back.at(1).dialect == QStringLiteral("FlexCAT"),
              "round trip: second dialect preserved");
        check(back.at(1).vfoB == 1, "round trip: vfoB preserved");
    }

    // ---- activePortCount honours master enable + privileged-port rule -------
    check(CatSettings::activePortCount() == 2, "active: 2 when all enabled");
    CatSettings::setEnabled(false);
    check(CatSettings::activePortCount() == 0, "active: 0 when master off");
    CatSettings::setEnabled(true);
    {
        QVector<CatPortSpec> p = CatSettings::ports();
        p[0].port = 80;                            // privileged
        CatSettings::setPorts(p);
    }
    check(CatSettings::activePortCount() == 1, "active: privileged port ignored");

    // ---- clamp to kMaxPorts -----------------------------------------------
    {
        QVector<CatPortSpec> many;
        for (int i = 0; i < 20; ++i) {
            many.append({quint16(4600 + i), QStringLiteral("Rigctld"), false, -1, -1});
        }
        CatSettings::setPorts(many);
    }
    check(CatSettings::ports().size() == CatSettings::kMaxPorts, "clamp: at most 8 listeners");

    // ---- corrupt store falls back to defaults, no crash -------------------
    AppSettings::instance().setValue(QStringLiteral("CatServer"),
                                     QStringLiteral("{ not json"));
    check(!CatSettings::enabled() && CatSettings::ports().size() >= 1,
          "corrupt store: falls back to defaults, no crash");

    // ---- one-way migration from legacy flat keys -------------------------
    {
        auto& s = AppSettings::instance();
        s.remove(QStringLiteral("CatServer"));       // pretend a fresh upgrade
        s.setValue(QStringLiteral("CatEnabled"), QStringLiteral("True"));
        s.setValue(QStringLiteral("CatPort_0_Port"), QStringLiteral("4532"));
        s.setValue(QStringLiteral("CatPort_0_Dialect"), QStringLiteral("Rigctld"));
        s.setValue(QStringLiteral("CatPort_0_Enabled"), QStringLiteral("True"));
        s.setValue(QStringLiteral("CatPort_0_VfoA"), QStringLiteral("0"));
        s.setValue(QStringLiteral("CatPort_0_VfoB"), QStringLiteral("-1"));
        for (int i = 1; i < 8; ++i) {
            const QString pfx = QStringLiteral("CatPort_%1_").arg(i);
            s.setValue(pfx + QStringLiteral("Port"), QString());
            s.setValue(pfx + QStringLiteral("Dialect"), QStringLiteral("FlexCAT"));
            s.setValue(pfx + QStringLiteral("Enabled"), QStringLiteral("False"));
            s.setValue(pfx + QStringLiteral("VfoA"), QStringLiteral("0"));
            s.setValue(pfx + QStringLiteral("VfoB"), QStringLiteral("-1"));
        }
        s.save();
    }
    check(CatSettings::migrate(), "migration: writes when legacy keys present, nested absent");
    check(CatSettings::enabled(), "migration: master enable carried over");
    {
        const QVector<CatPortSpec> p = CatSettings::ports();
        check(p.size() == 1, "migration: empty trailing listeners dropped");
        check(!p.isEmpty() && p.first().port == 4532, "migration: listener 0 port");
        check(!p.isEmpty() && p.first().enabled, "migration: listener 0 enabled");
    }
    check(AppSettings::instance().contains(QStringLiteral("CatEnabled")),
          "migration: legacy keys retained (downgrade safety)");
    check(!CatSettings::migrate(), "migration: second run is a no-op");

    if (g_failures == 0) {
        std::printf("cat_settings_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
