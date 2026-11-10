#pragma once

#include <QNetworkAccessManager>
#include <QObject>

namespace AetherSDR {

// Checks the GitHub releases API for a newer AetherSDR version.
//
// These are this fork's releases, not upstream's: version numbers are shared,
// so asking aethersdr/AetherSDR would report a fork build as current, or offer
// upstream's build as its update. Downloads the fork doesn't publish itself
// (the ASR models, the NVIDIA BNR pack) still come from upstream's releases.
class UpdateChecker : public QObject {
    Q_OBJECT
public:
    static constexpr char kReleasesPageUrl[] =
        "https://github.com/W5TSU/AetherSDR/releases/latest";
    static constexpr char kReleasesApiLatestUrl[] =
        "https://api.github.com/repos/W5TSU/AetherSDR/releases/latest";
    // %1 is the tag, percent-encoded.
    static constexpr char kReleasesApiTagUrl[] =
        "https://api.github.com/repos/W5TSU/AetherSDR/releases/tags/%1";

    explicit UpdateChecker(QObject* parent = nullptr);
    void checkNow();

signals:
    void updateAvailable(const QString& latestVersion);
    void upToDate(const QString& currentVersion);
    void checkFailed();

private:
    QNetworkAccessManager m_nam;
    bool m_inFlight = false;
};

} // namespace AetherSDR
