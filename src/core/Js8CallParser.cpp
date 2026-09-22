#include "Js8CallParser.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTimeZone>

#include <optional>

namespace AetherSDR::Js8CallParser {

namespace {

// A JSON number read defensively: absent, wrong-typed, or non-finite all
// come back as "not present" rather than silently coercing to 0 — a message
// missing FREQ must fall through to the DIAL+OFFSET path below, not parse
// as a spot sitting at 0 Hz.
std::optional<double> numberField(const QJsonObject& params, QLatin1String key)
{
    if (!params.contains(key)) return std::nullopt;
    const QJsonValue v = params.value(key);
    if (!v.isDouble()) return std::nullopt;
    return v.toDouble();
}

QString stringField(const QJsonObject& params, QLatin1String key)
{
    return params.value(key).toString();
}

// FREQ is JS8Call's own convenience sum of DIAL + OFFSET; some captures
// carry it, some don't, but DIAL/OFFSET are the authoritative pair when it's
// missing. Returns nullopt (never 0) when neither source yields a usable,
// positive frequency, so callers can tell "no frequency" from "0 Hz".
std::optional<double> resolveFreqHz(const QJsonObject& params)
{
    if (const auto freq = numberField(params, QLatin1String("FREQ")); freq && *freq > 0.0)
        return freq;
    const auto dial = numberField(params, QLatin1String("DIAL"));
    const auto offset = numberField(params, QLatin1String("OFFSET"));
    if (dial && offset) {
        const double sum = *dial + *offset;
        if (sum > 0.0) return sum;
    }
    return std::nullopt;
}

} // namespace

QString messageType(const QByteArray& line)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return {};
    return doc.object().value(QLatin1String("type")).toString();
}

bool parseLine(const QByteArray& line, Js8CallSpot& outSpot)
{
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return false;

    const QJsonObject obj = doc.object();
    const QJsonValue typeVal = obj.value(QLatin1String("type"));
    if (!typeVal.isString())
        return false;
    const QString type = typeVal.toString();

    const bool isSpot = (type == QLatin1String("RX.SPOT"));
    const bool isDirected = (type == QLatin1String("RX.DIRECTED"));
    if (!isSpot && !isDirected)
        return false;  // includes RX.ACTIVITY — see the header comment

    const QJsonValue paramsVal = obj.value(QLatin1String("params"));
    if (!paramsVal.isObject())
        return false;
    const QJsonObject params = paramsVal.toObject();

    // RX.SPOT names the station in CALL; RX.DIRECTED (a message reassembled
    // from someone else's transmission) names them in FROM instead.
    const QString callsign =
        stringField(params, isSpot ? QLatin1String("CALL") : QLatin1String("FROM")).trimmed();
    if (callsign.isEmpty())
        return false;

    const auto freqHz = resolveFreqHz(params);
    if (!freqHz)
        return false;

    QString comment;
    if (isSpot) {
        comment = stringField(params, QLatin1String("GRID")).trimmed();
    } else {
        comment = stringField(params, QLatin1String("TEXT")).trimmed();
    }

    int snr = 0;
    if (const auto snrVal = numberField(params, QLatin1String("SNR")))
        snr = static_cast<int>(*snrVal);

    QTime utcTime = QTime::currentTime();
    if (const auto utcMs = numberField(params, QLatin1String("UTC")); utcMs && *utcMs > 0.0) {
        utcTime = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(*utcMs), QTimeZone::UTC).time();
    }

    outSpot.dxCall = callsign;
    outSpot.freqMhz = *freqHz / 1.0e6;
    outSpot.comment = comment;
    outSpot.utcTime = utcTime;
    outSpot.snr = snr;
    return true;
}

} // namespace AetherSDR::Js8CallParser
