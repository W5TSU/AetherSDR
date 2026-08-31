// `audioCapture analyze <tap>` — turns an audio-quality check (dominant tone,
// level, clipping, comb) into one bridge call. Drives the bridge's private
// line dispatcher through its existing test friend, no socket. The captured
// audio is fed through the public feedAudioData() path, which taps `raw`
// verbatim before any DSP.

#include "core/AudioEngine.h"
#include "core/QsoRecorder.h"
#include "models/RadioModel.h"
#include "core/AutomationServer.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>
#include <cstdio>
#include <vector>

namespace AetherSDR {

class AutomationServerTestAccess {
public:
    static QJsonObject handleLine(AutomationServer& server, const QByteArray& line)
    {
        return server.handleLine(line, nullptr);
    }
};

} // namespace AetherSDR

namespace {

constexpr double kPi = 3.14159265358979323846;
int failures = 0;

void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++failures;
    }
}

QJsonObject bare(AetherSDR::AutomationServer& server, const QString& line)
{
    return AetherSDR::AutomationServerTestAccess::handleLine(server, line.toUtf8());
}

// 24 kHz stereo float32, a `hz` tone at amplitude `amp` on both channels.
QByteArray stereoTone(double hz, int frames, float amp)
{
    QByteArray pcm;
    pcm.resize(frames * 2 * static_cast<int>(sizeof(float)));
    auto* f = reinterpret_cast<float*>(pcm.data());
    for (int n = 0; n < frames; ++n) {
        const float s =
            amp * static_cast<float>(std::sin(2.0 * kPi * hz * n / 24000.0));
        f[2 * n] = s;
        f[2 * n + 1] = s;
    }
    return pcm;
}

} // namespace

int main(int argc, char** argv)
{
    qputenv("AETHER_AUTOMATION", "1");
    QCoreApplication app(argc, argv);

    AetherSDR::AudioEngine engine;
    AetherSDR::AutomationServer server;
    server.setAudioEngine(&engine);

    // Arm the `raw` tap, feed a 1 kHz half-scale tone, stop.
    engine.startAutomationAudioCapture(2000, {QStringLiteral("raw")});
    engine.feedAudioData(stereoTone(1000.0, 24000, 0.5f));
    engine.stopAutomationAudioCapture();

    const QJsonObject r = bare(server, QStringLiteral("audioCapture analyze raw"));
    check(r.value(QStringLiteral("ok")).toBool(), "analyze raw ok");
    check(std::fabs(r.value(QStringLiteral("dominantHz")).toDouble() - 1000.0) <= 2.0,
          "analyze reports the 1 kHz tone");
    check(r.value(QStringLiteral("clippedFraction")).toDouble() == 0.0,
          "clean tone -> 0 clipped fraction");
    {
        const double db = r.value(QStringLiteral("rmsDbfs")).toDouble();
        check(db < -6.0 && db > -12.0, "half-scale sine rms in the -9 dBFS region");
    }
    check(std::fabs(r.value(QStringLiteral("peak")).toDouble() - 0.5) < 0.05,
          "peak ~= 0.5");
    check(r.value(QStringLiteral("combSpacingHz")).toDouble() == 0.0,
          "single tone -> no comb");

    // Unknown tap is rejected.
    {
        const QJsonObject bad = bare(server, QStringLiteral("audioCapture analyze bogus"));
        check(!bad.value(QStringLiteral("ok")).toBool()
                  && bad.value(QStringLiteral("error")).toString().contains(
                         QStringLiteral("tap")),
              "analyze rejects an unknown tap");
    }

    // A tap with nothing captured is reported, not crashed.
    {
        const QJsonObject none = bare(server, QStringLiteral("audioCapture analyze final"));
        check(!none.value(QStringLiteral("ok")).toBool()
                  && none.value(QStringLiteral("error")).toString().contains(
                         QStringLiteral("no captured audio")),
              "analyze on an unused tap says so");
    }

    // Without AETHER_AUTOMATION the action declines, like its siblings.
    qunsetenv("AETHER_AUTOMATION");
    {
        const QJsonObject gated = bare(server, QStringLiteral("audioCapture analyze raw"));
        check(!gated.value(QStringLiteral("ok")).toBool()
                  && gated.value(QStringLiteral("error")).toString().contains(
                         QStringLiteral("AETHER_AUTOMATION")),
              "analyze needs AETHER_AUTOMATION=1");
    }

    std::printf("%s\n", failures == 0 ? "ALL PASS" : "SOME FAILED");
    return failures == 0 ? 0 : 1;
}
