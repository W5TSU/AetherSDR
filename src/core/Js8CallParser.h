#pragma once

#include <QByteArray>
#include <QString>
#include <QTime>

namespace AetherSDR {

// One JS8Call-heard station, from either RX.SPOT or RX.DIRECTED. A separate
// type from DxSpot (rather than reusing it) for the same reason N1mmSpot is
// separate from DxSpot: DxSpot lives in DxClusterClient.h alongside
// QTcpSocket, and pulling that in here would drag Qt6::Network into what's
// meant to be a Qt6::Core-only parsing seam (mirrors wsjtx_dial_tracker_test's
// header-only design). Js8CallClient converts this to a DxSpot at the point
// it emits spotReceived().
struct Js8CallSpot {
    QString dxCall;
    double  freqMhz{0.0};
    QString comment;    // GRID (RX.SPOT) or TEXT (RX.DIRECTED)
    QTime   utcTime;
    int     snr{0};
};

} // namespace AetherSDR

// Pure, dependency-light parsing for JS8Call's TCP JSON API (#21) —
// line-delimited JSON over TCP, default port 2442 — split out from
// Js8CallClient (the QTcpSocket owner) so it can be unit tested without
// pulling in socket infrastructure (mirrors N1MMSpotParser).
//
// Every message is one JSON object per line: {"type": "...", "value": "...",
// "params": {...}}. Only two types are spot-worthy:
//
//   RX.SPOT     — a station JS8Call decoded and identified.
//                 params: CALL, DIAL, FREQ, OFFSET, GRID, SNR.
//   RX.DIRECTED — a fully reassembled directed message, which also names
//                 the sender. params: FROM, TO, TEXT, DIAL, FREQ, OFFSET,
//                 SNR, SPEED, TDRIFT, UTC.
//
// RX.ACTIVITY (a raw, possibly-incomplete decode fragment) is deliberately
// NOT spot-worthy: its params carry no callsign field at all, only a
// frequency and fragment text, so there is no identity to key a marker on.
// It is still worth noting on the console once per session (mirrors how
// N1MMSpotClient handles other document types sharing its port) — that's
// what messageType() is for.
namespace AetherSDR::Js8CallParser {

// Parses one line. On RX.SPOT / RX.DIRECTED with a usable callsign and
// frequency, fills outSpot and returns true. Returns false for every other
// message type, malformed JSON, or a spot-shaped message missing its
// identity (CALL/FROM) or frequency (FREQ, or DIAL+OFFSET when FREQ is
// absent) — outSpot is left untouched on failure.
bool parseLine(const QByteArray& line, Js8CallSpot& outSpot);

// The "type" field's value, or empty if the line isn't a JSON object or the
// field is absent/not-a-string. Lets the client note "also receiving
// RIG.FREQ / STATION.STATUS / ..." once per type instead of logging every
// non-spot line (mirrors N1MMSpotParser::documentRoot()).
QString messageType(const QByteArray& line);

} // namespace AetherSDR::Js8CallParser
