#pragma once

#include <QString>
#include <QtGlobal>

#include <optional>

namespace AetherSDR {

// How much of the log `--debug-level` copies to the terminal. The log file
// always gets everything; this only picks what is mirrored to stderr.
// Each level adds to the one below it:
//   0  nothing (the default)
//   1  info
//   2  info, warnings and errors
//   3  all of the above, plus debug from the categories enabled in Help → Support
namespace ConsoleLogLevel {

constexpr int kOff = 0;
constexpr int kMax = 3;

[[nodiscard]] inline bool shows(int level, QtMsgType type) noexcept
{
    switch (type) {
    case QtInfoMsg:     return level >= 1;
    case QtWarningMsg:
    case QtCriticalMsg:
    case QtFatalMsg:    return level >= 2;
    case QtDebugMsg:    return level >= 3;
    }
    return false;
}

// "0" to "3". Anything else, including an empty string, is nullopt.
[[nodiscard]] inline std::optional<int> parse(const QString& text)
{
    bool ok = false;
    const int level = text.trimmed().toInt(&ok);
    if (!ok || level < kOff || level > kMax)
        return std::nullopt;
    return level;
}

}  // namespace ConsoleLogLevel
}  // namespace AetherSDR
