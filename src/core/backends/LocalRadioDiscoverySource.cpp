#include "core/backends/LocalRadioDiscoveryMapping.h"

#include "core/HackRfDiscovery.h"
#include "core/RadioDiscovery.h"
#include "core/RtlSdrDiscovery.h"
#include "core/backends/anan/AnanDiscovery.h"
#include "core/backends/hl2/Hl2Discovery.h"
#include "core/backends/sim/SimBackend.h"

namespace AetherSDR {
namespace {

class LocalRadioDiscoverySource final : public RadioDiscoverySource {
public:
    explicit LocalRadioDiscoverySource(LocalDiscoveryOptions options) : m_options(options) {}
    ~LocalRadioDiscoverySource() override { stop(); }

    QStringList enabledSources() const override
    {
        QStringList sources;
        for (const char* family : {"flex", "hl2", "anan", "rtl", "hackrf"}) {
            if (localEnabled(QLatin1String(family))) {
                sources.append(QLatin1String(family));
            }
        }
        if (m_options.simulator) {
            sources.append(QStringLiteral("sim"));
        }
        sources.sort();
        return sources;
    }

    void start() override
    {
        if (m_started) {
            return;
        }
        m_started = true;
        m_running = true;
        if (localEnabled(QStringLiteral("flex"))) {
            m_flex = std::make_unique<RadioDiscovery>();
            wire(m_flex.get(), QStringLiteral("flex"), QStringLiteral("lan"));
            m_flex->startListening();
        }
        if (localEnabled(QStringLiteral("hl2"))) {
            m_hl2 = std::make_unique<hl2::Hl2Discovery>();
            wire(m_hl2.get(), QStringLiteral("hl2"), QStringLiteral("lan"));
            m_hl2->start();
        }
        if (localEnabled(QStringLiteral("anan"))) {
            m_anan = std::make_unique<anan::AnanDiscovery>();
            wire(m_anan.get(), QStringLiteral("anan"), QStringLiteral("lan"));
            m_anan->start();
        }
        if (localEnabled(QStringLiteral("rtl"))) {
            m_rtl = std::make_unique<RtlSdrDiscovery>();
            wire(m_rtl.get(), QStringLiteral("rtl"), QStringLiteral("usb"));
            m_rtl->start();
        }
        if (localEnabled(QStringLiteral("hackrf"))) {
            m_hackrf = std::make_unique<HackRfDiscovery>();
            wire(m_hackrf.get(), QStringLiteral("hackrf"), QStringLiteral("usb"));
            m_hackrf->start();
        }
        if (m_options.simulator) {
            DiscoveredRadio demo;
            demo.family = SimBackend::familyName();
            demo.serial = SimBackend::demoSerial();
            demo.name = SimBackend::demoModelName();
            demo.model = SimBackend::demoModelName();
            demo.transport = QStringLiteral("sim");
            emit radioChanged(demo);
        }
    }

    void stop() override
    {
        m_started = true; // A stopped source is terminal, even before start.
        m_running = false;
        if (m_flex) { m_flex->stopListening(); }
        if (m_hl2) { m_hl2->stop(); }
        if (m_anan) { m_anan->stop(); }
        if (m_rtl) { m_rtl->stop(); }
        if (m_hackrf) { m_hackrf->stop(); }
    }

private:
    // The one answer to "does `local` cover this family", so what
    // enabledSources() reports and what start() starts cannot disagree. The
    // USB families also need their library compiled in.
    bool localEnabled(const QString& family) const
    {
        if (!m_options.local
            || (!m_options.families.isEmpty() && !m_options.families.contains(family))) {
            return false;
        }
        if (family == QLatin1String("rtl")) {
            return RtlSdrDiscovery::isAvailable();
        }
        if (family == QLatin1String("hackrf")) {
            return HackRfDiscovery::isAvailable();
        }
        return true;
    }

    template<typename Source>
    void wire(Source* source, const QString& family, const QString& transport)
    {
        const auto changed = [this, family, transport](const RadioInfo& info) {
            if (!m_running) {
                return;
            }
            emit radioChanged(discovery::normalize(info, family, transport));
        };
        connect(source, &Source::radioDiscovered, this, changed);
        connect(source, &Source::radioUpdated, this, changed);
        connect(source, &Source::radioLost, this, [this, family](const QString& serial) {
            if (m_running) {
                emit radioLost(family, serial);
            }
        });
    }

    const LocalDiscoveryOptions m_options;
    bool m_started{false};
    bool m_running{false};
    std::unique_ptr<RadioDiscovery> m_flex;
    std::unique_ptr<hl2::Hl2Discovery> m_hl2;
    std::unique_ptr<anan::AnanDiscovery> m_anan;
    std::unique_ptr<RtlSdrDiscovery> m_rtl;
    std::unique_ptr<HackRfDiscovery> m_hackrf;
};

} // namespace

std::unique_ptr<RadioDiscoverySource> makeLocalRadioDiscoverySource(LocalDiscoveryOptions options)
{
    return std::make_unique<LocalRadioDiscoverySource>(options);
}

} // namespace AetherSDR
