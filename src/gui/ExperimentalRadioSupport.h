#pragma once

#include <QString>

#include <optional>

namespace AetherSDR {

struct ExperimentalRadioDescriptor {
    QString displayName;
    QString noticeSettingKey;
};

inline std::optional<ExperimentalRadioDescriptor> experimentalRadioDescriptor(
    const QString& family)
{
    const QString normalized = family.trimmed().toLower();
    if (normalized == QLatin1String("icom")) {
        return ExperimentalRadioDescriptor{
            QStringLiteral("Icom"),
            QStringLiteral("ShowExperimentalRadioNoticeIcomV1")};
    }
    // Hermes-Lite 2 was promoted from experimental to supported (ADR 0001):
    // truthful mode menu, panadapter parity with the Flex path, a hardened
    // raw-IQ DSP chain, behind a hardware certification gate.
    return std::nullopt;
}

inline QString experimentalRadioNoticeText(const QString& displayName)
{
    return QStringLiteral(
               "Support for %1 radios is still experimental. Core receive and transmit "
               "functions are available, but some controls, meters, and radio-specific "
               "features may be incomplete or behave differently than expected.\n\n"
               "If you encounter a problem, use Help \u2192 File an Issue and include your "
               "radio model and firmware version. Your reports help us improve support for "
               "this radio family.")
        .arg(displayName);
}

} // namespace AetherSDR
