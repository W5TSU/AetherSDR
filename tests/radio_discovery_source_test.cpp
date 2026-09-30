#include "core/HackRfDiscovery.h"
#include "core/backends/LocalRadioDiscoveryMapping.h"
#include "core/control/RadioCatalogue.h"

#include <QCoreApplication>
#include <QJsonArray>

#include <cstdio>

using namespace AetherSDR;
using namespace AetherSDR::control;

// The real source factory is the subject. Both cases explicitly disable local
// discovery: no UDP socket, USB enumeration, radio backend, or settings access.
int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    for (const bool simulator : {false, true}) {
        ControlResourceStore store;
        RadioCatalogue catalogue(makeLocalRadioDiscoverySource({false, simulator}), &store);
        catalogue.start();
        const QJsonObject value = store.get({QStringLiteral("radioCatalogue"), {}, {}})->value;
        const QJsonArray entries = value.value(QStringLiteral("entries")).toArray();
        const QJsonArray sources = value.value(QStringLiteral("sources")).toArray();
        if (entries.size() != (simulator ? 1 : 0) || sources.size() != (simulator ? 1 : 0)
            || (simulator && (sources.first() != QStringLiteral("sim")
                || entries.first().toObject().value(QStringLiteral("serial")) != QStringLiteral("DEMO-0001")
                || entries.first().toObject().value(QStringLiteral("transport")) != QStringLiteral("sim")))) {
            std::fprintf(stderr, "production source must honor passive/simulator-only options\n");
            return 1;
        }
    }

    // The family filter narrows what a local source would start. Asked only
    // for enabledSources() -- never started -- so still no socket or USB scan.
    // This is how the desktop owns HackRF discovery without including the
    // vendor header above the seam (EB3).
    {
        const auto hackrfOnly = makeLocalRadioDiscoverySource(
            {true, false, {QStringLiteral("hackrf")}});
        const QStringList expected = HackRfDiscovery::isAvailable()
            ? QStringList{QStringLiteral("hackrf")} : QStringList{};
        if (hackrfOnly->enabledSources() != expected) {
            std::fprintf(stderr, "a families filter must enable exactly the named family\n");
            return 1;
        }
        const auto flexOnly = makeLocalRadioDiscoverySource(
            {true, false, {QStringLiteral("flex")}});
        if (flexOnly->enabledSources() != QStringList{QStringLiteral("flex")}) {
            std::fprintf(stderr, "a families filter must exclude every unnamed family\n");
            return 1;
        }
        const auto unfiltered = makeLocalRadioDiscoverySource({true, false});
        if (unfiltered->enabledSources().contains(QStringLiteral("hackrf"))
            != HackRfDiscovery::isAvailable()) {
            std::fprintf(stderr, "an unfiltered local source must include HackRF when built with it\n");
            return 1;
        }
    }

    // RadioInfo -> DiscoveredRadio -> RadioInfo must keep everything a USB
    // family's picker entry and connect depend on. serialIdentity is the one
    // that is easy to lose: HackRfBackend::connectRadio() opens by index only
    // when it says the serial is an index locator, and a dropped identity would
    // make an "hackrf:0" entry look like a real serial.
    {
        RadioInfo info;
        info.family = QStringLiteral("hackrf");
        info.serial = QStringLiteral("hackrf:0");
        info.serialIdentity.reportedSerial = QStringLiteral("0000000000000000977c64de21406a13");
        info.serialIdentity.indexLocator = true;
        info.name = QStringLiteral("HackRF One");
        info.model = QStringLiteral("HackRF One");
        info.status = QStringLiteral("Available");
        info.inUse = false;

        const RadioInfo back = discovery::toRadioInfo(
            discovery::normalize(info, QStringLiteral("hackrf"), QStringLiteral("usb")));
        if (back.family != info.family || back.serial != info.serial
            || back.name != info.name || back.model != info.model
            || back.status != info.status || back.inUse != info.inUse
            || back.serialIdentity.reportedSerial != info.serialIdentity.reportedSerial
            || back.serialIdentity.indexLocator != info.serialIdentity.indexLocator) {
            std::fprintf(stderr, "a USB radio must survive the seam round trip unchanged\n");
            return 1;
        }

        info.inUse = true;
        info.status = QStringLiteral("In_Use");
        const RadioInfo busy = discovery::toRadioInfo(
            discovery::normalize(info, QStringLiteral("hackrf"), QStringLiteral("usb")));
        if (!busy.inUse || busy.status != QStringLiteral("In_Use")) {
            std::fprintf(stderr, "an in-use radio must come back In_Use\n");
            return 1;
        }
    }
    return 0;
}
