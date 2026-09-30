#pragma once

#include "core/RadioDiscovery.h"   // RadioInfo
#include "core/discovery/RadioDiscoverySource.h"

namespace AetherSDR::discovery {

// The single RadioInfo -> DiscoveredRadio projection every native adapter
// uses. Hoisted out of LocalRadioDiscoverySource so it is reachable from a
// socket-free test: RadioCatalogue rejects a malformed endpoint silently, so a
// family that stopped populating `port` would just vanish from the catalogue,
// indistinguishable from "no radio on the LAN". A pure function of the
// observation plus its family/transport labels — no vendor payload crosses the
// seam, and nothing here can reach a radio command.
[[nodiscard]] inline DiscoveredRadio normalize(const RadioInfo& info, const QString& family,
                                              const QString& transport)
{
    DiscoveredRadio radio;
    radio.family = family;
    radio.serial = info.serial;
    radio.name = info.name;
    radio.model = info.model;
    radio.nickname = info.nickname;
    radio.version = info.version;
    radio.transport = transport;
    if (transport == QStringLiteral("lan")) {
        // Endpoint fields are LAN-only: RadioCatalogue rejects a USB or
        // simulator entry that carries either one.
        radio.address = info.address.toString();
        radio.port = info.port;
    }
    radio.inUse = info.inUse;
    radio.serialIdentity = info.serialIdentity;
    return radio;
}

// The inverse, for a desktop consumer whose picker still speaks RadioInfo
// (ConnectionPanel's onRadioDiscovered/onRadioUpdated). Exact for the USB and
// simulator families, which set nothing normalize() drops except `status` --
// and theirs is always derived from inUse, which is what is rebuilt here. A LAN
// family's vendor fields (Flex multi-client, licensing) do not survive, by
// design; those families are not routed through this.
[[nodiscard]] inline RadioInfo toRadioInfo(const DiscoveredRadio& radio)
{
    RadioInfo info;
    info.family = radio.family;
    info.serial = radio.serial;
    info.serialIdentity = radio.serialIdentity;
    info.name = radio.name;
    info.model = radio.model;
    info.nickname = radio.nickname;
    info.version = radio.version;
    if (radio.transport == QStringLiteral("lan")) {
        info.address = QHostAddress(radio.address);
        info.port = radio.port;
    }
    info.inUse = radio.inUse;
    info.status = radio.inUse ? QStringLiteral("In_Use") : QStringLiteral("Available");
    return info;
}

} // namespace AetherSDR::discovery
