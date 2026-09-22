#include "core/Js8CallParser.h"

#include <QCoreApplication>
#include <QString>
#include <cmath>
#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failed = 0;

void report(const char* name, bool ok)
{
    std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", name);
    if (!ok) ++g_failed;
}

void expectTrue(const char* name, bool ok)
{
    report(name, ok);
}

void expectEqual(const char* name, const QString& got, const QString& want)
{
    if (got == want) {
        report(name, true);
    } else {
        std::printf("[FAIL] %s — got \"%s\", want \"%s\"\n",
                    name, qUtf8Printable(got), qUtf8Printable(want));
        ++g_failed;
    }
}

void expectNear(const char* name, double got, double want, double eps = 1e-9)
{
    if (std::fabs(got - want) < eps) {
        report(name, true);
    } else {
        std::printf("[FAIL] %s — got %f, want %f\n", name, got, want);
        ++g_failed;
    }
}

// Captured (field-accurate) shapes from the JS8Call TCP JSON API.

void testWellFormedRxSpot()
{
    const QByteArray line =
        R"({"params":{"CALL":"M0PXO","DIAL":7078000,"FREQ":7080200,"GRID":"JO01","OFFSET":2200,"SNR":18,"_ID":-1},"type":"RX.SPOT","value":""})";
    Js8CallSpot spot;
    const bool ok = Js8CallParser::parseLine(line, spot);
    expectTrue("well-formed RX.SPOT parses", ok);
    expectEqual("dxCall", spot.dxCall, "M0PXO");
    expectNear("freqMhz from FREQ", spot.freqMhz, 7.0802);
    expectTrue("snr captured", spot.snr == 18);
    expectTrue("comment carries grid", spot.comment.contains("JO01"));
}

void testWellFormedRxDirected()
{
    const QByteArray line =
        R"({"params":{"CMD":" ","DIAL":7078000,"FREQ":7080200,"FROM":"M0PXO","TO":"2E0FGO","TEXT":"M0PXO: 2E0FGO +E65 GM ","OFFSET":2200,"SNR":18,"SPEED":1,"TDRIFT":1.3,"UTC":1769179137513,"_ID":-1},"type":"RX.DIRECTED","value":"message_text"})";
    Js8CallSpot spot;
    const bool ok = Js8CallParser::parseLine(line, spot);
    expectTrue("well-formed RX.DIRECTED parses", ok);
    expectEqual("dxCall comes from FROM", spot.dxCall, "M0PXO");
    expectNear("freqMhz from FREQ", spot.freqMhz, 7.0802);
    expectTrue("comment carries TEXT", spot.comment.contains("2E0FGO"));
    expectTrue("snr captured", spot.snr == 18);
}

void testFreqFallsBackToDialPlusOffset()
{
    // Some captures omit FREQ (it's a convenience field — DIAL + OFFSET is
    // the authoritative pair); the parser must not reject a spot just
    // because the redundant field is missing.
    const QByteArray line =
        R"({"params":{"CALL":"W1ABC","DIAL":14025000,"OFFSET":1500},"type":"RX.SPOT","value":""})";
    Js8CallSpot spot;
    const bool ok = Js8CallParser::parseLine(line, spot);
    expectTrue("RX.SPOT without FREQ parses", ok);
    expectNear("freqMhz from DIAL+OFFSET", spot.freqMhz, 14.0265);
}

void testMissingCallRejected()
{
    const QByteArray line =
        R"({"params":{"DIAL":7078000,"FREQ":7080200,"GRID":"JO01"},"type":"RX.SPOT","value":""})";
    Js8CallSpot spot;
    expectTrue("RX.SPOT without CALL rejected", !Js8CallParser::parseLine(line, spot));
}

void testMissingFromRejected()
{
    const QByteArray line =
        R"({"params":{"DIAL":7078000,"FREQ":7080200,"TEXT":"hello"},"type":"RX.DIRECTED","value":""})";
    Js8CallSpot spot;
    expectTrue("RX.DIRECTED without FROM rejected", !Js8CallParser::parseLine(line, spot));
}

void testMissingFrequencyRejected()
{
    const QByteArray line = R"({"params":{"CALL":"W1ABC"},"type":"RX.SPOT","value":""})";
    Js8CallSpot spot;
    expectTrue("RX.SPOT with no FREQ and no DIAL/OFFSET rejected",
               !Js8CallParser::parseLine(line, spot));
}

void testZeroFrequencyRejected()
{
    const QByteArray line =
        R"({"params":{"CALL":"W1ABC","FREQ":0},"type":"RX.SPOT","value":""})";
    Js8CallSpot spot;
    expectTrue("RX.SPOT with zero FREQ rejected", !Js8CallParser::parseLine(line, spot));
}

void testEmptyCallRejected()
{
    const QByteArray line =
        R"({"params":{"CALL":"   ","FREQ":7080200},"type":"RX.SPOT","value":""})";
    Js8CallSpot spot;
    expectTrue("RX.SPOT with whitespace-only CALL rejected",
               !Js8CallParser::parseLine(line, spot));
}

void testRxActivityNotSpotWorthy()
{
    // No callsign field exists on this message type at all — a fragment of
    // band activity, not a decoded, identified station.
    const QByteArray line =
        R"({"params":{"DIAL":7078000,"FREQ":7080200,"OFFSET":2200,"SNR":18,"SPEED":1,"TDRIFT":1.3,"UTC":1769179137513,"_ID":-1},"type":"RX.ACTIVITY","value":"ZDXB/R/U00 RP72 "})";
    Js8CallSpot spot;
    expectTrue("RX.ACTIVITY is never spot-worthy", !Js8CallParser::parseLine(line, spot));
    expectEqual("messageType still reports RX.ACTIVITY",
                Js8CallParser::messageType(line), "RX.ACTIVITY");
}

void testOtherMessageTypesRejectedButNamed()
{
    const QByteArray freqLine =
        R"({"params":{"DIAL":7078000,"FREQ":7079025,"OFFSET":1025,"_ID":1769178020732},"type":"RIG.FREQ","value":""})";
    Js8CallSpot spot;
    expectTrue("RIG.FREQ is never spot-worthy", !Js8CallParser::parseLine(freqLine, spot));
    expectEqual("messageType reports RIG.FREQ",
                Js8CallParser::messageType(freqLine), "RIG.FREQ");

    const QByteArray pttLine = R"({"params":{"PTT":true},"type":"RIG.PTT","value":"on"})";
    expectTrue("RIG.PTT is never spot-worthy", !Js8CallParser::parseLine(pttLine, spot));
    expectEqual("messageType reports RIG.PTT",
                Js8CallParser::messageType(pttLine), "RIG.PTT");
}

void testMalformedInputRejected()
{
    Js8CallSpot spot;
    expectTrue("empty line rejected", !Js8CallParser::parseLine("", spot));
    expectTrue("not-json rejected", !Js8CallParser::parseLine("this is not json", spot));
    expectTrue("truncated json rejected",
               !Js8CallParser::parseLine(R"({"type":"RX.SPOT","params":{"CALL":)", spot));
    expectTrue("json array (not object) rejected", !Js8CallParser::parseLine("[1,2,3]", spot));
    expectTrue("object without type rejected",
               !Js8CallParser::parseLine(R"({"params":{"CALL":"W1ABC","FREQ":7080200}})", spot));
    expectTrue("object without params rejected",
               !Js8CallParser::parseLine(R"({"type":"RX.SPOT"})", spot));

    expectEqual("messageType on empty line", Js8CallParser::messageType(""), "");
    expectEqual("messageType on garbage", Js8CallParser::messageType("garbage"), "");
    expectEqual("messageType on object without type field",
                Js8CallParser::messageType(R"({"params":{}})"), "");
}

void testSnrOptional()
{
    const QByteArray line =
        R"({"params":{"CALL":"W1ABC","FREQ":14025000},"type":"RX.SPOT","value":""})";
    Js8CallSpot spot;
    expectTrue("RX.SPOT without SNR still parses", Js8CallParser::parseLine(line, spot));
    expectTrue("snr defaults to 0 when absent", spot.snr == 0);
}

void testCallsignTrimmed()
{
    const QByteArray line =
        R"({"params":{"CALL":"  W1ABC  ","FREQ":14025000},"type":"RX.SPOT","value":""})";
    Js8CallSpot spot;
    expectTrue("RX.SPOT with padded CALL parses", Js8CallParser::parseLine(line, spot));
    expectEqual("dxCall is trimmed", spot.dxCall, "W1ABC");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    testWellFormedRxSpot();
    testWellFormedRxDirected();
    testFreqFallsBackToDialPlusOffset();
    testMissingCallRejected();
    testMissingFromRejected();
    testMissingFrequencyRejected();
    testZeroFrequencyRejected();
    testEmptyCallRejected();
    testRxActivityNotSpotWorthy();
    testOtherMessageTypesRejectedButNamed();
    testMalformedInputRejected();
    testSnrOptional();
    testCallsignTrimmed();

    return g_failed == 0 ? 0 : 1;
}
