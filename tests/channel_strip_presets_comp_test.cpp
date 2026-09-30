#include "TestSettingsProfile.h"
#include "core/AudioEngine.h"
#include "core/ChannelStripPresets.h"
#include "core/ClientComp.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cmath>
#include <functional>
#include <cstdio>

using namespace AetherSDR;

// Compressor Drive and Phase in TX channel-strip presets (upstream 76da21b1).
//
// Upstream pins this in aether_tx_profiles_test through AetherTxProfiles and a
// static ChannelStripPresets::applyTxJson(), neither of which this fork has.
// The same three claims are made here through the fork's own preset API --
// savePresetFromCurrent / loadPreset / exportCurrentToFile /
// importPresetFromFile -- which reach the same capture and apply code the fix
// changed.

namespace {

int g_failures = 0;

void report(const char* what, bool ok)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++g_failures;
    }
}

bool near(float a, float b) { return std::fabs(a - b) < 0.01f; }

QJsonObject readJson(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(f.readAll()).object();
}

bool writeJson(const QString& path, const QJsonObject& obj)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return f.write(QJsonDocument(obj).toJson()) > 0;
}

// Export the current strip as a single-preset file, let `edit` rewrite its
// "comp" object, import it, and load what was imported.
bool importEditedPreset(ChannelStripPresets& presets, const QString& dir,
                        const QString& name,
                        const std::function<void(QJsonObject&)>& edit)
{
    const QString path = dir + QStringLiteral("/") + name + QStringLiteral(".json");
    if (!presets.exportCurrentToFile(name, path)) {
        return false;
    }
    QJsonObject root = readJson(path);
    QJsonObject comp = root.value(QStringLiteral("comp")).toObject();
    edit(comp);
    root.insert(QStringLiteral("comp"), comp);
    if (!writeJson(path, root)) {
        return false;
    }
    const QString imported = presets.importPresetFromFile(path);
    return !imported.isEmpty() && presets.loadPreset(imported);
}

} // namespace

int main(int argc, char** argv)
{
    // Before QCoreApplication and before any settings or preset-store access:
    // the preset library lives under GenericConfigLocation.
    TestSettingsProfile profile(QStringLiteral("channel-strip-presets-comp-test"));
    QCoreApplication app(argc, argv);

    QTemporaryDir dir;
    if (!dir.isValid()) {
        std::fprintf(stderr, "no temporary directory\n");
        return 1;
    }

    AudioEngine engine;
    ChannelStripPresets presets(&engine);
    ClientComp* comp = engine.clientCompTx();
    if (!comp) {
        std::fprintf(stderr, "no TX compressor\n");
        return 1;
    }

    // ---- export carries both, and a stored preset restores both ----------
    {
        comp->setDriveDb(7.5f);
        comp->setPhaseRotatorStages(4);
        const QString path = dir.filePath(QStringLiteral("broadcast.json"));
        report("current strip exports", presets.exportCurrentToFile(
                   QStringLiteral("Broadcast"), path));
        const QJsonObject exported =
            readJson(path).value(QStringLiteral("comp")).toObject();
        report("export carries driveDb",
               exported.contains(QStringLiteral("driveDb"))
                   && exported.value(QStringLiteral("driveDb")).toDouble() == 7.5);
        report("export carries phaseRotatorStages",
               exported.contains(QStringLiteral("phaseRotatorStages"))
                   && exported.value(QStringLiteral("phaseRotatorStages")).toInt() == 4);

        report("preset saves", presets.savePresetFromCurrent(QStringLiteral("Broadcast")));
        comp->setDriveDb(1.0f);
        comp->setPhaseRotatorStages(1);
        report("preset loads", presets.loadPreset(QStringLiteral("Broadcast")));
        report("loading restores Drive", near(comp->driveDb(), 7.5f));
        report("loading restores Phase", comp->phaseRotatorStages() == 4);
    }

    // ---- a preset from before the fix leaves both where they are ----------
    {
        comp->setDriveDb(9.0f);
        comp->setPhaseRotatorStages(5);
        const bool loaded = importEditedPreset(
            presets, dir.path(), QStringLiteral("Legacy"), [](QJsonObject& c) {
                c.remove(QStringLiteral("driveDb"));
                c.remove(QStringLiteral("phaseRotatorStages"));
                c.insert(QStringLiteral("thresholdDb"), -24.0);
            });
        report("legacy preset imports and loads", loaded);
        report("legacy preset keeps the current Drive", near(comp->driveDb(), 9.0f));
        report("legacy preset keeps the current Phase", comp->phaseRotatorStages() == 5);
        report("legacy preset still applies what it does carry",
               near(comp->thresholdDb(), -24.0f));
    }

    // ---- out-of-range imported values are clamped, both ways --------------
    {
        const bool high = importEditedPreset(
            presets, dir.path(), QStringLiteral("TooHigh"), [](QJsonObject& c) {
                c.insert(QStringLiteral("driveDb"), 1000.0);
                c.insert(QStringLiteral("phaseRotatorStages"), 1000.0);
            });
        report("over-range preset imports and loads", high);
        report("Drive clamps to 18 dB", near(comp->driveDb(), 18.0f));
        report("Phase clamps to 6 stages", comp->phaseRotatorStages() == 6);

        const bool low = importEditedPreset(
            presets, dir.path(), QStringLiteral("TooLow"), [](QJsonObject& c) {
                c.insert(QStringLiteral("driveDb"), -1000.0);
                c.insert(QStringLiteral("phaseRotatorStages"), -1000.0);
            });
        report("under-range preset imports and loads", low);
        report("Drive clamps to 0 dB", near(comp->driveDb(), 0.0f));
        report("Phase clamps to 0 stages", comp->phaseRotatorStages() == 0);
    }

    return g_failures == 0 ? 0 : 1;
}
