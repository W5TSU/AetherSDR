// The update check asks this fork's releases. Version numbers are shared with
// upstream, so asking aethersdr/AetherSDR reported 26.11.2 as current while the
// fork had published 26.11.3.

#include "core/UpdateChecker.h"

#include <QString>

#include <cstdio>

using AetherSDR::UpdateChecker;

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_failures; }
}

int main()
{
    const QString page = QString::fromLatin1(UpdateChecker::kReleasesPageUrl);
    const QString latest = QString::fromLatin1(UpdateChecker::kReleasesApiLatestUrl);
    const QString tag = QString::fromLatin1(UpdateChecker::kReleasesApiTagUrl);

    check(page == QStringLiteral("https://github.com/W5TSU/AetherSDR/releases/latest"),
          "View Latest Release opens the fork's page");
    check(latest == QStringLiteral("https://api.github.com/repos/W5TSU/AetherSDR/releases/latest"),
          "the check asks the fork's latest release");
    check(tag.arg(QStringLiteral("v26.11.3"))
              == QStringLiteral("https://api.github.com/repos/W5TSU/AetherSDR/releases/tags/v26.11.3"),
          "What's New reads the fork's release by tag");
    check(!page.contains(QStringLiteral("aethersdr/"), Qt::CaseSensitive)
              && !latest.contains(QStringLiteral("aethersdr/"), Qt::CaseSensitive)
              && !tag.contains(QStringLiteral("aethersdr/"), Qt::CaseSensitive),
          "nothing asks upstream");

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("update_checker_repo_test: pass");
    return 0;
}
