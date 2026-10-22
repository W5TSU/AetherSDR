// The NRS level ceiling follows the Flex firmware (upstream 89828de8, #6498).
// Published FlexLib 4.3.16 caps Slice.NRSLevel at 50; 4.2.20 allows 100.
// FlexBackend declares the ceiling as RadioCapabilities::nrsLevelControl,
// RadioModel hands it to every slice, and SliceModel clamps the request it
// puts on the wire. Socket-free: no FlexBackend is connected and nothing
// reaches a radio. (Upstream's test drives its bridge dispatcher and
// ControlAvailabilityRegistry, which the fork does not have.)

#include "TestSettingsProfile.h"
#include "core/backends/IRadioBackend.h"
#include "core/backends/flex/FlexBackend.h"
#include "models/RadioModel.h"
#include "models/SliceModel.h"

#include <QCoreApplication>
#include <QStringList>

#include <cstdio>
#include <memory>
#include <utility>

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

int flexMaximum(const QString& firmware)
{
    FlexBackend flex;
    flex.setFirmwareVersionProvider([firmware] { return firmware; });
    const RadioCapabilities caps = flex.capabilities();
    return caps.nrsLevelControl ? caps.nrsLevelControl->levelMaximum : -1;
}

class CeilingBackend final : public IRadioBackend
{
public:
    RadioCapabilities caps;
    RadioCapabilities capabilities() const override { return caps; }
    void connectRadio(const RadioConnectRequest&) override {}
    void disconnectRadio() override {}
    bool isConnected() const override { return true; }
    void setSliceFrequency(int, double) override {}
    void setSliceMode(int, const QString&) override {}
    void setSliceFilter(int, int, int) override {}
    void setSliceAgc(int, const QString&, int) override {}
    void setPanCenter(const QString&, double, PanCenterIntent) override {}
    void setKeying(bool) override {}
    void invokeExtension(const QString&, const QString&, quint64, const QVariant&) override {}
};

void testFlexDeclaresTheFirmwareCeiling()
{
    check(flexMaximum(QStringLiteral("4.3.16.42034")) == 50,
          "FLEX 4.3.16 declares an NRS ceiling of 50");
    check(flexMaximum(QStringLiteral("4.4.0")) == 50, "and so does newer firmware");
    check(flexMaximum(QStringLiteral("4.2.20.41343")) == 100,
          "FLEX 4.2.20 keeps the 100 ceiling");
    check(flexMaximum(QString()) == 100, "unknown firmware keeps the 100 ceiling");
    check(flexMaximum(QStringLiteral("4.3.16-beta")) == 100,
          "a malformed version keeps the 100 ceiling");
}

void testSliceClampsTheRequest()
{
    RadioModel radio;
    auto owned = std::make_unique<CeilingBackend>();
    CeilingBackend* backend = owned.get();
    backend->caps.nrsLevelControl = RadioCapabilities::NrsLevelControl{50};
    radio.setBackendForTest(std::move(owned), QStringLiteral("nrs-test"));
    const QString pan = QStringLiteral("nrs/pan0");
    emit backend->panCenterBandwidthChanged(pan, 14.2, 0.1);
    SliceDelta delta;
    delta.panId = pan;
    delta.frequency = 14.2;
    delta.mode = QStringLiteral("USB");
    delta.inUse = true;
    emit backend->sliceChanged(0, delta);
    SliceModel* slice = radio.slice(0);
    if (!slice) {
        std::fprintf(stderr, "FATAL: the slice was not materialised\n");
        std::exit(2);
    }
    check(slice->nrsLevelMaximum() == 50, "the slice adopts the backend's NRS ceiling");

    QStringList wire;
    QObject::connect(slice, &SliceModel::commandReady, slice,
                     [&wire](const QString& cmd) { wire << cmd; });
    slice->setNrsLevel(20);   // off the default, so the clamped value is a change
    wire.clear();
    slice->setNrsLevel(80);
    check(slice->nrsLevel() == 50, "an NRS request above the ceiling is clamped to it");
    check(!wire.isEmpty() && wire.last().endsWith(QStringLiteral("speex_nr_level=50")),
          "and the wire carries the clamped level");
    slice->setNrsLevel(30);
    check(slice->nrsLevel() == 30, "a request below the ceiling passes unchanged");
}

} // namespace

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("nrs-level-range-test"));
    QCoreApplication app(argc, argv);
    check(profile.isValid(), "settings are isolated");
    testFlexDeclaresTheFirmwareCeiling();
    testSliceClampsTheRequest();
    if (g_failed) {
        std::printf("nrs_level_range_test: %d failure(s)\n", g_failed);
        return 1;
    }
    std::printf("nrs_level_range_test: all checks passed\n");
    return 0;
}
