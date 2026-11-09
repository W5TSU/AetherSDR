// `--debug-level` picks what the log copies to the terminal. The file keeps
// every line whatever the level, so a support log is never thinner for it.

#include "core/AsyncLogWriter.h"
#include "core/ConsoleLogLevel.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTime>

#include <cstdio>

#ifndef _WIN32
#include <unistd.h>
#endif

using namespace AetherSDR;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

#ifndef _WIN32
// Runs the writer at `level` with one line of each kind, and returns what
// reached stderr. `file` gets what reached the log file.
static QByteArray consoleAt(int level, const QString& dir, QByteArray* file)
{
    const QString errPath = dir + QStringLiteral("/stderr-%1.txt").arg(level);
    const QString logPath = dir + QStringLiteral("/log-%1.txt").arg(level);
    std::fflush(stderr);
    const int saved = dup(fileno(stderr));
    FILE* redirected = std::freopen(errPath.toLocal8Bit().constData(), "w", stderr);
    if (!redirected) return {};
    {
        AsyncLogWriter writer;
        writer.setConsoleLevel(level);
        writer.start(logPath, level > ConsoleLogLevel::kOff);
        const QTime now = QTime::currentTime();
        writer.enqueue(QtDebugMsg, now, QStringLiteral("t"), QStringLiteral("line-debug"));
        writer.enqueue(QtInfoMsg, now, QStringLiteral("t"), QStringLiteral("line-info"));
        writer.enqueue(QtWarningMsg, now, QStringLiteral("t"), QStringLiteral("line-warning"));
        writer.enqueue(QtCriticalMsg, now, QStringLiteral("t"), QStringLiteral("line-critical"));
        writer.flush();
        writer.shutdown();
    }
    std::fflush(stderr);
    dup2(saved, fileno(stderr));
    close(saved);
    QFile f(logPath);
    if (f.open(QIODevice::ReadOnly)) *file = f.readAll();
    QFile e(errPath);
    return e.open(QIODevice::ReadOnly) ? e.readAll() : QByteArray{};
}
#endif

int main()
{
    // Each level adds to the one below.
    check(!ConsoleLogLevel::shows(0, QtInfoMsg) && !ConsoleLogLevel::shows(0, QtCriticalMsg),
          "0 shows nothing");
    check(ConsoleLogLevel::shows(1, QtInfoMsg), "1 shows info");
    check(!ConsoleLogLevel::shows(1, QtWarningMsg) && !ConsoleLogLevel::shows(1, QtDebugMsg),
          "1 shows only info");
    check(ConsoleLogLevel::shows(2, QtWarningMsg) && ConsoleLogLevel::shows(2, QtCriticalMsg)
              && ConsoleLogLevel::shows(2, QtFatalMsg) && ConsoleLogLevel::shows(2, QtInfoMsg),
          "2 adds warnings and errors");
    check(!ConsoleLogLevel::shows(2, QtDebugMsg), "2 leaves out debug");
    check(ConsoleLogLevel::shows(3, QtDebugMsg) && ConsoleLogLevel::shows(3, QtInfoMsg),
          "3 shows everything");

    check(ConsoleLogLevel::parse(QStringLiteral("0")) == 0, "parses 0");
    check(ConsoleLogLevel::parse(QStringLiteral("3")) == 3, "parses 3");
    check(!ConsoleLogLevel::parse(QStringLiteral("4")), "rejects 4");
    check(!ConsoleLogLevel::parse(QStringLiteral("-1")), "rejects -1");
    check(!ConsoleLogLevel::parse(QStringLiteral("info")), "rejects a word");
    check(!ConsoleLogLevel::parse(QString()), "rejects nothing");

#ifndef _WIN32
    QTemporaryDir dir;
    check(dir.isValid(), "temp dir");
    QByteArray file;

    const QByteArray off = consoleAt(0, dir.path(), &file);
    check(off.isEmpty(), "level 0: the terminal gets nothing");
    check(file.contains("line-debug") && file.contains("line-critical"),
          "level 0: the file still gets every line");

    const QByteArray info = consoleAt(1, dir.path(), &file);
    check(info.contains("line-info"), "level 1: info");
    check(!info.contains("line-warning") && !info.contains("line-critical")
              && !info.contains("line-debug"),
          "level 1: nothing else");

    const QByteArray errors = consoleAt(2, dir.path(), &file);
    check(errors.contains("line-info") && errors.contains("line-warning")
              && errors.contains("line-critical") && !errors.contains("line-debug"),
          "level 2: info, warnings and errors");

    const QByteArray all = consoleAt(3, dir.path(), &file);
    check(all.contains("line-debug") && all.contains("line-critical"), "level 3: everything");
    check(file.contains("line-debug") && file.contains("line-info"),
          "level 3: the file is unchanged");
#endif

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("console_log_level_test: pass");
    return 0;
}
