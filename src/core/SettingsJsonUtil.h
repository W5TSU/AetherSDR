#pragma once

#include <QJsonValue>
#include <QLatin1String>

namespace AetherSDR {

// Read a JSON value as a bool, accepting both a native JSON bool and the
// legacy "True"/"False" string convention used across the AppSettings store.
inline bool jsonBool(const QJsonValue& v, bool fallback)
{
    if (v.isBool()) {
        return v.toBool();
    }
    if (v.isString()) {
        return v.toString() == QLatin1String("True");
    }
    return fallback;
}

} // namespace AetherSDR
